#!/usr/bin/env python3 -I
# SPDX-License-Identifier: Apache-2.0
# stub.py <binary> <fixups> <hexaddr>... : resolve stub address to imported symbol
import sys, re, subprocess
binp, fixp = sys.argv[1], sys.argv[2]
binds = {}
for l in open(fixp):
    m = re.search(r'(0x[0-9A-F]+)\s+bind\s+(\S+)', l)
    if m: binds[int(m.group(1), 16)] = m.group(2)
for a in sys.argv[3:]:
    a = int(a, 16)
    out = subprocess.run(['xcrun', 'llvm-objdump', '-d', '--no-show-raw-insn', '--start-address=%#x' % a, '--stop-address=%#x' % (a + 12), binp], capture_output=True, text=True).stdout
    pg = None; res = '?'
    for line in out.splitlines():
        m = re.search(r'adrp\s+x16, 0x([0-9a-f]+)', line)
        if m: pg = int(m.group(1), 16)
        m = re.search(r'ldr\s+x16, \[x16, #0x([0-9a-f]+)\]', line)
        if m and pg is not None: res = binds.get(pg + int(m.group(1), 16), '?%#x' % (pg + int(m.group(1), 16)))
        m = re.search(r'add\s+x16, x16, #0x([0-9a-f]+)', line)
        if m and pg is not None: res = binds.get(pg + int(m.group(1), 16), '?%#x' % (pg + int(m.group(1), 16)))
    print(hex(a), res)
