// FindX86Io.java - headless GhidraScript for x86 firmware (UEFI modules, coreboot stages,
// legacy BIOS): the hardware accesses a memory-reference scan cannot see.
//
//   - I/O ports: every IN/OUT/INS/OUTS with its port, size, direction, the constant written
//     when known, and the functions doing it;
//   - PCI configuration through 0xCF8/0xCFC: each constant address written to 0xCF8,
//     decoded as bus:device.function + register;
//   - MSRs: every RDMSR/WRMSR with its index (and the EDX:EAX value written when known);
//   - CPUID leaves the code asks for (the identity checks a board's CPUID must satisfy);
//   - PCI configuration through a PCI library: constant PCI_LIB_ADDRESS(bus, dev, fn, reg)
//     arguments of the functions that drive 0xCF8 (edk2 PciCf8Lib/PciLib: PciRead8(...)),
//     decoded - the configuration registers the firmware reads and writes, per function;
//   - MMIO through accessor functions: constant addresses passed to small leaf functions
//     that load or store through their argument (edk2 MmioRead32/MmioWrite32 and the like).
//
// Operands resolve by constant propagation inside the function; when the operand is the
// function's argument (edk2 IoLib/BaseLib/PciLib wrappers: IoRead8(Port), AsmReadMsr64
// (Index), AsmCpuid(Index, ...), IoWrite32(0xCF8, Address)), it resolves at each call site
// instead, following wrappers of wrappers: argument 0 is the port / MSR / leaf / address,
// argument 1 the value written (IA32: stack slots, cdecl; x64: RCX/RDX, Microsoft ABI, or
// RDI/RSI, System V). Rows found that way name the accessor ("via ...").
//
// Ports become ember devices or --mmio-rules at UAM_X86_PIO_BASE + port; MSRs become
// platform rdmsr/wrmsr hooks in the board; CPUID checks become the board's
// uam_x86_set_cpuid identity. Sites that still did not resolve are counted: read those
// functions (the port often comes from a PCI BAR the firmware programmed - PMBASE, SMBus,
// GPIO - or from a table).
//
// Usage (headless): -postScript FindX86Io.java <out.txt | an existing directory: <dir>/<program>.x86io.txt>
//@category Analysis.Firmware
import ghidra.app.plugin.core.analysis.ConstantPropagationContextEvaluator;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.OperandType;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.util.SymbolicPropogator;
import java.io.PrintWriter;
import java.util.*;

public class FindX86Io extends GhidraScript {

    private static final int ACCESSOR_MAX = 48;   // instructions: an accessor is small
    private static final int DEPTH_MAX = 3;       // wrappers of wrappers

    private static class Site {
        int reads, writes;
        TreeSet<Integer> sizes = new TreeSet<>();
        TreeSet<String> funcs = new TreeSet<>();
        TreeSet<String> values = new TreeSet<>();
    }

    /* An access whose operand is the function's argument 0 (value written: argument 1). */
    private static class Pending {
        String kind;      // "port", "msr", "cpuid", "mmio"
        boolean write;
        int size;
        String via;       // the accessor at the bottom of the chain
    }

    private final TreeMap<Long, Site> ports = new TreeMap<>(), msrs = new TreeMap<>(),
        cpuid = new TreeMap<>(), pci = new TreeMap<>(), mmio = new TreeMap<>(), pciLib = new TreeMap<>();
    private final Set<Function> cf8Writers = new HashSet<>();
    private final TreeSet<String> unresolved = new TreeSet<>();
    private int unresolvedSites, viaAccessor;
    private final Map<Function, SymbolicPropogator> props = new HashMap<>();
    private boolean is64;

    private Site site(TreeMap<Long, Site> m, long k) { return m.computeIfAbsent(k, x -> new Site()); }

    private SymbolicPropogator prop(Function f) {
        if (props.containsKey(f))
            return props.get(f);
        SymbolicPropogator p = null;
        try {
            p = new SymbolicPropogator(currentProgram);
            p.flowConstants(f.getEntryPoint(), f.getBody(),
                            new ConstantPropagationContextEvaluator(monitor, true), true, monitor);
        } catch (Exception e) {
            p = null;
        }
        props.put(f, p);
        return p;
    }

