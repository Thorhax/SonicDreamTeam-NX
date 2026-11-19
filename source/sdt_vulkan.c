/* sdt_vulkan.c -- "libvulkan.so" for the engine: the statically linked NVK.
 *
 * The Android player dlopen()s libvulkan.so and dlsym()s its entry points, and
 * the facade puts itself between the two (it rewrites libunity's dlsym slot and
 * wraps what comes back). So all this has to do is answer dlsym by name, and
 * turn the Android window-system calls into Horizon's:
 *
 *   VK_KHR_android_surface            is reported, and becomes VK_NN_vi_surface
 *   vkCreateAndroidSurfaceKHR(window) becomes vkCreateViSurfaceNN(the NWindow)
 *
 * Everything else is the driver's own entry point, straight through.
 * MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <switch.h>
#include <vulkan/vulkan.h>

#include "sdt_port.h"
#include "util.h"

#define ANDROID_SURFACE "VK_KHR_android_surface"

static VkInstance g_instance;

/* the engine's device and swapchain, for the video overlay further down */
static struct {
  VkDevice dev; uint32_t family;
  VkSwapchainKHR sc; VkImage img[8]; uint32_t nimg; VkExtent2D extent; VkFormat format;
  VkCommandPool pool; VkCommandBuffer cb; VkFence fence;
  VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size;
  int broken;
} V;

static const char *ver(uint32_t v, char *b) {
  snprintf(b, 24, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
  return b;
}

/* The driver's own answer plus the Android surface extension. */
static VkResult VKAPI_CALL b_EnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                                  VkExtensionProperties *props) {
  uint32_t n = 0;
  VkResult r = vkEnumerateInstanceExtensionProperties(layer, &n, NULL);
  if (r != VK_SUCCESS || layer) return vkEnumerateInstanceExtensionProperties(layer, count, props);
  if (!props) { *count = n + 1; return VK_SUCCESS; }
  VkExtensionProperties *all = calloc(n + 1, sizeof *all);
  if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
  vkEnumerateInstanceExtensionProperties(NULL, &n, all);
  strcpy(all[n].extensionName, ANDROID_SURFACE);
  all[n].specVersion = 6;
  n++;
  const uint32_t give = *count < n ? *count : n;
  memcpy(props, all, give * sizeof *all);
  free(all);
  *count = give;
  return give < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static void log_device(VkInstance inst) {
  uint32_t n = 1; VkPhysicalDevice pd = VK_NULL_HANDLE;
  if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { debugPrintf("[vk] no physical device\n"); return; }
  VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
  VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(pd, &f);
  char b[24];
  debugPrintf("[vk] device '%s' api %s vendor %04x; ASTC %d ETC2 %d BC %d; robustBufferAccess %d; "
              "maxImageDimension2D %u; anisotropy %d (max %.0f)\n",
              p.deviceName, ver(p.apiVersion, b), p.vendorID, f.textureCompressionASTC_LDR,
              f.textureCompressionETC2, f.textureCompressionBC, f.robustBufferAccess,
              p.limits.maxImageDimension2D, f.samplerAnisotropy, p.limits.maxSamplerAnisotropy);
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
    debugPrintf("[vk]   heap %u: %llu MB\n", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20));
}

static VkResult VKAPI_CALL b_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                            VkInstance *out) {
  VkInstanceCreateInfo c = *ci;
  const char **ext = calloc(c.enabledExtensionCount + 1, sizeof *ext);
  uint32_t n = 0;
  char b[24];
  debugPrintf("[vk] vkCreateInstance: app '%s' engine '%s' api %s, %u layers, %u extensions\n",
              ci->pApplicationInfo && ci->pApplicationInfo->pApplicationName ? ci->pApplicationInfo->pApplicationName : "?",
              ci->pApplicationInfo && ci->pApplicationInfo->pEngineName ? ci->pApplicationInfo->pEngineName : "?",
              ci->pApplicationInfo ? ver(ci->pApplicationInfo->apiVersion, b) : "?", ci->enabledLayerCount,
              ci->enabledExtensionCount);
  for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
    const char *e = ci->ppEnabledExtensionNames[i];
    debugPrintf("[vk]   instance extension %s\n", e);
    ext[n++] = strcmp(e, ANDROID_SURFACE) ? e : VK_NN_VI_SURFACE_EXTENSION_NAME;
  }
  for (uint32_t i = 0; i < ci->enabledLayerCount; i++)
    debugPrintf("[vk]   layer %s (not available here, dropped)\n", ci->ppEnabledLayerNames[i]);
  c.enabledLayerCount = 0; c.ppEnabledLayerNames = NULL;
  c.enabledExtensionCount = n; c.ppEnabledExtensionNames = ext;
  VkResult r = vkCreateInstance(&c, alloc, out);
  free(ext);
  debugPrintf("[vk] vkCreateInstance -> %d\n", r);
  if (r == VK_SUCCESS) { g_instance = *out; log_device(*out); }
  return r;
}

