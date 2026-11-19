/* sdt_input.c -- the controller, the way MainActivity.java reports it.
 *
 * The Android build does not use Unity's input for gamepads. Its activity
 * forwards them to the facade: nativePads (the list of connected pads, as
 * text), nativePadKey (a button) and nativePadMotion (12 axes, in the order of
 * MainActivity.PAD_AXES). The facade then plays the part of the iOS game
 * controller framework for the game.
 *
 * One pad is reported, as an Xbox controller so the game shows the Xbox glyphs
 * it ships. Buttons are mapped by LABEL: a prompt that shows "A" means the
 * button marked A on the Switch (so jump is on the right-hand button, as on
 * other Switch games). <root>/env.txt SDT_BUTTONS=position maps by position
 * instead (the Switch's B, at the bottom, becomes the Xbox A).
 * MIT.
 */
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "jni_fake.h"
#include "sdt_port.h"
#include "util.h"

#define PAD_ID 1
enum { K_UP = 19, K_DOWN = 20, K_LEFT = 21, K_RIGHT = 22, K_A = 96, K_B = 97, K_X = 99, K_Y = 100,
       K_L1 = 102, K_R1 = 103, K_L2 = 104, K_R2 = 105, K_THUMBL = 106, K_THUMBR = 107, K_START = 108, K_SELECT = 109 };
static struct { u64 nx; int key; } MAP[] = {
  { HidNpadButton_A, K_A }, { HidNpadButton_B, K_B }, { HidNpadButton_X, K_X }, { HidNpadButton_Y, K_Y },
  { HidNpadButton_L, K_L1 }, { HidNpadButton_R, K_R1 }, { HidNpadButton_ZL, K_L2 }, { HidNpadButton_ZR, K_R2 },
  { HidNpadButton_StickL, K_THUMBL }, { HidNpadButton_StickR, K_THUMBR },
  { HidNpadButton_Plus, K_START }, { HidNpadButton_Minus, K_SELECT },
  { HidNpadButton_Up, K_UP }, { HidNpadButton_Down, K_DOWN }, { HidNpadButton_Left, K_LEFT }, { HidNpadButton_Right, K_RIGHT },
};

typedef void (*fn_pads)(void *env, void *cls, void *str);
typedef void (*fn_key)(void *env, void *cls, int id, int key, int down);
typedef void (*fn_motion)(void *env, void *cls, int id, void *floats);

static PadState g_pad;
static int g_state;                 /* 0 = not started, 1 = running, -1 = natives missing */
static fn_key g_key; static fn_motion g_motion;
static void *g_cls;
static u64 g_prev;
static float g_axes[12];

static float stick(s32 v) {
  float f = (float)v / 32767.0f;
  if (f > -0.12f && f < 0.12f) return 0.0f;
  return f < -1.0f ? -1.0f : f > 1.0f ? 1.0f : f;
}

void sdt_input_frame(void) {
  if (g_state < 0) return;
  if (g_state == 0) {
    fn_pads pads = (fn_pads)jni_lookup_unity_native("nativePads");
    g_key = (fn_key)jni_lookup_unity_native("nativePadKey");
    g_motion = (fn_motion)jni_lookup_unity_native("nativePadMotion");
    if (!pads || !g_key || !g_motion) { g_state = -1; debugPrintf("[pad] the facade registered no gamepad natives\n"); return; }
    { const char *e = getenv("SDT_BUTTONS");
      if (e && !strcmp(e, "position")) {
        MAP[0].nx = HidNpadButton_B; MAP[1].nx = HidNpadButton_A; MAP[2].nx = HidNpadButton_Y; MAP[3].nx = HidNpadButton_X;
      }
      debugPrintf("[pad] buttons mapped by %s\n", e && !strcmp(e, "position") ? "position" : "label"); }
    g_cls = jni_make_object("com/sdt/port/MainActivity");
    padInitializeDefault(&g_pad);
    /* id, vendor, product, flags (1 triggers | 4 right stick on Z/RZ), name */
    pads(fake_env, g_cls, jni_make_string("1\t1118\t746\t5\tXbox Wireless Controller\n"));
    /* nativeLifecycle(true) means "gone to the background" (it pauses the
     * sound), so it is not called here: the game starts in the foreground. */
    g_state = 1;
    debugPrintf("[pad] one controller reported to the facade\n");
  }
  padUpdate(&g_pad);
  const u64 now = padGetButtons(&g_pad), diff = now ^ g_prev;
  for (size_t i = 0; diff && i < sizeof MAP / sizeof MAP[0]; i++)
    if (diff & MAP[i].nx) g_key(fake_env, g_cls, PAD_ID, MAP[i].key, (now & MAP[i].nx) != 0);
  g_prev = now;

  const HidAnalogStickState l = padGetStickPos(&g_pad, 0), r = padGetStickPos(&g_pad, 1);
  float a[12];
  a[0] = stick(l.x); a[1] = -stick(l.y);                    /* AXIS_X, AXIS_Y (down is positive) */
  a[2] = stick(r.x); a[3] = -stick(r.y);                    /* AXIS_Z, AXIS_RZ */
  a[4] = 0; a[5] = 0;                                       /* AXIS_RX, AXIS_RY */
  a[6] = (now & HidNpadButton_Right ? 1.f : 0.f) - (now & HidNpadButton_Left ? 1.f : 0.f);   /* AXIS_HAT_X */
  a[7] = (now & HidNpadButton_Down ? 1.f : 0.f) - (now & HidNpadButton_Up ? 1.f : 0.f);      /* AXIS_HAT_Y */
  a[8] = now & HidNpadButton_ZL ? 1.f : 0.f; a[9] = now & HidNpadButton_ZR ? 1.f : 0.f;      /* L/R TRIGGER */
  a[10] = a[8]; a[11] = a[9];                               /* AXIS_BRAKE, AXIS_GAS */
  /* Android delivers a motion event for every sample of a deflected stick, not
   * only when the value changes; send one a frame while anything is off centre. */
  int off_centre = 0;
  for (int i = 0; i < 12; i++) off_centre |= a[i] != 0.0f;
  if (off_centre || memcmp(a, g_axes, sizeof a)) {
    memcpy(g_axes, a, sizeof a);
    g_motion(fake_env, g_cls, PAD_ID, jni_make_int_array((const int *)a, 12));   /* 4-byte elements */
  }
}
