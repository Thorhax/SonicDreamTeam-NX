/* sdt_audio_out.c -- the port's sound output: one stereo 16-bit voice on the
 * system's audio renderer, fed by the mixer in opensles.c.
 *
 * It replaces SDL's audio device here. SDL's Switch driver, after handing a
 * buffer to the renderer, waits until that buffer is in the "playing" state.
 * If its thread does not get to run for longer than one buffer lasts (21 ms:
 * a level load does that), the buffer has gone from queued through playing to
 * done unseen, and the wait never ends: sound stops for good while the game
 * runs on. This loop asks a different question -- "is a buffer free?" -- whose
 * answer cannot be missed, and after a stall it simply carries on.
 * (`SDT_AUDIO=sdl` in env.txt selects SDL's device again.)
 * MIT.
 */
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "diag.h"
#include "sdt_port.h"
#include "util.h"

#define NBUF 4

static struct {
  AudioDriver drv;
  AudioDriverWaveBuf wb[NBUF];
  uint8_t *pool;
  int frames, bytes;
  void (*fill)(void *ud, uint8_t *stream, int len);
  Thread thread;
  volatile int run;
} A;

static void out_main(void *arg) {
  (void)arg;
  diag_set_name(NULL, "sdt-audio");
  while (A.run) {
    int i;
    audrvUpdate(&A.drv);
    for (i = 0; i < NBUF; i++)
      if (A.wb[i].state == AudioDriverWaveBufState_Free || A.wb[i].state == AudioDriverWaveBufState_Done) break;
    if (i == NBUF) { audrenWaitFrame(); continue; }      /* all four are waiting their turn */
    uint8_t *p = A.pool + (size_t)i * A.bytes;
    A.fill(NULL, p, A.bytes);
    armDCacheFlush(p, A.bytes);
    memset(&A.wb[i], 0, sizeof A.wb[i]);
    A.wb[i].data_raw = p; A.wb[i].size = A.bytes; A.wb[i].end_sample_offset = A.frames;
    audrvVoiceAddWaveBuf(&A.drv, 0, &A.wb[i]);
    if (!audrvVoiceIsPlaying(&A.drv, 0)) audrvVoiceStart(&A.drv, 0);   /* it stops when it runs dry */
  }
}

/* 0 on success. `fill` is called on the output thread for `frames` stereo frames at a time. */
int sdt_audio_out_start(void (*fill)(void *, uint8_t *, int), int frames) {
  static const AudioRendererConfig cfg = {
    .output_rate = AudioRendererOutputRate_48kHz, .num_voices = 4, .num_effects = 0,
    .num_sinks = 1, .num_mix_objs = 1, .num_mix_buffers = 2,
  };
  static const u8 sink_ch[2] = { 0, 1 };
  if (A.run) return 0;
  A.fill = fill; A.frames = frames; A.bytes = frames * 4;
  const size_t pool_sz = ((size_t)A.bytes * NBUF + 0xfff) & ~(size_t)0xfff;
  A.pool = memalign(AUDREN_MEMPOOL_ALIGNMENT, pool_sz);
  if (!A.pool) return -1;
  memset(A.pool, 0, pool_sz);
  Result rc = audrenInitialize(&cfg);
  if (R_FAILED(rc)) { debugPrintf("[audio-out] audrenInitialize: 0x%x\n", rc); return -1; }
  rc = audrvCreate(&A.drv, &cfg, 2);
  if (R_FAILED(rc)) { debugPrintf("[audio-out] audrvCreate: 0x%x\n", rc); audrenExit(); return -1; }
  const int mp = audrvMemPoolAdd(&A.drv, A.pool, pool_sz);
  audrvMemPoolAttach(&A.drv, mp);
  audrvDeviceSinkAdd(&A.drv, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_ch);
  audrvUpdate(&A.drv);
  rc = audrenStartAudioRenderer();
  if (R_FAILED(rc)) { debugPrintf("[audio-out] start renderer: 0x%x\n", rc); return -1; }
  audrvVoiceInit(&A.drv, 0, 2, PcmFormat_Int16, 48000);
  audrvVoiceSetDestinationMix(&A.drv, 0, AUDREN_FINAL_MIX_ID);
  audrvVoiceSetMixFactor(&A.drv, 0, 1.0f, 0, 0);
  audrvVoiceSetMixFactor(&A.drv, 0, 0.0f, 0, 1);
  audrvVoiceSetMixFactor(&A.drv, 0, 0.0f, 1, 0);
  audrvVoiceSetMixFactor(&A.drv, 0, 1.0f, 1, 1);
  audrvVoiceStart(&A.drv, 0);
  A.run = 1;
  /* above the engine's worker threads (0x3b): sound must not wait behind a level load */
  static const int PRIOS[] = { 0x2c, 0x30, 0x3b };
  rc = 1;
  for (size_t i = 0; i < sizeof PRIOS / sizeof PRIOS[0] && R_FAILED(rc); i++)
    rc = threadCreate(&A.thread, out_main, NULL, NULL, 0x40000, PRIOS[i], -2);
  if (R_SUCCEEDED(rc)) rc = threadStart(&A.thread);
  if (R_FAILED(rc)) { debugPrintf("[audio-out] thread: 0x%x\n", rc); A.run = 0; return -1; }
  debugPrintf("[audio-out] renderer voice: 48000 Hz stereo, %d frames a buffer, %d buffers\n", frames, NBUF);
  return 0;
}

void sdt_audio_out_stop(void) {
  if (!A.run) return;
  A.run = 0;
  threadWaitForExit(&A.thread); threadClose(&A.thread);
  audrvClose(&A.drv); audrenExit();
}