    private Long reg(SymbolicPropogator p, Address a, String... names) {
        if (p == null)
            return null;
        for (String n : names) {
            Register r = currentProgram.getRegister(n);
            if (r == null)
                continue;
            SymbolicPropogator.Value v = p.getRegisterValue(a, r);
            if (v != null && !v.isRegisterRelativeValue())
                return v.getValue();
        }
        return null;
    }

    private static int sizeOf(String mn, Instruction in) {
        if (mn.endsWith("SB")) return 1;
        if (mn.endsWith("SW")) return 2;
        if (mn.endsWith("SD")) return 4;
        for (int i = 0; i < in.getNumOperands(); i++)
            for (Object o : in.getOpObjects(i))
                if (o instanceof Register) {
                    String r = ((Register) o).getName().toUpperCase();
                    if (r.equals("AL")) return 1;
                    if (r.equals("AX")) return 2;
                    if (r.equals("EAX")) return 4;
                }
        return 0;
    }

    private static boolean isIoMnemonic(String mn) {
        return mn.equals("IN") || mn.equals("OUT") || mn.startsWith("INS") || mn.startsWith("OUTS");
    }

    private void record(String kind, long key, boolean write, int size, Long value, String fname) {
        if (kind.equals("port")) {
            key &= 0xFFFF;
            Site s = site(ports, key);
            if (write) s.writes++; else s.reads++;
            if (size > 0) s.sizes.add(size);
            s.funcs.add(fname);
            if (value != null) {
                long m = size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFL;
                if (s.values.size() < 12)
                    s.values.add(String.format("0x%x", value & m));
                if (key == 0xCF8 && (value & 0x80000000L) != 0) {
                    Site p = site(pci, value & 0x7FFFFFFCL);
                    p.writes++;
                    p.funcs.add(fname);
                }
            }
        } else if (kind.equals("msr")) {
            Site s = site(msrs, key & 0xFFFFFFFFL);
            if (write) s.writes++; else s.reads++;
            s.funcs.add(fname);
            if (value != null && s.values.size() < 8)
                s.values.add(String.format("0x%x", value));
        } else if (kind.equals("pci")) {
            Site s = site(pciLib, key & 0xFFFFFFFL);
            if (write) s.writes++; else s.reads++;
            if (size > 0) s.sizes.add(size);
            s.funcs.add(fname);
            if (value != null && s.values.size() < 8)
                s.values.add(String.format("0x%x", value));
        } else if (kind.equals("cpuid")) {
            Site s = site(cpuid, key & 0xFFFFFFFFL);
            s.reads++;
            s.funcs.add(fname);
        } else {
            Site s = site(mmio, key);
            if (write) s.writes++; else s.reads++;
            if (size > 0) s.sizes.add(size);
            s.funcs.add(fname);
            if (value != null && s.values.size() < 8)
                s.values.add(String.format("0x%x", value));
        }
    }

    private int instructionCount(Function f) {
        int n = 0;
        for (Instruction in : currentProgram.getListing().getInstructions(f.getBody(), true))
            if (++n > ACCESSOR_MAX)
                break;
        return n;
    }

    private boolean isLeaf(Function f) {
        for (Instruction in : currentProgram.getListing().getInstructions(f.getBody(), true))
            if (in.getFlowType().isCall())
                return false;
        return true;
    }

