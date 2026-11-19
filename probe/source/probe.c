/* sdt_probe -- see the Makefile. Writes sdmc:/switch/sdt/probe.log. MIT. */
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>

static FILE *g_log;
static void say(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  if (g_log) { vfprintf(g_log, fmt, ap); fputc('\n', g_log); fflush(g_log); }
  va_end(ap);
}

/* ---------------------------------------------------------------- memory -- */

#define PG 0x1000u
#define CODE_SIZE (16 * PG)
static Handle proc(void) { return envGetOwnProcessHandle(); }

/* "mov w0, #imm ; ret" at every page start, so any page can be called. */
static void fill_code(void *p, size_t size, uint16_t imm) {
  for (size_t off = 0; off < size; off += PG) {
    uint32_t *c = (uint32_t *)((char *)p + off);
    c[0] = 0x52800000u | ((uint32_t)imm << 5);
    c[1] = 0xd65f03c0u;
  }
}
static const char *perm_s(u32 p) {
  static char b[4][4]; static int i; char *s = b[i++ & 3];
  s[0] = p & Perm_R ? 'R' : '-'; s[1] = p & Perm_W ? 'W' : '-'; s[2] = p & Perm_X ? 'X' : '-'; s[3] = 0;
  return s;
}
static void query(const char *what, void *a) {
  MemoryInfo mi; u32 pi;
  Result rc = svcQueryMemory(&mi, &pi, (u64)a);
  say("    %-34s %p: rc=%x type=0x%x perm=%s attr=0x%x size=0x%llx", what, a, rc,
      (unsigned)mi.type, perm_s(mi.perm), (unsigned)mi.attr, (unsigned long long)mi.size);
}
static int call_at(void *p) { armDCacheFlush(p, 8); armICacheInvalidate(p, 8); return ((int (*)(void))p)(); }

