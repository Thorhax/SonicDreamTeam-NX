/* sdt_apk.c -- read entries out of the game's APK (a zip whose entries are all
 * stored, not deflated).
 *
 * The engine opens the APK itself for its data. What goes through Java on
 * Android -- AssetManager.open("bin/Data/boot.config") and the like -- has no
 * Java here, so those few small files are copied out to <root>/assets/ the
 * first time they are asked for, and the base's loose-file code serves them.
 * MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bp_root.h"
#include "sdt_port.h"
#include "util.h"

typedef struct { char *name; uint32_t local, size; uint16_t method; } Entry;
static Entry *g_e;
static int    g_n = -1;

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void apk_path(char *out, size_t n) { snprintf(out, n, "%s/" SDT_APK_NAME, bp_game_root()); }

static void load_index(void) {
  g_n = 0;
  char path[768]; apk_path(path, sizeof path);
  FILE *f = fopen(path, "rb");
  if (!f) return;
  fseeko(f, 0, SEEK_END);
  const off_t size = ftello(f);
  const size_t tail = size < 0x10016 ? (size_t)size : 0x10016;
  uint8_t *t = malloc(tail);
  fseeko(f, size - (off_t)tail, SEEK_SET);
  if (!t || fread(t, 1, tail, f) != tail) { free(t); fclose(f); return; }
  long eocd = -1;
  for (long i = (long)tail - 22; i >= 0; i--) if (r32(t + i) == 0x06054b50) { eocd = i; break; }
  if (eocd < 0) { debugPrintf("[apk] no end-of-central-directory record in %s\n", path); free(t); fclose(f); return; }
  const int n = r16(t + eocd + 10);
  const uint32_t cdsize = r32(t + eocd + 12), cdoff = r32(t + eocd + 16);
  free(t);
  uint8_t *cd = malloc(cdsize);
  fseeko(f, (off_t)cdoff, SEEK_SET);
  if (!cd || fread(cd, 1, cdsize, f) != cdsize) { free(cd); fclose(f); return; }
  fclose(f);
  g_e = calloc((size_t)n, sizeof *g_e);
  const uint8_t *p = cd, *end = cd + cdsize;
  while (g_e && g_n < n && p + 46 <= end && r32(p) == 0x02014b50) {
    const uint16_t nl = r16(p + 28), xl = r16(p + 30), cl = r16(p + 32);
    Entry *e = &g_e[g_n++];
    e->method = r16(p + 10); e->size = r32(p + 24); e->local = r32(p + 42);
    e->name = malloc((size_t)nl + 1);
    memcpy(e->name, p + 46, nl); e->name[nl] = 0;
    p += 46 + nl + xl + cl;
  }
  free(cd);
  debugPrintf("[apk] %d entries indexed\n", g_n);
}

static const Entry *find(const char *name) {
  if (g_n < 0) load_index();
  for (int i = 0; i < g_n; i++) if (!strcmp(g_e[i].name, name)) return &g_e[i];
  return NULL;
}

static void mkparents(char *path) {
  for (char *p = strchr(path + 6, '/'); p; p = strchr(p + 1, '/')) { *p = 0; mkdir(path, 0777); *p = '/'; }
}

/* Copy assets/<name> out of the APK to dst unless dst exists. 0 = dst is there. */
int sdt_apk_extract_asset(const char *name, const char *dst) {
  struct stat st;
  if (stat(dst, &st) == 0) return 0;
  char ename[512]; snprintf(ename, sizeof ename, "assets/%s", name);
  const Entry *e = find(ename);
  if (!e || e->method != 0) return -1;
  char path[768]; apk_path(path, sizeof path);
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  uint8_t lh[30];
  fseeko(f, (off_t)e->local, SEEK_SET);
  if (fread(lh, 1, 30, f) != 30 || r32(lh) != 0x04034b50) { fclose(f); return -1; }
  fseeko(f, (off_t)e->local + 30 + r16(lh + 26) + r16(lh + 28), SEEK_SET);
  char out[768]; snprintf(out, sizeof out, "%s", dst);
  mkparents(out);
  FILE *o = fopen(out, "wb");
  if (!o) { fclose(f); return -1; }
  static uint8_t buf[1 << 16];
  uint32_t left = e->size; int ok = 1;
  while (left && ok) {
    const size_t n = left < sizeof buf ? left : sizeof buf;
    ok = fread(buf, 1, n, f) == n && fwrite(buf, 1, n, o) == n;
    left -= (uint32_t)n;
  }
  fclose(o); fclose(f);
  debugPrintf("[apk] %s -> %s (%u bytes)%s\n", ename, out, e->size, ok ? "" : "  FAILED");
  if (!ok) remove(out);
  return ok ? 0 : -1;
}
