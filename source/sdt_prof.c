/* sdt_prof.c -- a sampling profiler for the two threads that decide the frame
 * rate: UnityMain (game logic, the frame loop) and UnityGfxDeviceWorker (the
 * engine's render thread, which makes the Vulkan calls).
 *
 * <root>/env.txt SDT_PROFILE=<hz> (e.g. 400) starts it. Every tick each thread
 * is stopped for an instant and its pc, lr and two callers are recorded;
 * samples are appended to <root>/prof.bin every few seconds, and <root>/prof.map
 * says where each module was loaded. tools/prof.py turns the two into a table.
 * MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bp_root.h"
#include "sdt_port.h"
#include "so_util.h"
#include "util.h"

int diag_sample_named(const char *name, uint64_t out[4]);
extern so_module main_mod, unity_mod, il2cpp_mod;

typedef struct { uint32_t thread, pad; uint64_t a[4]; } Sample;
#define CAP 8192
static Sample g_buf[CAP];
static int    g_n, g_hz;
static Thread g_thread;
static unsigned g_miss[2];
static char   g_path[600];

void sdt_prof_anchor(void) {}                      /* its address gives the NRO's load slide */

static void flush(void) {
  if (!g_n) {                                     /* nothing sampled: say why in the map */
    static int told; char map[600];
    if (told++) return;
    snprintf(map, sizeof map, "%s/prof.map", bp_game_root());
    FILE *m = fopen(map, "a");
    if (m) { fprintf(m, "no samples after the first interval: misses %u %u\n", g_miss[0], g_miss[1]); fclose(m); }
    return;
  }
  FILE *f = fopen(g_path, "ab");
  if (f) { fwrite(g_buf, sizeof g_buf[0], (size_t)g_n, f); fclose(f); }
  g_n = 0;
}

static void prof_main(void *arg) {
  (void)arg;
  static const char *const NAMES[2] = { "UnityMain", "UnityGfxDeviceWorker" };
  const u64 period = 1000000000ull / (u64)g_hz;
  u64 last_flush = armGetSystemTick();
  for (;;) {
    svcSleepThread((s64)period);
    for (uint32_t t = 0; t < 2; t++) {
      if (g_n >= CAP) break;
      Sample *s = &g_buf[g_n];
      if (diag_sample_named(NAMES[t], s->a)) { s->thread = t; s->pad = 0; g_n++; } else g_miss[t]++;
    }
    if (g_n >= CAP - 4 || armTicksToNs(armGetSystemTick() - last_flush) > 4000000000ull) { flush(); last_flush = armGetSystemTick(); }
  }
}

/* after the modules are loaded and the facade has mapped the image */
void sdt_prof_start(void) {
  const char *e = getenv("SDT_PROFILE");
  g_hz = e ? atoi(e) : 0;
  if (g_hz < 10 || g_hz > 2000) return;
  char map[600];
  snprintf(g_path, sizeof g_path, "%s/prof.bin", bp_game_root());
  snprintf(map, sizeof map, "%s/prof.map", bp_game_root());
  remove(g_path);
  FILE *f = fopen(map, "w");
  if (f) {
    uintptr_t lo = 0, hi = 0; sdt_cm_range(&lo, &hi);
    fprintf(f, "hz %d\n", g_hz);
    fprintf(f, "nro_anchor %llx\n", (unsigned long long)(uintptr_t)&sdt_prof_anchor);
    fprintf(f, "libmain %llx %llx\n", (unsigned long long)(uintptr_t)main_mod.load_virtbase, (unsigned long long)main_mod.load_size);
    fprintf(f, "libunity %llx %llx\n", (unsigned long long)(uintptr_t)unity_mod.load_virtbase, (unsigned long long)unity_mod.load_size);
    fprintf(f, "facade %llx %llx\n", (unsigned long long)(uintptr_t)il2cpp_mod.load_virtbase, (unsigned long long)il2cpp_mod.load_size);
    fprintf(f, "codemem %llx %llx\n", (unsigned long long)lo, (unsigned long long)(hi - lo));
    fclose(f);
  }
  /* above the engine's threads, so a busy core cannot starve the sampler; on core 2 */
  static const int PRIOS[] = { 0x20, 0x28, 0x2B, 0x2C };   /* the highest the process is allowed */
  Result rc = 1; int used = 0;
  for (size_t i = 0; i < sizeof PRIOS / sizeof PRIOS[0] && R_FAILED(rc); i++) {
    rc = threadCreate(&g_thread, prof_main, NULL, NULL, 0x8000, PRIOS[i], 2);
    used = PRIOS[i];
  }
  if (R_SUCCEEDED(rc)) rc = threadStart(&g_thread);
  f = fopen(map, "a");
  if (f) { fprintf(f, "sampler rc %x prio %x\n", rc, used); fclose(f); }
  debugPrintf("[prof] sampling UnityMain and UnityGfxDeviceWorker at %d Hz -> %s (rc %x, prio %x)\n", g_hz, g_path, rc, used);
}

/* SDT_AUDIO_TRACE: where the audio threads are, written when the queue stalls. */
void sdt_stall_dump(FILE *f) {
  static const char *const NAMES[] = { "sdl-audio", "FMOD mixer thread", "FMOD stream thread", "FMOD nonblocking thread (0)" };
  uintptr_t lo = 0, hi = 0; sdt_cm_range(&lo, &hi);
  fprintf(f, "  bases: nro_anchor %llx libunity %llx facade %llx codemem %llx\n",
          (unsigned long long)(uintptr_t)&sdt_prof_anchor, (unsigned long long)(uintptr_t)unity_mod.load_virtbase,
          (unsigned long long)(uintptr_t)il2cpp_mod.load_virtbase, (unsigned long long)lo);
  for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
    uint64_t s[4] = { 0, 0, 0, 0 };
    const int ok = diag_sample_named(NAMES[i], s);
    fprintf(f, "  %-28s %s pc %llx lr %llx callers %llx %llx\n", NAMES[i], ok ? "ok" : "NOT SAMPLED",
            (unsigned long long)s[0], (unsigned long long)s[1], (unsigned long long)s[2], (unsigned long long)s[3]);
  }
}
