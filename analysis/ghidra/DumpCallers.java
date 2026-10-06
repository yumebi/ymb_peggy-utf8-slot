import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import java.util.*;
import java.io.*;

public class DumpCallers extends GhidraScript {
  public void run() throws Exception {
    String[] apis = java.util.Arrays.copyOfRange(getScriptArgs(),1,getScriptArgs().length);
    PrintWriter out = new PrintWriter(new FileWriter(getScriptArgs()[0]));
    SymbolTable st = currentProgram.getSymbolTable();
    for (String a : apis) {
      Map<Function,Integer> m = new TreeMap<>((x,y)->x.getEntryPoint().compareTo(y.getEntryPoint()));
      for (Symbol s : st.getSymbols(a)) {
        for (Reference r : getReferencesTo(s.getAddress())) {
          Function f = getFunctionContaining(r.getFromAddress());
          if (f!=null) m.merge(f,1,Integer::sum);
        }
      }
      out.println("== "+a+" : "+m.size()+" functions");
      for (var e: m.entrySet()) out.println("  "+e.getKey().getEntryPoint()+" "+e.getKey().getName()+" size="+e.getKey().getBody().getNumAddresses()+" calls="+e.getValue()+" callers="+e.getKey().getCallingFunctions(monitor).size());
    }
    out.close();
  }
}
