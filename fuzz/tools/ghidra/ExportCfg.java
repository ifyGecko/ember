// ExportCfg.java - write the control-flow graph of the program open in Ghidra as an ember
// CFG file (JSON; the format is in fuzz/tools/frontier_cfg.py), for
// fuzz/tools/fuzz_frontier.py --cfg FILE.
//
// A plain Java GhidraScript: it needs nothing beyond Ghidra itself (no Jython, no
// PyGhidra), and runs from the Script Manager on the open program as well as headless.
// It reads the program as it is loaded: whatever base, processor, memory map and
// functions the import and the analyst gave it are what the file describes.
//
//   Script Manager: run ExportCfg.java; it asks for the output file.
//   Headless, on a project that already holds the analysed program (saves nothing):
//     analyzeHeadless <proj_dir> <proj_name> -process <program> -readOnly -noanalysis \
//         -scriptPath fuzz/tools/ghidra -postScript ExportCfg.java fw.cfg.json
//   Headless, importing a raw image at its base:
//     analyzeHeadless /tmp/p p -import fw.bin -loader BinaryLoader \
//         -loader-baseAddr 0x8000000 -processor ARM:LE:32:Cortex \
//         -scriptPath fuzz/tools/ghidra -postScript ExportCfg.java fw.cfg.json --seed 0x8000101
//
// Arguments: OUT [--range LO-HI]... [--seed ADDR]...
//   OUT            the file to write; if OUT is a directory, OUT/<program name>.json
//   --range LO-HI  only the blocks of functions that overlap [LO, HI) (default: all).
//                  The function list is always complete.
//   --seed ADDR    make ADDR a function first if it is not code yet, then let analysis
//                  follow it (a raw image has no entry point of its own). On ARM an odd
//                  ADDR is a Thumb function. Not for a read-only run.
//@category ember
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.block.BasicBlockModel;
import ghidra.program.model.block.CodeBlock;
import ghidra.program.model.block.CodeBlockIterator;
import ghidra.program.model.block.CodeBlockReference;
import ghidra.program.model.block.CodeBlockReferenceIterator;
import ghidra.program.model.lang.Register;
import ghidra.program.model.lang.RegisterValue;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Symbol;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.Writer;
import java.math.BigInteger;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class ExportCfg extends GhidraScript {
    private static final Pattern CASE = Pattern.compile("caseD_([0-9a-fA-F]+)");

    private static String q(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"' || c == '\\')
                b.append('\\').append(c);
            else if (c < 0x20 || c > 0x7e)
                b.append(String.format("\\u%04x", (int) c));
            else
                b.append(c);
        }
        return b.append('"').toString();
    }

    private static String num(Address a) {
        return a == null ? "null" : Long.toUnsignedString(a.getOffset());
    }

    private void fail(String msg) throws Exception {
        printerr("ExportCfg: " + msg);
        throw new IllegalArgumentException("ExportCfg: " + msg);
    }

    // The value of the switch case that lands on `dest`: from Ghidra's caseD_<hex> label.
    private String caseValue(Address dest, int index) {
        boolean dflt = false;
        for (Symbol s : currentProgram.getSymbolTable().getSymbols(dest)) {
            Matcher m = CASE.matcher(s.getName());
            if (m.find())
                return new BigInteger(m.group(1), 16).toString();
            if (s.getName().startsWith("default"))
                dflt = true;
        }
        return dflt ? "\"default\"" : q("#" + index);
    }

    private boolean seed(long value) {
        boolean thumb = false;
        Register tmode = currentProgram.getProgramContext().getRegister("TMode");
        if (tmode != null && (value & 1) != 0) {
            thumb = true;
            value &= ~1L;
        }
        Address a = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(value);
        if (!currentProgram.getMemory().contains(a))
            return false;
        if (getFunctionAt(a) != null)
            return false;
        if (currentProgram.getListing().getInstructionAt(a) == null) {
            try {
                if (tmode != null)
                    currentProgram.getProgramContext().setRegisterValue(a, a,
                        new RegisterValue(tmode, BigInteger.valueOf(thumb ? 1 : 0)));
            } catch (Exception e) {
                // the context is already set there: disassemble with what it has
            }
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
        }
        if (currentProgram.getListing().getInstructionAt(a) == null)
            return false;
        return new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        String out = null;
        List<long[]> ranges = new ArrayList<>();
        List<Long> seeds = new ArrayList<>();
        for (int i = 0; i < args.length; i++) {
            if (args[i].equals("--range") && i + 1 < args.length) {
                String[] p = args[++i].split("-");
                if (p.length != 2)
                    fail("--range wants LO-HI, got " + args[i]);
                ranges.add(new long[] { Long.decode(p[0]), Long.decode(p[1]) });
            } else if (args[i].equals("--seed") && i + 1 < args.length) {
                seeds.add(Long.decode(args[++i]));
            } else if (out == null && !args[i].startsWith("--")) {
                out = args[i];
            } else {
                fail("unknown argument " + args[i]);
            }
        }
        if (out == null) {
            File f = askFile("Write the CFG file", "Save");
            out = f.getAbsolutePath();
        }
        File outFile = new File(out);
        if (outFile.isDirectory())
            outFile = new File(outFile, currentProgram.getName() + ".json");

        boolean seeded = false;
        for (long s : seeds)
            seeded |= seed(s);
        if (seeded)
            analyzeChanges(currentProgram);

        BasicBlockModel model = new BasicBlockModel(currentProgram);
        Set<Address> seen = new HashSet<>();
        int nfn = 0, nblk = 0;
        try (Writer w = new BufferedWriter(new FileWriter(outFile))) {
            w.write("{\"format\": \"ember-cfg\", \"version\": 1, \"source\": "
                + q("Ghidra: " + currentProgram.getName() + " ("
                    + currentProgram.getLanguageID() + ", image base "
                    + currentProgram.getImageBase() + ")") + ",\n\"functions\": [");
            List<Function> want = new ArrayList<>();
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.isExternal())
                    continue;
                long lo = f.getEntryPoint().getOffset();
                long size = f.getBody().getNumAddresses();
                w.write((nfn++ == 0 ? "\n" : ",\n") + "{\"name\": " + q(f.getName())
                    + ", \"addr\": " + num(f.getEntryPoint()) + ", \"size\": " + size + "}");
                boolean in = ranges.isEmpty();
                long flo = f.getBody().getMinAddress().getOffset();
                long fhi = f.getBody().getMaxAddress().getOffset();
                for (long[] r : ranges)
                    in |= Long.compareUnsigned(flo, r[1]) < 0 && Long.compareUnsigned(fhi, r[0]) >= 0;
                if (in)
                    want.add(f);
            }
            w.write("],\n\"blocks\": [");
            for (Function f : want) {
                monitor.checkCancelled();
                CodeBlockIterator it = model.getCodeBlocksContaining(f.getBody(), monitor);
                while (it.hasNext()) {
                    CodeBlock b = it.next();
                    Address start = b.getMinAddress();
                    if (!seen.add(start))
                        continue;
                    Address cond = null, uncond = null, fall = null;
                    List<Address> computed = new ArrayList<>();
                    CodeBlockReferenceIterator d = b.getDestinations(monitor);
                    while (d.hasNext()) {
                        CodeBlockReference r = d.next();
                        FlowType ft = r.getFlowType();
                        Address to = r.getDestinationAddress();
                        if (ft.isCall() || to == null || !to.isMemoryAddress())
                            continue;
                        if (ft.isFallthrough())
                            fall = to;
                        else if (ft.isComputed())
                            computed.add(to);
                        else if (ft.isConditional())
                            cond = cond == null ? to : cond;
                        else if (ft.isJump())
                            uncond = uncond == null ? to : uncond;
                    }
                    Address jump, failTo = null;
                    if (!computed.isEmpty()) {
                        jump = null;
                    } else if (cond != null) {
                        jump = cond;
                        failTo = fall != null ? fall : uncond;
                    } else {
                        jump = uncond != null ? uncond : fall;
                    }
                    StringBuilder s = new StringBuilder();
                    s.append(nblk++ == 0 ? "\n" : ",\n");
                    s.append("{\"addr\": ").append(num(start));
                    s.append(", \"size\": ").append(b.getMaxAddress().subtract(start) + 1);
                    s.append(", \"func\": ").append(q(f.getName()));
                    s.append(", \"jump\": ").append(num(jump));
                    s.append(", \"fail\": ").append(num(failTo));
                    s.append(", \"cases\": [");
                    for (int i = 0; i < computed.size(); i++)
                        s.append(i == 0 ? "" : ", ").append("[")
                            .append(caseValue(computed.get(i), i)).append(", ")
                            .append(num(computed.get(i))).append("]");
                    s.append("], \"tail\": [");
                    List<Instruction> last = new ArrayList<>();
                    InstructionIterator ins = currentProgram.getListing()
                        .getInstructions(new AddressSet(b), true);
                    while (ins.hasNext()) {
                        last.add(ins.next());
                        if (last.size() > 3)
                            last.remove(0);
                    }
                    for (int i = 0; i < last.size(); i++)
                        s.append(i == 0 ? "" : ", ").append("[")
                            .append(num(last.get(i).getAddress())).append(", ")
                            .append(q(last.get(i).toString())).append("]");
                    s.append("]}");
                    w.write(s.toString());
                }
            }
            w.write("]}\n");
        }
        println("ExportCfg: " + nfn + " functions, " + nblk + " blocks -> " + outFile);
    }
}
