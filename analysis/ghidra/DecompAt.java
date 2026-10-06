import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.Address;
import java.io.*;
public class DecompAt extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs(); PrintWriter out=new PrintWriter(new FileWriter(a[0]));
    DecompInterface d=new DecompInterface(); d.openProgram(currentProgram);
    Address ad=toAddr(a[1]);
    Function f=getFunctionContaining(ad);
    if(f==null){ disassemble(ad); f=createFunction(ad,null); }
    if(f==null){ out.println("no func"); out.close(); return; }
    out.println("// func "+f.getEntryPoint()+" size "+f.getBody().getNumAddresses());
    out.println(d.decompileFunction(f,120,monitor).getDecompiledFunction().getC());
    for (Function c : f.getCallingFunctions(monitor)) out.println("// caller "+c.getEntryPoint());
    out.close(); } }
