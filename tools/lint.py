#!/usr/bin/env python3
"""style lint: the mechanical half of docs/coding-style.rst.

a style guide is only worth having if a machine can hold most of it, so this
checks the rules that need no judgement: the file header, the shape of a
multi-line comment, the function brace, dashes and first person in prose,
tabs, trailing whitespace, the `//` rule, and the include guard.

naming prefixes are reported as warnings rather than errors, because the tree
predates that rule and is being converted a file at a time. line length is a
warning too: the guide allows a long string that cannot be split.

usage:
    tools/lint.py [--strict] [file ...]

with no files, every C source and header git knows about is checked, so this
is what `make lint` runs and what CI runs before anything is built. a list of
files checks just those, which is what a person wants while working on one
file. `--strict` turns the warnings into failures as well.
"""

import re
import subprocess
import sys

SPDX = '// SPDX-License-Identifier: GPL-2.0-only'
# the header names whoever wrote the file: any name, any address, a
# comma, a year. checking for one person's name would fail every
# contributor for being someone else, which is not a rule.
AUTHOR_RE = re.compile(r'^\S.*\s<[^<>@\s]+@[^<>\s]+>,\s*\d{4}$')
C_EXT = ('.c', '.h')
LINE_MAX = 80

TYPES = ('bool', 'int', 'int32_t', 'int64_t', 'unsigned int', 'uint32_t',
         'uint64_t', 'size_t', 'float', 'double', 'char', 'std::string')
PREFIX = {'bool': 'b_', 'int': 'i_', 'int32_t': 'i_', 'int64_t': 'i_',
          'unsigned int': 'u_', 'uint32_t': 'u_', 'uint64_t': 'u_',
          'size_t': 'sz_', 'float': 'f_', 'double': 'd_', 'char': 'c_',
          'std::string': 'str_'}
DECL = re.compile(r'\b(' + '|'.join(re.escape(t) for t in TYPES)
                  + r')\s+(\w+)\s*[=;,)]')
# what a variable's scope says goes in front of what its type says:
# g_ for a global, s_ for a static, m_ for a member, so s_sz_count is a
# static size_t and is named right
SCOPE = ('g_', 's_', 'm_')
DASH = re.compile(r'(?<![-\s]) -- (?![-\s])|[—–]')
PERSONA = re.compile(r'\bthou\b|\bhath\b|velvet|arcana|persona|\bigor\b|'
                     r'\bmargaret\b|\btheodore\b|\blavenza\b|'
                     r'\bpixie\b|\bsouls\b', re.I)
FIRST = re.compile(r"\b(I|me|my|mine|myself|we|us|our)\b")


def scan(text):
    """(kind, start, end) for every comment, string and character literal"""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j == -1 else j + 2
            out.append(('comment', i, j))
            i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j == -1 else j
            out.append(('line', i, j))
            i = j
        elif text[i] in '"\'':
            q = text[i]
            j = i + 1
            while j < n:
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == q:
                    j += 1
                    break
                j += 1
            out.append(('string' if q == '"' else 'char', i, j))
            i = j
        else:
            i += 1
    return out


def mask(text, tokens):
    buf = list(text)
    for _, a, b in tokens:
        for k in range(a, b):
            if buf[k] != '\n':
                buf[k] = ' '
    return ''.join(buf)


def lineno(text, off):
    return text.count('\n', 0, off) + 1


def prose_problems(path, text):
    """dashes and first person inside comments and strings"""
    bad = []
    tokens = scan(text)
    for kind, a, b in tokens:
        if kind in ('line', 'char'):
            continue
        body = text[a:b]
        if PERSONA.search(body):
            continue
        for m in DASH.finditer(body):
            where = 'comment' if kind == 'comment' else 'string'
            bad.append((lineno(text, a + m.start()),
                        'dash in %s: %r' % (where, m.group(0).strip())))
        for m in FIRST.finditer(body):
            s, e = m.start(), m.end()
            before = body[max(0, s - 1):s]
            after = body[e:e + 1]
            if before in ('`', "'", '%') or after in ('`', "'"):
                continue
            rest = body[e:e + 7]
            if rest.startswith('()') or rest.startswith(' layout'):
                continue
            if body[s:s + 4] == 'i.e.':
                continue
            bad.append((lineno(text, a + s),
                        'first person %r in %s' % (m.group(0),
                        'comment' if kind == 'comment' else 'string')))
    return bad


def comment_shape(text, tokens):
    bad = []
    for kind, a, b in tokens:
        if kind != 'comment':
            continue
        body = text[a:b]
        if '\n' not in body:
            continue
        line_start = text.rfind('\n', 0, a) + 1
        if text[line_start:a].strip():
            # a trailing comment may stay where the code ends
            continue
        first = body.split('\n', 1)[0].strip()
        if first not in ('/*', '/**'):
            bad.append((lineno(text, a),
                        'multi-line comment starts with %r, not /* alone'
                        % first[:20]))
        if not body.rstrip().endswith('*/'):
            bad.append((lineno(text, b - 2), 'comment does not close with */'))
    return bad


