#!/usr/bin/env python3
"""Read a profile made with SDT_PROFILE (source/sdt_prof.c).

usage: prof.py <prof.map> <prof.bin> <sdt_nx.elf> [--libs DIR] [--unity-syms FILE] [--methods FILE]
                [--last SECONDS] [--skip-last SECONDS] [--top N]

Prints, for each sampled thread, where its samples fell: by module, then by
function. A sample whose pc is a kernel call (svc*) is the thread waiting, and
is charged to the first caller outside libnx's own wrappers.
--libs: the folder with libunity.so / libil2cpp.so, to name their exported functions.
--unity-syms: Unity's libunity.sym.so for the same engine build (from the Editor's Android support,
  Variations/il2cpp/Release/Symbols/arm64-v8a), to name every engine function.
--methods: methods.txt written by the port beside prof.bin, to name the game's managed methods.
--skip-last: leave out the final seconds (closing the game).
Diagnostics, after the tables: --raw-waits (a few of UnityMain's waiting samples, every slot named),
--callers-of NAME (the commonest call chains of samples inside a function whose name contains NAME),
--gi-waits (UnityMain's waits entered from the game image, with offsets into it).
"""
import bisect, struct, sys, os
from collections import Counter
from elftools.elf.elffile import ELFFile

args = sys.argv[1:]
def opt(name, default):
    if name in args:
        i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
    return default
libs = opt('--libs', None); last = float(opt('--last', 0)); top = int(opt('--top', 28))
usyms = opt('--unity-syms', None); methods = opt('--methods', None); skip = float(opt('--skip-last', 0))
mapf, binf, elff = args[:3]

m = {}
for line in open(mapf):
    p = line.split()
    if p: m[p[0]] = p[1:]
hz = int(m['hz'][0])

def symtab(path, section):
    e = ELFFile(open(path, 'rb')); s = e.get_section_by_name(section)
    t = sorted((x['st_value'], x.name) for x in s.iter_symbols()
               if x['st_info']['type'] == 'STT_FUNC' and x['st_value'] and x.name)
    return [a for a, _ in t], [n for _, n in t]

nro_a, nro_n = symtab(elff, '.symtab')
anchor = nro_a[nro_n.index('sdt_prof_anchor')]
slide = int(m['nro_anchor'][0], 16) - anchor
e = ELFFile(open(elff, 'rb'))
text = e.get_section_by_name('.text')
nro_lo = slide + text['sh_addr']; nro_hi = nro_lo + text['sh_size']

mods = []     # (name, base, size, addrs, names)
for key, fn in (('libunity', 'libunity.so'), ('facade', 'libil2cpp.so'), ('libmain', 'libmain.so')):
    base, size = int(m[key][0], 16), int(m[key][1], 16)
    a, n = ([], [])
    full = False
    if key == 'libunity' and usyms: a, n = symtab(usyms, '.symtab'); full = True
    elif libs and os.path.exists(os.path.join(libs, fn)): a, n = symtab(os.path.join(libs, fn), '.dynsym')
    mods.append((key, base, size, a, n, full))
cm_base, cm_size = int(m['codemem'][0], 16), int(m['codemem'][1], 16)
cm_a, cm_n = [], []
if methods:
    t = sorted((int(l.split(None, 1)[0], 16), l.split(None, 1)[1].strip()) for l in open(methods, errors='replace') if ' ' in l)
    cm_a = [a for a, _ in t]; cm_n = [x for _, x in t]

def where(pc):
    """(module, function) of an address"""
    if nro_lo <= pc < nro_hi:
        i = bisect.bisect_right(nro_a, pc - slide) - 1
        return 'port', nro_n[i] if i >= 0 else '?'
    for name, base, size, a, n, full in mods:
        if base <= pc < base + size:
            i = bisect.bisect_right(a, pc - base) - 1
            # stripped modules have few names: give the 64 KB region when the nearest one is far
            if i >= 0 and (full or pc - base - a[i] < 0x4000): return name, n[i]
            return name, '+%#x' % ((pc - base) & ~0xffff)
    if cm_base <= pc < cm_base + cm_size:
        i = bisect.bisect_right(cm_a, pc - cm_base) - 1
        if i >= 0 and pc - cm_base - cm_a[i] < 0x3000: return 'game image', cm_n[i]
        return 'game image', '+%#x' % ((pc - cm_base) & ~0xfffff)
    return 'other', '%#x' % (pc & ~0xfffff)