    /* Argument `idx` at a call site: x64 registers, IA32 the stack slot [ESP + 4*idx] as the
     * pushes and stores before the call leave it. */
    private Long argAt(Instruction call, int idx) {
        Function caller = getFunctionContaining(call.getAddress());
        SymbolicPropogator p = caller != null ? prop(caller) : null;
        if (is64) {
            String[][] regs = { { "RCX", "RDI" }, { "RDX", "RSI" } };
            return idx < 2 ? reg(p, call.getAddress(), regs[idx]) : null;
        }
        int pushes = 0;
        Instruction in = call.getPrevious();
        for (int n = 0; in != null && n < 16; n++, in = in.getPrevious()) {
            String mn = in.getMnemonicString().toUpperCase();
            if (in.getFlowType().isCall() || in.getFlowType().isJump() || in.getFlowType().isTerminal())
                break;
            if (mn.equals("PUSH")) {
                if (pushes == idx) {
                    Scalar s = in.getScalar(0);
                    if (s != null)
                        return s.getUnsignedValue() & 0xFFFFFFFFL;
                    Object[] o = in.getOpObjects(0);
                    if (o.length == 1 && o[0] instanceof Register)
                        return reg(p, in.getAddress(), ((Register) o[0]).getName());
                    return null;
                }
                pushes++;
            } else if (mn.equals("MOV") && in.getNumOperands() == 2) {
                Object[] dst = in.getOpObjects(0);
                boolean esp = false;
                long disp = 0;
                for (Object o : dst) {
                    if (o instanceof Register && ((Register) o).getName().equalsIgnoreCase("ESP"))
                        esp = true;
                    else if (o instanceof Scalar)
                        disp = ((Scalar) o).getSignedValue();
                }
                if (esp && (in.getOperandType(0) & OperandType.DYNAMIC) != 0 && disp == 4L * (idx - pushes)) {
                    Scalar s = in.getScalar(1);
                    if (s != null)
                        return s.getUnsignedValue() & 0xFFFFFFFFL;
                    Object[] o = in.getOpObjects(1);
                    if (o.length == 1 && o[0] instanceof Register)
                        return reg(p, in.getAddress(), ((Register) o[0]).getName());
                    return null;
                }
            }
        }
        return null;
    }

    /* The instruction that last wrote register `r` (or a part of it) before `at`, within 24
     * instructions and without crossing a call, or null. */
    private Instruction lastWriter(Instruction at, Register r) {
        Register base = r.getBaseRegister();
        Instruction in = at.getPrevious();
        for (int n = 0; in != null && n < 24; n++, in = in.getPrevious()) {
            if (in.getFlowType().isCall())
                return null;
            for (Object o : in.getResultObjects())
                if (o instanceof Register && ((Register) o).getBaseRegister().equals(base))
                    return in;
        }
        return null;
    }

    private static boolean stackMem(Instruction in, int op) {
        if ((in.getOperandType(op) & OperandType.DYNAMIC) == 0)
            return false;
        for (Object o : in.getOpObjects(op))
            if (o instanceof Register) {
                String r = ((Register) o).getName().toUpperCase();
                if (r.endsWith("SP") || r.endsWith("BP"))
                    return true;
            }
        return false;
    }

    /* Does the call pass this function's own argument through unchanged as argument idx (a
     * wrapper), rather than a value it computed (PciCf8Read8 computing 0xCFC + offset)? */
    private boolean passThrough(Instruction call, int idx) {
        if (is64) {
            Register r = currentProgram.getRegister(idx == 0 ? "RCX" : "RDX");
            Instruction w = lastWriter(call, r);
            if (w == null)
                return true;                               // the incoming register itself
            String mn = w.getMnemonicString().toUpperCase();
            return mn.equals("MOV") && (stackMem(w, 1) || w.getOpObjects(1).length == 1 &&
                                        w.getOpObjects(1)[0] instanceof Register);
        }
        int pushes = 0;
        Instruction in = call.getPrevious();
        for (int n = 0; in != null && n < 16; n++, in = in.getPrevious()) {
            String mn = in.getMnemonicString().toUpperCase();
            if (in.getFlowType().isCall() || in.getFlowType().isJump() || in.getFlowType().isTerminal())
                return false;
            boolean slot = false;
            int src = 0;
            if (mn.equals("PUSH")) {
                slot = pushes++ == idx;
            } else if (mn.equals("MOV") && in.getNumOperands() == 2 && stackMem(in, 0)) {
                long disp = 0;
                boolean esp = false;
                for (Object o : in.getOpObjects(0)) {
                    if (o instanceof Register && ((Register) o).getName().equalsIgnoreCase("ESP"))
                        esp = true;
                    else if (o instanceof Scalar)
                        disp = ((Scalar) o).getSignedValue();
                }
                slot = esp && disp == 4L * (idx - pushes);
                src = 1;
            }
            if (!slot)
                continue;
            if (stackMem(in, src))
                return true;                               // pushed straight from an argument
            Object[] o = in.getOpObjects(src);
            if (o.length != 1 || !(o[0] instanceof Register))
                return false;
            Instruction w = lastWriter(in, (Register) o[0]);
            for (int k = 0; w != null && k < 2; k++) {
                if (!w.getMnemonicString().equalsIgnoreCase("MOV"))
                    return false;
                if (stackMem(w, 1))
                    return true;
                Object[] o2 = w.getOpObjects(1);
                if (o2.length != 1 || !(o2[0] instanceof Register))
                    return false;
                w = lastWriter(w, (Register) o2[0]);
            }
            return false;
        }
        return false;
    }