/* VkAndroidSurfaceCreateInfoKHR: sType, pNext, flags, ANativeWindow *window.
 * The window is the engine's fake one; there is one real window. */
static VkResult VKAPI_CALL b_CreateAndroidSurfaceKHR(VkInstance inst, const void *info,
                                                     const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
  (void)info;
  VkViSurfaceCreateInfoNN sci = { VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, NULL, 0, nwindowGetDefault() };
  PFN_vkCreateViSurfaceNN create = (PFN_vkCreateViSurfaceNN)vkGetInstanceProcAddr(inst, "vkCreateViSurfaceNN");
  VkResult r = create ? create(inst, &sci, alloc, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
  u32 w = 0, h = 0; nwindowGetDimensions(nwindowGetDefault(), &w, &h);
  debugPrintf("[vk] surface on the %ux%u window -> %d\n", w, h, r);
  return r;
}

static VkResult VKAPI_CALL b_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                          const VkAllocationCallbacks *alloc, VkDevice *out) {
  for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
    debugPrintf("[vk]   device extension %s\n", ci->ppEnabledExtensionNames[i]);
  VkResult r = vkCreateDevice(pd, ci, alloc, out);
  if (r == VK_SUCCESS) { memset(&V, 0, sizeof V); V.dev = *out; V.family = ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0; }
  debugPrintf("[vk] vkCreateDevice (%u extensions) -> %d\n", ci->enabledExtensionCount, r);
  return r;
}

/* The window system offers its two 8-bit formats as UNORM only. The game
 * renders in linear colour and wants the sRGB twin, so the twins are listed
 * too, and a swapchain asked for in one is created UNORM with a mutable format:
 * the engine's image views, which carry the sRGB format, then do the encoding. */
static VkFormat srgb_of(VkFormat f) {
  return f == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB :
         f == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_UNDEFINED;
}
static VkFormat unorm_of(VkFormat f) {
  return f == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM :
         f == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_UNDEFINED;
}
static VkResult VKAPI_CALL b_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR surf, uint32_t *count,
                                                                VkSurfaceFormatKHR *out) {
  VkSurfaceFormatKHR real[16], all[32]; uint32_t n = 16, m = 0;
  VkResult r = vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &n, real);
  if (r < 0) return r;
  for (uint32_t i = 0; i < n; i++) all[m++] = real[i];
  for (uint32_t i = 0; i < n; i++) {
    const VkFormat s = srgb_of(real[i].format);
    int have = 0;
    for (uint32_t k = 0; k < n; k++) have |= real[k].format == s;
    if (s != VK_FORMAT_UNDEFINED && !have) { all[m] = real[i]; all[m++].format = s; }
  }
  if (!out) { *count = m; return VK_SUCCESS; }
  const uint32_t give = *count < m ? *count : m;
  memcpy(out, all, give * sizeof *all);
  *count = give;
  return give < m ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL b_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *ci,
                                                const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
  PFN_vkCreateSwapchainKHR real = (PFN_vkCreateSwapchainKHR)vkGetDeviceProcAddr(dev, "vkCreateSwapchainKHR");
  VkSwapchainCreateInfoKHR c = *ci;
  VkFormat list[2];
  VkImageFormatListCreateInfo fl = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, ci->pNext, 2, list };
  const VkFormat un = unorm_of(ci->imageFormat);
  if (un != VK_FORMAT_UNDEFINED) {
    list[0] = un; list[1] = ci->imageFormat;
    c.imageFormat = un;
    c.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
    c.pNext = &fl;
  }
  /* The game asks for IMMEDIATE (it paces itself with a frame limiter), which
   * tears. FIFO waits for the screen's refresh instead. SDT_VSYNC=0 in env.txt
   * keeps what the game asked for. */
  { const char *e = getenv("SDT_VSYNC");
    if (!(e && !strcmp(e, "0")) && c.presentMode != VK_PRESENT_MODE_FIFO_KHR) {
      debugPrintf("[vk] present mode %d -> FIFO (vsync)\n", c.presentMode);
      c.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    } }
  VkResult r = real ? real(dev, &c, alloc, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
  if (r == VK_SUCCESS) {
    V.sc = *out; V.extent = c.imageExtent; V.format = c.imageFormat; V.nimg = 8;
    if (vkGetSwapchainImagesKHR(dev, *out, &V.nimg, V.img) < 0) V.nimg = 0;
  }
  debugPrintf("[vk] swapchain %ux%u format %d%s colorspace %d images>=%u present mode %d transform %x usage %x -> %d\n",
              ci->imageExtent.width, ci->imageExtent.height, ci->imageFormat,
              un != VK_FORMAT_UNDEFINED ? " (created UNORM, mutable)" : "", ci->imageColorSpace,
              ci->minImageCount, ci->presentMode, ci->preTransform, ci->imageUsage, r);
  return r;
}

