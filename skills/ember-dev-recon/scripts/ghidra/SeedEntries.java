// SeedEntries.java - headless GhidraScript (run as -preScript, before auto-analysis):
// disassemble and make a function at each address given, so analysis starts from the
// reset and interrupt vectors of a raw image whose language names no entry point (the
// 8051's code at 0 and its interrupt vectors, for example).
//
// Usage: analyzeHeadless ... -import raw.bin -loader BinaryLoader ... \
//          -scriptPath <this dir> -preScript SeedEntries.java 0x0 0x3 0x13
// Addresses are in the default (code) space; ones no memory block holds are skipped.
//@category Analysis.Firmware
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class SeedEntries extends GhidraScript {
    @Override
    protected void run() throws Exception {
        for (String arg : getScriptArgs()) {
            Address a;
            try {
                a = currentProgram.getAddressFactory().getDefaultAddressSpace()
                        .getAddress(Long.decode(arg));
            } catch (Exception e) {
                printerr("SeedEntries: not an address: " + arg);
                continue;
            }
            if (!currentProgram.getMemory().contains(a))
                continue;
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            if (currentProgram.getListing().getInstructionAt(a) != null
                    && getFunctionAt(a) == null)
                new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
            currentProgram.getSymbolTable().addExternalEntryPoint(a);
        }
    }
}
