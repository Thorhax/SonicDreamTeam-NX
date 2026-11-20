/* sdt_output_hold.c -- the hold the Thorhax intro (thorhax_intro/) takes on the
 * screen, the sound and the controllers while it plays. The game loads behind
 * it; whatever would show, sound or read a button first waits or stays quiet:
 *
 *   sdt_vulkan.c      creating the game's surface waits (the window is the intro's)
 *   sdt_audio_out.c   buffers go out silent
 *   sdt_input.c       no buttons or sticks are reported
 *   error.c           the text console waits
 * MIT.
 */
#include <stdbool.h>
#include <switch.h>

#include "sdt_port.h"

static volatile bool g_held;

void RexSwitchHoldOutput(void)    { __atomic_store_n(&g_held, true, __ATOMIC_RELEASE); }
void RexSwitchReleaseOutput(void) { __atomic_store_n(&g_held, false, __ATOMIC_RELEASE); }
bool RexSwitchOutputHeld(void)    { return __atomic_load_n(&g_held, __ATOMIC_ACQUIRE); }

int  sdt_output_held(void) { return RexSwitchOutputHeld(); }
void sdt_output_wait(void) { while (RexSwitchOutputHeld()) svcSleepThread(5000000); }
