/* sdt_tune.c -- settings the port imposes on the game from outside.
 *
 * Render scale: the game draws its 3D view at half the screen's resolution
 * whatever the device class (640x360 on the Switch's 720p), which is what its
 * phone profiles want for 1170p+ screens. The port sets the render pipeline
 * asset's renderScale to 1.0 instead (<root>/env.txt SDT_RENDER_SCALE = 0.25 ..
 * 1.0 for another value, 0 for the game's own), through
 * the il2cpp API the facade exports, and puts it back when the game changes it.
 * MIT.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "so_util.h"
#include "util.h"

#define API(ret, name, ...) ret (*name)(__VA_ARGS__) = (ret (*)(__VA_ARGS__))so_resolve_external(#name)

static void *class_named(const char *ns, const char *name) {
  API(void *, il2cpp_domain_get, void);
  API(void **, il2cpp_domain_get_assemblies, void *, size_t *);
  API(void *, il2cpp_assembly_get_image, void *);
  API(void *, il2cpp_class_from_name, void *, const char *, const char *);
  if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image || !il2cpp_class_from_name) return NULL;
  size_t n = 0;
  void **as = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
  for (size_t i = 0; as && i < n; i++) {
    void *k = il2cpp_class_from_name(il2cpp_assembly_get_image(as[i]), ns, name);
    if (k) return k;
  }
  return NULL;
}

/* ---- experiment switches (env.txt), frame-loop thread ---------------------
 * SDT_FIXED_STEP=<seconds>   Time.fixedDeltaTime: the physics and FixedUpdate step (the game's is 1/60)
 * SDT_MAX_DELTA=<seconds>    Time.maximumDeltaTime: the longest frame the game clock follows; at the
 *                            fixed step it means one physics step a frame at most, and slow motion below it
 * SDT_LOD_BIAS=<factor>      QualitySettings.lodBias (below 1: lower-detail models nearer the camera)
 * SDT_URP=<name>=<value>;... properties of the render pipeline asset, e.g.
 *                            maxAdditionalLightsCount=0;supportsMainLightShadows=0;msaaSampleCount=1
 * Each is put back when the game changes it. */
static void tune_static_float(const char *cls, const char *prop, float want, unsigned frame) {
  API(void *, il2cpp_class_get_method_from_name, void *, const char *, int);
  API(void *, il2cpp_runtime_invoke, void *, void *, void **, void **);
  static int told;
  char g[64], st[64]; snprintf(g, sizeof g, "get_%s", prop); snprintf(st, sizeof st, "set_%s", prop);
  void *k = class_named("UnityEngine", cls);
  void *get = k ? il2cpp_class_get_method_from_name(k, g, 0) : NULL;
  void *set = k ? il2cpp_class_get_method_from_name(k, st, 1) : NULL;
  if (!set) { if (told++ < 4) debugPrintf("[tune] %s.%s has no setter in this game\n", cls, prop); return; }
  void *exc = NULL; float cur = -1;
  if (get) { uint8_t *b = il2cpp_runtime_invoke(get, NULL, NULL, &exc); if (b && !exc) cur = *(float *)(b + 0x10); }
  if (get && cur > want * 0.999f && cur < want * 1.001f) return;
  if (!get && frame > 600) return;                     /* nothing to compare with: set it for the first seconds only */
  void *a[1] = { &want }; exc = NULL;
  il2cpp_runtime_invoke(set, NULL, a, &exc);
  if (told++ < 12) debugPrintf("[tune] %s.%s %.5f -> %.5f (frame %u)%s\n", cls, prop, cur, want, frame, exc ? "  (the call threw)" : "");
}

