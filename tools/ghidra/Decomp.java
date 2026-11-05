// Decompile functions for decomp.ps1: args <out file> <offset>... (hex from the image base; <off>! creates a function at
// that address if there is none, x<off> lists only the callers of the function containing it)
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;
public class Decomp extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(a[0]));
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        long base = currentProgram.getImageBase().getOffset();
        for (int i = 1; i < a.length; i++) {
            boolean xonly = a[i].startsWith("x");
            Address ad = sp.getAddress(base + Long.parseLong(a[i].replaceFirst("^x", "").replace("!", ""), 16));
            Function f = getFunctionContaining(ad);
            if (f == null || f.getEntryPoint().equals(ad) == false && a[i].endsWith("!")) {
                ad = sp.getAddress(base + Long.parseLong(a[i].replaceFirst("^x", "").replace("!", ""), 16));
                f = getFunctionAt(ad);
                if (f == null) { disassemble(ad); f = createFunction(ad, null); }
            }
            if (f == null) { out.println("== " + a[i] + ": no function"); continue; }
            out.println("== " + a[i] + " in " + f.getName() + " @" + Long.toHexString(f.getEntryPoint().getOffset() - base));
            StringBuilder sb = new StringBuilder("callers:");
            for (Reference r : getReferencesTo(f.getEntryPoint()))
                sb.append(" ").append(Long.toHexString(r.getFromAddress().getOffset() - base)).append(r.getReferenceType().isCall() ? "" : "(" + r.getReferenceType() + ")");
            out.println(sb);
            if (xonly) continue;
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "decompile failed: " + r.getErrorMessage());
        }
        out.close();
    }
}