    /* The report file: the argument, or <dir>/<program>.x86io.txt when it names a directory
     * (one report per program of a multi-file import). */
    private String outPath(String arg, String suffix) {
        java.io.File f = new java.io.File(arg);
        return f.isDirectory() ? new java.io.File(f, currentProgram.getName() + "." + suffix + ".txt").getPath() : arg;
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = args.length > 0 ? new PrintWriter(new java.io.FileWriter(outPath(args[0], "x86io")))
                                          : new PrintWriter(System.out);
        try {
            String lang = currentProgram.getLanguageID().toString();
            if (!lang.startsWith("x86")) {
                out.println("FindX86Io: not an x86 program (" + lang + ")");
                return;
            }
            is64 = currentProgram.getDefaultPointerSize() == 8;
            Listing listing = currentProgram.getListing();
            Map<Function, List<Pending>> accessors = new LinkedHashMap<>();
            int nfunc = 0;
            for (Function f : listing.getFunctions(true)) {
                if (monitor.isCancelled())
                    break;
                nfunc++;
                scanFunction(f, listing, accessors);
            }
            // functions that drive 0xCF8 through a port-write accessor: the PCI library
            for (Map.Entry<Function, List<Pending>> e : accessors.entrySet())
                for (Pending pd : e.getValue()) {
                    if (!pd.kind.equals("port") || !pd.write)
                        continue;
                    for (Reference r : getReferencesTo(e.getKey().getEntryPoint())) {
                        Instruction call = r.getReferenceType().isCall() ? getInstructionAt(r.getFromAddress()) : null;
                        Long port = call != null ? argAt(call, 0) : null;
                        Function caller = call != null ? getFunctionContaining(call.getAddress()) : null;
                        if (port != null && (port & 0xFFFF) == 0xCF8 && caller != null)
                            cf8Writers.add(caller);
                    }
                }
            // resolve accessor arguments at their call sites, through wrappers
            Deque<Object[]> work = new ArrayDeque<>();
            for (Map.Entry<Function, List<Pending>> e : accessors.entrySet())
                for (Pending pd : e.getValue())
                    work.add(new Object[] { e.getKey(), pd, 0 });
            Set<String> done = new HashSet<>();
            while (!work.isEmpty() && !monitor.isCancelled()) {
                Object[] w = work.poll();
                Function acc = (Function) w[0];
                Pending pd = (Pending) w[1];
                int depth = (Integer) w[2];
                if (pd.kind.equals("port") && cf8Writers.contains(acc)) {
                    // a PCI library function with its port access inlined (PciCf8Read16)
                    Pending lib = new Pending();
                    lib.kind = "pci"; lib.write = pd.write; lib.size = pd.size;
                    lib.via = acc.getName() + "@" + acc.getEntryPoint();
                    pd = lib;
                    depth = 0;
                }
                if (!done.add(acc.getEntryPoint() + "/" + pd.kind + pd.write + pd.size))
                    continue;
                for (Reference r : getReferencesTo(acc.getEntryPoint())) {
                    if (!r.getReferenceType().isCall())
                        continue;
                    Instruction call = getInstructionAt(r.getFromAddress());
                    Function caller = call != null ? getFunctionContaining(call.getAddress()) : null;
                    if (call == null || caller == null)
                        continue;
                    String cname = caller.getName() + "@" + caller.getEntryPoint() + " via " + pd.via;
                    Long key = argAt(call, 0);
                    if (key != null) {
                        Long val = pd.write ? argAt(call, 1) : null;
                        record(pd.kind, key, pd.write, pd.size, val, cname);
                        viaAccessor++;
                    } else if (!pd.kind.equals("pci") && pd.kind.equals("port") && cf8Writers.contains(caller)) {
                        // the PCI library: its argument 0 is a PCI_LIB_ADDRESS, not a port
                        Pending lib = new Pending();
                        lib.kind = "pci"; lib.write = pd.write; lib.size = pd.size;
                        lib.via = caller.getName() + "@" + caller.getEntryPoint();
                        work.add(new Object[] { caller, lib, 0 });
                    } else if (depth < DEPTH_MAX && instructionCount(caller) <= ACCESSOR_MAX &&
                               passThrough(call, 0)) {
                        work.add(new Object[] { caller, pd, depth + 1 });
                    } else {
                        unresolvedSites++;
                        unresolved.add(caller.getName() + "@" + caller.getEntryPoint() + " (" + pd.kind + " via " + pd.via + ")");
                    }
                }
            }
            report(out, nfunc);
        } finally {
            out.close();
        }
    }

