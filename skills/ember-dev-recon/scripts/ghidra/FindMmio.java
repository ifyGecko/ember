// FindMmio.java - headless GhidraScript: harvest candidate MMIO (device-register) windows
// from a loaded firmware image, with the per-register detail a device model needs.
//
// Real peripheral accesses materialize an absolute address and read/write it. Any such
// address that lands OUTSIDE every defined memory block is not image code/data: it is a
// hardware register (or RAM the loader did not map - see the RAM-alias filter below).
// References into a second address space (AVR8 data, PIC DATA, 8051 SFR/EXTMEM, Z80 io)
// are all reported, per space: there the RAM and the device registers share the space,
// and the part's data sheet (or the ember model's layout) tells them apart.
//
// Passes (each access is counted once, keyed by instruction address + target + kind):
//   (A) references Ghidra already resolved; read/write from the reference type;
//   (B) MIPS lui + addiu/ori and lui + load/store-displacement reconstruction, for
//       accesses Ghidra did not turn into references (state reset at function boundaries).
//
// Output per page (default 64 KiB; 256 bytes on 8- and 16-bit processors): total / reads /
// writes, the register offsets touched (offset:count), and the functions that touch the
// page - so each row can be labeled (UART, timer, interrupt controller, flash, ...) by
// reading those functions. Pages are kept per address space: on Harvard and port-I/O
// processors a reference into another space is printed with the space's name (AVR8 `mem`
// = the data space, 8051 `SFR`/`EXTMEM`, Z80 `io`, PIC `DATA`), and that space's
// addresses are the ones the data sheet uses. References into the 8051's internal RAM
// (INTMEM: the register banks, the stack) and into register spaces are not device accesses
// and are skipped.
//
// Usage (headless): -postScript FindMmio.java <out.txt | an existing directory: <dir>/<program>.mmio.txt>
//                     [page_size_hex] [ram_size_hex]
//   ram_size_hex: kseg0/kseg1 addresses whose physical address is below it are treated
//   as RAM aliases, not MMIO (MIPS only; default 0x10000000).
//   On MIPS, kuseg targets (< 0x80000000) are dropped too: in kernel images they are
//   lui-built constants / struct offsets, not device registers.
//   On x86 (PC firmware) addresses below ram_size (default 0x80000000, the usual bottom of
//   the PCI/MMIO hole) are RAM - temporary RAM, page tables, HOBs - except the legacy
//   0xA0000-0xFFFFF window (VGA, shadowed BIOS areas); device MMIO lives above.
//@category Analysis.Firmware
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

public class FindMmio extends GhidraScript {

    private long pageSize = 0x10000L;
    private long ramSize = 0x10000000L;
    private boolean mips, x86, ramGiven, pageGiven;
    private String defaultSpace = "";

    private static class Page {
        int reads, writes, formed;
        TreeMap<Long, Integer> offs = new TreeMap<>();
        TreeMap<String, Integer> funcs = new TreeMap<>();
    }

    /* key: address space name + page base */
    private final Map<String, Page> pages = new HashMap<>();
    private final Set<String> seen = new HashSet<>();

