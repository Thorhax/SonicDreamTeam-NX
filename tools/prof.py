#!/usr/bin/env python3
"""Read a profile made with SDT_PROFILE (source/sdt_prof.c).

usage: prof.py <prof.map> <prof.bin> <sdt_nx.elf> [--libs DIR] [--last SECONDS] [--top N]

Prints, for each sampled thread, where its samples fell: by module, then by
function. A sample whose pc is a kernel call (svc*) is the thread waiting, and
is charged to the first caller outside libnx's own wrappers.
--libs: the folder with libunity.so / libil2cpp.so, to name their exported functions.
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
    if libs and os.path.exists(os.path.join(libs, fn)): a, n = symtab(os.path.join(libs, fn), '.dynsym')
    mods.append((key, base, size, a, n))
cm_base, cm_size = int(m['codemem'][0], 16), int(m['codemem'][1], 16)

def where(pc):
    """(module, function) of an address"""
    if nro_lo <= pc < nro_hi:
        i = bisect.bisect_right(nro_a, pc - slide) - 1
        return 'port', nro_n[i] if i >= 0 else '?'
    for name, base, size, a, n in mods:
        if base <= pc < base + size:
            i = bisect.bisect_right(a, pc - base) - 1
            # stripped modules have few names: give the 64 KB region when the nearest one is far
            if i >= 0 and pc - base - a[i] < 0x4000: return name, n[i]
            return name, '+%#x' % ((pc - base) & ~0xffff)
    if cm_base <= pc < cm_base + cm_size:
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
n = len(data) // 40
samples = [struct.unpack_from('<II4Q', data, i * 40) for i in range(n)]
if last: samples = samples[-int(last * hz * 2):]
for t, tname in enumerate(('UnityMain', 'UnityGfxDeviceWorker')):
    ss = [s for s in samples if s[0] == t]
    if not ss: continue
    bymod, byfn, waits = Counter(), Counter(), Counter()
    for _, _, pc, lr, r1, r2 in ss:
        mod, fn = where(pc)
        if mod == 'port' and fn.startswith('svc'):
            # waiting: who asked?
            who = 'port'
            for a in (lr, r1, r2):
                if not a: continue
                mm, ff = where(a)
                if mm != 'port': who = mm + ' ' + ff; break
                if not ff.startswith(WRAP): who = 'port ' + ff; break
            bymod['(waiting in a kernel call)'] += 1; waits[fn + '  <-  ' + who] += 1
            continue
        g = port_group(fn) if mod == 'port' else mod
        bymod[g] += 1; byfn[(g, fn)] += 1
    tot = len(ss)
    print('== %s: %d samples (%.0f s at %d Hz)' % (tname, tot, tot / hz, hz))
    for g, c in bymod.most_common(): print('   %5.1f%%  %s' % (100 * c / tot, g))
    print('   -- busiest functions')
    for (g, fn), c in byfn.most_common(top): print('   %5.1f%%  %-22s %s' % (100 * c / tot, g, fn[:90]))
    print('   -- waits')
    for w, c in waits.most_common(10): print('   %5.1f%%  %s' % (100 * c / tot, w[:120]))
