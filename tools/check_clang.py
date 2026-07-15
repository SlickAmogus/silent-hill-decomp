#!/usr/bin/env python3
"""Local strict-compiler check for the PC port.

Re-compiles the C++ translation units with clang's ``-fsyntax-only`` using the
EXACT flags from the CMake compile database, to catch errors that the local
MinGW GCC (and the Ubuntu-GCC Linux CI) silently accept but a stricter compiler
rejects -- e.g. the "declaration has a different language linkage" error that
broke the macOS CI (which builds with a newer Homebrew GCC). clang is used as a
locally-available proxy for that stricter compiler; it flags the same
C++-conformance issues without needing macOS.

Scope: C++ TUs only by default (PsyCross HAL + pc_port C++), where the
extern "C"/linkage/strictness class lives. The decomp C in src/ is GCC-targeted
(and macOS builds it with GCC too), so clang would only false-positive there --
pass --all to include everything anyway.

Usage (normally via tools/check-clang.sh, which regenerates the DB first):
    python tools/check_clang.py [BUILD_DIR] [--all] [--filter SUBSTR] [--cxx PATH] [--cc PATH]
Exit code: 0 if every checked TU passes, 1 if any fails (or setup is wrong).
"""
import json
import os
import shlex
import subprocess
import sys

CXX_EXTS = ('.cpp', '.cc', '.cxx', '.c++', '.C')


def parse_args(argv):
    opts = {'build': None, 'all': False, 'filter': None,
            'cxx': os.environ.get('CLANGXX', 'clang++'),
            'cc': os.environ.get('CLANGCC', 'clang')}
    it = iter(argv)
    for a in it:
        if a == '--all':
            opts['all'] = True
        elif a == '--filter':
            opts['filter'] = next(it, None)
        elif a == '--cxx':
            opts['cxx'] = next(it, None)
        elif a == '--cc':
            opts['cc'] = next(it, None)
        elif not a.startswith('-') and opts['build'] is None:
            opts['build'] = a
    return opts


def entry_args(entry):
    if 'arguments' in entry and entry['arguments']:
        return list(entry['arguments'])
    # posix=False so Windows backslash paths (C:\Claude\...) aren't treated as
    # escapes and stripped; then peel off any fully-wrapping quotes per token.
    toks = shlex.split(entry.get('command', ''), posix=False)
    out = []
    for t in toks:
        if len(t) >= 2 and t[0] == t[-1] and t[0] in ('"', "'"):
            t = t[1:-1]
        out.append(t)
    return out


def is_cpp(entry, args):
    f = entry['file']
    if f.endswith(CXX_EXTS):
        return True
    comp = os.path.basename(args[0]) if args else ''
    return any(tok in comp for tok in ('++', 'c++'))


def rewrite(args, compiler):
    """Swap the compiler, strip output/codegen/-Werror, add -fsyntax-only."""
    out = [compiler]
    it = iter(args[1:])
    for a in it:
        if a == '-o':
            next(it, None)          # drop "-o <file>"
            continue
        if a.startswith('-o') and len(a) > 2:
            continue                # drop "-o<file>"
        if a == '-c':
            continue
        if a.startswith('-Werror'):
            continue                # don't let clang-only warnings fail the check
        out.append(a)
    out += ['-fsyntax-only', '-Wno-unused-command-line-argument',
            '-Wno-unknown-warning-option', '-Wno-unknown-attributes']
    return out


def main():
    opts = parse_args(sys.argv[1:])
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
    build = opts['build'] or os.path.join(root, 'pc_port', 'build')
    db_path = os.path.join(build, 'compile_commands.json')

    if not os.path.isfile(db_path):
        print(f"error: {db_path} not found. Configure with "
              f"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON (tools/check-clang.sh does this).",
              file=sys.stderr)
        return 1

    with open(db_path, 'r', encoding='utf-8') as fh:
        db = json.load(fh)

    checked, failures = 0, []
    seen = set()
    for entry in db:
        f = os.path.normpath(entry['file'])
        # Skip CMake's own compiler-probe TUs and anything outside the tree.
        low = f.replace('\\', '/').lower()
        if '/cmakefiles/cmakescratch/' in low or '/cmaketmp/' in low:
            continue
        args = entry_args(entry)
        if not args:
            continue
        cpp = is_cpp(entry, args)
        if not opts['all'] and not cpp:
            continue                # default: C++ TUs only
        if opts['filter'] and opts['filter'].replace('\\', '/').lower() not in low:
            continue
        if f in seen:
            continue
        seen.add(f)

        cmd = rewrite(args, opts['cxx'] if cpp else opts['cc'])
        try:
            r = subprocess.run(cmd, cwd=entry.get('directory', build),
                               capture_output=True, text=True)
        except FileNotFoundError as e:
            print(f"error: could not run clang ({e}). Is it on PATH / --cxx correct?",
                  file=sys.stderr)
            return 1
        checked += 1
        rel = os.path.relpath(f, root)
        if r.returncode != 0:
            failures.append((rel, r.stderr))
            print(f"  FAIL  {rel}")
        else:
            print(f"  ok    {rel}")

    print()
    if failures:
        print(f"=== {len(failures)} of {checked} C++ TU(s) FAILED the clang check ===")
        for rel, err in failures:
            print(f"\n----- {rel} -----")
            lines = [ln for ln in err.splitlines()
                     if ('error:' in ln or 'note:' in ln or 'warning:' in ln)]
            for ln in (lines[:25] if lines else err.splitlines()[:25]):
                print(ln)
        print(f"\nFAILED: {len(failures)} file(s). Fix before pushing/releasing.")
        return 1

    print(f"OK: {checked} C++ TU(s) passed clang -fsyntax-only.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
