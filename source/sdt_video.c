/* sdt_video.c -- the game's videos (the intro), played by the port.
 *
 * The engine's Android VideoPlayer decodes with MediaCodec (libmediandk),
 * which does not exist here, and this build of the game does not even carry
 * the shader the engine would draw the decoded frames with. So the engine's
 * player is left to fail, and the clip is played beside it:
 *
 *   1. The engine opens a clip by handing an AMediaDataSource (its own read
 *      callbacks over the VideoClip inside the asset bundle) to
 *      AMediaExtractor_setDataSourceCustom. That call reads the whole clip
 *      through those callbacks into memory, then refuses the open as before.
 *   2. A decode thread demuxes and decodes it with ffmpeg: video to RGBA at the
 *      screen's size, audio to the OpenSL shim's movie ring (opensles.c).
 *   3. sdt_vulkan.c's vkQueuePresentKHR copies the frame that is due over the
 *      swapchain image, so the clip covers whatever the game drew.
 *   4. When the clip ends, or A / B / + is pressed, the game's own
 *      VideoPlayerWrapper.SkipVideo() is called (through the il2cpp API the
 *      facade exports) and the game goes on.
 * MIT.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "opensles.h"
#include "sdt_port.h"
#include "so_util.h"
#include "util.h"

#define OUT_W 1280
#define OUT_H 720
#define NQ    4                      /* decoded frames waiting to be shown */

/* ---------------------------------------------------- AMediaDataSource -- */

typedef long (*ds_read_fn)(void *ud, int64_t off, void *buf, size_t size);
typedef long (*ds_size_fn)(void *ud);
typedef struct { void *ud; ds_read_fn read; ds_size_fn size; void (*close)(void *); } DataSource;

void *sdt_ams_new(void) { return calloc(1, sizeof(DataSource)); }
void  sdt_ams_delete(void *ds) { free(ds); }
void  sdt_ams_set_userdata(DataSource *ds, void *ud) { if (ds) ds->ud = ud; }
void  sdt_ams_set_read_at(DataSource *ds, ds_read_fn f) { if (ds) ds->read = f; }
void  sdt_ams_set_get_size(DataSource *ds, ds_size_fn f) { if (ds) ds->size = f; }
void  sdt_ams_set_close(DataSource *ds, void (*f)(void *)) { if (ds) ds->close = f; }

/* -------------------------------------------------------------- player -- */

enum { ST_IDLE = 0, ST_PLAYING, ST_ENDED };
static volatile int g_state;
static volatile int g_stop;
static uint8_t *g_clip; static size_t g_clip_size;
static pthread_t g_thread;

typedef struct { uint8_t *rgba; int64_t pts_us; } Frame;
static Frame  g_q[NQ];
static int    g_q_head, g_q_count;
static Mutex  g_q_lock;
static uint8_t *g_shown;             /* the frame last handed to the presenter */
static u64    g_t0;                  /* tick of pts 0; 0 until the first frame is shown */
static unsigned g_shown_n, g_dropped_n, g_decoded_n;

static int64_t clock_us(void) { return g_t0 ? (int64_t)(armTicksToNs(armGetSystemTick() - g_t0) / 1000) : 0; }

typedef struct { const uint8_t *p; int64_t size, pos; } Mem;
static int mem_read(void *o, uint8_t *buf, int n) {
  Mem *m = o; const int64_t left = m->size - m->pos;
  if (left <= 0) return AVERROR_EOF;
  if (n > left) n = (int)left;
  memcpy(buf, m->p + m->pos, (size_t)n); m->pos += n;
  return n;
}
static int64_t mem_seek(void *o, int64_t off, int whence) {
  Mem *m = o;
  if (whence == AVSEEK_SIZE) return m->size;
  if (whence == SEEK_END) off += m->size; else if (whence == SEEK_CUR) off += m->pos;
  if (off < 0) off = 0;
  if (off > m->size) off = m->size;
  m->pos = off; return off;
}

