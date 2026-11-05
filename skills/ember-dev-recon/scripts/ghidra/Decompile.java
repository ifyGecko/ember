// Decompile.java - headless GhidraScript: the Ghidra MCP queries the skills rely on, for when
// the MCP server is unavailable (down, unreachable, or holding another program). It reads an
// existing, analysed project; run it through ../ghidra-query.sh, which finds Ghidra and the
// project, opens it read-only and prints the result.
//
// Modes (first argument after the output file):
//   info                      program, language, image base, memory blocks, function count
//                             (the MCP get_program_info + list_segments check: right program?)
//   decompile T...            C of the function containing each target
//   disasm T...               the function's instructions (address, bytes, mnemonic);
//                             read this for delay and bit-bang loops, which the decompiler drops
//   xrefs T...                references to each target: from address, type, calling function
//   functions [SUBSTRING]     every function (entry, size, name), optionally filtered
//   strings [SUBSTRING]       defined strings with address and reference count
// A target T is an address (0x80012340, 80012340, segment:offset in real mode, or
// SPACE:ADDR in a program with several address spaces: code:0x34, mem:0x100 on AVR8, where
// Ghidra's code addresses count 16-bit words) or a function name. An address no function contains is disassembled and given a temporary
// function, so code the analysis missed still decompiles (read-only: nothing is saved).
//
// A project holding several programs (UEFI: one per module) runs this once per program;
// only programs where a target resolves write a section, so an address finds its module.
//
// Usage (headless; ghidra-query.sh does this):
//   analyzeHeadless <proj_dir> <proj> -process [<program>] -readOnly -noanalysis \
//     -scriptPath <this dir> -postScript Decompile.java <out.txt> <mode> [targets...]
//
// Authored in Java (not Jython) so it runs under plain headless without PyGhidra.
//@category Analysis.Firmware

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.util.DefinedStringIterator;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class Decompile extends GhidraScript {

    private static final int DECOMPILE_TIMEOUT_S = 120;
    private static final int MAX_LIST = 5000;

    private PrintWriter out;
    private boolean headerDone;
    private DecompInterface decomp;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: Decompile.java <out.txt> info|decompile|disasm|xrefs|functions|strings [targets...]");
            return;
        }
        String mode = args[1];
        List<String> targets = new ArrayList<>();
        for (int i = 2; i < args.length; i++)
            targets.add(args[i]);
        out = new PrintWriter(new FileWriter(args[0], true));   // append: one section per program
        try {
            switch (mode) {
                case "info":      info(); break;
                case "functions": functions(targets.isEmpty() ? "" : targets.get(0)); break;
                case "strings":   strings(targets.isEmpty() ? "" : targets.get(0)); break;
                case "decompile":
                case "disasm":
                case "xrefs":
                    if (targets.isEmpty()) {
                        printerr("Decompile.java: " + mode + " needs at least one address or function name");
                        return;
                    }
                    for (String t : targets)
                        target(mode, t);
                    break;
                default:
                    printerr("Decompile.java: unknown mode " + mode);
            }
        } finally {
            if (decomp != null)
                decomp.dispose();
            out.close();
        }
    }

    private void header() {
        if (headerDone)
            return;
        headerDone = true;
        out.printf("=== program %s (%s, image base %s)%n", currentProgram.getName(),
                   currentProgram.getLanguageID(), currentProgram.getImageBase());
    }

    private void info() {
        header();
        out.printf("executable: %s%n", currentProgram.getExecutablePath());
        out.printf("compiler:   %s%n", currentProgram.getCompilerSpec().getCompilerSpecID());
        out.printf("functions:  %d%n", currentProgram.getFunctionManager().getFunctionCount());
        out.println("memory blocks:");
        for (MemoryBlock b : currentProgram.getMemory().getBlocks())
            out.printf("  %-24s %s-%s %8x %s%s%s%s%n", b.getName(), b.getStart(), b.getEnd(), b.getSize(),
                       b.isRead() ? "r" : "-", b.isWrite() ? "w" : "-", b.isExecute() ? "x" : "-",
                       b.isInitialized() ? "" : " (uninitialized)");
        out.println();
    }

    private void functions(String filter) {
        String f = filter.toLowerCase();
        int n = 0;
        for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!f.isEmpty() && !fn.getName().toLowerCase().contains(f))
                continue;
            if (n == 0) {
                header();
                out.println("entry            size  name");
            }
            if (++n > MAX_LIST) {
                out.printf("... more than %d functions; narrow the filter%n", MAX_LIST);
                break;
            }
            out.printf("%-14s %6d  %s%n", fn.getEntryPoint(), fn.getBody().getNumAddresses(), fn.getName());
        }
        if (n > 0)
            out.println();
    }

    private void strings(String filter) {
        String f = filter.toLowerCase();
        int n = 0;
        for (Data d : DefinedStringIterator.forProgram(currentProgram)) {
            Object v = d.getValue();
            String s = v == null ? "" : v.toString();
            if (!f.isEmpty() && !s.toLowerCase().contains(f))
                continue;
            if (n == 0) {
                header();
                out.println("address        refs  string");
            }
            if (++n > MAX_LIST) {
                out.printf("... more than %d strings; narrow the filter%n", MAX_LIST);
                break;
            }
            int refs = currentProgram.getReferenceManager().getReferenceCountTo(d.getAddress());
            out.printf("%-14s %4d  %s%n", d.getAddress(), refs, s.replace("\n", "\\n").replace("\r", "\\r"));
        }
        if (n > 0)
            out.println();
    }

    /* An address in this program, or null (not an address, or outside every block). */
    private Address targetAddress(String t) {
        String space = "";
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("([A-Za-z_][A-Za-z0-9_]*):(.*)").matcher(t);
        if (m.matches() && currentProgram.getAddressFactory().getAddressSpace(m.group(1)) != null) {
            space = m.group(1) + ":";          // SPACE:ADDR, e.g. code:0x34 or mem:0x100 (AVR)
            t = m.group(2);
        }
        String s = t.startsWith("0x") || t.startsWith("0X") ? t.substring(2) : t;
        if (!s.matches("[0-9a-fA-F]+(:[0-9a-fA-F]+)?"))
            return null;
        s = space + s;
        Address a = null;
        try {
            a = currentProgram.getAddressFactory().getAddress(s);
        } catch (Exception e) {
            return null;
        }
        return a != null && currentProgram.getMemory().contains(a) ? a : null;
    }

    private Function byName(String name) {
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        for (Function fn : it)
            if (fn.getName().equals(name))
                return fn;
        return null;
    }

    /* The function containing a, creating a temporary one when the analysis made none. */
    private Function functionAt(Address a) {
        Function fn = getFunctionContaining(a);
        if (fn != null)
            return fn;
        if (currentProgram.getListing().getInstructionAt(a) == null) {
            DisassembleCommand dc = new DisassembleCommand(a, null, true);
            if (!dc.applyTo(currentProgram, monitor) || currentProgram.getListing().getInstructionAt(a) == null)
                return null;
        }
        CreateFunctionCmd cf = new CreateFunctionCmd(a);
        if (!cf.applyTo(currentProgram, monitor))
            return null;
        fn = getFunctionAt(a);
        if (fn != null)
            out.printf("(no function contained %s: made a temporary one there, not saved)%n", a);
        return fn;
    }

    private void target(String mode, String t) throws Exception {
        Address a = targetAddress(t);
        Function fn = a == null ? byName(t) : null;
        if (a == null && fn == null)
            return;            // not in this program: another program of the project may have it
        header();
        if (mode.equals("xrefs")) {
            xrefs(a != null ? a : fn.getEntryPoint(), t);
            return;
        }
        if (fn == null)
            fn = functionAt(a);
        if (fn == null) {
            out.printf("--- %s: no code at %s (data, or not disassemblable)%n%n", t, a);
            return;
        }
        out.printf("--- %s: %s @ %s (%d bytes)%n", t, fn.getName(), fn.getEntryPoint(),
                   fn.getBody().getNumAddresses());
        if (mode.equals("disasm"))
            disasm(fn);
        else
            decompile(fn);
        out.println();
    }

    private void decompile(Function fn) {
        if (decomp == null) {
            decomp = new DecompInterface();
            decomp.setOptions(new DecompileOptions());
            decomp.toggleCCode(true);
            decomp.toggleSyntaxTree(false);
            if (!decomp.openProgram(currentProgram)) {
                out.println("decompiler failed to open the program: " + decomp.getLastMessage());
                return;
            }
        }
        DecompileResults r = decomp.decompileFunction(fn, DECOMPILE_TIMEOUT_S, monitor);
        if (r == null || !r.decompileCompleted() || r.getDecompiledFunction() == null) {
            out.println("decompile failed: " + (r == null ? "no result" : r.getErrorMessage()));
            return;
        }
        out.print(r.getDecompiledFunction().getC());
    }

    private void disasm(Function fn) throws Exception {
        InstructionIterator it = currentProgram.getListing().getInstructions(fn.getBody(), true);
        while (it.hasNext()) {
            Instruction in = it.next();
            StringBuilder hex = new StringBuilder();
            for (byte b : in.getBytes())
                hex.append(String.format("%02x", b & 0xff));
            String label = in.getAddress().equals(fn.getEntryPoint()) ? "" :
                           getSymbolAt(in.getAddress()) != null ? getSymbolAt(in.getAddress()).getName() + ":" : "";
            if (!label.isEmpty())
                out.println(label);
            out.printf("  %-14s %-20s %s%n", in.getAddress(), hex, in);
        }
    }

    private void xrefs(Address a, String t) {
        out.printf("--- %s: references to %s%n", t, a);
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
        int n = 0;
        while (it.hasNext()) {
            Reference r = it.next();
            Function from = getFunctionContaining(r.getFromAddress());
            out.printf("  %-14s %-16s %s%n", r.getFromAddress(), r.getReferenceType(),
                       from == null ? "(no function)" : from.getName() + " @ " + from.getEntryPoint());
            if (++n >= MAX_LIST) {
                out.printf("  ... more than %d references%n", MAX_LIST);
                break;
            }
        }
        if (n == 0)
            out.println("  (none)");
        out.println();
    }
}
