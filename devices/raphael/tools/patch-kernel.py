#!/usr/bin/env python3
"""Stopgap: make a copy of the prebuilt kernel whose GICv3 code takes the virtual timer on INTID 19.

usage: devices/raphael/tools/patch-kernel.py [IN [OUT]]    (default prebuilt/mach.development.bcm2837 -> mach.raphael)

Qualcomm's SM8150 wires its timers as INTIDs 17/18/19, not the architecture's 26..30, so the kernel's
"virtual timer on PPI 27" never gets a tick there and the boot stalls after the network init. The source
now reads the number from the device tree (xnu/files/osfmk/arm64/gic_v3_irq.c, "vtimer-intid", written by
the loader's raphael platform); until the prebuilt kernel is rebuilt from it, this changes the five
instructions that carry the 27 in the prebuilt binary (each found by the instructions around it, and
refused if not exactly one place matches). `xl` picks up mach.$DEVICE when it exists.
"""
import struct
import sys

w = lambda x: struct.pack('<I', x)
src = sys.argv[1] if len(sys.argv) > 1 else 'prebuilt/mach.development.bcm2837'
dst = sys.argv[2] if len(sys.argv) > 2 else 'mach.raphael'
d = bytearray(open(src, 'rb').read())

# (the words around the one to change, index of that word, its new value, what it is)
SITES = [
    (w(0x52800068) + w(0x72a10008), 4, 0x72a00108, 'movk w8,#0x800,lsl16 -> #0x8   (1<<27 -> 1<<19, SMP)'),
    (w(0x14000002) + w(0x52a10008), 4, 0x52a00108, 'mov w8,#0x8000000 -> #0x80000   (1<<27 -> 1<<19)'),
    (w(0xd123952a) + w(0x5280140b), 0, 0xd123b52a, 'sub x10,x9,#0x8e5 -> #0x8ed     (IPRIORITYR27 -> 19)'),
    (w(0x7100069f) + w(0x54fffe80) + w(0x71006e9f), 8, 0x71004e9f, 'cmp w20,#0x1b -> #0x13      (the handler)'),
]
for context, at, new, what in SITES:
    if d.count(context) != 1:
        sys.exit(f'patch-kernel.py: not exactly one place for: {what} (this is not the kernel it was written for)')
    pos = d.find(context) + at
    d[pos:pos + 4] = w(new)
    print(f'{pos:#x}: {what}')
open(dst, 'wb').write(d)
print(f'wrote {dst}')
