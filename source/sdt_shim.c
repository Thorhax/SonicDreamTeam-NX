/* sdt_shim.c -- what the facade (the game's libil2cpp.so) expects of Android
 * and Linux beyond the base's tables. See sdt_port.h.
 * MIT.
 */
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "bp_root.h"
#include "diag.h"
#include "imports.h"
#include "libc_shim.h"
#include "sdt_port.h"
#include "so_util.h"
#include "util.h"

int diag_pthread_tid(void *target_pthread);

/* ------------------------------------------------------------ environment -- */

static char g_apk[512], g_files[512];
const char *sdt_apk_path(void)   { return g_apk; }
const char *sdt_files_path(void) { return g_files; }

static void mkdirs(const char *path) {
  char b[512]; snprintf(b, sizeof b, "%s", path);
  for (char *p = b + 1; *p; p++)
    if (*p == '/') { *p = 0; mkdir(b, 0777); *p = '/'; }
  mkdir(b, 0777);
}

/* What MainActivity.onCreate does with android.system.Os.setenv before the
 * player starts. Paths carry no "sdmc:": the facade and the engine treat them
 * as absolute Unix paths, and the default device resolves them. */
void sdt_env_init(void) {
  const char *root = managed_path(bp_game_root());
  char lib[512], tmp[512];
  snprintf(g_apk, sizeof g_apk, "%s/%s", root, SDT_APK_NAME);
  snprintf(g_files, sizeof g_files, "%s/%s", root, SDT_FILES_DIR);
  snprintf(lib, sizeof lib, "%s/%s", root, SDT_LIB_DIR);
  snprintf(tmp, sizeof tmp, "%s/tmp", root);
  mkdirs(g_files); mkdirs(tmp);
  setenv("COTL_FILES", g_files, 1);
  setenv("COTL_EXTFILES", g_files, 1);
  setenv("COTL_LOGDIR", root, 1);            /* sdt.log, the facade's own log, next to debug.log */
  setenv("COTL_APK", g_apk, 1);
  setenv("COTL_LIBDIR", lib, 1);
  setenv("COTL_MAX_REFRESH", "60", 1);
  setenv("MONO_REFLECTION_SERIALIZER", "yes", 1);
  debugPrintf("[sdt] files %s, apk %s, libs %s\n", g_files, g_apk, lib);
  /* <root>/env.txt: extra KEY=VALUE lines, for the facade's own switches
   * (COTL_TRACE_FILES, COTL_PROBE, COTL_PERF, ...) without a rebuild. */
  char ep[512]; snprintf(ep, sizeof ep, "%s/env.txt", bp_game_root());
  FILE *ef = fopen(ep, "r");
  if (ef) {
    char line[512];
    while (fgets(line, sizeof line, ef)) {
      line[strcspn(line, "\r\n")] = 0;
      char *eq = strchr(line, '=');
      if (!eq || line[0] == '#') continue;
      *eq = 0; setenv(line, eq + 1, 1);
      debugPrintf("[sdt] env.txt: %s=%s\n", line, eq + 1);
    }
    fclose(ef);
  }
}

/* ------------------------------------------------------------------ AAudio -- */
/* The facade feeds FMOD's mix to an AAudio stream with a data callback. SDL's
 * audio device is the same shape. */

typedef int32_t (*aaudio_data_cb)(void *stream, void *user, void *audio, int32_t frames);
typedef struct { int rate, channels, format; aaudio_data_cb cb; void *user; } ABuilder;
typedef struct {
  int rate, channels, format, state, burst;
  aaudio_data_cb cb; void *user;
  SDL_AudioDeviceID dev;
  int tls_set; uint8_t tls[BIONIC_TLS_SIZE];
} AStream;
enum { AA_OPEN = 2, AA_STARTED = 4, AA_PAUSED = 6, AA_STOPPED = 10, AA_CLOSED = 12 };
#define AA_ERR_UNAVAILABLE (-889)

