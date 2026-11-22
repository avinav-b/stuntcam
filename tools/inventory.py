#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""inventory.py — static inventory of what the engine depends on (its private dependencies).

Scans engine/src (the single translation unit and its fragments) and lists:
  frameworks   dlopen()ed framework paths (public / private by path)
  functions    C functions resolved at runtime with dlsym()
  data-syms    exported constants resolved through a dlsym helper (e.g. BarcodeSymbol("MRCSymbologyEAN13"))
  local-syms   local (non-exported) symbols looked up in a loaded image's symbol table
  vtable-slots Fig object vtable slots the engine fills or replaces (source class, session class)
  classes      Objective-C classes looked up by name
  hooks        methods whose implementation the engine replaces (class, selector, site); also the ones installed through
               ActivationHookResolve (lazy arming), with its label
  kvc-keys     keys read with key-value coding (SafeKVC(o, @"key"), valueForKey:@"key"); heuristic
  ivars        instance variables read by name
  selectors    other selectors the engine names (called, checked or trapped)
  strings      contract strings in fig/ and providers/ (heuristic: identifier-like @"…"/CFSTR literals)
  env          environment switches the engine reads

With --sdk <iPhoneSimulator.sdk> (on the host: `xcrun --sdk iphonesimulator --show-sdk-path`), classes and
selectors are marked public if they appear in the SDK's public framework headers, otherwise private.
Without it, a name heuristic is used (marked "guess").

