// FirmwareRecon.java - headless GhidraScript: dump the facts ember-dev-build needs.
//
// Reports: program/language info, image base, entry-point candidates, the memory
// block (segment) map, function/symbol counts, and version/RTOS banner strings.
//
// Usage (headless):
//   analyzeHeadless <proj_dir> <proj> -import <blob> -loader BinaryLoader \
//     -loader-baseAddr 0x<base> -processor <lang> \
//     -scriptPath <this dir> -postScript FirmwareRecon.java <out.txt | an existing directory>
//
// Authored in Java (not Jython) so it runs under plain headless without PyGhidra.
//@category Analysis.Firmware
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataType;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;
import java.io.PrintWriter;
import java.util.regex.Pattern;

public class FirmwareRecon extends GhidraScript {

    // Banner/version markers worth surfacing for RTOS identification.
    private static final Pattern BANNER = Pattern.compile(
        "(?i)(vxworks|wind river|linux version|ecos|u-boot|psos|threadx|nucleus|qnx|edk ii|uefi|american megatrends|insyde|phoenix|coreboot|seabios|bios|"
        + "freertos|bsp|@\\(#\\)|copyright|kernel|bootrom|redboot|esp-idf|esp_idf|esp8266|esp32|nonos|"
        + "zephyr|nuttx|chibios|contiki|riot|arduino|keil|sdcc|microchip|atmel|mplab|iar )");

    /* The report file: the argument, or <dir>/<program>.recon.txt when it names a directory
     * (one report per program of a multi-file import). */
    private String outPath(String arg, String suffix) {
        java.io.File f = new java.io.File(arg);
        return f.isDirectory() ? new java.io.File(f, currentProgram.getName() + "." + suffix + ".txt").getPath() : arg;
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = (args.length > 0)
            ? new PrintWriter(new java.io.FileWriter(outPath(args[0], "recon")))
            : new PrintWriter(System.out);
        try {
            Program p = currentProgram;
            Listing listing = p.getListing();

            out.println("=== program ===");
            out.println("name      : " + p.getName());
            out.println("language  : " + p.getLanguageID());
            out.println("compiler  : " + p.getCompilerSpec().getCompilerSpecID());
            out.println("image base: " + p.getImageBase() + "   (lowest mapped address: "
                + p.getMinAddress() + ")");
            out.println("addr size : " + p.getAddressFactory().getDefaultAddressSpace().getSize() + " bits");
            Address entry = p.getSymbolTable().getExternalEntryPointIterator().hasNext()
                ? p.getSymbolTable().getExternalEntryPointIterator().next() : null;
            out.println("entry (declared): " + (entry != null ? entry : "none; see candidates below"));

            out.println();
            out.println("=== memory blocks (segment map) ===");
            for (MemoryBlock b : p.getMemory().getBlocks()) {
                out.printf("%-16s %s - %s  %7d bytes  %s%s%s%s%n",
                    b.getName(), b.getStart(), b.getEnd(), b.getSize(),
                    b.isRead() ? "r" : "-", b.isWrite() ? "w" : "-",
                    b.isExecute() ? "x" : "-", b.isInitialized() ? " init" : " uninit");
            }

            out.println();
            out.println("=== counts ===");
            int funcs = 0;
            for (Function f : listing.getFunctions(true)) { funcs++; }
            out.println("functions : " + funcs);
            SymbolTable st = p.getSymbolTable();
            out.println("symbols   : " + st.getNumSymbols());

            out.println();
            out.println("=== entry-point candidates (first functions from the lowest mapped address) ===");
            Address base = p.getMinAddress();
            int shown = 0;
            FunctionIterator fit = listing.getFunctions(base, true);
            while (fit.hasNext() && shown < 10) {
                Function f = fit.next();
                out.printf("  %s  %s%n", f.getEntryPoint(), f.getName());
                shown++;
            }

            out.println();
            out.println("=== version / RTOS banner strings (first 60 matches) ===");
            int hits = 0;
            DataIterator dit = listing.getDefinedData(true);
            while (dit.hasNext() && hits < 60) {
                Data d = dit.next();
                if (d.getDataType() instanceof StringDataType
                        || "string".equalsIgnoreCase(d.getDataType().getName())) {
                    Object v = d.getValue();
                    if (v == null) continue;
                    String s = v.toString();
                    if (s.length() >= 6 && BANNER.matcher(s).find()) {
                        out.printf("  %s  %s%n", d.getAddress(),
                            s.length() > 100 ? s.substring(0, 100) + "..." : s);
                        hits++;
                    }
                }
            }
            if (hits == 0) {
                out.println("  (none found among defined strings; run auto-analysis so strings"
                    + " are defined, or inspect raw bytes for an @(#) banner)");
            }
        } finally {
            out.flush();
            if (args.length > 0) out.close();
        }
        println("FirmwareRecon: wrote report" + (getScriptArgs().length > 0
            ? " to " + getScriptArgs()[0] : ""));
    }
}