/* ---- the port's video over the game's picture (sdt_video.c) -----------------
 * When a clip is playing, the frame that is due is copied over the swapchain
 * image the engine is about to present: staging buffer -> image, on the
 * engine's own queue, inside its present call (so the queue is ours to use).
 * The swapchain is RGBA8/BGRA8 UNORM underneath, so the clip's bytes go in as
 * they are. */
const uint8_t *sdt_video_due_frame(int *w, int *h);


static int video_setup(VkPhysicalDevice pd_unused, VkDeviceSize need) {
  (void)pd_unused;
  if (V.broken) return 0;
  if (!V.pool) {
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V.family };
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    if (vkCreateCommandPool(V.dev, &pi, NULL, &V.pool) != VK_SUCCESS) goto fail;
    ai.commandPool = V.pool;
    if (vkAllocateCommandBuffers(V.dev, &ai, &V.cb) != VK_SUCCESS || vkCreateFence(V.dev, &fi, NULL, &V.fence) != VK_SUCCESS) goto fail;
  }
  if (!V.buf || V.size < need) {
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (vkCreateBuffer(V.dev, &bi, NULL, &V.buf) != VK_SUCCESS) goto fail;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, V.buf, &mr);
    /* this driver has two memory types, both host-visible; take the first the buffer allows */
    uint32_t type = 0; while (type < 32 && !(mr.memoryTypeBits & (1u << type))) type++;
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, type };
    if (vkAllocateMemory(V.dev, &mi, NULL, &V.mem) != VK_SUCCESS || vkBindBufferMemory(V.dev, V.buf, V.mem, 0) != VK_SUCCESS ||
        vkMapMemory(V.dev, V.mem, 0, VK_WHOLE_SIZE, 0, &V.map) != VK_SUCCESS) goto fail;
    V.size = need;
  }
  return 1;
fail:
  V.broken = 1;
  debugPrintf("[vk] video overlay: could not make its command buffer or staging buffer -- the clip plays unseen\n");
  return 0;
}

static void video_overlay(VkQueue queue, VkSwapchainKHR sc, uint32_t index) {
  int w = 0, h = 0;
  const uint8_t *px = sdt_video_due_frame(&w, &h);
  if (!px || !V.dev || sc != V.sc || index >= V.nimg) return;
  if ((uint32_t)w != V.extent.width || (uint32_t)h != V.extent.height) return;   /* the clip is made at the screen's size */
  const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
  if (!video_setup(VK_NULL_HANDLE, bytes)) return;
  if (V.format == VK_FORMAT_B8G8R8A8_UNORM || V.format == VK_FORMAT_B8G8R8A8_SRGB) {
    const uint32_t *s = (const uint32_t *)px; uint32_t *d = V.map;
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) { const uint32_t v = s[i]; d[i] = (v & 0xff00ff00u) | ((v & 0xff) << 16) | ((v >> 16) & 0xff); }
  } else memcpy(V.map, px, (size_t)bytes);

  const VkImage img = V.img[index];
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
  VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
  VkBufferImageCopy region = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { (uint32_t)w, (uint32_t)h, 1 } };
  vkResetCommandBuffer(V.cb, 0);
  vkBeginCommandBuffer(V.cb, &bi);
  vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
  vkCmdCopyBufferToImage(V.cb, V.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  vkCmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
  vkEndCommandBuffer(V.cb);
  VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &V.cb, 0, NULL };
  vkResetFences(V.dev, 1, &V.fence);
  if (vkQueueSubmit(queue, 1, &si, V.fence) == VK_SUCCESS) vkWaitForFences(V.dev, 1, &V.fence, VK_TRUE, 200000000ull);
  static int told;
  if (!told) { told = 1; debugPrintf("[vk] video overlay: first frame copied to the %ux%u swapchain image\n", V.extent.width, V.extent.height); }
}