static void *decode_main(void *arg) {
  (void)arg;
  /* off core 0, where the game's main thread lives */
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 0x6);
  Mem mem = { g_clip, (int64_t)g_clip_size, 0 };
  AVFormatContext *fmt = avformat_alloc_context();
  uint8_t *iobuf = av_malloc(1 << 16);
  AVIOContext *io = avio_alloc_context(iobuf, 1 << 16, 0, &mem, mem_read, NULL, mem_seek);
  AVCodecContext *vc = NULL, *ac = NULL; struct SwsContext *sws = NULL; SwrContext *swr = NULL;
  AVFrame *fr = av_frame_alloc(); AVPacket *pkt = av_packet_alloc();
  int vi = -1, ai = -1, dev_rate = 0;
  int16_t *pcm = NULL; int pcm_cap = 0;
  fmt->pb = io;
  if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
    debugPrintf("[video] the clip is not something ffmpeg can open\n"); goto done;
  }
  vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
  if (vi < 0) { debugPrintf("[video] no video stream\n"); goto done; }
  { const AVCodec *c = avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
    vc = c ? avcodec_alloc_context3(c) : NULL;
    if (!vc || avcodec_parameters_to_context(vc, fmt->streams[vi]->codecpar) < 0) goto done;
    vc->thread_count = 1;
    vc->skip_loop_filter = AVDISCARD_NONREF;      /* a little sharpness for a lot of time */
    vc->flags2 |= AV_CODEC_FLAG2_FAST;
    if (avcodec_open2(vc, c, NULL) < 0) { debugPrintf("[video] no decoder for %s\n", c->name); goto done; }
    debugPrintf("[video] %s %dx%d, %.1f s\n", c->name, vc->width, vc->height, (double)fmt->duration / AV_TIME_BASE); }
  if (ai >= 0) {
    const AVCodec *c = avcodec_find_decoder(fmt->streams[ai]->codecpar->codec_id);
    ac = c ? avcodec_alloc_context3(c) : NULL;
    if (ac && avcodec_parameters_to_context(ac, fmt->streams[ai]->codecpar) >= 0 && avcodec_open2(ac, c, NULL) >= 0) {
      dev_rate = opensles_movie_begin(ac->sample_rate);
      AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
      if (dev_rate > 0 && swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, dev_rate, &ac->ch_layout,
                                              ac->sample_fmt, ac->sample_rate, 0, NULL) >= 0 && swr_init(swr) >= 0)
        debugPrintf("[video] audio %s %d Hz -> %d Hz\n", c->name, ac->sample_rate, dev_rate);
      else { debugPrintf("[video] no audio sink (rate %d): the clip plays silent\n", dev_rate); swr_free(&swr); }
    } else { avcodec_free_context(&ac); ai = -1; }
  }
  for (int i = 0; i < NQ; i++) if (!g_q[i].rgba) g_q[i].rgba = malloc((size_t)OUT_W * OUT_H * 4);
  if (!g_shown) g_shown = malloc((size_t)OUT_W * OUT_H * 4);

  /* letterbox into OUT_W x OUT_H */
  int dw = OUT_W, dh = vc->height * OUT_W / (vc->width ? vc->width : 1);
  if (dh > OUT_H) { dh = OUT_H; dw = vc->width * OUT_H / (vc->height ? vc->height : 1); }
  dw &= ~1; dh &= ~1;
  const int ox = (OUT_W - dw) / 2, oy = (OUT_H - dh) / 2;
  const AVRational vtb = fmt->streams[vi]->time_base;

  while (!g_stop && av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == vi) {
      if (avcodec_send_packet(vc, pkt) >= 0)
        while (!g_stop && avcodec_receive_frame(vc, fr) >= 0) {
          g_decoded_n++;
          const int64_t pts = av_rescale_q(fr->best_effort_timestamp, vtb, (AVRational){1, 1000000});
          if (g_t0 && pts + 60000 < clock_us()) { g_dropped_n++; continue; }      /* late: not worth converting */
          if (!sws || fr->format != vc->pix_fmt)
            sws = sws_getCachedContext(sws, fr->width, fr->height, fr->format, dw, dh, AV_PIX_FMT_RGBA,
                                       SWS_FAST_BILINEAR, NULL, NULL, NULL);
          for (;;) {                                   /* room in the queue */
            mutexLock(&g_q_lock); const int full = g_q_count == NQ; mutexUnlock(&g_q_lock);
            if (!full || g_stop) break;
            svcSleepThread(2000000);
          }
          if (g_stop) break;
          Frame *f = &g_q[(g_q_head + g_q_count) % NQ];   /* only this thread appends */
          if (dw != OUT_W || dh != OUT_H) memset(f->rgba, 0, (size_t)OUT_W * OUT_H * 4);
          uint8_t *dst[4] = { f->rgba + ((size_t)oy * OUT_W + ox) * 4, NULL, NULL, NULL };
          int dls[4] = { OUT_W * 4, 0, 0, 0 };
          sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, dls);
          f->pts_us = pts;
          mutexLock(&g_q_lock); g_q_count++; mutexUnlock(&g_q_lock);
        }
    } else if (pkt->stream_index == ai && swr) {
      if (avcodec_send_packet(ac, pkt) >= 0)
        while (avcodec_receive_frame(ac, fr) >= 0) {
          const int cap = (int)av_rescale_rnd(fr->nb_samples, dev_rate, ac->sample_rate, AV_ROUND_UP) + 64;
          if (cap > pcm_cap) { free(pcm); pcm = malloc((size_t)cap * 4); pcm_cap = cap; }
          uint8_t *out[1] = { (uint8_t *)pcm };
          const int n = swr_convert(swr, out, cap, (const uint8_t **)fr->extended_data, fr->nb_samples);
          while (!g_stop && opensles_movie_buffered_frames() > 48000) svcSleepThread(2000000);   /* stay about a second ahead */
          if (n > 0) opensles_movie_queue(pcm, n);
        }
    }
    av_packet_unref(pkt);
  }
  /* let the queued frames play out */
  for (int i = 0; i < 400 && !g_stop; i++) {
    mutexLock(&g_q_lock); const int left = g_q_count; mutexUnlock(&g_q_lock);
    if (!left) break;
    svcSleepThread(10000000);
  }
