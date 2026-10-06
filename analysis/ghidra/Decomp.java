import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class Decomp extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    PrintWriter out = new PrintWriter(new FileWriter(a[0]));
    DecompInterface d = new DecompInterface();
    d.openProgram(currentProgram);
    for (int i=1;i<a.length;i++) {
      Function f = getFunctionAt(toAddr(a[i])); if (f==null) { disassemble(toAddr(a[i])); f = createFunction(toAddr(a[i]), null); }
      if (f==null) { out.println("// no func "+a[i]); continue; }
      DecompileResults r = d.decompileFunction(f, 120, monitor);
      out.println("// ===== "+a[i]+" =====");
      out.println(r.decompileCompleted()? r.getDecompiledFunction().getC() : "// FAILED");
    }
    out.close();
  }
}