static void tune_urp(const char *spec, unsigned frame) {
  API(void *, il2cpp_class_get_method_from_name, void *, const char *, int);
  API(void *, il2cpp_runtime_invoke, void *, void *, void **, void **);
  API(void *, il2cpp_object_get_class, void *);
  API(void *, il2cpp_method_get_param, void *, uint32_t);
  API(int, il2cpp_type_get_type, void *);
  static int told;
  void *gs = class_named("UnityEngine.Rendering", "GraphicsSettings");
  void *get_rp = gs ? il2cpp_class_get_method_from_name(gs, "get_currentRenderPipeline", 0) : NULL;
  void *exc = NULL;
  void *asset = get_rp ? il2cpp_runtime_invoke(get_rp, NULL, NULL, &exc) : NULL;
  if (!asset || exc || !il2cpp_method_get_param || !il2cpp_type_get_type) return;
  void *k = il2cpp_object_get_class(asset);
  for (const char *p = spec; *p; ) {
    const size_t len = strcspn(p, ";");
    const char *eq = memchr(p, '=', len);
    if (eq && eq - p < 48) {
      char g[64], st[64];
      snprintf(g, sizeof g, "get_%.*s", (int)(eq - p), p); snprintf(st, sizeof st, "set_%.*s", (int)(eq - p), p);
      void *get = il2cpp_class_get_method_from_name(k, g, 0), *set = il2cpp_class_get_method_from_name(k, st, 1);
      if (!set) { if (told++ < 8) debugPrintf("[tune] pipeline asset: no %s\n", st); }
      else {
        const int ty = il2cpp_type_get_type(il2cpp_method_get_param(set, 0));   /* 2 bool, 0xc float, else a 32-bit integer or enum */
        union { float f; int32_t i; uint8_t b; } v = { .i = 0 }, cur = { .i = 0x7fffffff };
        if (ty == 0xc) v.f = (float)atof(eq + 1); else if (ty == 2) v.b = atoi(eq + 1) != 0; else v.i = atoi(eq + 1);
        exc = NULL;
        uint8_t *b = get ? il2cpp_runtime_invoke(get, asset, NULL, &exc) : NULL;
        if (b && !exc) { cur.i = 0; memcpy(&cur, b + 0x10, ty == 2 ? 1 : 4); }
        if (cur.i != v.i && (get || frame < 600)) {
          void *a[1] = { &v }; exc = NULL;
          il2cpp_runtime_invoke(set, asset, a, &exc);
          if (told++ < 24) debugPrintf("[tune] pipeline asset %s (type %#x): %d -> %d (frame %u)%s\n", st, ty, cur.i, v.i, frame, exc ? "  (the call threw)" : "");
        }
      }
    }
    p += len; if (*p == ';') p++;
  }
}

static void tune_experiments(unsigned frame) {
  static int init; static float fixed_step, max_delta, lod_bias; static const char *urp;
  if (!init) {
    const char *e;
    init = 1;
    if ((e = getenv("SDT_FIXED_STEP")) != NULL) fixed_step = (float)atof(e);
    if ((e = getenv("SDT_MAX_DELTA")) != NULL) max_delta = (float)atof(e);
    if ((e = getenv("SDT_LOD_BIAS")) != NULL) lod_bias = (float)atof(e);
    urp = getenv("SDT_URP");
  }
  { /* SDT_TARGET_FPS=<n>: Application.targetFrameRate (the game asks for 60). With SDT_VSYNC=0 a higher
     * value shows how fast a scene could run; it is a measuring aid, not a play setting. */
    static int target = -1, told;
    if (target < 0) { const char *e = getenv("SDT_TARGET_FPS"); target = e ? atoi(e) : 0; }
    if (target >= 20 && target <= 240) {
      API(void *, il2cpp_class_get_method_from_name, void *, const char *, int);
      API(void *, il2cpp_runtime_invoke, void *, void *, void **, void **);
      void *k = class_named("UnityEngine", "Application");
      void *get = k ? il2cpp_class_get_method_from_name(k, "get_targetFrameRate", 0) : NULL;
      void *set = k ? il2cpp_class_get_method_from_name(k, "set_targetFrameRate", 1) : NULL;
      void *exc = NULL;
      uint8_t *b = get && set ? il2cpp_runtime_invoke(get, NULL, NULL, &exc) : NULL;
      if (b && !exc && *(int *)(b + 0x10) != target) {
        void *a[1] = { &target };
        il2cpp_runtime_invoke(set, NULL, a, &exc);
        if (told++ < 8) debugPrintf("[tune] targetFrameRate %d -> %d (frame %u)\n", *(int *)(b + 0x10), target, frame);
      }
    } }
  if (fixed_step > 0.004f && fixed_step < 0.1f) tune_static_float("Time", "fixedDeltaTime", fixed_step, frame);
  if (max_delta > 0.004f && max_delta < 1.0f) tune_static_float("Time", "maximumDeltaTime", max_delta, frame);
  if (lod_bias > 0.01f) {
    /* the managed setter is stripped from this game: call the engine's own */
    API(void *, il2cpp_resolve_icall, const char *);
    static void (*set)(float); static float (*get)(void); static int told;
    if (!set && il2cpp_resolve_icall && !told) {
      set = (void (*)(float))il2cpp_resolve_icall("UnityEngine.QualitySettings::set_lodBias");
      get = (float (*)(void))il2cpp_resolve_icall("UnityEngine.QualitySettings::get_lodBias");
      debugPrintf("[tune] lodBias: engine setter %p getter %p\n", (void *)set, (void *)get);
      told = 1;
    }
    if (set && get) {
      const float cur = get();
      if (cur < lod_bias * 0.999f || cur > lod_bias * 1.001f) {
        set(lod_bias);
        if (told++ < 8) debugPrintf("[tune] lodBias %.2f -> %.2f (frame %u)\n", cur, lod_bias, frame);
      }
    }
  }
  if (urp && *urp) tune_urp(urp, frame);
}