done:
  debugPrintf("[video] clip over: %u frames decoded, %u shown, %u dropped as late%s\n", g_decoded_n, g_shown_n,
              g_dropped_n, g_stop ? " (skipped)" : "");
  if (swr || dev_rate > 0) opensles_movie_end();
  free(pcm);
  sws_freeContext(sws); swr_free(&swr);
  avcodec_free_context(&vc); avcodec_free_context(&ac);
  av_frame_free(&fr); av_packet_free(&pkt);
  if (fmt) avformat_close_input(&fmt);
  if (io) { av_freep(&io->buffer); avio_context_free(&io); }
  free(g_clip); g_clip = NULL; g_clip_size = 0;
  g_state = ST_ENDED;
  return NULL;
}

/* AMediaExtractor_setDataSourceCustom: take the clip, refuse the open. */
void sdt_video_open(DataSource *ds) {
  if (!ds || !ds->read || !ds->size) { debugPrintf("[video] an open with no data source callbacks\n"); g_state = ST_ENDED; return; }
  if (g_state == ST_PLAYING) return;
  const long size = ds->size(ds->ud);
  if (size <= 0 || size > (400l << 20)) { debugPrintf("[video] clip size %ld: not played\n", size); g_state = ST_ENDED; return; }
  uint8_t *buf = malloc((size_t)size);
  if (!buf) { debugPrintf("[video] no memory for a %ld MB clip\n", size >> 20); g_state = ST_ENDED; return; }
  const u64 t = armGetSystemTick();
  long got = 0;
  while (got < size) {
    const long want = size - got < (1 << 20) ? size - got : (1 << 20);
    const long n = ds->read(ds->ud, got, buf + got, (size_t)want);
    if (n <= 0) break;
    got += n;
  }
  debugPrintf("[video] clip of %ld MB read in %llu ms%s\n", size >> 20,
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t) / 1000000), got < size ? "  (SHORT)" : "");
  if (got < size) { free(buf); g_state = ST_ENDED; return; }
  g_clip = buf; g_clip_size = (size_t)size;
  g_stop = 0; g_t0 = 0; g_q_head = g_q_count = 0; g_shown_n = g_dropped_n = g_decoded_n = 0;
  g_state = ST_PLAYING;
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1 << 20);
  if (pthread_create(&g_thread, &at, decode_main, NULL) != 0) { free(buf); g_clip = NULL; g_state = ST_ENDED; }
  else pthread_detach(g_thread);
}

