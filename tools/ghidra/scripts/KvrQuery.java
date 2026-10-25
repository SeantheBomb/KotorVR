// Headless query helper for the KotorVR project database.
// args: <out.txt> <cmd> <args...>
//   decomp <addr...>       decompile the functions containing each address
//   xrefs <addr...>        references to each address (with the calling function)
//   strings <regex>        defined strings matching regex, with their references
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.StringDataInstance;
import java.io.*;
import java.util.regex.*;

public class KvrQuery extends GhidraScript {
    PrintWriter out;

    Address addr(String s) { return currentProgram.getAddressFactory().getAddress(s.startsWith("0x") ? s.substring(2) : s); }

    String fn(Address a) {
        Function f = getFunctionContaining(a);
        return f == null ? "?" : f.getName() + "@" + f.getEntryPoint();
    }

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        out = new PrintWriter(new FileWriter(a[0]));
        String cmd = a[1];
        if (cmd.equals("decomp")) {
            DecompInterface d = new DecompInterface();
            d.openProgram(currentProgram);
            for (int i = 2; i < a.length; i++) {
                Address ad = addr(a[i]);
                Function f = getFunctionContaining(ad);
                if (f == null) {
                    disassemble(ad);
                    createFunction(ad, null);
                    f = getFunctionAt(ad);
                }
                if (f == null) { out.println("// no function at " + a[i]); continue; }
                DecompileResults r = d.decompileFunction(f, 120, monitor);
                out.println("// ===== " + f.getName() + " @ " + f.getEntryPoint());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        } else if (cmd.equals("xrefs")) {
            for (int i = 2; i < a.length; i++) {
                Address ad = addr(a[i]);
                out.println("// refs to " + ad);
                for (Reference r : getReferencesTo(ad))
                    out.println(r.getFromAddress() + "  " + r.getReferenceType() + "  in " + fn(r.getFromAddress()));
            }
        } else if (cmd.equals("strings")) {
            Pattern p = Pattern.compile(a[2].replace(',', '|'), Pattern.CASE_INSENSITIVE);  // commas = alternatives (cmd eats |)
            for (Data dt : currentProgram.getListing().getDefinedData(true)) {
                if (!dt.hasStringValue()) continue;
                String v = StringDataInstance.getStringDataInstance(dt).getStringValue();
                if (v == null || !p.matcher(v).find()) continue;
                out.println(dt.getAddress() + "  \"" + v + "\"");
                for (Reference r : getReferencesTo(dt.getAddress()))
                    out.println("    <- " + r.getFromAddress() + " in " + fn(r.getFromAddress()));
            }
        }
        out.close();
    }
}
