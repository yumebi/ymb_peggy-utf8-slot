import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import java.io.*;
public class Refs extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs(); PrintWriter o=new PrintWriter(new FileWriter(a[0]));
    for(int i=1;i<a.length;i++){ o.println("== refs to "+a[i]);
      for(Reference r:getReferencesTo(toAddr(a[i]))){ var f=getFunctionContaining(r.getFromAddress());
        o.println("  "+r.getFromAddress()+" "+r.getReferenceType()+" in "+(f==null?"-":f.getEntryPoint())+"  "+getInstructionAt(r.getFromAddress())); } }
    o.close(); } }
