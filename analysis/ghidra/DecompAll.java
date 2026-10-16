import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class DecompAll extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    PrintWriter out = new PrintWriter(new FileWriter(a[0]));
    DecompInterface d = new DecompInterface();
    d.openProgram(currentProgram);
    FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
    while (it.hasNext()) {
      Function f = it.next();
      DecompileResults r = d.decompileFunction(f, 60, monitor);
      out.println("// ===== " + f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses() + " =====");
      out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// FAILED");
    }
    out.close();
  }
}
