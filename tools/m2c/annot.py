#!/usr/bin/env python3 -I
# SPDX-License-Identifier: Apache-2.0
# annot.py <binary> <fixups.txt> <symbol>...: disassembles each symbol and annotates the addresses its instructions
# build with what they point at: GOT binds, constant CFString literals, C strings and selector stubs.
#
# fixups.txt is the binary's chained-fixups listing, with "rebase" lines (a pointer slot and its target address) and
# "bind" lines (a pointer slot and the imported symbol it is bound to).
import re
import subprocess
import sys

# In a constant CFString (isa, flags, character pointer, length) the pointer to the characters is at this offset.
CFSTRING_CHARS_OFFSET = 16
# A C string is shown only if it is printable and longer than one and shorter than this many characters.
MAX_C_STRING_LENGTH = 100

REBASE_LINE = re.compile(r'(0x[0-9A-F]+)\s+rebase\s+(0x[0-9A-F]+)')
BIND_LINE = re.compile(r'(0x[0-9A-F]+)\s+bind\s+(\S+)')
INSTRUCTION_LINE = re.compile(r'\s*([0-9a-f]+):\s+(\w+)\s+(.*)')
ADRP_ARGS = re.compile(r'(x\d+), 0x([0-9a-f]+)')
ADD_OR_LDR_ARGS = re.compile(r'(x\d+), \[?(x\d+), #0x([0-9a-f]+)')
USAGE = 'usage: annot.py <binary> <fixups.txt> <symbol>...'


def usage():
    """Prints the usage line on stderr and exits 2."""
    print(USAGE, file=sys.stderr)
    sys.exit(2)


def fail(message):
    """Prints "annot.py: <message>" on stderr and exits 1."""
    print(f'annot.py: {message}', file=sys.stderr)
    sys.exit(1)


def read_fixups(path):
    """Returns (rebases, binds): pointer slot -> target address, and pointer slot -> imported symbol name."""
    rebases, binds = {}, {}
    for line in open(path):
        match = REBASE_LINE.search(line)
        if match:
            rebases[int(match.group(1), 16)] = int(match.group(2), 16)
            continue

        match = BIND_LINE.search(line)
        if match:
            binds[int(match.group(1), 16)] = match.group(2).split('/')[-1]

    return rebases, binds


class Annotator:
    """Names what an address in the binary points at."""

    def __init__(self, data, rebases, binds):
        self.data = data
        self.rebases = rebases
        self.binds = binds

    def c_string(self, address):
        """Returns the NUL-terminated string at the address. Raises ValueError if no NUL follows it."""
        end = self.data.index(b'\0', address)
        return self.data[address:end].decode('utf8', 'replace')

    def cfstring(self, address):
        """Returns the text of the constant CFString at the address, or None if it doesn't look like one."""
        chars = self.rebases.get(address + CFSTRING_CHARS_OFFSET)
        if chars is None:
            return None

        try:
            return self.c_string(chars)
        except ValueError:  # no NUL after it: not a C string
            return None

    def note(self, op, address):
        """Returns the annotation for an `add` or `ldr` instruction that builds the address, or ''."""
        if address in self.binds and self.cfstring(address) is not None and 'CFConstantString' in self.binds[address]:
            return '@"' + self.cfstring(address) + '"'

        if address in self.binds:
            return 'GOT ' + self.binds[address]

        text = self.cfstring(address)
        if text is not None:
            return '@"' + text + '"'

        if op == 'add':
            try:
                text = self.c_string(address)
                if text.isprintable() and 1 < len(text) < MAX_C_STRING_LENGTH:
                    return 'c"' + text + '"'
            except ValueError:  # no NUL after it: not a C string, so no note
                pass

            return ''

        if address in self.rebases:
            # A pointer slot that holds the address of a constant CFString.
            text = self.cfstring(self.rebases[address])
            if text:
                return '@"' + text + '"'

        return ''


def disassembly(binary_path, symbol):
    """Returns llvm-objdump's disassembly of one symbol. Its errors (an unknown symbol, say) go to stderr."""
    command = ['xcrun', 'llvm-objdump', '--disassemble-symbols=' + symbol, '--no-show-raw-insn', binary_path]
    return subprocess.run(command, stdout=subprocess.PIPE, text=True).stdout


def annotate(annotator, listing):
    """Prints the listing, each instruction followed by its annotation if it has one.

    It follows the page address each `adrp` puts in a register, so that a later `add` or `ldr` with an offset from
    that register gives the full address.
    """
    page_in_register = {}
    for line in listing.splitlines():
        match = INSTRUCTION_LINE.match(line)
        note = ''
        if match:
            op, args = match.group(2), match.group(3)
            if op == 'adrp':
                adrp = ADRP_ARGS.match(args)
                if adrp:
                    page_in_register[adrp.group(1)] = int(adrp.group(2), 16)
            elif op in ('add', 'ldr'):
                offset = ADD_OR_LDR_ARGS.match(args)
                if offset and offset.group(2) in page_in_register:
                    address = page_in_register[offset.group(2)] + int(offset.group(3), 16)
                    note = annotator.note(op, address)

        print(line + ('   ; ' + note if note else ''))


def main():
    if len(sys.argv) < 3:
        usage()

    binary_path, fixups_path, symbols = sys.argv[1], sys.argv[2], sys.argv[3:]
    try:
        data = open(binary_path, 'rb').read()
        rebases, binds = read_fixups(fixups_path)
    except OSError as error:
        fail(f'{error.filename}: {error.strerror}')

    annotator = Annotator(data, rebases, binds)
    for symbol in symbols:
        annotate(annotator, disassembly(binary_path, symbol))


if __name__ == '__main__':
    main()
