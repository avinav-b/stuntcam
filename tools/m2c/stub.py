#!/usr/bin/env python3 -I
# SPDX-License-Identifier: Apache-2.0
# stub.py <binary> <fixups> <hexaddr>...: resolves each symbol stub address to the imported symbol it calls.
#
# An arm64 stub loads the address to jump to from a pointer slot (adrp x16, then ldr or add with an offset); the
# fixups listing's "bind" line for that slot names the symbol. A slot with no bind is printed as "?<slot address>".
import re
import subprocess
import sys

# An arm64 stub is three 4-byte instructions: adrp, ldr and br.
STUB_SIZE = 12

BIND_LINE = re.compile(r'(0x[0-9A-F]+)\s+bind\s+(\S+)')
ADRP_X16 = re.compile(r'adrp\s+x16, 0x([0-9a-f]+)')
LDR_X16 = re.compile(r'ldr\s+x16, \[x16, #0x([0-9a-f]+)\]')
ADD_X16 = re.compile(r'add\s+x16, x16, #0x([0-9a-f]+)')
USAGE = 'usage: stub.py <binary> <fixups> <hexaddr>...'


def usage():
    """Prints the usage line on stderr and exits 2."""
    print(USAGE, file=sys.stderr)
    sys.exit(2)


def fail(message):
    """Prints "stub.py: <message>" on stderr and exits 1."""
    print(f'stub.py: {message}', file=sys.stderr)
    sys.exit(1)


def read_binds(path):
    """Returns pointer slot -> imported symbol, from the fixups listing's bind lines."""
    binds = {}
    for line in open(path):
        match = BIND_LINE.search(line)
        if match:
            binds[int(match.group(1), 16)] = match.group(2)

    return binds


def stub_disassembly(binary_path, address):
    """Returns llvm-objdump's disassembly of the stub at the address. Its errors go to stderr."""
    command = ['xcrun', 'llvm-objdump', '-d', '--no-show-raw-insn', '--start-address=%#x' % address,
               '--stop-address=%#x' % (address + STUB_SIZE), binary_path]
    return subprocess.run(command, stdout=subprocess.PIPE, text=True).stdout


def stub_target(binds, listing):
    """Returns the symbol bound to the slot the stub loads, '?<slot>' if the slot has no bind, or '?'."""
    page = None
    target = '?'
    for line in listing.splitlines():
        match = ADRP_X16.search(line)
        if match:
            page = int(match.group(1), 16)

        for offset_pattern in (LDR_X16, ADD_X16):
            match = offset_pattern.search(line)
            if match and page is not None:
                slot = page + int(match.group(1), 16)
                target = binds.get(slot, '?%#x' % slot)

    return target


def main():
    if len(sys.argv) < 3:
        usage()

    binary_path, fixups_path = sys.argv[1], sys.argv[2]
    try:
        binds = read_binds(fixups_path)
    except OSError as error:
        fail(f'{fixups_path}: {error.strerror}')

    for address_text in sys.argv[3:]:
        try:
            address = int(address_text, 16)
        except ValueError:
            fail(f'{address_text!r} is not a hex address')

        print(hex(address), stub_target(binds, stub_disassembly(binary_path, address)))


if __name__ == '__main__':
    main()