/* SDT_FPS=1: every 300 frames one line into <root>/fps.txt -- the rate, the
 * longest frame, and how many frames took more than one refresh (18.5 ms) and
 * more than two (34 ms). Works in the release build, which writes no log. */
#include <switch.h>
const char *bp_game_root(void);
void sdt_fps_frame(void) {
  static int on = -1; static u64 t_prev, t_win; static unsigned n, late, late2; static double worst;
  if (on < 0) { const char *e = getenv("SDT_FPS"); on = e && atoi(e) > 0; }
  if (!on) return;
  const u64 t = armGetSystemTick();
  if (t_prev) {
    const double ms = (double)armTicksToNs(t - t_prev) / 1e6;
    if (ms > worst) worst = ms;
    if (ms > 18.5) late++;
    if (ms > 34.0) late2++;
    if (++n >= 300) {
      char path[600]; snprintf(path, sizeof path, "%s/fps.txt", bp_game_root());
      FILE *f = fopen(path, "a");
      if (f) {
        fprintf(f, "%.1f fps over %u frames; longest %.0f ms; %u over 18.5 ms, %u over 34 ms\n",
                n / ((double)armTicksToNs(t - t_win) / 1e9), n, worst, late, late2);
        fclose(f);
      }
      n = late = late2 = 0; worst = 0; t_win = t;
    }
  } else t_win = t;
  t_prev = t;
}

/* frame-loop thread */
void sdt_tune_frame(unsigned frame) {
  if (frame % 30 == 7 && frame > 60) tune_experiments(frame);
  static float want = -1;
  static int told;
  if (want < 0) {
    const char *e = getenv("SDT_RENDER_SCALE");
    want = e ? (float)atof(e) : 1.0f;                /* default: the full 720p */
    if (want && (want < 0.25f || want > 1.0f)) want = 1.0f;   /* "0" leaves the game's own scale */
    if (want) debugPrintf("[tune] render scale wanted: %.2f\n", want);
  }
  if (!want || frame % 30 != 15) return;

  API(void *, il2cpp_class_get_method_from_name, void *, const char *, int);
  API(void *, il2cpp_runtime_invoke, void *, void *, void **, void **);
  API(void *, il2cpp_object_get_class, void *);
  if (!il2cpp_class_get_method_from_name || !il2cpp_runtime_invoke || !il2cpp_object_get_class) return;
  static void *get_rp;
  if (!get_rp) {
    void *gs = class_named("UnityEngine.Rendering", "GraphicsSettings");
    if (gs) get_rp = il2cpp_class_get_method_from_name(gs, "get_currentRenderPipeline", 0);
    if (!get_rp) { if (told++ < 2) debugPrintf("[tune] no GraphicsSettings.currentRenderPipeline\n"); return; }
  }
  void *exc = NULL;
  void *asset = il2cpp_runtime_invoke(get_rp, NULL, NULL, &exc);
  if (!asset || exc) return;
  void *k = il2cpp_object_get_class(asset);
  void *get = il2cpp_class_get_method_from_name(k, "get_renderScale", 0);
  void *set = il2cpp_class_get_method_from_name(k, "set_renderScale", 1);
  if (!get || !set) { if (told++ < 2) debugPrintf("[tune] the pipeline asset has no renderScale\n"); return; }
  /* SDT_SHADOW_DISTANCE=<metres>: how far real-time shadows are drawn (0 = none).
   * Every shadow caster is one more draw the CPU has to prepare. Not set = the game's own. */
  { static float sd = -2; static int sd_told;
    if (sd < -1) { const char *e = getenv("SDT_SHADOW_DISTANCE"); sd = e ? (float)atof(e) : -1; }
    if (sd >= 0) {
      void *sget = il2cpp_class_get_method_from_name(k, "get_shadowDistance", 0);
      void *sset = il2cpp_class_get_method_from_name(k, "set_shadowDistance", 1);
      uint8_t *b = sget ? il2cpp_runtime_invoke(sget, asset, NULL, &exc) : NULL;
      if (b && !exc && sset) {
        const float cur = *(float *)(b + 0x10);
        if (cur > sd + 0.01f || cur < sd - 0.01f) {
          void *a[1] = { &sd };
          il2cpp_runtime_invoke(sset, asset, a, &exc);
          if (sd_told++ < 6) debugPrintf("[tune] shadow distance %.1f -> %.1f (frame %u)%s\n", cur, sd, frame, exc ? "  (the call threw)" : "");
        }
      } else if (sd_told++ < 2) debugPrintf("[tune] the pipeline asset has no shadowDistance\n");
      exc = NULL;
    } }
  uint8_t *boxed = il2cpp_runtime_invoke(get, asset, NULL, &exc);
  if (!boxed || exc) return;
  const float now = *(float *)(boxed + 0x10);          /* a boxed value: its data follows the object header */
  if (now > want - 0.01f && now < want + 0.01f) return;
  void *args[1] = { &want };
  il2cpp_runtime_invoke(set, asset, args, &exc);
  if (told++ < 6) debugPrintf("[tune] render scale %.2f -> %.2f (frame %u)%s\n", now, want, frame, exc ? "  (the call threw)" : "");
}