static void aa_sdl_cb(void *ud, Uint8 *out, int len) {
  AStream *s = ud;
  /* SDL's audio thread was not made by pthread_create_fake: give it the bionic
   * block the game's code reads its stack guard from. */
  if (!s->tls_set) { install_bionic_tls(s->tls); s->tls_set = 1; }
  const int bytes = s->channels * (s->format == 2 ? 4 : 2);
  memset(out, 0, (size_t)len);
  if (s->cb && s->state == AA_STARTED) s->cb(s, s->user, out, len / bytes);
}
static int aa_createStreamBuilder(ABuilder **out) {
  ABuilder *b = calloc(1, sizeof *b);
  if (!b) return AA_ERR_UNAVAILABLE;
  b->rate = 48000; b->channels = 2; b->format = 1;
  *out = b; return 0;
}
static void aa_setFormat(ABuilder *b, int f)        { b->format = f; }
static void aa_setChannelCount(ABuilder *b, int c)  { b->channels = c; }
static void aa_setSampleRate(ABuilder *b, int r)    { if (r > 0) b->rate = r; }
static void aa_setIgnored(ABuilder *b, int v)       { (void)b; (void)v; }
static void aa_setDataCallback(ABuilder *b, aaudio_data_cb cb, void *user) { b->cb = cb; b->user = user; }
static void aa_setErrorCallback(ABuilder *b, void *cb, void *user) { (void)b; (void)cb; (void)user; }
static int aa_openStream(ABuilder *b, AStream **out) {
  AStream *s = calloc(1, sizeof *s);
  if (!s) return AA_ERR_UNAVAILABLE;
  s->rate = b->rate; s->channels = b->channels; s->format = b->format; s->cb = b->cb; s->user = b->user;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) { debugPrintf("[aaudio] SDL audio: %s\n", SDL_GetError()); free(s); return AA_ERR_UNAVAILABLE; }
  SDL_AudioSpec want, have; SDL_zero(want);
  want.freq = s->rate; want.channels = (Uint8)s->channels;
  want.format = s->format == 2 ? AUDIO_F32SYS : AUDIO_S16SYS;
  want.samples = 1024; want.callback = aa_sdl_cb; want.userdata = s;
  s->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!s->dev) { debugPrintf("[aaudio] open failed: %s\n", SDL_GetError()); free(s); return AA_ERR_UNAVAILABLE; }
  s->burst = have.samples; s->state = AA_OPEN;
  debugPrintf("[aaudio] stream open: %d Hz, %d ch, %s, %d frames a callback\n", s->rate, s->channels,
              s->format == 2 ? "float" : "s16", s->burst);
  *out = s; return 0;
}
static int aa_builderDelete(ABuilder *b) { free(b); return 0; }
static int aa_requestStart(AStream *s) { s->state = AA_STARTED; SDL_PauseAudioDevice(s->dev, 0); return 0; }
static int aa_requestPause(AStream *s) { s->state = AA_PAUSED;  SDL_PauseAudioDevice(s->dev, 1); return 0; }
static int aa_requestStop(AStream *s)  { s->state = AA_STOPPED; SDL_PauseAudioDevice(s->dev, 1); return 0; }
static int aa_requestFlush(AStream *s) { (void)s; return 0; }
static int aa_close(AStream *s) { if (s->dev) SDL_CloseAudioDevice(s->dev); s->state = AA_CLOSED; return 0; }
static int aa_getSampleRate(AStream *s) { return s->rate; }
static int aa_getFramesPerBurst(AStream *s) { return s->burst; }
static int aa_getState(AStream *s) { return s->state; }
static int aa_waitForStateChange(AStream *s, int from, int *next, int64_t timeout_ns) {
  (void)from; (void)timeout_ns; if (next) *next = s->state; return 0;
}
static const char *aa_resultText(int r) { return r == 0 ? "AAUDIO_OK" : "AAUDIO_ERROR_UNAVAILABLE"; }

/* ------------------------------------------------------------- small libc -- */