Usage: tools/inventory.py [--src engine/src] [--sdk PATH] [--json FILE] [--md FILE]
This is the static half of the inventory. The runtime half (a fingerprint of what the preflight checks, and the list
of libraries Xcode inserts) needs the simulator and is not done here.
"""
import argparse
import json
import os
import re
import sys
from collections import OrderedDict, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# A statement longer than this many physical lines is cut there with a warning: it means a layout that
# statements() doesn't know.
MAX_STATEMENT_LINES = 40
# How far to look for the context of a match, in statements: back for the assignment of a class variable (within the
# function), back for the class_get*Method a method_setImplementation uses, ahead for the class_getInstanceMethod a
# selector table is applied with, and back for the SEL array an ActivationInstallTrap loop reads.
CLASS_LOOKBACK = 80
METHOD_LOOKBACK = 6
TABLE_CLASS_LOOKAHEAD = 30
SELECTOR_ARRAY_LOOKBACK = 8

# The categories in the order the Markdown summary table lists them.
SUMMARY_CATEGORIES = ['frameworks', 'functions', 'data-syms', 'local-syms', 'vtable-slots', 'classes', 'hooks',
                      'ivars', 'selectors', 'kvc-keys', 'strings', 'env']
# The categories whose entries are never public or private, so the summary shows no private count for them.
UNCLASSIFIED_CATEGORIES = ("strings", "env", "functions", "local-syms", "ivars", "vtable-slots")

JOIN_LITERALS = re.compile(r'(?<!\\)"\s+@?"')
R_LITERAL_OR_COMMENT = re.compile(r'''"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])'|(//.*)''')
R_CHAR_LITERAL = re.compile(r"'(?:\\.|[^'\\])'")
R_STRING_LITERAL = re.compile(r'"(?:\\.|[^"\\])*"')
R_BLOCK_COMMENT = re.compile(r'/\*.*?\*/')
R_INITIALIZER_START = re.compile(r'=\s*\{')

R_DLOPEN = re.compile(r'dlopen\("(/System/Library/[^"]+)"')
R_DLSYM = re.compile(r'dlsym\(\s*([A-Za-z_]+)\s*,\s*"([A-Za-z0-9_]+)"\s*\)')
# Helpers that resolve a name passed in: static T F(const char *n) { ... dlsym(..., n) ... } (e.g. BarcodeSymbol for
# exported constants).
R_DLSYM_HELPER = re.compile(r'static\s+[\w\s\*]+?\b(\w+)\s*\(\s*const\s+char\s*\*\s*(\w+)\s*\)\s*\{'
                            r'[^}]*dlsym\(\s*\w+\s*,\s*\2\s*\)')
R_LOCAL = re.compile(r'PreflightFindLocalSymbol\("([^"]+)",\s*"([^"]+)"\)')
R_CLASS = re.compile(r'(?:objc_getClass|NSClassFromString|objc_lookUpClass)\(@?"([A-Za-z0-9_]+)"\)')
R_IVAR = re.compile(r'class_getInstanceVariable\(([^,]+),\s*"([^"]+)"\)')
R_SELNAME = re.compile(r'sel_registerName\("([^"]+)"\)|@selector\(([^)]+)\)')
R_GETMETHOD = re.compile(r'class_get(Instance|Class)Method\(([^,]+),\s*'
                         r'(?:sel_registerName\("([^"]+)"\)|@selector\(([^)]+)\)|([A-Za-z_][A-Za-z0-9_]*))\)')
R_SETIMP = re.compile(r'method_setImplementation\(|class_replaceMethod\(')
R_TABLE = re.compile(r'\{\s*"([A-Za-z0-9_:]+)"\s*,\s*\(IMP\)')
R_TABLE_CLASS = re.compile(r'class_getInstanceMethod\(([A-Za-z_]+),')
R_TRAP = re.compile(r'ActivationInstallTrap\(([A-Za-z_]\w*),\s*(?:@selector\(([^)]+)\)|(\w+)\[i\])\s*,\s*(YES|NO)\)')
R_SELECTOR = re.compile(r'@selector\(([^)]+)\)')
R_CLASSVAR = re.compile(r'(?:Class\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*objc_getClass\("([A-Za-z0-9_]+)"\)')
R_ANY_CLASSVAR = re.compile(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*=\s*objc_getClass\("([^"]+)"\)')
R_IDENTIFIER = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
R_CLASS_OF = re.compile(r'object_getClass\((\w+)\)')
R_GETENV = re.compile(r'getenv\("([A-Z0-9_]+)"\)')
# The engine's named variables: #define ENV_<NAME> "<VARIABLE>" (core/env.inc), read as getenv(ENV_<NAME>).
R_ENVDEF = re.compile(r'^#define (ENV_\w+)\s+"([A-Z0-9_]+)"')
R_GETENV_NAMED = re.compile(r'(?:getenv|TestFault(?:On|Int|Long))\((ENV_\w+)\)')
# An off switch read through its predicate (core/switches.inc): static BOOL <Name>(void) { return getenv(ENV_<NAME>)
# ... }. The definition is not a site; each call <Name>() is one.
R_SWITCHDEF = re.compile(r'^static\s+BOOL\s+(\w+)\(void\)\s*\{\s*return\s+getenv\((ENV_\w+)\)')
# The six variables the CLI sets when arming are read under two names: EnvConfig(ENV_<NAME>, ENV_LEGACY_<NAME>).
R_ENVCONFIG = re.compile(r'EnvConfig\((ENV_\w+),\s*(ENV_\w+)\)')
R_SLOT = re.compile(r'gSessionLayer\.derived\[(\w+)\]\s*=\s*\(void \*\)(\w+)|gSessionHook\.base\.(\w+)\s*=\s*(\w+)'
                    r'|(?:^|[{,])\s*\.(\w+)\s*=\s*(src_\w+)|\(void \*\)(src_\w+)')
# The session vtable's slot names (an enum in core/preflight.inc), so a patched slot is listed by its index.
R_SLOTDEF = re.compile(r'\b(kSessSlot\w+)\s*=\s*(\d+)')
# ActivationHookResolve(group, "label", <class expr>, "selector", meta, add, ...) (core/act.inc).
R_HOOKRESOLVE = re.compile(r'ActivationHookResolve\(\s*\w+\s*,\s*"([^"]*)"\s*,\s*'
                           r'(objc_getClass\("[A-Za-z0-9_]+"\)|[A-Za-z_]\w*)\s*,\s*"([^"]+)"\s*,\s*(YES|NO)')
# In a statement with SafeKVC(: the key argument(s).
R_KVC_ARG = re.compile(r',\s*@"([A-Za-z_][A-Za-z0-9_]*)"\s*\)')
# A key path: SafeKVC(SafeKVC(o, @"a"), @"b").
R_KVC_NEST = re.compile(r'SafeKVC\(\s*SafeKVC\([^()]*,\s*@"(\w+)"\s*\)\s*,\s*@"(\w+)"\s*\)')
R_KVC_VFK = re.compile(r'valueForKey:\s*@"([A-Za-z_][A-Za-z0-9_]*)"')
R_STR = re.compile(r'@"([A-Za-z][A-Za-z0-9_.]{3,})"|CFSTR\("([A-Za-z][A-Za-z0-9_.]{3,})"\)')
# Contract strings are identifier-like CamelCase only.
R_CAMEL_CASE = re.compile(r'[a-z][A-Z]|^[A-Z][a-z]')
R_HOOK_KEY = re.compile(r'[-+]\[(\S+) (.+)\]')
# In the SDK's headers: an @property declaration (attributes, name).
R_PROPERTY = re.compile(r'@property\s*(\([^)]*\))?[^;]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:API_|NS_|__|;)')


def read_sources(src):
    """Returns (path relative to the repo, lines) for every source file under src, sorted by path."""
    files = []
    for directory, _, names in os.walk(src):
        for name in sorted(names):
            if name.endswith(('.m', '.inc', '.h', '.c')):
                path = os.path.join(directory, name)
                text = open(path, encoding='utf-8', errors='replace').read()
                files.append((os.path.relpath(path, ROOT), text.split('\n')))

    return sorted(files)


def without_trailing_comment(line):
    """The line up to its `//` comment, if it has one (a `//` inside a string or char literal is not one)."""
    for match in R_LITERAL_OR_COMMENT.finditer(line):
        if match.group(1):
            return line[:match.start(1)]

    return line


def code_only(line):
    """The line with char and string literal contents, `/* */` comments and its `//` comment removed."""
    code = R_CHAR_LITERAL.sub("''", line)
    code = R_STRING_LITERAL.sub('""', code)
    return R_BLOCK_COMMENT.sub('', code).split('//')[0].rstrip()


class StatementJoiner:
    """Collects the physical lines of one statement at a time; see statements()."""

    def __init__(self):
        self.statements = []
        self.first_line_numbers = []
        self.start = None
        self.reset()

    def reset(self):
        self.pending = []
        self.paren_depth = 0
        self.in_initializer = False

    def flush(self):
        """Ends the statement being collected, if there is one."""
        if len(self.pending) > 1:
            self.pending[0] = without_trailing_comment(self.pending[0]).rstrip()

        if self.pending:
            self.statements.append(JOIN_LITERALS.sub('', ' '.join(self.pending)))
            self.first_line_numbers.append(self.start)

        self.reset()

    def add(self, number, line):
        """Adds physical line number `number`, and ends the statement if this line completes it."""
        if self.pending and line.lstrip().startswith('//'):
            return

        code = code_only(line)
        if not self.pending:
            self.start = number
            self.pending.append(line)
        else:
            self.pending.append(without_trailing_comment(line).strip())

        self.paren_depth = max(0, self.paren_depth + code.count('(') - code.count(')'))
        if self.in_initializer:
            if '};' in code:
                self.in_initializer = False
        elif self.paren_depth == 0:
            match = R_INITIALIZER_START.search(code)
            self.in_initializer = bool(match) and code.count('{', match.start()) > code.count('}', match.start())

        if not self.in_initializer and (self.paren_depth == 0 or code.endswith('{')):
            self.flush()
        elif number - self.start + 1 >= MAX_STATEMENT_LINES:
            print(f'inventory: a statement starting at line {self.start} is still open after {MAX_STATEMENT_LINES} '
                  f'lines; cut there', file=sys.stderr)
            self.flush()


def statements(lines):
    """Joins the physical lines of one statement, so the patterns below see it whole however it is wrapped.

    A statement continues while its parentheses are open, and from a line that opens an initializer (`= {` whose
    brace that line doesn't close) to the line with its `};`. A line that ends by opening a block (`{`) ends the
    statement there, so a block passed as an argument is scanned line by line. Char and string literal contents and
    comments (`//`, and `/* */` on one line) don't count; comment lines inside a statement are left out, and so are
    the trailing comments of a statement that spans lines. Adjacent string literals ("a" "b", @"a" @"b") are joined
    into one. A statement longer than MAX_STATEMENT_LINES is cut there with a warning: that means a layout this rule
    doesn't know. Returns the joined lines and, for each, the number of its first physical line.
    """
    joiner = StatementJoiner()
    for number, line in enumerate(lines, 1):
        joiner.add(number, line)

    joiner.flush()
    return joiner.statements, joiner.first_line_numbers


def resolve_class(expr, lines, index):
    """Best-effort: turns a class expression in statement `index` into a class name."""
    expr = expr.strip()
    match = R_CLASS.search(expr)
    if match:
        return match.group(1)

    if R_IDENTIFIER.fullmatch(expr):
        for back in range(index, max(-1, index - CLASS_LOOKBACK), -1):
            for variable, class_name in R_CLASSVAR.findall(lines[back]):
                if variable == expr:
                    return class_name

            for variable, class_name in R_ANY_CLASSVAR.findall(lines[back]):
                if variable == expr:
                    return class_name

    match = R_CLASS_OF.search(expr)
    if match:
        return f'<class of {match.group(1)}>'

    return f'<{expr}>'


def name_alternatives(names):
    """A regex alternation of the names, sorted, each escaped."""
    return '|'.join(map(re.escape, sorted(names)))


def dlsym_helpers(files):
    """Returns the names of the helpers that resolve a name passed in with dlsym.

    Helper bodies are looked for in whole files. Helpers that hand their name on to one count too
    (BarcodeVisionSymbol -> BarcodeSymbol).
    """
    helpers = {m.group(1) for _, lines in files for m in R_DLSYM_HELPER.finditer('\n'.join(lines))}
    while helpers:
        r_via = re.compile(r'static\s+[\w\s\*]+?\b(\w+)\s*\(\s*const\s+char\s*\*\s*(\w+)\s*\)\s*\{[^}]*\b(?:' +
                           name_alternatives(helpers) + r')\(\s*\2\s*\)')
        more = {m.group(1) for _, lines in files for m in r_via.finditer('\n'.join(lines))} - helpers
        if not more:
            break

        helpers |= more

    return helpers


def hook_key(is_class_method, class_name, selector):
    """The name of a hooked method: -[Class selector] or +[Class selector]."""
    return f'{"+" if is_class_method else "-"}[{class_name} {selector}]'


class Inventory(defaultdict):
    """Category -> name -> entry. An entry has its sites (path:line) and whatever else add() was given."""

    def __init__(self):
        super().__init__(OrderedDict)

    def add(self, category, key, site, **fields):
        entry = self[category].setdefault(key, dict(sites=[], **fields))
        entry['sites'].append(site)


class Scanner:
    """Scans the source files statement by statement and fills an Inventory."""

    def __init__(self, files):
        self.inventory = Inventory()

        # Helper bodies and the ENV_ defines are looked for in whole files; everything else statement by statement.
        helpers = dlsym_helpers(files)
        self.envdefs = {c: v for _, lines in files for line in lines for c, v in R_ENVDEF.findall(line)}
        self.switches = {f: c for _, lines in files for line in lines for f, c in R_SWITCHDEF.findall(line)}
        self.slot_index = {n: v for _, lines in files for line in lines for n, v in R_SLOTDEF.findall(line)}
        self.r_switch_call = None
        if self.switches:
            self.r_switch_call = re.compile(r'\b(' + name_alternatives(self.switches) + r')\(\)')

        self.r_helper_call = None
        if helpers:
            self.r_helper_call = re.compile(r'\b(' + name_alternatives(helpers) + r')\("([A-Za-z0-9_]+)"\)')

    def add(self, category, key, site, **fields):
        self.inventory.add(category, key, site, **fields)

    def scan_file(self, path, physical_lines):
        subdir = path.split('/')[2] if path.count('/') >= 3 else ''
        lines, numbers = statements(physical_lines)
        for index, line in enumerate(lines):
            site = f'{path}:{numbers[index]}'
            if line.lstrip().startswith('//'):
                continue

            self.scan_symbols(line, site, lines, index)
            self.scan_vtable_slots(line, site)
            self.scan_env(line, site)
            if subdir in ('fig', 'providers'):
                self.scan_strings(line, site)

            self.scan_hooks(line, site, lines, index)
            self.scan_kvc_keys(line, site)
            for registered, selector in R_SELNAME.findall(line):
                self.add('selectors', registered or selector, site)

    def scan_symbols(self, line, site, lines, index):
        """Frameworks, data symbols, functions, local symbols, classes and ivars."""
        for path in R_DLOPEN.findall(line):
            self.add('frameworks', path, site, private='/PrivateFrameworks/' in path)

        if self.r_helper_call:
            for helper, symbol in self.r_helper_call.findall(line):
                self.add('data-syms', symbol, site, helper=helper)

        for handle, symbol in R_DLSYM.findall(line):
            self.add('functions', symbol, site, handle=handle)

        for image, symbol in R_LOCAL.findall(line):
            self.add('local-syms', symbol, site, image=image)

        for class_name in R_CLASS.findall(line):
            self.add('classes', class_name, site)

        for class_expr, ivar in R_IVAR.findall(line):
            self.add('ivars', f'{resolve_class(class_expr, lines, index)}.{ivar}', site)

    def scan_vtable_slots(self, line, site):
        for match in R_SLOT.finditer(line):
            index, function, base_slot, base_function, source_slot, source_function, source_function2 = match.groups()
            if index:
                self.add('vtable-slots', f'session[{self.slot_index.get(index, index)}] {function}', site)
            elif base_slot:
                self.add('vtable-slots', f'session.base.{base_slot} {base_function}', site)
            elif source_slot:
                self.add('vtable-slots', f'source.base.{source_slot} {source_function}', site)
            elif source_function2:
                self.add('vtable-slots', f'source {source_function2}', site)

    def scan_env(self, line, site):
        """Environment variables: read by literal name, by ENV_ name, through EnvConfig or an off-switch predicate."""
        named = R_GETENV_NAMED.findall(line) + [name for pair in R_ENVCONFIG.findall(line) for name in pair]
        if R_SWITCHDEF.match(line.strip()):
            named = []
        elif self.r_switch_call:
            named += [self.switches[f] for f in self.r_switch_call.findall(line)]

        for variable in R_GETENV.findall(line) + [self.envdefs[name] for name in named if name in self.envdefs]:
            self.add('env', variable, site)

    def scan_strings(self, line, site):
        for objc_string, cf_string in R_STR.findall(line):
            text = objc_string or cf_string
            if not R_CAMEL_CASE.search(text):
                continue

            self.add('strings', text, site)

    def scan_hooks(self, line, site, lines, index):
        """Methods whose implementation the engine replaces, in four forms."""

        # A method_setImplementation in this statement: find the Method's class and selector here or just above.
        if R_SETIMP.search(line):
            for back in range(index, max(-1, index - METHOD_LOOKBACK), -1):
                get_method = R_GETMETHOD.search(lines[back])
                if not get_method:
                    continue

                kind, class_expr, registered, selector, selector_variable = get_method.groups()
                selector = registered or selector

                # A selector held in a variable or table (the motion table, the lazy-arming traps) is not reported here.
                if not selector and selector_variable:
                    break

                self.add('hooks', hook_key(kind == "Class", resolve_class(class_expr, lines, back), selector), site)
                break

        # Selector tables: {"sel", (IMP)fn}, applied to the class of the class_getInstanceMethod that follows.
        for selector in R_TABLE.findall(line):
            class_name = '?'
            for ahead in range(index, min(len(lines), index + TABLE_CLASS_LOOKAHEAD)):
                match = R_TABLE_CLASS.search(lines[ahead])
                if match:
                    class_name = resolve_class(match.group(1), lines, ahead)
                    break

            self.add('hooks', f'-[{class_name} {selector}]', site)

        # Lazy-arming traps: ActivationInstallTrap(class, @selector(x), YES) or
        # ActivationInstallTrap(class, sels[i], YES).
        for class_variable, selector, array, meta in R_TRAP.findall(line):
            class_name = resolve_class(class_variable, lines, index)
            selectors = [selector] if selector else []
            if not selector:
                # The SEL sels[] = {...} array above.
                for back in range(index, max(-1, index - SELECTOR_ARRAY_LOOKBACK), -1):
                    match = re.search(r'SEL ' + re.escape(array) + r'\[\] = \{(.*)\};', lines[back])
                    if match:
                        selectors = R_SELECTOR.findall(match.group(1))
                        break

            for trapped in selectors:
                self.add('hooks', hook_key(meta == "YES", class_name, trapped), site, trap=True)

        for label, class_expr, selector, meta in R_HOOKRESOLVE.findall(line):
            # The SIMCAM_TEST_ACTIVATE_FAIL=missing fault's made-up selector.
            if selector.startswith('simcamTest'):
                continue

            self.add('hooks', hook_key(meta == "YES", resolve_class(class_expr, lines, index), selector), site,
                     what=label)

    def scan_kvc_keys(self, line, site):
        if 'SafeKVC(' in line and 'static id SafeKVC' not in line:
            nested = R_KVC_NEST.findall(line)
            inner_keys = {inner for _, inner in nested}
            for outer, inner in nested:
                self.add('kvc-keys', f'{outer}.{inner}', site)

            for key in R_KVC_ARG.findall(line):
                if key not in inner_keys:
                    self.add('kvc-keys', key, site)

        for key in R_KVC_VFK.findall(line):
            self.add('kvc-keys', key, site)


def scan(files):
    """Returns the inventory of the source files."""
    scanner = Scanner(files)
    for path, lines in files:
        scanner.scan_file(path, lines)

    inventory = scanner.inventory

    # Selectors that are hooked are reported as hooks only.
    hooked = {key.split(' ', 1)[1].rstrip(']') for key in inventory['hooks']}
    for selector in list(inventory['selectors']):
        if selector in hooked:
            del inventory['selectors'][selector]

    return inventory


def sdk_index(sdk):
    """Public class names and selector keywords from the SDK's public framework headers."""
    classes, words = set(), set()
    frameworks = os.path.join(sdk, 'System/Library/Frameworks')
    for directory, _, names in os.walk(frameworks):
        if '/Headers' not in directory:
            continue

        for name in names:
            if not name.endswith('.h'):
                continue

            text = open(os.path.join(directory, name), encoding='utf-8', errors='replace').read()
            classes.update(re.findall(r'@interface\s+([A-Za-z0-9_]+)', text))
            words.update(re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*:', text))
            words.update(re.findall(r'\)\s*([A-Za-z_][A-Za-z0-9_]*)\s*[;:\s]', text))
            for attributes, property_name in R_PROPERTY.findall(text):
                words.add(property_name)
                words.update(re.findall(r'\b(?:getter|setter)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)', attributes))
                if 'readonly' not in attributes:
                    words.add('set' + property_name[:1].upper() + property_name[1:])

    return classes, words


def classify(inventory, sdk):
    """Marks classes, hooks, selectors, KVC keys and data symbols public or not, from the SDK's headers or by name."""
    if sdk:
        classes, words = sdk_index(sdk)

        def public_class(name):
            return name in classes

        def public_selector(selector):
            return not selector.startswith('_') and all(k in words for k in selector.split(':') if k)

        how = 'sdk'
    else:
        def public_class(name):
            return not (name.startswith('Fig') or name.endswith('Internal') or name.startswith('AVCaptureFig'))

        def public_selector(selector):
            return not (selector.startswith('_') or 'Fig' in selector)

        how = 'guess'

    for name, entry in inventory['classes'].items():
        entry['public'] = public_class(name)
        entry['by'] = how

    for key, entry in inventory['hooks'].items():
        class_name, selector = R_HOOK_KEY.match(key).groups()
        entry['public'] = public_class(class_name) and public_selector(selector)
        entry['by'] = how

    for selector, entry in inventory['selectors'].items():
        entry['public'] = public_selector(selector)
        entry['by'] = how

    # A key is public if a public header declares a property (or method) of that name.
    for path, entry in inventory['kvc-keys'].items():
        entry['public'] = all((key in words) if sdk else not key.startswith('_') for key in path.split('.'))
        entry['by'] = how

    # Exported constants are public if a public header names them.
    for symbol, entry in inventory['data-syms'].items():
        entry['public'] = (symbol in words) if sdk else not symbol.startswith('MRC')
        entry['by'] = how


def is_private(entry):
    return entry.get('private') or entry.get('public') is False


def markdown(inventory):
    """Returns the inventory as Markdown: a summary table, then one section per category."""
    out = ['# Engine dependency inventory (static)', '',
           'Generated by `tools/inventory.py`; counts are of distinct names. "public" means found in the SDK\'s public',
           'headers (or, without `--sdk`, a name guess). Hooks marked "trap" are lazy-arming traps (forward to the '
           'original).', '']
    summary = []
    for category in SUMMARY_CATEGORIES:
        entries = inventory.get(category, {})
        private_count = sum(1 for entry in entries.values() if is_private(entry))
        private_text = private_count if category not in UNCLASSIFIED_CATEGORIES else "—"
        summary.append(f'| {category} | {len(entries)} | {private_text} |')

    out += ['| category | count | private |', '|---|---|---|'] + summary + ['']
    for category, entries in inventory.items():
        if category == 'strings':
            out += [f'## strings ({len(entries)}, heuristic)', '', ', '.join(f'`{k}`' for k in entries), '']
            continue

        if category == 'env':
            out += [f'## env ({len(entries)})', '', ', '.join(f'`{k}`' for k in entries), '']
            continue

        with_label = category == 'hooks'
        heuristic = ", heuristic" if category == "kvc-keys" else ""
        out += [f'## {category} ({len(entries)}{heuristic})', '',
                '| name | public | ' + ('label | ' if with_label else '') + 'first site | sites |',
                '|---|---|---|' + ('---|' if with_label else '') + '---|']
        for key, entry in entries.items():
            if 'public' not in entry and 'private' not in entry:
                public = ''
            else:
                public = 'no' if is_private(entry) else 'yes'

            if entry.get('trap'):
                public += ' (trap)'

            label = f'{entry.get("what", "")} | ' if with_label else ''
            out.append(f'| `{key}` | {public} | ' + label + f'{entry["sites"][0]} | {len(entry["sites"])} |')

        out.append('')

    return '\n'.join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--src', default=os.path.join(ROOT, 'engine/src'))
    parser.add_argument('--sdk', help='iPhoneSimulator SDK path, to classify public/private from headers')
    parser.add_argument('--json')
    parser.add_argument('--md')
    args = parser.parse_args()
    inventory = scan(read_sources(args.src))
    classify(inventory, args.sdk)
    text = markdown(inventory)
    if args.json:
        json.dump(inventory, open(args.json, 'w'), indent=1)

    if args.md:
        open(args.md, 'w').write(text + '\n')

    if not args.json and not args.md:
        print(text)
    else:
        for category, entries in inventory.items():
            print(f'{category}: {len(entries)}', file=sys.stderr)


if __name__ == '__main__':
    main()