def brace_problems(text, tokens):
    bad = []
    masked = mask(text, tokens)
    depth = 0
    prev = -1
    for i, c in enumerate(masked):
        if c == '{':
            if depth == 0 and prev != -1 and masked[prev] == ')':
                ls = text.rfind('\n', 0, i) + 1
                head = text[ls:i]
                if head.strip() and not head.lstrip().startswith('#'):
                    bad.append((lineno(text, i),
                                'function brace belongs on its own line'))
            depth += 1
            prev = i
        elif c == '}':
            depth -= 1
            prev = i
        elif not c.isspace():
            prev = i
    return bad


def header_problems(path, text):
    lines = text.split('\n')
    bad = []
    if not lines or lines[0] != SPDX:
        bad.append((1, 'first line must be %r' % SPDX))
        return bad
    if len(lines) < 6 or lines[1].strip() != '/*':
        bad.append((2, 'second line must open the header comment'))
        return bad
    block = []
    for line in lines[2:]:
        block.append(line)
        if line.strip() == '*/':
            break
    body = [l.strip().lstrip('*').strip() for l in block[:-1]]
    if not any(l == path for l in body):
        bad.append((2, 'header must name the file: %s' % path))
    if not any(AUTHOR_RE.match(l) for l in body):
        bad.append((2, 'header must name the author as NAME <email>, YYYY'))
    if not any(l for l in body):
        bad.append((2, 'header must carry a brief description'))
    return bad


def guard_problems(path, text, tokens):
    if not path.endswith('.h'):
        return []
    masked = mask(text, tokens)
    code = [l.strip() for l in masked.split('\n') if l.strip()]
    if not code:
        return []
    if not code[0].startswith('#ifndef '):
        return [(lineno(text, text.find(code[0])),
                 'header must open with #ifndef, found %r' % code[0][:30])]
    name = code[0].split()[1]
    if not name.endswith('_H'):
        return [(1, 'include guard %s should end in _H' % name)]
    if len(code) < 2 or code[1] != '#define ' + name:
        return [(1, 'include guard #define does not match #ifndef %s' % name)]
    return []


def check_file(path, strict):
    text = open(path, encoding='utf-8', errors='ignore').read()
    problems = []          # (line, message, kind)
    is_c = path.endswith(C_EXT)
    tokens = scan(text)

    for i, line in enumerate(text.split('\n'), 1):
        if '\t' in line:
            problems.append((i, 'tab character; the indent is four spaces',
                             'error'))
        if line != line.rstrip():
            problems.append((i, 'trailing whitespace', 'error'))
        if len(line) > LINE_MAX:
            problems.append((i, 'line is %d columns' % len(line), 'warn'))
        if is_c:
            for m in DECL.finditer(mask(line, scan(line))):
                t, name = m.group(1), m.group(2)
                want = PREFIX[t]
                bare = name[2:] if name[:2] in SCOPE else name
                if not bare.startswith(want):
                    msg = "naming: '%s' (%s) should start with '%s'"
                    problems.append((i, msg % (name, t, want), 'warn'))

    if not is_c:
        return problems

    for kind, a, b in tokens:
        if kind != 'line':
            continue
        if text[a:b].strip() != SPDX:
            problems.append((lineno(text, a),
                             '// comments are not used; see the style guide',
                             'error'))

    for line, msg in header_problems(path, text):
        problems.append((line, msg, 'error'))
    for line, msg in comment_shape(text, tokens):
        problems.append((line, msg, 'error'))
    for line, msg in brace_problems(text, tokens):
        problems.append((line, msg, 'error'))
    for line, msg in guard_problems(path, text, tokens):
        problems.append((line, msg, 'error'))
    for line, msg in prose_problems(path, text):
        problems.append((line, msg, 'error'))
    return problems


def git_files():
    """the C sources, and the python and shell tools held to the same rules.

    every C file git tracks is this project's own. a submodule is one
    entry naming a commit rather than the files in it, so a tree that
    carries another project as a submodule lints what it wrote and leaves
    the other project's files to that project's own lint."""
    out = subprocess.run(['git', 'ls-files'], capture_output=True, text=True)
    keep = []
    for f in out.stdout.split():
        if f.endswith(C_EXT):
            keep.append(f)
        elif (f.startswith('tools/')
              and f.endswith(('.py', '.sh', '.c', '.h'))):
            keep.append(f)
    return keep


def main(argv):
    strict = '--strict' in argv
    args = [a for a in argv[1:] if not a.startswith('-')]
    files = args if args else git_files()
    if not files:
        print('style lint: no files to check')
        return 0

    errors = 0
    warnings = 0
    shown = 0
    for path in files:
        try:
            problems = check_file(path, strict)
        except OSError as e:
            print('%s: %s' % (path, e))
            errors += 1
            continue
        for line, msg, kind in sorted(problems):
            if kind == 'error' or strict:
                errors += 1
            else:
                warnings += 1
                if shown >= 40:
                    continue
                shown += 1
            print('%s:%d: %s: %s' % (path, line, kind, msg))

    print()
    print('style lint: %d files, %d error(s), %d warning(s)'
          % (len(files), errors, warnings))
    if warnings > shown:
        print('  (%d more warnings not shown)' % (warnings - shown))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
