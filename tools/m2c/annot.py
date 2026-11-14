#!/usr/bin/env python3 -I
# SPDX-License-Identifier: Apache-2.0
# annot.py <binary> <fixups.txt> <symbol>... : disassemble, annotating GOT binds, cfstring literals, C strings, selector stubs.
import sys, re, subprocess
binp, fixp, syms = sys.argv[1], sys.argv[2], sys.argv[3:]
data = open(binp, 'rb').read()
fx, binds = {}, {}
for l in open(fixp):
    m = re.search(r'(0x[0-9A-F]+)\s+rebase\s+(0x[0-9A-F]+)', l)
    if m: fx[int(m.group(1), 16)] = int(m.group(2), 16); continue
    m = re.search(r'(0x[0-9A-F]+)\s+bind\s+(\S+)', l)
    if m: binds[int(m.group(1), 16)] = m.group(2).split('/')[-1]
def cstr(a):
    e = data.index(b'\0', a); return data[a:e].decode('utf8', 'replace')
def cfstring(a):
    p = fx.get(a + 16)
    if p is None: return None
    try: return cstr(p)
    except Exception: return None
for s in syms:
    out = subprocess.run(['xcrun', 'llvm-objdump', '--disassemble-symbols=' + s, '--no-show-raw-insn', binp], capture_output=True, text=True).stdout
    regs = {}
    for line in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(\w+)\s+(.*)', line)
        note = ''
        if m:
            op, args = m.group(2), m.group(3)
            if op == 'adrp':
                mm = re.match(r'(x\d+), 0x([0-9a-f]+)', args)
                if mm: regs[mm.group(1)] = int(mm.group(2), 16)
            elif op in ('add', 'ldr'):
                mm = re.match(r'(x\d+), \[?(x\d+), #0x([0-9a-f]+)', args)
                if mm and mm.group(2) in regs:
                    a = regs[mm.group(2)] + int(mm.group(3), 16)
                    if a in binds and cfstring(a) is not None and 'CFConstantString' in binds[a]: note = '@"' + cfstring(a) + '"'
                    elif a in binds: note = 'GOT ' + binds[a]
                    else:
                        v = cfstring(a)
                        if v is not None: note = '@"' + v + '"'
                        elif op == 'add':
                            try:
                                c = cstr(a)
                                if c.isprintable() and 1 < len(c) < 100: note = 'c"' + c + '"'
                            except Exception: pass
                        elif a in fx:
                            v2 = cfstring(fx[a])
                            if v2: note = '@"' + v2 + '"'
        print(line + ('   ; ' + note if note else ''))