    private void scanFunction(Function f, Listing listing, Map<Function, List<Pending>> accessors) {
        boolean any = false;
        for (Instruction in : listing.getInstructions(f.getBody(), true)) {
            String mn = in.getMnemonicString().toUpperCase();
            if (isIoMnemonic(mn) || mn.equals("RDMSR") || mn.equals("WRMSR") || mn.equals("CPUID")) {
                any = true;
                break;
            }
        }
        boolean small = instructionCount(f) <= ACCESSOR_MAX;
        String fname = f.getName() + "@" + f.getEntryPoint();
        if (any) {
            SymbolicPropogator p = prop(f);
            for (Instruction in : listing.getInstructions(f.getBody(), true)) {
                String mn = in.getMnemonicString().toUpperCase();
                Address a = in.getAddress();
                String kind = null;
                boolean write = false;
                int size = 0;
                Long key = null, val = null;
                if (isIoMnemonic(mn)) {
                    kind = "port";
                    write = mn.startsWith("OUT");
                    size = sizeOf(mn, in);
                    for (int i = 0; i < in.getNumOperands() && key == null; i++) {
                        Scalar s = in.getScalar(i);
                        if (s != null)
                            key = s.getUnsignedValue();
                    }
                    if (key == null)
                        key = reg(p, a, "DX", "EDX", "RDX");
                    if (write && !mn.startsWith("OUTS"))
                        val = reg(p, a, size == 1 ? "AL" : size == 2 ? "AX" : "EAX", "EAX", "RAX");
                } else if (mn.equals("RDMSR") || mn.equals("WRMSR")) {
                    kind = "msr";
                    write = mn.equals("WRMSR");
                    key = reg(p, a, "ECX", "RCX");
                    if (write) {
                        Long lo = reg(p, a, "EAX", "RAX"), hi = reg(p, a, "EDX", "RDX");
                        if (lo != null && hi != null)
                            val = ((hi & 0xFFFFFFFFL) << 32) | (lo & 0xFFFFFFFFL);
                    }
                } else if (mn.equals("CPUID")) {
                    kind = "cpuid";
                    key = reg(p, a, "EAX", "RAX");
                }
                if (kind == null)
                    continue;
                if (key != null) {
                    record(kind, key, write, size, val, fname);
                    if (kind.equals("port") && write && (key & 0xFFFF) == 0xCF8)
                        cf8Writers.add(f);
                } else if (small) {
                    Pending pd = new Pending();
                    pd.kind = kind; pd.write = write; pd.size = size; pd.via = fname;
                    accessors.computeIfAbsent(f, x -> new ArrayList<>()).add(pd);
                } else {
                    unresolvedSites++;
                    unresolved.add(fname + " (" + kind + ")");
                }
            }
        }
        // a small leaf that loads or stores through a register: an MMIO accessor candidate
        if (small && !any && isLeaf(f)) {
            int mem = 0;
            boolean write = false;
            for (Instruction in : listing.getInstructions(f.getBody(), true)) {
                String mn = in.getMnemonicString().toUpperCase();
                if (!mn.equals("MOV") && !mn.equals("MOVZX") && !mn.equals("MOVSX"))
                    continue;
                for (int i = 0; i < in.getNumOperands(); i++) {
                    if ((in.getOperandType(i) & OperandType.DYNAMIC) == 0)
                        continue;
                    boolean stack = false;
                    for (Object o : in.getOpObjects(i))
                        if (o instanceof Register) {
                            String r = ((Register) o).getName().toUpperCase();
                            if (r.endsWith("SP") || r.endsWith("BP"))
                                stack = true;
                        }
                    if (stack)
                        continue;
                    mem++;
                    write = i == 0;
                }
            }
            if (mem == 1) {
                Pending pd = new Pending();
                pd.kind = "mmio"; pd.write = write; pd.size = 0; pd.via = fname;
                accessors.computeIfAbsent(f, x -> new ArrayList<>()).add(pd);
            }
        }
    }