    /* The report file: the argument, or <dir>/<program>.mmio.txt when it names a directory
     * (one report per program of a multi-file import). */
    private String outPath(String arg, String suffix) {
        java.io.File f = new java.io.File(arg);
        return f.isDirectory() ? new java.io.File(f, currentProgram.getName() + "." + suffix + ".txt").getPath() : arg;
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length > 1) { pageSize = Long.decode(args[1]); pageGiven = true; }
        if (args.length > 2) { ramSize = Long.decode(args[2]); ramGiven = true; }
        PrintWriter out = (args.length > 0)
            ? new PrintWriter(new java.io.FileWriter(outPath(args[0], "mmio")))
            : new PrintWriter(System.out);
        try {
            Program p = currentProgram;
            Memory mem = p.getMemory();
            Listing listing = p.getListing();
            mips = p.getLanguageID().toString().startsWith("MIPS");
            x86 = p.getLanguageID().toString().startsWith("x86");
            if (x86 && !ramGiven) ramSize = 0x80000000L;
            defaultSpace = p.getAddressFactory().getDefaultAddressSpace().getName();
            String proc = p.getLanguage().getProcessor().toString();
            boolean small = p.getDefaultPointerSize() <= 2 || proc.startsWith("PIC") || proc.equals("AVR8")
                || proc.equals("8051") || proc.equals("80251") || proc.equals("80390")
                || proc.equals("6502") || proc.equals("65C02") || proc.equals("Z80") || proc.equals("Z180");
            if (small && !pageGiven) pageSize = 0x100L;

            // ---- pass A: resolved references outside mapped memory ----
            InstructionIterator iit = listing.getInstructions(true);
            while (iit.hasNext() && !monitor.isCancelled()) {
                Instruction insn = iit.next();
                for (Reference r : insn.getReferencesFrom()) {
                    if (!r.isMemoryReference()) continue;
                    Address to = r.getToAddress();
                    if (to == null) continue;
                    String sp = to.getAddressSpace().getName();
                    long off = to.getOffset();
                    if (to.getAddressSpace().isRegisterSpace() || sp.equals("INTMEM") || sp.equals("register")
                            || sp.equals("HWSTACK") || sp.equals("codebyte")) continue;
                    if (sp.equals("BITS")) {                // 8051 bit addresses: 0x80+ are SFR bits
                        if (off < 0x80) continue;
                        sp = "SFR"; off &= 0xF8;
                    }
                    // the default (code) space: only what no block maps is a device. Another
                    // space (a Harvard data space, SFRs, I/O ports) is reported whole: Ghidra
                    // maps it with placeholder blocks, and its device registers sit there.
                    if (sp.equals(defaultSpace) && mem.contains(to)) continue;
                    char kind = r.getReferenceType().isWrite() ? 'W'
                              : r.getReferenceType().isRead() ? 'R' : 'A';
                    record(p, insn, sp, off, kind);
                }
            }

            // ---- pass B (MIPS): lui/addiu|ori and lui + load/store displacement ----
            if (mips) {
                Map<Register, Long> hi = new HashMap<>();
                Function cur = null;
                iit = listing.getInstructions(true);
                while (iit.hasNext() && !monitor.isCancelled()) {
                    Instruction insn = iit.next();
                    Function f = getFunctionContaining(insn.getAddress());
                    if (f != cur) { hi.clear(); cur = f; }
                    String mn = insn.getMnemonicString().toLowerCase().replace("_", "");
                    try {
                        if (mn.equals("lui")) {
                            Register rd = insn.getRegister(0);
                            Scalar imm = insn.getScalar(1);
                            if (rd != null && imm != null)
                                hi.put(rd, (imm.getUnsignedValue() & 0xffffL) << 16);
                        } else if (mn.equals("addiu") || mn.equals("ori")) {
                            Register rt = insn.getRegister(0);
                            Register rs = insn.getRegister(1);
                            Scalar imm = insn.getScalar(2);
                            if (rt != null && rs != null && imm != null && hi.containsKey(rs)) {
                                long full = mn.equals("ori")
                                    ? hi.get(rs) | (imm.getUnsignedValue() & 0xffffL)
                                    : hi.get(rs) + imm.getSignedValue();
                                full &= 0xffffffffL;
                                if (!mem.contains(addr(p, full))) record(p, insn, defaultSpace, full, 'A');
                                hi.put(rt, full);
                            } else if (rt != null) {
                                hi.remove(rt);
                            }
                        } else if (isLoad(mn) || isStore(mn)) {
                            Register base = loadStoreBase(insn);
                            Scalar disp = firstScalar(insn);
                            if (base != null && disp != null && hi.containsKey(base)) {
                                long full = (hi.get(base) + disp.getSignedValue()) & 0xffffffffL;
                                if (!mem.contains(addr(p, full)))
                                    record(p, insn, defaultSpace, full, isStore(mn) ? 'W' : 'R');
                            }
                            if (isLoad(mn)) {
                                Register rt = insn.getRegister(0);
                                if (rt != null) hi.remove(rt);
                            }
                        } else if (mn.equals("jr") || mn.equals("jalr")) {
                            hi.clear();
                        }
                    } catch (Exception ignore) { /* operand shapes vary; skip */ }
                }
            }

            // ---- rank & print ----
            List<Map.Entry<String, Page>> ranked = new ArrayList<>(pages.entrySet());
            ranked.sort((a, b) -> Integer.compare(b.getValue().reads + b.getValue().writes,
                                                  a.getValue().reads + a.getValue().writes));
            out.printf("=== candidate MMIO pages (page size 0x%x), ranked by load/store count ===%n",
                       pageSize);
            out.println("page_base   phys        total  R     W     A(address formed)");
            List<String> formedOnly = new ArrayList<>();
            for (Map.Entry<String, Page> e : ranked) {
                Page pg = e.getValue();
                String key = e.getKey();
                int colon = key.lastIndexOf(':');
                String sp = key.substring(0, colon);
                long page = Long.parseLong(key.substring(colon + 1), 16);
                String label = sp.equals(defaultSpace) ? String.format("0x%08x", page)
                                                       : String.format("%s:0x%04x", sp, page);
                if (pg.reads + pg.writes == 0) { formedOnly.add(label); continue; }
                out.printf("%-10s  0x%08x  %5d  %-5d %-5d %d%n", label, phys(page),
                    pg.reads + pg.writes, pg.reads, pg.writes, pg.formed);
                StringBuilder sb = new StringBuilder("    offsets: ");
                for (Map.Entry<Long, Integer> o : pg.offs.entrySet())
                    sb.append(String.format("+0x%x:%d ", o.getKey(), o.getValue()));
                out.println(sb.toString().trim());
                List<Map.Entry<String, Integer>> fs = new ArrayList<>(pg.funcs.entrySet());
                fs.sort((a, b) -> b.getValue() - a.getValue());
                sb = new StringBuilder("    functions: ");
                for (int i = 0; i < Math.min(6, fs.size()); i++)
                    sb.append(fs.get(i).getKey()).append("(").append(fs.get(i).getValue()).append(") ");
                if (fs.size() > 6) sb.append("... +").append(fs.size() - 6);
                out.println(sb.toString().trim());
            }
            if (!formedOnly.isEmpty()) {
                out.println();
                out.println("address formed but never loaded/stored directly (constants, KSEG");
                out.println("conversions, or base pointers stored in driver structs - check xrefs):");
                StringBuilder sb = new StringBuilder("  ");
                for (String pgb : formedOnly) sb.append(pgb).append(' ');
                out.println(sb);
            }
            if (ranked.isEmpty())
                out.println("(none) -- ensure auto-analysis ran and the base address is correct.");
        } finally {
            out.flush();
            if (args.length > 0) out.close();
        }
        println("FindMmio: done.");
    }

    private long phys(long a) {
        return (mips && a >= 0x80000000L && a <= 0xBFFFFFFFL) ? (a & 0x1FFFFFFFL) : a;
    }

    private Address addr(Program p, long off) {
        return p.getAddressFactory().getDefaultAddressSpace().getAddress(off & 0xffffffffL);
    }

    private void record(Program p, Instruction insn, String space, long full, char kind) {
        long a = full & 0xffffffffL;
        boolean dflt = space.equals(defaultSpace);
        if (dflt && mips && a < 0x80000000L) return;            // kuseg: constants, not MMIO
        if (dflt && mips && a >= 0x80000000L && a <= 0xBFFFFFFFL && (a & 0x1FFFFFFFL) < ramSize)
            return;                                              // RAM alias, not MMIO
        if (dflt && x86 && a < ramSize && !(a >= 0xA0000L && a < 0x100000L))
            return;                                              // PC RAM, not MMIO
        if (!seen.add(insn.getAddress() + ">" + space + ":" + Long.toHexString(a) + kind)) return;
        long page = a & ~(pageSize - 1);
        Page pg = pages.computeIfAbsent(space + ":" + Long.toHexString(page), k -> new Page());
        if (kind == 'R') pg.reads++;
        else if (kind == 'W') pg.writes++;
        else { pg.formed++; return; }
        pg.offs.merge(a - page, 1, Integer::sum);
        Function f = p.getFunctionManager().getFunctionContaining(insn.getAddress());
        pg.funcs.merge(f != null ? f.getName() : insn.getAddress().toString(), 1, Integer::sum);
    }

    private boolean isLoad(String mn) {
        return mn.equals("lw") || mn.equals("lh") || mn.equals("lhu") || mn.equals("lb")
            || mn.equals("lbu") || mn.equals("lwl") || mn.equals("lwr");
    }

    private boolean isStore(String mn) {
        return mn.equals("sw") || mn.equals("sh") || mn.equals("sb")
            || mn.equals("swl") || mn.equals("swr");
    }

    // load/store operand layout: op rt, disp(base) -> the register inside operand 1
    private Register loadStoreBase(Instruction insn) {
        for (int i = 1; i < insn.getNumOperands(); i++)
            for (Object o : insn.getOpObjects(i))
                if (o instanceof Register) return (Register) o;
        return null;
    }

    private Scalar firstScalar(Instruction insn) {
        for (int i = 0; i < insn.getNumOperands(); i++)
            for (Object o : insn.getOpObjects(i))
                if (o instanceof Scalar) return (Scalar) o;
        return null;
    }
}
