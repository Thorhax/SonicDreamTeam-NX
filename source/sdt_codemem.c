/* sdt_codemem.c -- writable memory that can become executable in place.
 *
 * The facade maps the iOS image like a Linux loader: mmap(RW), copy and fix up
 * the segments, then mprotect(__TEXT, R|X). It does the same for its own
 * trampoline pages. On Horizon:
 *   - only code-memory mappings (svcMapProcessCodeMemory) can be executable;
 *   - svcSetProcessMemoryPermission moves a fresh mapping to RW or to RX, and
 *     RX to RW, but not RW back to RX (the so-loader lineage's finding; the
 *     probe NRO checks it on the console).
 * So the arena is heap pages mapped as code memory and made RW. A request for
 * PROT_EXEC unmaps those pages and maps them again -- same address, same
 * physical pages, so same contents -- which leaves them fresh, and fresh pages
 * accept RX. Nothing may touch the range during that window: the facade does
 * this at load, before the image runs.
 * MIT.
 */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "sdt_port.h"
#include "error.h"
#include "util.h"

#define PG        0x1000ul
#define CM_ALIGN  0x10000ul          /* the image's segments are 16 KB aligned; 64 KB keeps allocations apart */
#define BPROT_READ  1
#define BPROT_WRITE 2
#define BPROT_EXEC  4

enum { ST_RW = 0, ST_RX = 1 };

static uint8_t *g_src;               /* heap backing (inaccessible while mapped) */
static uint8_t *g_dst;               /* the arena as the game sees it */
static size_t   g_size, g_used;
static uint8_t *g_state;             /* one byte a page */
static int      g_phase;
static Mutex    g_lock;

int sdt_cm_init(size_t bytes) {
  bytes = (bytes + 0x1fffff) & ~0x1ffffful;
  extern void *__real_memalign(size_t, size_t);
  g_src = __real_memalign(0x200000, bytes);
  if (!g_src) { debugPrintf("[codemem] no heap for a %zu MB arena\n", bytes >> 20); return -1; }
  memset(g_src, 0, bytes);
  virtmemLock();
  g_dst = virtmemFindCodeMemory(bytes, 0x200000);
  if (g_dst) virtmemAddReservation(g_dst, bytes);
  virtmemUnlock();
  if (!g_dst) { debugPrintf("[codemem] no address space for the arena\n"); return -1; }
  Result rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)g_dst, (u64)g_src, bytes);
  if (R_FAILED(rc)) { debugPrintf("[codemem] svcMapProcessCodeMemory failed: %08x\n", rc); return -1; }
  rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), (u64)g_dst, bytes, Perm_Rw);
  if (R_FAILED(rc)) { debugPrintf("[codemem] arena -> RW failed: %08x\n", rc); return -1; }
  g_state = calloc(bytes / PG, 1);
  g_size = bytes; g_used = 0;
  debugPrintf("[codemem] arena %zu MB at %p (backing %p)\n", bytes >> 20, g_dst, g_src);
  return 0;
}

void sdt_cm_phase(int on) { g_phase = on; }
int  sdt_cm_phase_on(void) { return g_phase && g_dst; }

int sdt_cm_owns(const void *addr) {
  return g_dst && (const uint8_t *)addr >= g_dst && (const uint8_t *)addr < g_dst + g_size;
}
int sdt_cm_range(uintptr_t *lo, uintptr_t *hi) {
  if (!g_dst) return 0;
  *lo = (uintptr_t)g_dst; *hi = (uintptr_t)g_dst + g_used;
  return 1;
}

void *sdt_cm_alloc(size_t len) {
  len = (len + CM_ALIGN - 1) & ~(CM_ALIGN - 1);
  mutexLock(&g_lock);
  void *p = NULL;
  if (g_dst && g_used + len <= g_size) { p = g_dst + g_used; g_used += len; }
  mutexUnlock(&g_lock);
  debugPrintf("[codemem] alloc %zu KB -> %p (%zu of %zu MB used)\n", len >> 10, p, g_used >> 20, g_size >> 20);
  return p;                           /* never handed out twice, so still zero */
}

/* One run of pages in the same state, to `want`. */
static int flip(uint8_t *a, size_t len, int want) {
  const Handle h = envGetOwnProcessHandle();
  Result rc;
  if (want == ST_RX) {
    uint8_t *s = g_src + (a - g_dst);
    rc = svcUnmapProcessCodeMemory(h, (u64)a, (u64)s, len);
    if (R_FAILED(rc)) { debugPrintf("[codemem] unmap %p+%zx failed: %08x\n", a, len, rc); return -1; }
    rc = svcMapProcessCodeMemory(h, (u64)a, (u64)s, len);
    if (R_FAILED(rc)) fatal_error("codemem: cannot map %p+%zx back (%08x)", a, len, rc);
    rc = svcSetProcessMemoryPermission(h, (u64)a, len, Perm_Rx);
    if (R_FAILED(rc)) {                 /* keep the pages usable, report */
      svcSetProcessMemoryPermission(h, (u64)a, len, Perm_Rw);
      debugPrintf("[codemem] %p+%zx -> RX failed: %08x\n", a, len, rc);
      return -1;
    }
    armICacheInvalidate(a, len);
  } else {
    rc = svcSetProcessMemoryPermission(h, (u64)a, len, Perm_Rw);
    if (R_FAILED(rc)) { debugPrintf("[codemem] %p+%zx -> RW failed: %08x\n", a, len, rc); return -1; }
  }
  return 0;
}

int sdt_cm_protect(void *addr, size_t len, int prot) {
  uint8_t *a = (uint8_t *)((uintptr_t)addr & ~(PG - 1));
  uint8_t *e = (uint8_t *)(((uintptr_t)addr + len + PG - 1) & ~(PG - 1));
  if (e > g_dst + g_size) e = g_dst + g_size;
  /* PROT_READ alone and PROT_NONE stay RW: read-only data needs no enforcing,
   * and a page once RX can only go back through RW anyway. */
  const int want = (prot & BPROT_EXEC) ? ST_RX : ST_RW;
  if (!(prot & BPROT_EXEC) && !(prot & BPROT_WRITE)) {
    /* ...unless it is executable now and is being made plain data */
    int any_rx = 0;
    for (uint8_t *p = a; p < e; p += PG) any_rx |= g_state[(p - g_dst) / PG] == ST_RX;
    if (!any_rx) return 0;
  }
  int rc = 0;
  mutexLock(&g_lock);
  size_t first = (size_t)(a - g_dst) / PG, last = (size_t)(e - g_dst) / PG, pg = first;
  while (pg < last) {
    if (g_state[pg] == want) { pg++; continue; }
    size_t run = pg;
    while (run < last && g_state[run] != want) run++;
    if (flip(g_dst + pg * PG, (run - pg) * PG, want) == 0) memset(g_state + pg, want, run - pg);
    else rc = -1;
    pg = run;
  }
  mutexUnlock(&g_lock);
  if (len >= (1u << 20) || rc)
    debugPrintf("[codemem] protect %p+%zu KB prot=%d -> %s%s\n", addr, len >> 10, prot,
                want == ST_RX ? "RX" : "RW", rc ? "  FAILED" : "");
  return rc;
}
