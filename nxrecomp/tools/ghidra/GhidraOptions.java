// Pre-script for GhidraExport.java: turns off analyzers the lifter gets nothing from (types, signatures,
// strings, demangling), which take most of the time on a module the size of Violet's main. Disassembly, function
// discovery, known non-returning functions and switch recovery stay on.
// "Non-Returning Functions - Discovered" is off: it takes PLT stubs (imports it cannot see into) as non-returning
// and spreads that to their callers (9,706 of Violet's sdk functions), cutting every caller's body short.
//@category nxrecomp

import java.util.Map;

import ghidra.app.script.GhidraScript;

public class GhidraOptions extends GhidraScript {

    static final String[] OFF = {
        "ASCII Strings", "Call Convention ID", "Decompiler Parameter ID",
        "Demangler GNU", "Embedded Media", "Function ID", "Stack",
        "Variadic Function Signature Override", "Apply Data Archives", "Create Address Tables",
        "Non-Returning Functions - Discovered",
    };

    @Override
    public void run() throws Exception {
        Map<String, String> opts = getCurrentAnalysisOptionsAndValues(currentProgram);
        for (String name : OFF) {
            if (opts.containsKey(name)) {
                setAnalysisOption(currentProgram, name, "false");
            } else {
                println("no analyzer option '" + name + "'");
            }
        }
    }
}
