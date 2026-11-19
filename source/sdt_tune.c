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

/* frame-loop thread */
void sdt_tune_frame(unsigned frame) {
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
  uint8_t *boxed = il2cpp_runtime_invoke(get, asset, NULL, &exc);
  if (!boxed || exc) return;
  const float now = *(float *)(boxed + 0x10);          /* a boxed value: its data follows the object header */
  if (now > want - 0.01f && now < want + 0.01f) return;
  void *args[1] = { &want };
  il2cpp_runtime_invoke(set, asset, args, &exc);
  if (told++ < 6) debugPrintf("[tune] render scale %.2f -> %.2f (frame %u)%s\n", now, want, frame, exc ? "  (the call threw)" : "");
}