static void test_memory(void) {
  say("== memory");
  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  say("  process memory: total %llu MB, used %llu MB; applet type %d; heap override %d",
      (unsigned long long)(total >> 20), (unsigned long long)(used >> 20),
      (int)appletGetAppletType(), (int)envHasHeapOverride());
  say("  own process handle %x; syscall hints: MapProcessCodeMemory(0x77) %d SetProcessMemoryPermission(0x73) %d "
      "CreateCodeMemory(0x4B) %d ControlCodeMemory(0x4C) %d MapPhysicalMemory(0x2C) %d",
      proc(), envIsSyscallHinted(0x77), envIsSyscallHinted(0x73), envIsSyscallHinted(0x4B),
      envIsSyscallHinted(0x4C), envIsSyscallHinted(0x2C));

  u8 *src = memalign(PG, CODE_SIZE);
  virtmemLock();
  u8 *dst = virtmemFindCodeMemory(CODE_SIZE, PG);
  VirtmemReservation *rv = virtmemAddReservation(dst, CODE_SIZE);
  virtmemUnlock();
  (void)rv;
  fill_code(src, CODE_SIZE, 1);
  say("  src %p (heap) dst %p (code region)", src, dst);

  Result rc = svcMapProcessCodeMemory(proc(), (u64)dst, (u64)src, CODE_SIZE);
  say("  T1 map dst<-src: rc=%x", rc);
  if (R_FAILED(rc)) return;
  query("fresh mapping", dst); query("source after map", src);

  rc = svcSetProcessMemoryPermission(proc(), (u64)dst, CODE_SIZE, Perm_Rw);
  say("  T2 fresh -> RW: rc=%x", rc); query("after RW", dst);
  if (R_SUCCEEDED(rc)) { fill_code(dst, CODE_SIZE, 2); say("     wrote code through dst"); }

  rc = svcSetProcessMemoryPermission(proc(), (u64)dst, CODE_SIZE, Perm_Rx);
  say("  T3 RW -> RX directly (whole range): rc=%x%s", rc, R_SUCCEEDED(rc) ? "  ** allowed **" : "  (refused)");
  query("after T3", dst);
  if (R_SUCCEEDED(rc)) say("     call -> %d (2 expected)", call_at(dst));
  /* back to RW for the next test, whichever state T3 left */
  rc = svcSetProcessMemoryPermission(proc(), (u64)dst, CODE_SIZE, Perm_Rw);
  say("  T4 -> RW again: rc=%x", rc);
  if (R_SUCCEEDED(rc)) fill_code(dst, CODE_SIZE, 3);

  /* in-place flip of a middle sub-range by unmap + map + RX */
  u8 *sub = dst + 4 * PG, *ssub = src + 4 * PG; const size_t sublen = 4 * PG;
  rc = svcUnmapProcessCodeMemory(proc(), (u64)sub, (u64)ssub, sublen);
  say("  T5 unmap middle 4 pages: rc=%x", rc);
  if (R_SUCCEEDED(rc)) {
    query("hole", sub); query("source of hole", ssub); query("page before hole", sub - PG);
    say("     source content survived: first word %08x (52800060 expected)", *(uint32_t *)ssub);
    rc = svcMapProcessCodeMemory(proc(), (u64)sub, (u64)ssub, sublen);
    say("  T6 map the hole again: rc=%x", rc);
    rc = svcSetProcessMemoryPermission(proc(), (u64)sub, sublen, Perm_Rx);
    say("  T7 hole fresh -> RX: rc=%x", rc); query("hole now", sub);
    if (R_SUCCEEDED(rc)) say("     call in hole -> %d (3 expected)", call_at(sub));
    dst[0] = 0x11; say("     page outside the hole still writable: ok");
    rc = svcSetProcessMemoryPermission(proc(), (u64)sub, sublen, Perm_Rw);
    say("  T8 RX -> RW in the hole: rc=%x", rc);
    if (R_SUCCEEDED(rc)) {
      fill_code(sub, sublen, 4);
      rc = svcSetProcessMemoryPermission(proc(), (u64)sub, sublen, Perm_Rx);
      say("  T9 RW -> RX again directly: rc=%x", rc);
      if (R_FAILED(rc)) {
        Result r1 = svcUnmapProcessCodeMemory(proc(), (u64)sub, (u64)ssub, sublen);
        Result r2 = svcMapProcessCodeMemory(proc(), (u64)sub, (u64)ssub, sublen);
        Result r3 = svcSetProcessMemoryPermission(proc(), (u64)sub, sublen, Perm_Rx);
        say("  T9b via unmap/map/RX: %x %x %x", r1, r2, r3);
        if (R_SUCCEEDED(r3)) say("     call in hole -> %d (4 expected)", call_at(sub));
      } else say("     call in hole -> %d (4 expected)", call_at(sub));
    }
    rc = svcSetProcessMemoryPermission(proc(), (u64)sub, sublen, Perm_R);
    say("  T10 -> R only: rc=%x", rc);
  }
  /* one page RX in the middle of an RW run, without unmapping (direct) */
  rc = svcSetProcessMemoryPermission(proc(), (u64)dst + 12 * PG, PG, Perm_Rx);
  say("  T11 single RW page -> RX directly: rc=%x", rc);

  /* a 256 MB code mapping in one call, as the game image will need about 100 MB */
  { const size_t big = 256u << 20; u8 *bs = memalign(0x200000, big);
    if (bs) {
      virtmemLock(); u8 *bd = virtmemFindCodeMemory(big, 0x200000); virtmemAddReservation(bd, big); virtmemUnlock();
      u64 t0 = armGetSystemTick();
      rc = svcMapProcessCodeMemory(proc(), (u64)bd, (u64)bs, big);
      Result r2 = R_SUCCEEDED(rc) ? svcSetProcessMemoryPermission(proc(), (u64)bd, big, Perm_Rw) : 0;
      say("  T12 256 MB map: rc=%x, RW rc=%x, %llu us; dst %p", rc, r2,
          (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000), bd);
      if (R_SUCCEEDED(rc)) { if (R_SUCCEEDED(r2)) { bd[0] = 1; bd[big - 1] = 1; }
        svcUnmapProcessCodeMemory(proc(), (u64)bd, (u64)bs, big); }
      free(bs);
    } else say("  T12 no heap for 256 MB");
  }

  Jit j; rc = jitCreate(&j, 0x10000);
  say("  jitCreate: rc=%x type=%d rw=%p rx=%p", rc, R_SUCCEEDED(rc) ? (int)j.type : -1,
      R_SUCCEEDED(rc) ? j.rw_addr : NULL, R_SUCCEEDED(rc) ? j.rx_addr : NULL);
  if (R_SUCCEEDED(rc)) jitClose(&j);

  u64 ro, rw;
  __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(ro));
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(rw));
  say("  tpidrro_el0=%llx tpidr_el0=%llx (libnx TLS %p)", (unsigned long long)ro, (unsigned long long)rw, armGetTls());
  __asm__ volatile("msr tpidr_el0, %0" :: "r"((u64)0x1234567000ull));
  svcSleepThread(20000000);
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(rw));
  say("  tpidr_el0 after write + sleep: %llx (1234567000 expected)", (unsigned long long)rw);
}

