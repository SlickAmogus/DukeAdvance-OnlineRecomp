// Headless: set up GBA memory map for Duke Nukem Advance, seed functions from
// the recompiler's dispatch table, decompile everything to one C file.
import java.io.*;
import java.math.BigInteger;
import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.app.cmd.disassemble.ArmDisassembleCommand;

public class DukeDecompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String seedPath = args[0], outPath = args[1];
        Memory mem = currentProgram.getMemory();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        // IWRAM image copied by crt0: ROM 0x087FA21C -> 0x03003150, 0x411C bytes.
        byte[] img = new byte[0x411C];
        mem.getBytes(sp.getAddress(0x087FA21CL), img);
        mem.createUninitializedBlock("iwram_lo", sp.getAddress(0x03000000L), 0x3150, false);
        MemoryBlock b = mem.createInitializedBlock("iwram_img", sp.getAddress(0x03003150L),
                new ByteArrayInputStream(img), 0x411C, monitor, false);
        b.setExecute(true); b.setWrite(true);
        mem.createUninitializedBlock("iwram_hi", sp.getAddress(0x0300726CL), 0x8000 - 0x726C, false);
        mem.createUninitializedBlock("ewram", sp.getAddress(0x02000000L), 0x40000, false);
        mem.createUninitializedBlock("io", sp.getAddress(0x04000000L), 0x400, false);
        mem.createUninitializedBlock("pal", sp.getAddress(0x05000000L), 0x400, false);
        mem.createUninitializedBlock("vram", sp.getAddress(0x06000000L), 0x18000, false);
        mem.createUninitializedBlock("oam", sp.getAddress(0x07000000L), 0x400, false);

        List<long[]> seeds = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(seedPath))) {
            String line;
            while ((line = r.readLine()) != null) {
                String[] p = line.trim().split(" ");
                if (p.length == 2) seeds.add(new long[]{Long.parseLong(p[0], 16), Long.parseLong(p[1])});
            }
        }
        for (long[] s : seeds) {
            Address a = sp.getAddress(s[0]);
            new ArmDisassembleCommand(a, null, s[1] == 1).applyTo(currentProgram, monitor);
        }
        int made = 0;
        for (long[] s : seeds) {
            Address a = sp.getAddress(s[0]);
            if (getFunctionAt(a) == null && getInstructionAt(a) != null) {
                if (createFunction(a, null) != null) made++;
            }
        }
        println("seeds=" + seeds.size() + " functions created=" + made);
        analyzeAll(currentProgram);

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        int n = 0, fail = 0;
        try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(outPath)))) {
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                DecompileResults res = dec.decompileFunction(f, 60, monitor);
                w.println("// ===== " + f.getEntryPoint() + " " + f.getName() + " size=" + f.getBody().getNumAddresses());
                if (res != null && res.decompileCompleted()) { w.println(res.getDecompiledFunction().getC()); n++; }
                else { w.println("// decompile failed"); fail++; }
            }
        }
        println("decompiled=" + n + " failed=" + fail);
    }
}
