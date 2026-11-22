"""Make GenCoreKgFpu.scala (karabas) from litex GenCoreDefault.scala: package vexriscv.kg, option --fpu true
adds FpuPlugin (RV32F, single only, internal FPU) as PocketQuake's GenPocketQuake.scala.
usage: python -I mk_genfpu.py <GenCoreDefault.scala> <out.scala>"""
import sys
src, out = sys.argv[1], sys.argv[2]
s = open(src, encoding='utf-8').read()

def rep(a, b, n=1):
    global s
    assert s.count(a) == n, (a, s.count(a))
    s = s.replace(a, b)

rep('package vexriscv\n', '// karabas: GenCoreDefault of litex pythondata-cpu-vexriscv 642ecfe + option --fpu (FpuPlugin, RV32F)\n'
    '// (made by mk_genfpu.py; see src/rtl/vexriscv/README.md)\npackage vexriscv.kg\n\nimport vexriscv._\n')
rep('import vexriscv.ip.{DataCacheConfig, InstructionCacheConfig}\n',
    'import vexriscv.ip.{DataCacheConfig, InstructionCacheConfig}\nimport vexriscv.ip.fpu.FpuParameter\n')
rep('object GenCoreDefault{', 'object GenCoreKgFpu{')
# the argument
i = s.index('case class ArgConfig(')
j = s.index('(', i) + 1
s = s[:j] + '\n  fpu : Boolean = false,' + s[j:]
rep('      opt[Boolean]("mulDiv")',
    '      opt[Boolean]("fpu")    action { (v, c) => c.copy(fpu = v)   } text("set RV32IMF (FpuPlugin, single)")\n'
    '      opt[Boolean]("mulDiv")')
rep('      // CPU configuration\n',
    '      if (argConfig.fpu) plugins += new FpuPlugin(externalFpu = false, p = FpuParameter(withDouble = false))\n\n'
    '      // CPU configuration\n')
open(out, 'w', encoding='utf-8', newline='\n').write(s)
print('ok', out)