/* Diagnostics for SDT_AUDIO_TRACE (frame-loop thread): what the engine says
 * about its own listener. */
void sdt_audio_probe(char *out, size_t cap) {
  API(void *, il2cpp_class_get_method_from_name, void *, const char *, int);
  API(void *, il2cpp_runtime_invoke, void *, void *, void **, void **);
  out[0] = 0;
  if (!il2cpp_class_get_method_from_name || !il2cpp_runtime_invoke) return;
  static void *gv, *gp;
  if (!gv) {
    void *k = class_named("UnityEngine", "AudioListener");
    if (!k) return;
    gv = il2cpp_class_get_method_from_name(k, "get_volume", 0);
    gp = il2cpp_class_get_method_from_name(k, "get_pause", 0);
  }
  void *exc = NULL;
  uint8_t *v = gv ? il2cpp_runtime_invoke(gv, NULL, NULL, &exc) : NULL;
  uint8_t *p = gp && !exc ? il2cpp_runtime_invoke(gp, NULL, NULL, &exc) : NULL;
  if (v && p && !exc) snprintf(out, cap, "listener volume=%.2f pause=%d", *(float *)(v + 0x10), (int)*(uint8_t *)(p + 0x10));
}

/* SDT_PROFILE diagnostics (frame-loop thread): the engine's timing and quality
 * settings, which decide how much work one frame holds. */
