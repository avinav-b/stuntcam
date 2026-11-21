#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""inventory.py — static inventory of what the engine depends on (design §3 "private dependencies", §10.7).

Scans engine/src (the single translation unit and its fragments) and lists:
  frameworks   dlopen()ed framework paths (public / private by path)
  functions    C functions resolved at runtime with dlsym()
  data-syms    exported constants resolved through a dlsym helper (e.g. BarcodeSymbol("MRCSymbologyEAN13"))
  local-syms   local (non-exported) symbols looked up in a loaded image's symbol table
  vtable-slots Fig object vtable slots the engine fills or replaces (source class, session class)
  classes      Objective-C classes looked up by name
  hooks        methods whose implementation the engine replaces (class, selector, site); also the ones installed through
               ActHookResolve (lazy arming), with its label
  kvc-keys     keys read with key-value coding (SafeKVC(o, @"key"), valueForKey:@"key"); heuristic
  ivars        instance variables read by name
  selectors    other selectors the engine names (called, checked or trapped)
  strings      contract strings in fig/ and providers/ (heuristic: identifier-like @"…"/CFSTR literals)
  env          environment switches the engine reads

With --sdk <iPhoneSimulator.sdk> (on the Mac: `xcrun --sdk iphonesimulator --show-sdk-path`), classes and
selectors are marked public if they appear in the SDK's public framework headers, otherwise private.
Without it, a name heuristic is used (marked "guess").