static void pc_maybe_save(void);
static VkResult VKAPI_CALL b_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi) {
  PFN_vkQueuePresentKHR real = (PFN_vkQueuePresentKHR)vkGetDeviceProcAddr(V.dev, "vkQueuePresentKHR");
  if (pi && pi->swapchainCount == 1) video_overlay(queue, pi->pSwapchains[0], pi->pImageIndices[0]);
  pc_maybe_save();
  return real ? real(queue, pi) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

/* ---- the pipeline cache, kept across runs -----------------------------------
 * Compiling a pipeline costs up to ~100 ms on this console, and the game makes
 * a few hundred. The engine would save its VkPipelineCache at onPause, but the
 * port is suspended or closed without one. So the bridge does it: the engine's
 * cache is created from <root>/cache/pipelines.bin when it brings no data of
 * its own, and written back whenever it has grown (checked every 20 s from the
 * present call, written on a thread of its own). The driver checks the file's
 * header itself and ignores a cache from another driver build. */
#include <pthread.h>
#include "bp_root.h"
static struct { VkPipelineCache cache; size_t saved; u64 checked; volatile int writing; void *buf; size_t len; } PC;

static void pc_path(char *out, size_t n, const char *leaf) { snprintf(out, n, "%s/cache/%s", bp_game_root(), leaf); }

static VkResult VKAPI_CALL b_CreatePipelineCache(VkDevice dev, const VkPipelineCacheCreateInfo *ci,
                                                 const VkAllocationCallbacks *alloc, VkPipelineCache *out) {
  VkPipelineCacheCreateInfo c = *ci;
  void *data = NULL; long len = 0;
  if (!PC.cache && ci->initialDataSize == 0) {
    char p[700]; pc_path(p, sizeof p, "pipelines.bin");
    FILE *f = fopen(p, "rb");
    if (f) {
      fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
      data = len > 0 && len < (64l << 20) ? malloc((size_t)len) : NULL;
      if (data && fread(data, 1, (size_t)len, f) == (size_t)len) { c.initialDataSize = (size_t)len; c.pInitialData = data; }
      fclose(f);
    }
  }
  VkResult r = vkCreatePipelineCache(dev, &c, alloc, out);
  if (r != VK_SUCCESS && c.pInitialData != ci->pInitialData) r = vkCreatePipelineCache(dev, ci, alloc, out);
  if (r == VK_SUCCESS && !PC.cache) {
    PC.cache = *out; PC.checked = armGetSystemTick();
    size_t now = 0; vkGetPipelineCacheData(dev, *out, &now, NULL);
    PC.saved = now;
    debugPrintf("[vk] pipeline cache: %ld KB read from the card, %zu KB taken by the driver\n", len >> 10, now >> 10);
  }
  free(data);
  return r;
}

static void *pc_write_main(void *arg) {
  (void)arg;
  char tmp[700], dst[700], dir[700];
  pc_path(dir, sizeof dir, ""); dir[strlen(dir) - 1] = 0; mkdir(dir, 0777);
  pc_path(tmp, sizeof tmp, "pipelines.tmp"); pc_path(dst, sizeof dst, "pipelines.bin");
  FILE *f = fopen(tmp, "wb");
  int ok = 0;
  if (f) { ok = fwrite(PC.buf, 1, PC.len, f) == PC.len; fclose(f); }
  if (ok) { remove(dst); ok = rename(tmp, dst) == 0; }
  debugPrintf("[vk] pipeline cache: %zu KB written%s\n", PC.len >> 10, ok ? "" : "  FAILED");
  free(PC.buf); PC.buf = NULL;
  PC.writing = 0;
  return NULL;
}

/* render thread, from the present call */
static void pc_maybe_save(void) {
  if (!PC.cache || PC.writing) return;
  const u64 t = armGetSystemTick();
  if (armTicksToNs(t - PC.checked) < 20000000000ull) return;
  PC.checked = t;
  size_t len = 0;
  if (vkGetPipelineCacheData(V.dev, PC.cache, &len, NULL) != VK_SUCCESS || len <= PC.saved + 4096) return;
  void *buf = malloc(len);
  if (!buf) return;
  if (vkGetPipelineCacheData(V.dev, PC.cache, &len, buf) < 0) { free(buf); return; }
  PC.buf = buf; PC.len = len; PC.saved = len; PC.writing = 1;
  pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 0x10000);
  if (pthread_create(&th, &at, pc_write_main, NULL) != 0) { free(buf); PC.buf = NULL; PC.writing = 0; }
  else pthread_detach(th);
}