static int   x_sigrtmin(void) { return 34; }
static long  x_write_chk(int fd, const void *buf, size_t n, size_t cap) {
  (void)cap; return ((long (*)(int, const void *, size_t))dynlib_find_export("write"))(fd, buf, n);
}
static long  x_pwrite_chk(int fd, const void *buf, size_t n, long off, size_t cap) {
  (void)cap; return ((long (*)(int, const void *, size_t, long))dynlib_find_export("pwrite"))(fd, buf, n, off);
}
static int   x_dirfd(void *d) { (void)d; errno = ENOTSUP; return -1; }
struct x_rlimit { uint64_t cur, max; };
static int   x_getrlimit(int res, struct x_rlimit *r) {
  (void)res; if (r) { r->cur = 8u << 20; r->max = ~0ull; } return 0;
}
struct x_mallinfo { size_t f[10]; };
static struct x_mallinfo x_mallinfo(void) {
  struct x_mallinfo m; memset(&m, 0, sizeof m);
  struct mallinfo n = mallinfo();
  m.f[0] = (size_t)n.arena; m.f[7] = (size_t)n.uordblks; m.f[8] = (size_t)n.fordblks;
  return m;
}
static size_t x_malloc_usable_size(void *p) { return p ? malloc_usable_size(p) : 0; }
static int   x_pthread_gettid_np(pthread_t t) {
  if (pthread_equal(t, pthread_self())) return gettid_fake();
  return diag_pthread_tid((void *)t);
}
static int   x_sched_getcpu(void) { return (int)svcGetCurrentProcessorNumber(); }
static unsigned x_sleep(unsigned s) { svcSleepThread((s64)s * 1000000000ll); return 0; }

/* ----------------------------------------------------------- thread names -- */
/* The facade does its per-frame work (skipping the Apple Arcade splash, saves,
 * menus) only on the thread whose name is "UnityMain", and asks with
 * prctl(PR_GET_NAME). On Android UnityPlayer.java names that thread; here it is
 * the thread that runs the frame loop, the one main() runs on. */
#define TN_MAX 96
static struct { u64 tid; char name[16]; } g_tn[TN_MAX];
static Mutex g_tn_lock;
static u64 g_main_tid;
static u64 cur_tid(void) { u64 t = 0; svcGetThreadId(&t, CUR_THREAD_HANDLE); return t; }
void sdt_main_thread_is_this(void) { g_main_tid = cur_tid(); }
static void tn_set(const char *name) {
  const u64 t = cur_tid();
  mutexLock(&g_tn_lock);
  int slot = -1;
  for (int i = 0; i < TN_MAX; i++) {
    if (g_tn[i].tid == t) { slot = i; break; }
    if (slot < 0 && !g_tn[i].tid) slot = i;
  }
  if (slot >= 0) { g_tn[slot].tid = t; snprintf(g_tn[slot].name, sizeof g_tn[slot].name, "%s", name); }
  mutexUnlock(&g_tn_lock);
}
static void tn_get(char *out) {
  const u64 t = cur_tid();
  const char *n = t == g_main_tid ? "UnityMain" : "Thread";
  mutexLock(&g_tn_lock);
  for (int i = 0; i < TN_MAX; i++) if (g_tn[i].tid == t) { n = g_tn[i].name; break; }
  snprintf(out, 16, "%s", n);
  mutexUnlock(&g_tn_lock);
}
int sdt_prctl(int option, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5) {
  (void)a3; (void)a4; (void)a5;
  if (option == 15 && a2) { if (cur_tid() != g_main_tid) tn_set((const char *)a2); return 0; }   /* PR_SET_NAME */
  if (option == 16 && a2) { tn_get((char *)a2); return 0; }                                      /* PR_GET_NAME */
  return 0;
}
int sdt_pthread_setname_np(pthread_t t, const char *name) {
  if (name && pthread_equal(t, pthread_self()) && cur_tid() != g_main_tid) tn_set(name);
  return 0;
}