Usage: tools/inventory.py [--src engine/src] [--sdk PATH] [--json FILE] [--md FILE]
The runtime half of §10.7 (fingerprint of what the preflight checks, Xcode's insert list) needs the simulator and is
not done here.
"""
import argparse, json, os, re, sys
from collections import OrderedDict, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_sources(src):
    files = []
    for d, _, fs in os.walk(src):
        for f in sorted(fs):
            if f.endswith(('.m', '.inc', '.h', '.c')):
                p = os.path.join(d, f)
                files.append((os.path.relpath(p, ROOT), open(p, encoding='utf-8', errors='replace').read().split('\n')))
    return sorted(files)


def statements(lines):
    """Joins the physical lines of one statement, so the patterns below see it whole however it is wrapped.

    A statement continues while its parentheses are open, and from a line that opens an initializer (`= {` whose
    brace that line doesn't close) to the line with its `};`. A line that ends by opening a block (`{`) ends the
    statement there, so a block passed as an argument is scanned line by line. Char and string literal contents and
    comments (`//`, and `/* */` on one line) don't count; comment lines inside a statement are left out, and so are the
    trailing comments of a statement that spans lines. Adjacent string literals ("a" "b", @"a" @"b") are joined into one. A
    statement longer than MAX_STATEMENT_LINES is cut there with a warning: that means a layout this rule doesn't know.
    Returns the joined lines and, for each, the number of its first physical line.
    """
    out, nums, cur, depth, init = [], [], [], 0, False
    def flush():
        nonlocal cur, depth, init
        if len(cur) > 1: cur[0] = without_trailing_comment(cur[0]).rstrip()
        if cur: out.append(JOIN_LITERALS.sub('', ' '.join(cur))); nums.append(start)
        cur, depth, init = [], 0, False
    for n, l in enumerate(lines, 1):
        if cur and l.lstrip().startswith('//'): continue
        code = re.sub(r"'(?:\\.|[^'\\])'", "''", l)
        code = re.sub(r'"(?:\\.|[^"\\])*"', '""', code)
        code = re.sub(r'/\*.*?\*/', '', code).split('//')[0].rstrip()
        if not cur: start = n
        cur.append(l if not cur else without_trailing_comment(l).strip())
        depth = max(0, depth + code.count('(') - code.count(')'))
        if init:
            if '};' in code: init = False
        elif depth == 0:
            m = re.search(r'=\s*\{', code)
            init = bool(m) and code.count('{', m.start()) > code.count('}', m.start())
        if not init and (depth == 0 or code.endswith('{')): flush()
        elif n - start + 1 >= MAX_STATEMENT_LINES:
            print(f'inventory: a statement starting at line {start} is still open after {MAX_STATEMENT_LINES} lines; '
                  f'cut there', file=sys.stderr)
            flush()
    flush()
    return out, nums


R_LITERAL_OR_COMMENT = re.compile(r'''"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])'|(//.*)''')


def without_trailing_comment(l):
    """The line up to its `//` comment, if it has one (a `//` inside a string or char literal is not one)."""
    for m in R_LITERAL_OR_COMMENT.finditer(l):
        if m.group(1): return l[:m.start(1)]
    return l


MAX_STATEMENT_LINES = 40
JOIN_LITERALS = re.compile(r'(?<!\\)"\s+@?"')


R_DLOPEN = re.compile(r'dlopen\("(/System/Library/[^"]+)"')
R_DLSYM = re.compile(r'dlsym\(\s*([A-Za-z_]+)\s*,\s*"([A-Za-z0-9_]+)"\s*\)')
# helpers that resolve a name passed in: static T F(const char *n) { ... dlsym(..., n) ... } (e.g. BarcodeSymbol for exported constants)
R_DLSYM_HELPER = re.compile(r'static\s+[\w\s\*]+?\b(\w+)\s*\(\s*const\s+char\s*\*\s*(\w+)\s*\)\s*\{[^}]*dlsym\(\s*\w+\s*,\s*\2\s*\)')
R_LOCAL = re.compile(r'PreflightFindLocalSymbol\("([^"]+)",\s*"([^"]+)"\)')
R_CLASS = re.compile(r'(?:objc_getClass|NSClassFromString|objc_lookUpClass)\(@?"([A-Za-z0-9_]+)"\)')
R_IVAR = re.compile(r'class_getInstanceVariable\(([^,]+),\s*"([^"]+)"\)')
R_SELNAME = re.compile(r'sel_registerName\("([^"]+)"\)|@selector\(([^)]+)\)')
R_GETMETHOD = re.compile(r'class_get(Instance|Class)Method\(([^,]+),\s*(?:sel_registerName\("([^"]+)"\)|@selector\(([^)]+)\)|([A-Za-z_][A-Za-z0-9_]*))\)')
R_SETIMP = re.compile(r'method_setImplementation\(|class_replaceMethod\(')
R_TABLE = re.compile(r'\{\s*"([A-Za-z0-9_:]+)"\s*,\s*\(IMP\)')
R_TRAP = re.compile(r'ActInstallTrap\(([A-Za-z_]\w*),\s*(?:@selector\(([^)]+)\)|(\w+)\[i\])\s*,\s*(YES|NO)\)')
R_CLASSVAR = re.compile(r'(?:Class\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*objc_getClass\("([A-Za-z0-9_]+)"\)')
R_GETENV = re.compile(r'getenv\("([A-Z0-9_]+)"\)')
# the engine's named variables: #define ENV_<NAME> "<VARIABLE>" (core/env.inc), read as getenv(ENV_<NAME>)
R_ENVDEF = re.compile(r'^#define (ENV_\w+)\s+"([A-Z0-9_]+)"')
R_GETENV_NAMED = re.compile(r'getenv\((ENV_\w+)\)')
# the six variables the CLI sets when arming are read under two names: EnvConfig(ENV_<NAME>, ENV_LEGACY_<NAME>)
R_ENVCONFIG = re.compile(r'EnvConfig\((ENV_\w+),\s*(ENV_\w+)\)')
R_SLOT = re.compile(r'gSessDerived\[(\d+)\]\s*=\s*\(void \*\)(\w+)|gSessBase\.(\w+)\s*=\s*(\w+)|(?:^|[{,])\s*\.(\w+)\s*=\s*(src_\w+)|\(void \*\)(src_\w+)')
# ActHookResolve(group, "label", <class expr>, "selector", meta, add, ...) (core/act.inc)
R_HOOKRESOLVE = re.compile(r'ActHookResolve\(\s*\w+\s*,\s*"([^"]*)"\s*,\s*(objc_getClass\("[A-Za-z0-9_]+"\)|[A-Za-z_]\w*)\s*,\s*"([^"]+)"\s*,\s*(YES|NO)')
R_KVC_ARG = re.compile(r',\s*@"([A-Za-z_][A-Za-z0-9_]*)"\s*\)')      # in a statement with SafeKVC(: the key argument(s)
R_KVC_NEST = re.compile(r'SafeKVC\(\s*SafeKVC\([^()]*,\s*@"(\w+)"\s*\)\s*,\s*@"(\w+)"\s*\)')   # a key path: SafeKVC(SafeKVC(o, @"a"), @"b")
R_KVC_VFK = re.compile(r'valueForKey:\s*@"([A-Za-z_][A-Za-z0-9_]*)"')
R_STR = re.compile(r'@"([A-Za-z][A-Za-z0-9_.]{3,})"|CFSTR\("([A-Za-z][A-Za-z0-9_.]{3,})"\)')


def resolve_class(expr, lines, i):
    """Best-effort: turn a class expression at line i into a class name."""
    expr = expr.strip()
    m = R_CLASS.search(expr)
    if m: return m.group(1)
    if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', expr):
        for j in range(i, max(-1, i - 80), -1):   # look back within the function
            for v, c in R_CLASSVAR.findall(lines[j]):
                if v == expr: return c
            for v, c in re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*=\s*objc_getClass\("([^"]+)"\)', lines[j]):
                if v == expr: return c
    m = re.search(r'object_getClass\((\w+)\)', expr)
    if m: return f'<class of {m.group(1)}>'
    return f'<{expr}>'


def scan(files):
    # helper bodies and the ENV_ defines are looked for in whole files; everything else statement by statement
    helpers = {m.group(1) for _, lines in files for m in R_DLSYM_HELPER.finditer('\n'.join(lines))}
    while helpers:   # and helpers that hand their name on to one (BarcodeVisionSymbol -> BarcodeSymbol)
        r_via = re.compile(r'static\s+[\w\s\*]+?\b(\w+)\s*\(\s*const\s+char\s*\*\s*(\w+)\s*\)\s*\{[^}]*\b(?:' +
                           '|'.join(map(re.escape, sorted(helpers))) + r')\(\s*\2\s*\)')
        more = {m.group(1) for _, lines in files for m in r_via.finditer('\n'.join(lines))} - helpers
        if not more: break
        helpers |= more
    envdefs = {c: v for _, lines in files for l in lines for c, v in R_ENVDEF.findall(l)}
    r_helper_call = re.compile(r'\b(' + '|'.join(map(re.escape, sorted(helpers))) + r')\("([A-Za-z0-9_]+)"\)') if helpers else None
    inv = defaultdict(OrderedDict)
    def add(cat, key, site, **kw):
        e = inv[cat].setdefault(key, dict(sites=[], **kw)); e['sites'].append(site)
    for path, lines in files:
        sub = path.split('/')[2] if path.count('/') >= 3 else ''
        lines, nums = statements(lines)
        for i, l in enumerate(lines):
            site = f'{path}:{nums[i]}'
            if l.lstrip().startswith('//'): continue
            for p in R_DLOPEN.findall(l):
                add('frameworks', p, site, private='/PrivateFrameworks/' in p)
            if r_helper_call:
                for h, s in r_helper_call.findall(l):
                    add('data-syms', s, site, helper=h)
            for h, s in R_DLSYM.findall(l):
                add('functions', s, site, handle=h)
            for img, s in R_LOCAL.findall(l):
                add('local-syms', s, site, image=img)
            for c in R_CLASS.findall(l):
                add('classes', c, site)
            for cexpr, iv in R_IVAR.findall(l):
                add('ivars', f'{resolve_class(cexpr, lines, i)}.{iv}', site)
            for m in R_SLOT.finditer(l):
                idx, fn, bslot, bfn, sslot, sfn, sfn2 = m.groups()
                if idx: add('vtable-slots', f'session[{idx}] {fn}', site)
                elif bslot: add('vtable-slots', f'session.base.{bslot} {bfn}', site)
                elif sslot: add('vtable-slots', f'source.base.{sslot} {sfn}', site)
                elif sfn2: add('vtable-slots', f'source {sfn2}', site)
            named = R_GETENV_NAMED.findall(l) + [c for pair in R_ENVCONFIG.findall(l) for c in pair]
            for e in R_GETENV.findall(l) + [envdefs[c] for c in named if c in envdefs]:
                add('env', e, site)
            if sub in ('fig', 'providers'):
                for a, b in R_STR.findall(l):
                    s = a or b
                    if not re.search(r'[a-z][A-Z]|^[A-Z][a-z]', s): continue   # identifier-like CamelCase only
                    add('strings', s, site)
            # hooks: a method_setImplementation on this line; find the Method's class/selector on this line or above
            if R_SETIMP.search(l):
                for j in range(i, max(-1, i - 6), -1):
                    gm = R_GETMETHOD.search(lines[j])
                    if gm:
                        kind, cexpr, s1, s2, svar = gm.groups()
                        sel = s1 or s2
                        if not sel and svar:   # selector held in a variable/table (motion table, lazy-arming traps): reported below
                            break
                        add('hooks', f'{"+" if kind == "Class" else "-"}[{resolve_class(cexpr, lines, j)} {sel}]', site)
                        break
            for s in R_TABLE.findall(l):   # selector tables: {"sel", (IMP)fn}
                cls = '?'
                for j in range(i, min(len(lines), i + 30)):
                    m = re.search(r'class_getInstanceMethod\(([A-Za-z_]+),', lines[j])
                    if m: cls = resolve_class(m.group(1), lines, j); break
                add('hooks', f'-[{cls} {s}]', site)
            for cv, s, arr, meta in R_TRAP.findall(l):
                cls = resolve_class(cv, lines, i)
                sels = [s] if s else []
                if not s:   # ActInstallTrap(d, sels[i], YES): the SEL sels[] = {...} array above
                    for j in range(i, max(-1, i - 8), -1):
                        m = re.search(r'SEL ' + re.escape(arr) + r'\[\] = \{(.*)\};', lines[j])
                        if m: sels = re.findall(r'@selector\(([^)]+)\)', m.group(1)); break
                for x in sels:
                    add('hooks', f'{"+" if meta == "YES" else "-"}[{cls} {x}]', site, trap=True)
            for what, cexpr, sel, meta in R_HOOKRESOLVE.findall(l):
                if sel.startswith('simcamTest'): continue   # the SIMCAM_TEST_ACTIVATE_FAIL=missing fault's made-up selector
                add('hooks', f'{"+" if meta == "YES" else "-"}[{resolve_class(cexpr, lines, i)} {sel}]', site, what=what)
            if 'SafeKVC(' in l and 'static id SafeKVC' not in l:
                nest = R_KVC_NEST.findall(l); inner = {b for _, b in nest}
                for a, b in nest: add('kvc-keys', f'{a}.{b}', site)
                for k in R_KVC_ARG.findall(l):
                    if k not in inner: add('kvc-keys', k, site)
            for k in R_KVC_VFK.findall(l): add('kvc-keys', k, site)
            for a, b in R_SELNAME.findall(l):
                add('selectors', a or b, site)
    # selectors that are hooked are reported as hooks only
    hooked = {k.split(' ', 1)[1].rstrip(']') for k in inv['hooks']}
    for s in list(inv['selectors']):
        if s in hooked: del inv['selectors'][s]
    return inv


def sdk_index(sdk):
    """Public class names and selector keywords from the SDK's public framework headers."""
    classes, words = set(), set()
    fw = os.path.join(sdk, 'System/Library/Frameworks')
    for d, _, fs in os.walk(fw):
        if '/Headers' not in d: continue
        for f in fs:
            if not f.endswith('.h'): continue
            t = open(os.path.join(d, f), encoding='utf-8', errors='replace').read()
            classes.update(re.findall(r'@interface\s+([A-Za-z0-9_]+)', t))
            words.update(re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*:', t))
            words.update(re.findall(r'\)\s*([A-Za-z_][A-Za-z0-9_]*)\s*[;:\s]', t))
            for attrs, name in re.findall(r'@property\s*(\([^)]*\))?[^;]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:API_|NS_|__|;)', t):
                words.add(name)
                words.update(re.findall(r'\b(?:getter|setter)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)', attrs))
                if 'readonly' not in attrs: words.add('set' + name[:1].upper() + name[1:])
    return classes, words


def classify(inv, sdk):
    if sdk:
        classes, words = sdk_index(sdk)
        pub_cls = lambda c: c in classes
        pub_sel = lambda s: not s.startswith('_') and all(k in words for k in s.split(':') if k)
        how = 'sdk'
    else:
        pub_cls = lambda c: not (c.startswith('Fig') or c.endswith('Internal') or c.startswith('AVCaptureFig'))
        pub_sel = lambda s: not (s.startswith('_') or 'Fig' in s)
        how = 'guess'
    for c, e in inv['classes'].items():
        e['public'] = pub_cls(c); e['by'] = how
    for k, e in inv['hooks'].items():
        m = re.match(r'[-+]\[(\S+) (.+)\]', k)
        cls, sel = m.groups()
        e['public'] = pub_cls(cls) and pub_sel(sel); e['by'] = how
    for s, e in inv['selectors'].items():
        e['public'] = pub_sel(s); e['by'] = how
    for s, e in inv['kvc-keys'].items():   # a key is public if a public header declares a property (or method) of that name
        e['public'] = all((k in words) if sdk else not k.startswith('_') for k in s.split('.')); e['by'] = how
    for s, e in inv['data-syms'].items():   # exported constants: public if a public header names them
        e['public'] = (s in words) if sdk else not s.startswith('MRC'); e['by'] = how


def markdown(inv):
    out = ['# Engine dependency inventory (static)', '',
           'Generated by `tools/inventory.py`; counts are of distinct names. "public" means found in the SDK\'s public',
           'headers (or, without `--sdk`, a name guess). Hooks marked "trap" are lazy-arming traps (forward to the original).', '']
    summary = []
    for cat in ['frameworks', 'functions', 'data-syms', 'local-syms', 'vtable-slots', 'classes', 'hooks', 'ivars', 'selectors', 'kvc-keys', 'strings', 'env']:
        d = inv.get(cat, {})
        priv = sum(1 for e in d.values() if e.get('private') or e.get('public') is False)
        summary.append(f'| {cat} | {len(d)} | {priv if cat not in ("strings", "env", "functions", "local-syms", "ivars", "vtable-slots") else "—"} |')
    out += ['| category | count | private |', '|---|---|---|'] + summary + ['']
    for cat, d in inv.items():
        if cat == 'strings':
            out += [f'## strings ({len(d)}, heuristic)', '', ', '.join(f'`{k}`' for k in d), '']
            continue
        if cat == 'env':
            out += [f'## env ({len(d)})', '', ', '.join(f'`{k}`' for k in d), '']
            continue
        label = cat == 'hooks'
        out += [f'## {cat} ({len(d)}{", heuristic" if cat == "kvc-keys" else ""})', '',
                '| name | public | ' + ('label | ' if label else '') + 'first site | sites |', '|---|---|---|' + ('---|' if label else '') + '---|']
        for k, e in d.items():
            pub = '' if 'public' not in e and 'private' not in e else ('no' if e.get('private') or e.get('public') is False else 'yes')
            if e.get('trap'): pub += ' (trap)'
            out.append(f'| `{k}` | {pub} | ' + (f'{e.get("what", "")} | ' if label else '') + f'{e["sites"][0]} | {len(e["sites"])} |')
        out.append('')
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', default=os.path.join(ROOT, 'engine/src'))
    ap.add_argument('--sdk', help='iPhoneSimulator SDK path, to classify public/private from headers')
    ap.add_argument('--json'); ap.add_argument('--md')
    a = ap.parse_args()
    inv = scan(read_sources(a.src))
    classify(inv, a.sdk)
    md = markdown(inv)
    if a.json: json.dump(inv, open(a.json, 'w'), indent=1)
    if a.md: open(a.md, 'w').write(md + '\n')
    if not a.json and not a.md: print(md)
    else:
        for cat, d in inv.items(): print(f'{cat}: {len(d)}', file=sys.stderr)


if __name__ == '__main__':
    main()
