// Ghidra (headless) counterpart of ida_export.py: exports what the lifter needs from an analysed module, in the
// same <module>_ida.json format: function entries and their chunks, the data items (literal pools, jump tables)
// that sit inside .text, and the targets of indirect jumps Ghidra resolved (switch tables). Addresses are offsets
// from the image base (nso2elf's ELF is at 0; Ghidra may load it elsewhere).
//
// Usage (see ghidra_export.ps1):
//   analyzeHeadless <projdir> <proj> -import <module.elf> -preScript GhidraOptions.java
//       -postScript GhidraExport.java <out.json> [seeds.json] -scriptPath <this folder>
// seeds.json (nxmod.py seeds): code addresses to make functions at after analysis (virtual tables, callbacks).
//@category nxrecomp

import java.io.FileWriter;
import java.io.Writer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeMap;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;

public class GhidraExport extends GhidraScript {

    long base;

    long off(Address a) {
        return a.getOffset() - base;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: GhidraExport.java <out.json> [seeds.json]");
        }
        long t0 = System.currentTimeMillis();
        base = currentProgram.getImageBase().getOffset();
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        AddressSet textSet = new AddressSet(text.getStart(), text.getEnd());
        Listing listing = currentProgram.getListing();
        if (args.length > 1) {
            seed(args[1], textSet, listing);
        }

        StringBuilder js = new StringBuilder(1 << 24);
        js.append("{\"module\": \"").append(currentProgram.getName()).append("\", \"text\": [")
          .append(off(text.getStart())).append(", ").append(off(text.getEnd()) + 1).append("], \"functions\": [");
        TreeMap<Long, List<Long>> switches = new TreeMap<>();
        long codeBytes = 0;
        int nfunc = 0;
        FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(textSet, true);
        while (fi.hasNext()) {
            Function f = fi.next();
            Address entry = f.getEntryPoint();
            if (!textSet.contains(entry)) {
                continue;
            }
            long end = off(entry) + 4;
            StringBuilder chunks = new StringBuilder();
            for (AddressRange r : f.getBody().getAddressRanges()) {
                if (r.contains(entry)) {
                    end = off(r.getMaxAddress()) + 1;
                }
                if (chunks.length() > 0) {
                    chunks.append(", ");
                }
                chunks.append('[').append(off(r.getMinAddress())).append(", ").append(off(r.getMaxAddress()) + 1).append(']');
                codeBytes += r.getLength();
            }
            if (nfunc++ > 0) {
                js.append(", ");
            }
            js.append("{\"ea\": ").append(off(entry)).append(", \"end\": ").append(end)
              .append(", \"chunks\": [").append(chunks).append("], \"noret\": ").append(f.hasNoReturn())
              .append(", \"thunk\": ").append(f.isThunk()).append('}');
            InstructionIterator ii = listing.getInstructions(f.getBody(), true);
            while (ii.hasNext()) {
                Instruction ins = ii.next();
                FlowType ft = ins.getFlowType();
                if (!(ft.isJump() && ft.isComputed())) {
                    continue;
                }
                TreeSet<Long> targets = new TreeSet<>();
                for (Reference ref : ins.getReferencesFrom()) {
                    if (ref.getReferenceType().isFlow() && ref.getToAddress().isMemoryAddress()) {
                        targets.add(off(ref.getToAddress()));
                    }
                }
                if (!targets.isEmpty()) {
                    switches.put(off(ins.getAddress()), new ArrayList<>(targets));
                }
            }
        }
        // data items inside .text, merged into ranges
        js.append("], \"text_data\": [");
        long ds = -1, de = -1, dataBytes = 0;
        int ndata = 0;
        DataIterator di = listing.getDefinedData(textSet, true);
        while (di.hasNext()) {
            Data d = di.next();
            long s = off(d.getMinAddress()), e = s + Math.max(d.getLength(), 1);
            if (s == de) {
                de = e;
                continue;
            }
            if (ds >= 0) {
                js.append(ndata++ > 0 ? ", " : "").append('[').append(ds).append(", ").append(de).append(']');
                dataBytes += de - ds;
            }
            ds = s;
            de = e;
        }
        if (ds >= 0) {
            js.append(ndata++ > 0 ? ", " : "").append('[').append(ds).append(", ").append(de).append(']');
            dataBytes += de - ds;
        }
        js.append("], \"switches\": {");
        boolean first = true;
        for (var e : switches.entrySet()) {
            js.append(first ? "" : ", ").append("\"").append(Long.toHexString(e.getKey())).append("\": ")
              .append(e.getValue().toString());
            first = false;
        }
        js.append("}}");
        try (Writer w = new FileWriter(args[0])) {
            w.write(js.toString());
        }
        println(String.format("switch tables %d", switches.size()));
        println(String.format("functions %d  code bytes in functions %x of %x  data ranges in .text %d (%x bytes)  export %.0fs",
            nfunc, codeBytes, text.getSize(), ndata, dataBytes, (System.currentTimeMillis() - t0) / 1000.0));
    }

    // Create functions at seed addresses (offsets from the image base) that analysis did not reach, then let
    // analysis follow them.
    void seed(String path, AddressSet textSet, Listing listing) throws Exception {
        String s = Files.readString(Path.of(path)).replaceAll("[\\[\\]\\s]", "");
        int made = 0;
        for (String tok : s.split(",")) {
            if (tok.isEmpty()) {
                continue;
            }
            Address a = currentProgram.getImageBase().add(Long.parseLong(tok));
            if (a.getOffset() % 4 != 0 || !textSet.contains(a) || getFunctionContaining(a) != null
                    || listing.getDefinedDataContaining(a) != null) {
                continue;
            }
            if (listing.getInstructionAt(a) == null && !disassemble(a)) {
                continue;
            }
            if (createFunction(a, null) != null) {
                made++;
            }
        }
        analyzeChanges(currentProgram);
        println(String.format("seeds: %d functions created", made));
    }
}