DynLibFunction sdt_imports[] = {
  { "AAudio_createStreamBuilder", (uintptr_t)&aa_createStreamBuilder },
  { "AAudioStreamBuilder_setFormat", (uintptr_t)&aa_setFormat },
  { "AAudioStreamBuilder_setChannelCount", (uintptr_t)&aa_setChannelCount },
  { "AAudioStreamBuilder_setSampleRate", (uintptr_t)&aa_setSampleRate },
  { "AAudioStreamBuilder_setPerformanceMode", (uintptr_t)&aa_setIgnored },
  { "AAudioStreamBuilder_setSharingMode", (uintptr_t)&aa_setIgnored },
  { "AAudioStreamBuilder_setDataCallback", (uintptr_t)&aa_setDataCallback },
  { "AAudioStreamBuilder_setErrorCallback", (uintptr_t)&aa_setErrorCallback },
  { "AAudioStreamBuilder_openStream", (uintptr_t)&aa_openStream },
  { "AAudioStreamBuilder_delete", (uintptr_t)&aa_builderDelete },
  { "AAudioStream_requestStart", (uintptr_t)&aa_requestStart },
  { "AAudioStream_requestPause", (uintptr_t)&aa_requestPause },
  { "AAudioStream_requestStop", (uintptr_t)&aa_requestStop },
  { "AAudioStream_requestFlush", (uintptr_t)&aa_requestFlush },
  { "AAudioStream_close", (uintptr_t)&aa_close },
  { "AAudioStream_getSampleRate", (uintptr_t)&aa_getSampleRate },
  { "AAudioStream_getFramesPerBurst", (uintptr_t)&aa_getFramesPerBurst },
  { "AAudioStream_getState", (uintptr_t)&aa_getState },
  { "AAudioStream_waitForStateChange", (uintptr_t)&aa_waitForStateChange },
  { "AAudio_convertResultToText", (uintptr_t)&aa_resultText },
  { "__libc_current_sigrtmin", (uintptr_t)&x_sigrtmin },
  { "__write_chk", (uintptr_t)&x_write_chk },
  { "__pwrite_chk", (uintptr_t)&x_pwrite_chk },
  { "atof", (uintptr_t)&atof },
  { "dirfd", (uintptr_t)&x_dirfd },
  { "funopen", (uintptr_t)&funopen },
  { "getrlimit", (uintptr_t)&x_getrlimit },
  { "mallinfo", (uintptr_t)&x_mallinfo },
  { "malloc_usable_size", (uintptr_t)&x_malloc_usable_size },
  { "pthread_gettid_np", (uintptr_t)&x_pthread_gettid_np },
  { "sched_getcpu", (uintptr_t)&x_sched_getcpu },
  { "sigsetjmp", (uintptr_t)&setjmp },          /* (env, savemask): there is no mask to save */
  { "sleep", (uintptr_t)&x_sleep },
  { "strndup", (uintptr_t)&strndup },
  { "strtok", (uintptr_t)&strtok },
};
size_t sdt_imports_num = sizeof sdt_imports / sizeof sdt_imports[0];

/* --------------------------------------------- thread suspension by signal -- */
/* The iOS runtime stops the world with Mach's thread_suspend / thread_get_state
 * / thread_resume. The facade builds those from a real-time signal: it sends
 * the signal with tgkill, and its handler, on the target thread, copies the
 * registers out of the signal context into the thread's table entry, marks it
 * stopped, and sleeps on a futex until resumed.
 *
 * Horizon has no signals, so the sender's tgkill does the handler's work from
 * outside: pause the thread, read its registers, fill the entry. The matching
 * futex wake resumes it. Table layout (read from the handler's code, guarded
 * below so another build of the facade disables this instead of corrupting):
 *
 *   facade+0x26645c  int  the suspend signal
 *   facade+0x266460  int  entry count
 *   facade+0x266468  entries, 0x138 bytes each:
 *     +0x04 tid   +0x08 pthread_t   +0x10 free flag (0 = in use)
 *     +0x18 stopped (handler sets 1, clears on resume)   +0x1c resume futex
 *     +0x28 x0..x30   +0x120 sp   +0x128 pc   +0x130 0
 */
#define TH_SIG_OFF    0x26645c
#define TH_COUNT_OFF  0x266460
#define TH_TABLE_OFF  0x266468
#define TH_ENTRY      0x138
#define TH_MAX        1024
static const struct { uint32_t off, word; } th_guards[] = {
  { 0x6aec0, 0x5280270a },   /* mov w10, #0x138 : the entry size */
  { 0x6aed0, 0x9baa7d0a },   /* umull x10, w8, w10 */
  { 0x6af98, 0x52800028 },   /* mov w8, #1 : stopped */
  { 0x6afa0, 0x889ffec8 },   /* stlr w8, [x22] */
  { 0x6afac, 0x52800c40 },   /* mov w0, #0x62 : futex */
};
static uintptr_t g_fb;
static size_t    g_fsz;
static int       g_th_ok;

void sdt_threads_arm(uintptr_t base, size_t size) {
  g_fb = base; g_fsz = size; g_th_ok = 1;
  if (TH_TABLE_OFF + (size_t)TH_MAX * TH_ENTRY > size) g_th_ok = 0;
  for (size_t i = 0; g_th_ok && i < sizeof th_guards / sizeof th_guards[0]; i++)
    if (*(volatile uint32_t *)(base + th_guards[i].off) != th_guards[i].word) {
      debugPrintf("[threads] guard %zu failed at facade+0x%x: this is another build of the facade. "
                  "Thread suspension is OFF; the first garbage collection will hang.\n", i, th_guards[i].off);
      g_th_ok = 0;
    }
  if (g_th_ok) debugPrintf("[threads] suspension by signal emulated (table at facade+0x%x)\n", TH_TABLE_OFF);
}