/* ---------------------------------------------------------------- vulkan -- */

static const char *vkver(uint32_t v) {
  static char b[32]; snprintf(b, sizeof b, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
  return b;
}
static void bools(const char *name, const VkBool32 *first, int n, const char *const *names) {
  char line[4096]; size_t o = 0; int on = 0;
  for (int i = 0; i < n; i++) if (!first[i]) o += snprintf(line + o, sizeof line - o, " %s", names[i]); else on++;
  say("  %s: %d of %d on; OFF:%s", name, on, n, o ? line : " (none)");
}
static const char *const F10[] = {"robustBufferAccess","fullDrawIndexUint32","imageCubeArray","independentBlend","geometryShader","tessellationShader","sampleRateShading","dualSrcBlend","logicOp","multiDrawIndirect","drawIndirectFirstInstance","depthClamp","depthBiasClamp","fillModeNonSolid","depthBounds","wideLines","largePoints","alphaToOne","multiViewport","samplerAnisotropy","textureCompressionETC2","textureCompressionASTC_LDR","textureCompressionBC","occlusionQueryPrecise","pipelineStatisticsQuery","vertexPipelineStoresAndAtomics","fragmentStoresAndAtomics","shaderTessellationAndGeometryPointSize","shaderImageGatherExtended","shaderStorageImageExtendedFormats","shaderStorageImageMultisample","shaderStorageImageReadWithoutFormat","shaderStorageImageWriteWithoutFormat","shaderUniformBufferArrayDynamicIndexing","shaderSampledImageArrayDynamicIndexing","shaderStorageBufferArrayDynamicIndexing","shaderStorageImageArrayDynamicIndexing","shaderClipDistance","shaderCullDistance","shaderFloat64","shaderInt64","shaderInt16","shaderResourceResidency","shaderResourceMinLod","sparseBinding","sparseResidencyBuffer","sparseResidencyImage2D","sparseResidencyImage3D","sparseResidency2Samples","sparseResidency4Samples","sparseResidency8Samples","sparseResidency16Samples","sparseResidencyAliased","variableMultisampleRate","inheritedQueries"};

static void fmt(VkPhysicalDevice pd, const char *name, VkFormat f) {
  VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(pd, f, &p);
  say("    %-28s linear %05x optimal %05x buffer %05x%s%s%s", name, p.linearTilingFeatures, p.optimalTilingFeatures,
      p.bufferFeatures, p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? " sample" : "",
      p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? " color" : "",
      p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? " depth" : "");
}
#define FMT(x) fmt(pd, #x + 10, x)

static void test_vulkan(void) {
  say("== vulkan");
  uint32_t iv = 0; vkEnumerateInstanceVersion(&iv); say("  instance version %s", vkver(iv));
  uint32_t n = 0; vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
  VkExtensionProperties *ext = calloc(n ? n : 1, sizeof *ext); vkEnumerateInstanceExtensionProperties(NULL, &n, ext);
  say("  %u instance extensions:", n); for (uint32_t i = 0; i < n; i++) say("    %s %u", ext[i].extensionName, ext[i].specVersion);
  free(ext);

  const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME};
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "sdt_probe", 1, "probe", 1, VK_API_VERSION_1_3};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iext};
  VkInstance inst; VkResult r = vkCreateInstance(&ici, NULL, &inst);
  say("  vkCreateInstance(1.3): %d", r);
  if (r) { app.apiVersion = VK_API_VERSION_1_1; r = vkCreateInstance(&ici, NULL, &inst); say("  vkCreateInstance(1.1): %d", r); }
  if (r) return;
  n = 1; VkPhysicalDevice pd; r = vkEnumeratePhysicalDevices(inst, &n, &pd);
  say("  physical devices: %u (%d)", n, r); if (!n) return;

  VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
  say("  device '%s' type %d api %s driver %08x vendor %04x device %04x", p.deviceName, p.deviceType, vkver(p.apiVersion),
      p.driverVersion, p.vendorID, p.deviceID);
  const VkPhysicalDeviceLimits *l = &p.limits;
  say("  limits: maxImageDimension2D %u cube %u 3D %u arrayLayers %u; maxUniformBufferRange %u; maxPushConstants %u;"
      " maxBoundDescriptorSets %u; maxPerStage samplers %u UBO %u SSBO %u sampled %u; maxColorAttachments %u;"
      " maxVertexInputAttributes %u bindings %u; maxSamplerAnisotropy %.0f; minUBOAlign %llu; minTexelBufAlign %llu;"
      " fbColorSamples %x depthSamples %x; maxComputeWorkGroupInvocations %u; timestampPeriod %.2f; maxMemoryAllocationCount %u;"
      " bufferImageGranularity %llu; nonCoherentAtomSize %llu; maxDrawIndexedIndexValue %x; maxClipDistances %u",
      l->maxImageDimension2D, l->maxImageDimensionCube, l->maxImageDimension3D, l->maxImageArrayLayers, l->maxUniformBufferRange,
      l->maxPushConstantsSize, l->maxBoundDescriptorSets, l->maxPerStageDescriptorSamplers, l->maxPerStageDescriptorUniformBuffers,
      l->maxPerStageDescriptorStorageBuffers, l->maxPerStageDescriptorSampledImages, l->maxColorAttachments,
      l->maxVertexInputAttributes, l->maxVertexInputBindings, l->maxSamplerAnisotropy,
      (unsigned long long)l->minUniformBufferOffsetAlignment, (unsigned long long)l->minTexelBufferOffsetAlignment,
      l->framebufferColorSampleCounts, l->framebufferDepthSampleCounts, l->maxComputeWorkGroupInvocations, l->timestampPeriod,
      l->maxMemoryAllocationCount, (unsigned long long)l->bufferImageGranularity, (unsigned long long)l->nonCoherentAtomSize,
      l->maxDrawIndexedIndexValue, l->maxClipDistances);

  VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
  VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
  VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
  vkGetPhysicalDeviceFeatures2(pd, &f2);
  bools("1.0 features", &f2.features.robustBufferAccess, 55, F10);
  { const VkBool32 *b = &f11.storageBuffer16BitAccess; char s[64]; for (int i = 0; i < 12; i++) s[i] = b[i] ? '1' : '0'; s[12] = 0;
    say("  1.1 features (storageBuffer16BitAccess..shaderDrawParameters): %s", s);
    b = &f12.samplerMirrorClampToEdge; char t[64]; for (int i = 0; i < 47; i++) t[i] = b[i] ? '1' : '0'; t[47] = 0;
    say("  1.2 features (samplerMirrorClampToEdge..subgroupBroadcastDynamicId): %s", t);
    b = &f13.robustImageAccess; for (int i = 0; i < 15; i++) s[i] = b[i] ? '1' : '0'; s[15] = 0;
    say("  1.3 features (robustImageAccess..maintenance4): %s", s); }

  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryHeapCount; i++) say("  heap %u: %llu MB flags %x", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20), mp.memoryHeaps[i].flags);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++) say("  memory type %u: heap %u flags %x", i, mp.memoryTypes[i].heapIndex, mp.memoryTypes[i].propertyFlags);

  n = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
  ext = calloc(n ? n : 1, sizeof *ext); vkEnumerateDeviceExtensionProperties(pd, NULL, &n, ext);
  say("  %u device extensions:", n); for (uint32_t i = 0; i < n; i++) say("    %s %u", ext[i].extensionName, ext[i].specVersion);
  free(ext);

  say("  formats:");
  FMT(VK_FORMAT_R8G8B8A8_UNORM); FMT(VK_FORMAT_R8G8B8A8_SRGB); FMT(VK_FORMAT_B8G8R8A8_UNORM); FMT(VK_FORMAT_B8G8R8A8_SRGB);
  FMT(VK_FORMAT_A2B10G10R10_UNORM_PACK32); FMT(VK_FORMAT_B10G11R11_UFLOAT_PACK32); FMT(VK_FORMAT_R16G16B16A16_SFLOAT);
  FMT(VK_FORMAT_R5G6B5_UNORM_PACK16); FMT(VK_FORMAT_R8_UNORM); FMT(VK_FORMAT_R16_SFLOAT); FMT(VK_FORMAT_R32_SFLOAT);
  FMT(VK_FORMAT_D16_UNORM); FMT(VK_FORMAT_D24_UNORM_S8_UINT); FMT(VK_FORMAT_X8_D24_UNORM_PACK32); FMT(VK_FORMAT_D32_SFLOAT);
  FMT(VK_FORMAT_D32_SFLOAT_S8_UINT); FMT(VK_FORMAT_S8_UINT);
  FMT(VK_FORMAT_ASTC_4x4_UNORM_BLOCK); FMT(VK_FORMAT_ASTC_4x4_SRGB_BLOCK); FMT(VK_FORMAT_ASTC_5x5_UNORM_BLOCK);
  FMT(VK_FORMAT_ASTC_6x6_UNORM_BLOCK); FMT(VK_FORMAT_ASTC_6x6_SRGB_BLOCK); FMT(VK_FORMAT_ASTC_8x8_UNORM_BLOCK);
  FMT(VK_FORMAT_ASTC_8x8_SRGB_BLOCK); FMT(VK_FORMAT_ASTC_10x10_UNORM_BLOCK); FMT(VK_FORMAT_ASTC_12x12_UNORM_BLOCK);
  FMT(VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK); FMT(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK); FMT(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK);
  FMT(VK_FORMAT_EAC_R11_UNORM_BLOCK); FMT(VK_FORMAT_BC1_RGBA_UNORM_BLOCK); FMT(VK_FORMAT_BC3_UNORM_BLOCK);
  FMT(VK_FORMAT_BC5_UNORM_BLOCK); FMT(VK_FORMAT_BC6H_UFLOAT_BLOCK); FMT(VK_FORMAT_BC7_UNORM_BLOCK); FMT(VK_FORMAT_BC7_SRGB_BLOCK);

  VkViSurfaceCreateInfoNN sci = {VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, NULL, 0, nwindowGetDefault()};
  PFN_vkCreateViSurfaceNN create = (PFN_vkCreateViSurfaceNN)vkGetInstanceProcAddr(inst, "vkCreateViSurfaceNN");
  VkSurfaceKHR surf = VK_NULL_HANDLE; r = create ? create(inst, &sci, NULL, &surf) : -999;
  say("  vkCreateViSurfaceNN: %d", r);
  if (!r) {
    VkSurfaceCapabilitiesKHR c; r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &c);
    say("  surface caps (%d): images %u..%u, extent %ux%u (min %ux%u max %ux%u), transforms %x, alpha %x, usage %x", r,
        c.minImageCount, c.maxImageCount, c.currentExtent.width, c.currentExtent.height, c.minImageExtent.width,
        c.minImageExtent.height, c.maxImageExtent.width, c.maxImageExtent.height, c.supportedTransforms,
        c.supportedCompositeAlpha, c.supportedUsageFlags);
    VkSurfaceFormatKHR sf[32]; n = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &n, sf);
    for (uint32_t i = 0; i < n; i++) say("    surface format %d colorspace %d", sf[i].format, sf[i].colorSpace);
    VkPresentModeKHR pm[8]; n = 8; vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &n, pm);
    for (uint32_t i = 0; i < n; i++) say("    present mode %d", pm[i]);
    vkDestroySurfaceKHR(inst, surf, NULL);
  }
  /* the device itself, with what the game's port asks for */
  float prio = 1; uint32_t qn = 8; VkQueueFamilyProperties qf[8]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
  for (uint32_t i = 0; i < qn; i++) say("  queue family %u: flags %x count %u", i, qf[i].queueFlags, qf[i].queueCount);
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
  VkPhysicalDeviceFeatures want = {0}; want.robustBufferAccess = f2.features.robustBufferAccess;
  const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, dext, &want};
  VkDevice dev; r = vkCreateDevice(pd, &dci, NULL, &dev);
  say("  vkCreateDevice (swapchain, robustBufferAccess %d): %d", want.robustBufferAccess, r);
  if (!r) vkDestroyDevice(dev, NULL);
  vkDestroyInstance(inst, NULL);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  mkdir("sdmc:/switch/sdt", 0777);
  g_log = fopen("sdmc:/switch/sdt/probe.log", "w");
  say("sdt_probe, built " __DATE__ " " __TIME__);
  test_memory();
  test_vulkan();
  say("== done");
  if (g_log) fclose(g_log);
  return 0;
}
