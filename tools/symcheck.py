#!/usr/bin/env python3
"""List the imports of the staged game libraries that no shim table answers.

usage: symcheck.py <libdir> [--source source]
An unresolved import is not a link error in this loader: the slot is tainted and
the game aborts at the first call. Run this after every table change.
"""
import re, subprocess, sys, glob, os
libdir = sys.argv[1]
src = sys.argv[sys.argv.index('--source') + 1] if '--source' in sys.argv else 'source'
names = set()
for f in glob.glob(os.path.join(src, '*.c')) + glob.glob(os.path.join(src, '*.h')):
    names |= set(re.findall(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,', open(f, errors='replace').read()))
def syms(path, undefined):
    out = subprocess.run(['readelf', '-sW', '--dyn-syms', path], capture_output=True, text=True).stdout
    r = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) < 8 or not p[0].endswith(':') or p[3] not in ('FUNC', 'OBJECT', 'NOTYPE', 'TLS'): continue
        if (p[6] == 'UND') == undefined and p[4] in ('GLOBAL', 'WEAK'):
            r[p[7].split('@')[0]] = (p[3], p[4])
    return r
libs = sorted(glob.glob(os.path.join(libdir, '*.so')))
libs = [l for l in libs if open(l, 'rb').read(4) == b'\x7fELF']
exports = set()
for l in libs: exports |= set(syms(l, False))
total = 0
for l in libs:
    und = syms(l, True)
    miss = sorted(n for n in und if n not in names and n not in exports)
    total += len(miss)
    print(f'{os.path.basename(l)}: {len(und)} imports, {len(miss)} unanswered')
    for n in miss: print(f'    {n:40s} {und[n][0]} {und[n][1]}')
sys.exit(1 if total else 0)
