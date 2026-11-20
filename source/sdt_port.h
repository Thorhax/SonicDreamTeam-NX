/* sdt_port.h -- what is specific to Sonic Dream Team ("com.sdt.port").
 *
 * The game's libil2cpp.so is not IL2CPP: it is a layer ("the facade") that loads
 * the iOS game image (libUnityFramework.so, a Mach-O), answers its Darwin calls
 * and exports il2cpp_* to the stock Android libunity.so. It was written for
 * Android/Linux, so it leans on kernel behaviour Horizon does not have. Each
 * module here stands in for one such behaviour:
 *
 *   sdt_codemem.c  mmap + mprotect(PROT_EXEC) in place, for the image it maps
 *   sdt_shim.c     its environment variables, missing libc/NDK imports, AAudio,
 *                  memfd_create, and thread suspension by signal (tgkill)
 *   sdt_vulkan.c   "libvulkan.so": the statically linked NVK driver
 * MIT.
 */
#ifndef SDT_PORT_H
#define SDT_PORT_H

#include <stddef.h>
#include <stdint.h>

/* ---- layout on the SD card, under the game root ------------------------- */
#define SDT_APK_NAME   "base.apk"                 /* the unmodified APK; the engine reads assets from it */
#define SDT_LIB_DIR    "lib"                      /* the four files of the APK's lib/arm64-v8a/ */
#define SDT_FILES_DIR  "com.sdt.port/files"       /* saves; the facade looks for "/com.sdt.port/files/" in paths */
#define SDT_IMAGE_NAME "libUnityFramework.so"

/* ---- sdt_codemem.c ------------------------------------------------------- */
/* Memory the facade can write and later make executable at the same address.
 * Horizon only executes code-memory mappings and never turns a written page
 * executable in place, so the arena is one code mapping of heap pages, made
 * writable, whose pages are remapped (same address, same contents) when the
 * facade asks for PROT_EXEC. */
int   sdt_cm_init(size_t bytes);                  /* 0 on success */
void  sdt_cm_phase(int on);                       /* anonymous mmaps go to the arena while on */
int   sdt_cm_phase_on(void);
void *sdt_cm_alloc(size_t len);                   /* zeroed, 64 KB aligned; NULL when full */
int   sdt_cm_owns(const void *addr);
int   sdt_cm_protect(void *addr, size_t len, int prot);   /* bionic PROT_* */
int   sdt_cm_range(uintptr_t *lo, uintptr_t *hi); /* the arena, for backtraces */

/* ---- sdt_shim.c ---------------------------------------------------------- */
void  sdt_env_init(void);                         /* the COTL_* variables MainActivity.java sets */
const char *sdt_apk_path(void);                   /* "/switch/sdt/base.apk" */
const char *sdt_files_path(void);
long  sdt_syscall(long number, long a, long b, long c, long d, long e, long f, int *handled);
int   sdt_futex_wake_hook(void *uaddr);           /* 1 = a suspended thread was resumed */
void  sdt_threads_arm(uintptr_t facade_base, size_t facade_size);
void  sdt_main_thread_is_this(void);              /* the frame-loop thread: "UnityMain" to prctl(PR_GET_NAME) */
int   sdt_prctl(int option, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5);

/* ---- sdt_output_hold.c --------------------------------------------------- */
int   sdt_output_held(void);                      /* the intro has the screen, the sound and the pads */
void  sdt_output_wait(void);                      /* returns when it has given them back */

/* ---- sdt_apk.c ----------------------------------------------------------- */
int   sdt_apk_extract_asset(const char *name, const char *dst);   /* assets/<name> of the APK -> dst, once */

/* ---- sdt_vulkan.c -------------------------------------------------------- */
typedef struct { const char *name; void *fn; } SdtVkEntry;
extern const SdtVkEntry sdt_vk_table[];
extern const size_t     sdt_vk_table_n;
void *sdt_vk_dlsym(const char *name);             /* NULL when not a Vulkan entry point */

#endif