    private static String funcs(Site s) {
        StringBuilder b = new StringBuilder();
        int n = 0;
        for (String f : s.funcs) {
            if (n++ == 4) { b.append(" +").append(s.funcs.size() - 4).append(" more"); break; }
            b.append(n > 1 ? ", " : "").append(f);
        }
        return b.toString();
    }

    private void report(PrintWriter out, int nfunc) {
        out.printf("=== x86 hardware access (%s, %s, %d functions) ===%n", currentProgram.getName(),
                   is64 ? "64-bit" : "32-bit", nfunc);
        out.printf("resolved through accessor call sites: %d; still unresolved: %d%n", viaAccessor, unresolvedSites);
        out.println();
        out.printf("--- I/O ports: %d%n", ports.size());
        out.println("port    R    W   sizes  values written            functions");
        for (Map.Entry<Long, Site> e : ports.entrySet()) {
            Site s = e.getValue();
            out.printf("0x%04x %4d %4d   %-5s  %-24s  %s%n", e.getKey(), s.reads, s.writes,
                       s.sizes.toString().replace(" ", ""), String.join(" ", s.values), funcs(s));
        }
        out.println();
        out.printf("--- PCI configuration addresses written to 0xCF8: %d%n", pci.size());
        for (Map.Entry<Long, Site> e : pci.entrySet()) {
            long v = e.getKey();
            out.printf("%02x:%02x.%x reg 0x%03x  (0x%08x)  %s%n", (v >> 16) & 0xFF, (v >> 11) & 0x1F,
                       (v >> 8) & 7, (v & 0xFC) | ((v >> 16) & 0xF00), v | 0x80000000L, funcs(e.getValue()));
        }
        out.println();
        out.printf("--- PCI configuration through the PCI library (PCI_LIB_ADDRESS arguments): %d%n", pciLib.size());
        for (Map.Entry<Long, Site> e : pciLib.entrySet()) {
            long v = e.getKey();
            Site s = e.getValue();
            out.printf("%02x:%02x.%x reg 0x%03x  R %3d  W %3d  %-5s %-20s  %s%n", (v >> 20) & 0xFF, (v >> 15) & 0x1F,
                       (v >> 12) & 7, v & 0xFFF, s.reads, s.writes, s.sizes.toString().replace(" ", ""),
                       String.join(" ", s.values), funcs(s));
        }
        out.println();
        out.printf("--- MSRs: %d%n", msrs.size());
        for (Map.Entry<Long, Site> e : msrs.entrySet()) {
            Site s = e.getValue();
            out.printf("0x%08x  rd %3d  wr %3d  %-30s  %s%n", e.getKey(), s.reads, s.writes,
                       String.join(" ", s.values), funcs(s));
        }
        out.println();
        out.printf("--- CPUID leaves: %d%n", cpuid.size());
        for (Map.Entry<Long, Site> e : cpuid.entrySet())
            out.printf("0x%08x  x%d  %s%n", e.getKey(), e.getValue().reads, funcs(e.getValue()));
        out.println();
        out.printf("--- MMIO addresses passed to accessor functions: %d%n", mmio.size());
        for (Map.Entry<Long, Site> e : mmio.entrySet()) {
            Site s = e.getValue();
            out.printf("0x%08x  R %3d  W %3d  %-24s  %s%n", e.getKey(), s.reads, s.writes,
                       String.join(" ", s.values), funcs(s));
        }
        if (!unresolved.isEmpty()) {
            out.println();
            out.println("--- unresolved (read these: the operand is computed, or comes from a table or a BAR)");
            int n = 0;
            for (String u : unresolved) {
                if (n++ == 40) { out.println("... " + (unresolved.size() - 40) + " more"); break; }
                out.println(u);
            }
        }
    }
}