static float static_float(const char *ns, const char *cls, const char *getter) {
  API(void *, il2cpp_class_get_method_from_name, void *, const char *, int);
  API(void *, il2cpp_runtime_invoke, void *, void *, void **, void **);
  if (!il2cpp_class_get_method_from_name || !il2cpp_runtime_invoke) return -1;
  void *k = class_named(ns, cls);
  void *m = k ? il2cpp_class_get_method_from_name(k, getter, 0) : NULL;
  if (!m) return -2;
  void *exc = NULL;
  uint8_t *boxed = il2cpp_runtime_invoke(m, NULL, NULL, &exc);
  if (!boxed || exc) return -3;
  return *(float *)(boxed + 0x10);
}
static int static_int(const char *ns, const char *cls, const char *getter) {
  API(void *, il2cpp_class_get_method_from_name, void *, const char *, int);
  API(void *, il2cpp_runtime_invoke, void *, void *, void **, void **);
  if (!il2cpp_class_get_method_from_name || !il2cpp_runtime_invoke) return -1;
  void *k = class_named(ns, cls);
  void *m = k ? il2cpp_class_get_method_from_name(k, getter, 0) : NULL;
  if (!m) return -2;
  void *exc = NULL;
  uint8_t *boxed = il2cpp_runtime_invoke(m, NULL, NULL, &exc);
  if (!boxed || exc) return -3;
  return *(int *)(boxed + 0x10);
}
void sdt_tune_probe(unsigned frame) {
  static int on = -1;
  if (on < 0) on = getenv("SDT_PROFILE") != NULL;
  if (!on || frame % 600 != 300) return;
  debugPrintf("[probe] frame %u: fixedDeltaTime %.5f maximumDeltaTime %.3f timeScale %.2f deltaTime %.4f targetFrameRate %d vSync %d\n", frame,
              static_float("UnityEngine", "Time", "get_fixedDeltaTime"), static_float("UnityEngine", "Time", "get_maximumDeltaTime"),
              static_float("UnityEngine", "Time", "get_timeScale"), static_float("UnityEngine", "Time", "get_deltaTime"),
              static_int("UnityEngine", "Application", "get_targetFrameRate"), static_int("UnityEngine", "QualitySettings", "get_vSyncCount"));
  debugPrintf("[probe] quality level %d shadows %d shadowDistance %.0f skinWeights %d lodBias %.2f maxLod %d aa %d | physics autoSim %d solverIter %d\n",
              static_int("UnityEngine", "QualitySettings", "GetQualityLevel"), static_int("UnityEngine", "QualitySettings", "get_shadows"),
              static_float("UnityEngine", "QualitySettings", "get_shadowDistance"), static_int("UnityEngine", "QualitySettings", "get_skinWeights"),
              static_float("UnityEngine", "QualitySettings", "get_lodBias"), static_int("UnityEngine", "QualitySettings", "get_maximumLODLevel"),
              static_int("UnityEngine", "QualitySettings", "get_antiAliasing"),
              static_int("UnityEngine", "Physics", "get_autoSimulation"), static_int("UnityEngine", "Physics", "get_defaultSolverIterations"));
}

/* SDT_PROFILE: the address of every managed method, so tools/prof.py can name
 * samples that fall in the game image. Written once to <root>/methods.txt
 * (frame-loop thread; takes a few seconds). */
#include "sdt_port.h"
const char *bp_game_root(void);
void sdt_tune_dump_methods(unsigned frame) {
  static int state = -1;
  if (state < 0) state = getenv("SDT_PROFILE") != NULL;
  if (state != 1 || frame < 1500) return;
  state = 2;
  API(void *, il2cpp_domain_get, void);
  API(void **, il2cpp_domain_get_assemblies, void *, size_t *);
  API(void *, il2cpp_assembly_get_image, void *);
  API(size_t, il2cpp_image_get_class_count, void *);
  API(void *, il2cpp_image_get_class, void *, size_t);
  API(const char *, il2cpp_image_get_name, void *);
  API(void *, il2cpp_class_get_methods, void *, void **);
  API(const char *, il2cpp_method_get_name, void *);
  API(const char *, il2cpp_class_get_name, void *);
  API(const char *, il2cpp_class_get_namespace, void *);
  if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image || !il2cpp_image_get_class_count ||
      !il2cpp_image_get_class || !il2cpp_class_get_methods || !il2cpp_method_get_name || !il2cpp_class_get_name) return;
  uintptr_t lo = 0, hi = 0; sdt_cm_range(&lo, &hi);
  char path[600]; snprintf(path, sizeof path, "%s/methods.txt", bp_game_root());
  FILE *f = fopen(path, "w");
  if (!f) return;
  static char buf[1 << 16]; setvbuf(f, buf, _IOFBF, sizeof buf);
  size_t n = 0, written = 0;
  void **as = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
  for (size_t i = 0; as && i < n; i++) {
    void *img = il2cpp_assembly_get_image(as[i]);
    const size_t nc = il2cpp_image_get_class_count(img);
    for (size_t c = 0; c < nc; c++) {
      void *k = il2cpp_image_get_class(img, c);
      if (!k) continue;
      void *it = NULL, *m;
      while ((m = il2cpp_class_get_methods(k, &it)) != NULL) {
        const uintptr_t p = *(uintptr_t *)m;               /* MethodInfo.methodPointer */
        if (p < lo || p >= hi) continue;
        const char *ns = il2cpp_class_get_namespace ? il2cpp_class_get_namespace(k) : "";
        fprintf(f, "%llx %s%s%s::%s\n", (unsigned long long)(p - lo), ns ? ns : "", ns && ns[0] ? "." : "",
                il2cpp_class_get_name(k), il2cpp_method_get_name(m));
        written++;
      }
    }
  }
  fclose(f);
  debugPrintf("[prof] %zu managed methods written to %s\n", written, path);
}