/* The frame that is due, for the presenter (render thread). NULL: draw nothing. */
const uint8_t *sdt_video_due_frame(int *w, int *h) {
  if (g_state != ST_PLAYING) return NULL;
  *w = OUT_W; *h = OUT_H;
  mutexLock(&g_q_lock);
  if (!g_t0 && g_q_count) {                       /* the clock starts with the first frame on screen */
    g_t0 = armGetSystemTick() - armNsToTicks((u64)g_q[g_q_head].pts_us * 1000);
    opensles_movie_set_paused(0);
  }
  const int64_t now = clock_us();
  int took = 0;
  while (g_q_count && g_q[g_q_head].pts_us <= now) {
    if (took) g_dropped_n++;                      /* more than one became due: show the newest */
    uint8_t *t = g_shown; g_shown = g_q[g_q_head].rgba; g_q[g_q_head].rgba = t;
    g_q_head = (g_q_head + 1) % NQ; g_q_count--; took = 1;
  }
  if (took) g_shown_n++;
  mutexUnlock(&g_q_lock);
  return g_shown_n ? g_shown : NULL;
}

/* ------------------------------------------------ telling the game it ended -- */

#define API(ret, name, ...) ret (*name)(__VA_ARGS__) = (ret (*)(__VA_ARGS__))so_resolve_external(#name)

static void *find_class(const char *ns, const char *name) {
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

static void finish_videos(void) {
  API(void *, il2cpp_class_get_method_from_name, void *, const char *, int);
  API(void *, il2cpp_runtime_invoke, void *, void *, void **, void **);
  API(void *, il2cpp_class_get_type, void *);
  API(void *, il2cpp_type_get_object, void *);
  if (!il2cpp_class_get_method_from_name || !il2cpp_runtime_invoke || !il2cpp_type_get_object) { debugPrintf("[video] no il2cpp API\n"); return; }
  void *wrap = find_class("HardlightProject", "VideoPlayerWrapper");
  void *uobj = find_class("UnityEngine", "Object");
  void *find = uobj ? il2cpp_class_get_method_from_name(uobj, "FindObjectsOfType", 1) : NULL;
  void *skip = wrap ? il2cpp_class_get_method_from_name(wrap, "SkipVideo", 0) : NULL;
  if (!find || !skip) { debugPrintf("[video] VideoPlayerWrapper.SkipVideo not found (%p %p %p)\n", wrap, find, skip); return; }
  void *exc = NULL, *args[1] = { il2cpp_type_get_object(il2cpp_class_get_type(wrap)) };
  uint8_t *arr = il2cpp_runtime_invoke(find, NULL, args, &exc);
  if (!arr || exc) { debugPrintf("[video] FindObjectsOfType failed\n"); return; }
  const uint64_t count = *(uint64_t *)(arr + 0x18);        /* Il2CppArray: max_length, then the items */
  void **items = (void **)(arr + 0x20);
  for (uint64_t i = 0; i < count && i < 8; i++) { exc = NULL; il2cpp_runtime_invoke(skip, items[i], NULL, &exc); }
  debugPrintf("[video] %llu VideoPlayerWrapper(s) told the clip is over\n", (unsigned long long)count);
}

/* frame-loop thread, once a frame */
void sdt_video_frame(void) {
  static PadState pad; static int pad_ready; static int settle;
  if (g_state == ST_PLAYING) {
    if (!pad_ready) { padInitializeDefault(&pad); pad_ready = 1; }
    padUpdate(&pad);
    if (g_shown_n > 24 && (padGetButtonsDown(&pad) & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus))) g_stop = 1;
  } else if (g_state == ST_ENDED) {
    /* a few frames, so the engine has finished reporting its failed open */
    if (++settle >= 20) { settle = 0; g_state = ST_IDLE; finish_videos(); }
  }
}