static PFN_vkVoidFunction VKAPI_CALL b_GetInstanceProcAddr(VkInstance inst, const char *name);
static PFN_vkVoidFunction VKAPI_CALL b_GetDeviceProcAddr(VkDevice dev, const char *name);

static const struct { const char *name; void *fn; } g_over[] = {
  { "vkEnumerateInstanceExtensionProperties", b_EnumerateInstanceExtensionProperties },
  { "vkCreateInstance",                       b_CreateInstance },
  { "vkCreateAndroidSurfaceKHR",              b_CreateAndroidSurfaceKHR },
  { "vkCreateDevice",                         b_CreateDevice },
  { "vkCreateSwapchainKHR",                   b_CreateSwapchainKHR },
  { "vkGetPhysicalDeviceSurfaceFormatsKHR",   b_GetPhysicalDeviceSurfaceFormatsKHR },
  { "vkQueuePresentKHR",                      b_QueuePresentKHR },
  { "vkCreatePipelineCache",                  b_CreatePipelineCache },
  { "vkGetInstanceProcAddr",                  b_GetInstanceProcAddr },
  { "vkGetDeviceProcAddr",                    b_GetDeviceProcAddr },
};
static void *override_of(const char *name) {
  for (size_t i = 0; i < sizeof g_over / sizeof g_over[0]; i++)
    if (!strcmp(name, g_over[i].name)) return g_over[i].fn;
  return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL b_GetInstanceProcAddr(VkInstance inst, const char *name) {
  void *o = override_of(name);
  if (o) return (PFN_vkVoidFunction)o;
  PFN_vkVoidFunction p = vkGetInstanceProcAddr(inst, name);
  if (!p && inst) debugPrintf("[vk] vkGetInstanceProcAddr(%s) -> NULL\n", name);
  return p;
}
static PFN_vkVoidFunction VKAPI_CALL b_GetDeviceProcAddr(VkDevice dev, const char *name) {
  if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)b_CreateSwapchainKHR;
  if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)b_QueuePresentKHR;
  if (!strcmp(name, "vkCreatePipelineCache")) return (PFN_vkVoidFunction)b_CreatePipelineCache;
  PFN_vkVoidFunction p = vkGetDeviceProcAddr(dev, name);
  if (!p) debugPrintf("[vk] vkGetDeviceProcAddr(%s) -> NULL\n", name);
  return p;
}

void *sdt_vk_dlsym(const char *name) {
  if (!name || name[0] != 'v' || name[1] != 'k') return NULL;
  void *o = override_of(name);
  if (o) return o;
  /* the table is sorted (generated from a sorted list) */
  size_t lo = 0, hi = sdt_vk_table_n;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = strcmp(name, sdt_vk_table[mid].name);
    if (!c) return sdt_vk_table[mid].fn;
    if (c < 0) hi = mid; else lo = mid + 1;
  }
  static int told;
  if (told < 40) { told++; debugPrintf("[vk] dlsym(%s): the driver has no such entry point\n", name); }
  return NULL;
}

/* ---- the driver's "how much memory is free" ---------------------------------
 * Mesa's Horizon os_get_available_system_memory() calls mallinfo(), which walks
 * every bin of the heap with the heap lock held. The engine asks for the memory
 * budget (VK_EXT_memory_budget) many times a frame, so on the console the render
 * thread spent a quarter of its time in that walk and the main thread a fifth
 * of its time waiting for the lock (measured with SDT_PROFILE). The answer
 * moves slowly, and one walk takes about 5 ms during which nothing else can
 * allocate: ask at most once every 5 seconds. (-Wl,--wrap in the Makefile.) */
#include <stdbool.h>
bool __real_os_get_available_system_memory(uint64_t *size);
bool __wrap_os_get_available_system_memory(uint64_t *size) {
  static uint64_t cached, at;
  static bool ok;
  const uint64_t now = armGetSystemTick();
  if (!at || armTicksToNs(now - at) > 5000000000ull) {
    uint64_t v = 0;
    const bool r = __real_os_get_available_system_memory(&v);
    cached = v; ok = r; at = now ? now : 1;
  }
  if (size) *size = cached;
  return ok;
}