def port_group(fn):
    if fn.startswith('svc'): return 'kernel call'
    for pre, g in (('nvk_', 'NVK'), ('nak_', 'NVK'), ('nil_', 'NVK'), ('nouveau', 'NVK'), ('vk_', 'NVK'), ('vk', 'NVK'),
                   ('nir_', 'NVK'), ('wsi_', 'NVK'), ('mme_', 'NVK'), ('nv', 'NVK'), ('SDL_', 'SDL'), ('sdt_', 'port shims'),
                   ('b_', 'port shims'), ('av', 'ffmpeg'), ('ff_', 'ffmpeg'), ('sws', 'ffmpeg')):
        if fn.startswith(pre): return g
    return 'port + libc + libnx'

WRAP = ('svc', 'mutex', 'condvar', 'rwlock', 'semaphore', 'uevent', 'wait', '__', '_', 'pthread', 'futex', 'thrd', 'cnd', 'mtx',
        'nx_', 'sem_', 'usleep', 'nanosleep', 'sched', 'sleep', 'diag_')
data = open(binf, 'rb').read()
depth = int(m['depth'][0]) if 'depth' in m else 4
rec = 8 + 8 * depth
n = len(data) // rec
samples = [struct.unpack_from('<II%dQ' % depth, data, i * rec) for i in range(n)]
if skip: samples = samples[:-int(skip * hz * 2)]
if last: samples = samples[-int(last * hz * 2):]
for t, tname in enumerate(('UnityMain', 'UnityGfxDeviceWorker')):
    ss = [s for s in samples if s[0] == t]
    if not ss: continue
    bymod, byfn, waits = Counter(), Counter(), Counter()
    for s in ss:
        pc = s[2]; mod, fn = where(pc)
        if mod == 'port' and fn.startswith('svc'):
            # waiting: who asked? The first callers outside the port's own wrappers (up to three).
            who = []
            for a in s[3:]:
                if not a: continue
                mm, ff = where(a)
                if mm in ('port', 'other'): continue          # wrappers, and the frame loop the engine was called from
                if 'Baselib_' in ff or 'Futex' in ff or 'Semaphore' in ff or 'CortexA53' in ff: continue
                if who and who[-1] == (mm + ' ' + ff)[:70]: continue
                who.append((mm + ' ' + ff)[:70])
                if len(who) == 5: break
            bymod['(waiting in a kernel call)'] += 1; waits[fn + '  <-  ' + ('  <-  '.join(who) or 'port')] += 1
            continue
        g = port_group(fn) if mod == 'port' else mod
        bymod[g] += 1; byfn[(g, fn)] += 1
    tot = len(ss)
    print('== %s: %d samples (%.0f s at %d Hz)' % (tname, tot, tot / hz, hz))
    for g, c in bymod.most_common(): print('   %5.1f%%  %s' % (100 * c / tot, g))
    print('   -- busiest functions')
    for (g, fn), c in byfn.most_common(top): print('   %5.1f%%  %-22s %s' % (100 * c / tot, g, fn[:90]))
    print('   -- waits')
    for w, c in waits.most_common(14): print('   %5.1f%%  %s' % (100 * c / tot, w[:420]))
if '--raw-waits' in sys.argv:
    k = 0
    for s in samples[-40000:]:
        if s[0] == 0 and where(s[2])[1].startswith('svcWaitProcessWideKey'):
            print(' | '.join('%s %s' % where(a) if a else '-' for a in s[2:])[:900]); k += 1
            if k > 6: break
if '--callers-of' in sys.argv:
    want = sys.argv[sys.argv.index('--callers-of') + 1]; cc = Counter()
    for s in samples:
        if want in where(s[2])[1]:
            cc[(s[0], ' | '.join('%s' % where(a)[1][:48] for a in s[3:9] if a))] += 1
    for (t, k), c in cc.most_common(8): print(t, c, k)
if '--gi-waits' in sys.argv:
    cc = Counter()
    for s in samples:
        if s[0] == 0 and where(s[2])[1].startswith('svcWait'):
            ch = [a for a in s[3:] if a and where(a)[0] not in ('port', 'other') and not any(x in where(a)[1] for x in ('Baselib_', 'Futex', 'Semaphore', 'CortexA53'))]
            if ch and where(ch[0])[0] == 'game image':
                cc[' | '.join(('%s+%#x' % (where(a)[1][:40], a - cm_base)) if where(a)[0] == 'game image' else where(a)[0][:3] + ':' + where(a)[1][:34] for a in ch[:6])] += 1
    for k, c in cc.most_common(6): print(c, k)
