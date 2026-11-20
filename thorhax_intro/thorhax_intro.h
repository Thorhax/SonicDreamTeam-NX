// Thorhax intro: the short video every Thorhax port shows at startup (Switch only; elsewhere it does nothing).
// Self-contained so it can be copied into another port: this folder plus one call (see README.md).

#pragma once

#ifdef __cplusplus

namespace thorhax::intro {

// Starts the intro on its own threads and returns at once, so the game loads while it plays. It draws on the default
// window with a CPU framebuffer and plays its sound through audout, holding the port's output (RexSwitchHoldOutput,
// source/sdt_output_hold.c) until it ends: meanwhile the game loads unseen, unheard and with untouched controllers,
// and waits for the window if it gets that far. Call it on the main thread before anything draws on the window. The first kUnskippableSeconds cannot be skipped; after that any button ends it. - and + together end it at
// any time.
void Start();

}  // namespace thorhax::intro

extern "C" {
#endif

// The same, for C.
void thorhax_intro_start(void);

#ifdef __cplusplus
}
#endif