static unsigned g_th_suspends, g_th_resumes, g_th_unpaused;
void sdt_threads_stats(unsigned *s, unsigned *r, unsigned *u) { *s = g_th_suspends; *r = g_th_resumes; *u = g_th_unpaused; }

static int th_suspend(int tid) {
  const int n = *(volatile int *)(g_fb + TH_COUNT_OFF);
  for (int i = 0; i < n && i < TH_MAX; i++) {
    uint8_t *e = (uint8_t *)(g_fb + TH_TABLE_OFF + (size_t)i * TH_ENTRY);
    if (*(volatile int *)(e + 4) != tid || *(volatile int *)(e + 0x10) != 0) continue;
    ThreadContext ctx;
    const int pr = diag_pause_pthread_ctx(*(void **)(e + 8), &ctx);
    g_th_suspends++; if (!pr) g_th_unpaused++;
    if (pr == 1) {
      uint64_t *r = (uint64_t *)(e + 0x28);
      for (int k = 0; k < 29; k++) r[k] = ctx.cpu_gprs[k].x;
      r[29] = ctx.fp; r[30] = ctx.lr;
      *(uint64_t *)(e + 0x120) = ctx.sp;
      *(uint64_t *)(e + 0x128) = ctx.pc.x;
      *(uint64_t *)(e + 0x130) = 0;
    }
    static int told;
    if (!pr && told < 8) { told++; debugPrintf("[threads] tid %d could not be paused (not a thread of ours)\n", tid); }
    /* Acknowledge either way: the sender waits for this. */
    __atomic_store_n((int *)(e + 0x18), 1, __ATOMIC_SEQ_CST);
    return 0;
  }
  return -1;
}

/* The sender sets the entry's futex word and wakes it. */
int sdt_futex_wake_hook(void *uaddr) {
  if (!g_th_ok) return 0;
  const uintptr_t a = (uintptr_t)uaddr, t = g_fb + TH_TABLE_OFF;
  if (a < t || a >= t + (uintptr_t)TH_MAX * TH_ENTRY || (a - t) % TH_ENTRY != 0x1c) return 0;
  uint8_t *e = (uint8_t *)(a - 0x1c);
  if (!__atomic_load_n((int *)(e + 0x18), __ATOMIC_SEQ_CST)) return 0;
  __atomic_store_n((int *)(e + 0x18), 0, __ATOMIC_SEQ_CST);
  diag_resume_pthread(*(void **)(e + 8));
  g_th_resumes++;
  return 1;
}

/* ------------------------------------------------------------- syscall() -- */

#define SYS_TGKILL        131
#define SYS_MEMFD_CREATE  279

long sdt_syscall(long number, long a, long b, long c, long d, long e, long f, int *handled) {
  (void)d; (void)e; (void)f;
  *handled = 1;
  switch (number) {
    case SYS_TGKILL: {
      const int tid = (int)b, sig = (int)c;
      if (g_th_ok && sig == *(volatile int *)(g_fb + TH_SIG_OFF)) {
        if (th_suspend(tid) == 0) return 0;
        errno = ESRCH; return -1;
      }
      static int told;
      if (told < 8) { told++; debugPrintf("[sdt] tgkill(tid %d, signal %d): no signals here, ignored\n", tid, sig); }
      return 0;
    }
    case SYS_MEMFD_CREATE: {
      /* An anonymous file. The facade copies small entries of the APK into one
       * and hands the engine's iOS-side code the descriptor. */
      static int seq;
      char path[512];
      snprintf(path, sizeof path, "%s/tmp/mfd%04d", managed_path(bp_game_root()),
               __atomic_fetch_add(&seq, 1, __ATOMIC_SEQ_CST) & 0x1fff);
      int fd = ((int (*)(const char *, int, int))dynlib_find_export("open"))(path, 01102 /* O_RDWR|O_CREAT|O_TRUNC */, 0666);
      static int told;
      if (told < 4) { told++; debugPrintf("[sdt] memfd_create(%s) -> fd %d (%s)\n", (const char *)a, fd, path); }
      return fd;
    }
  }
  *handled = 0;
  return -1;
}
