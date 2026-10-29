# SPDX-License-Identifier: GPL-2.0-or-later
"""Apply the small local board integration to an extracted QEMU 10.2.4 tree."""
from pathlib import Path
import sys, shutil, re
root = Path(sys.argv[1]).resolve()
project = Path(__file__).resolve().parent
assert (root / 'VERSION').read_text().strip() == '10.2.4'
source, target = project / 'nikon-z7-lab.c', root / 'hw/arm/nikon-z7-lab.c'
if not target.exists() or source.read_bytes() != target.read_bytes():
    shutil.copyfile(source, target)
for name in ['nikon-z7-shoulder.h', 'nikon-z7-gpu.h', 'nikon-z7-isp.h', 'nikon-z7-isp-pixels.h']:
    source, target = project / name, root / 'hw/arm' / name
    if not target.exists() or source.read_bytes() != target.read_bytes():
        shutil.copyfile(source, target)
changes = {
    'hw/arm/meson.build': "arm_ss.add(when: 'CONFIG_NIKON_Z7_LAB', if_true: files('nikon-z7-lab.c'))\n",
    'hw/arm/Kconfig': '\nconfig NIKON_Z7_LAB\n    bool\n    depends on TCG && ARM\n    select GENERIC_LOADER\n    select A15MPCORE\n',
    'configs/devices/arm-softmmu/default.mak': '\nCONFIG_NIKON_Z7_LAB=y\n',
}
for name, addition in changes.items():
    p = root / name
    original = p.read_text()
    data = original
    if name == 'hw/arm/Kconfig':
        data = re.sub(r'\nconfig NIKON_Z7_LAB\n(?:[ \t].*\n)*', '', data)
    if addition.strip() not in data:
        data += addition
    if data != original:
        p.write_text(data)
print('Prepared', root)

# QEMU 10.2.4 dereferences current_cpu on banked GIC reads made by GDB when
# both vCPUs are stopped. Like qtest accesses, such CPU-less diagnostic reads
# use bank 0. Normal guest accesses retain the original core selection.
gic = root / 'hw/intc/arm_gic.c'
original = gic.read_text()
needle = 'if (!qtest_enabled() && s->num_cpu > 1) {'
replacement = 'if (!qtest_enabled() && current_cpu && s->num_cpu > 1) {'
if needle not in original and replacement not in original:
    raise RuntimeError('Unexpected GIC source; refuse to apply the debug-read guard')
if needle in original:
    gic.write_text(original.replace(needle, replacement, 1))

# Nikon RTOS locks frequently write CPSR's control byte without changing mode.
# QEMU10.2.4 already rebuilds M/E/IL-dependent hflags inside cpsr_write, then
# its TCG helper does the same work a second time. Rebuild only on an actual
# M/E/IL change (after invalid-mode handling), retaining all CPSR write logic.
# Thumb/IT and NZCV are read directly by get_tb_cpu_state/generated code;
# IRQ mask updates still go through cpsr_write and the usual TB exit.
helper = root/'target/arm/helper.c'
original = helper.read_text()
needle = '''    bool rebuild_hflags = (write_type != CPSRWriteRaw) &&
        (mask & (CPSR_M | CPSR_E | CPSR_IL));'''
replacement = '''    uint32_t old_hflags_cpsr = env->uncached_cpsr;'''
if needle not in original and replacement not in original:
    raise RuntimeError('Unexpected CPSR hflags source')
original = original.replace(needle, replacement, 1)
needle = '    if (tcg_enabled() && rebuild_hflags) {'
replacement = '''    if (tcg_enabled() && write_type != CPSRWriteRaw &&
        ((old_hflags_cpsr ^ env->uncached_cpsr) & (CPSR_M | CPSR_E | CPSR_IL))) {'''
if needle not in original and replacement not in original:
    raise RuntimeError('Unexpected CPSR hflags rebuild')
original = original.replace(needle, replacement, 1)
if helper.read_text() != original:
    helper.write_text(original)

op = root/'target/arm/tcg/op_helper.c'
original = op.read_text()
needle = '''    cpsr_write(env, val, mask, CPSRWriteByInstr);
    /* TODO: Not all cpsr bits are relevant to hflags.  */
    arm_rebuild_hflags(env);'''
replacement = '''    cpsr_write(env, val, mask, CPSRWriteByInstr);
    /* Nikon lab: cpsr_write already maintains the M/E/IL-dependent cache.
     * Opt-in shadow recomputation validates this against the original full
     * recompute on every real guest MSR, including mode changes. */
    static gsize verify_mode;
    if (g_once_init_enter(&verify_mode)) {
        g_once_init_leave(&verify_mode, getenv("Z7_VERIFY_CPSR_HFLAGS") ? 2 : 1);
    }
    if (unlikely(verify_mode == 2)) {
        static unsigned checks;
        CPUARMTBFlags cached = env->hflags;
        arm_rebuild_hflags(env);
        g_assert(cached.flags == env->hflags.flags);
        g_assert(cached.flags2 == env->hflags.flags2);
        ++checks;
        if (!(checks & (checks - 1))) {
            fprintf(stderr, "Z7_CPSR_HFLAGS_CHECK checks=%u match=1\\n", checks);
        }
    }'''
original = original.replace('        if (!(++checks & (checks - 1))) {',
                            '        ++checks;\n        if (!(checks & (checks - 1))) {')
if needle not in original and replacement not in original:
    raise RuntimeError('Unexpected TCG CPSR helper')
patched = original.replace(needle, replacement, 1)
if op.read_text() != patched:
    op.write_text(patched)
