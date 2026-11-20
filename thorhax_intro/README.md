# Thorhax intro

The short video Thorhax ports show at startup. The game loads behind it, so it adds little or nothing to the time
to the title screen. The first 3 seconds cannot be skipped; after that any button (A, B, X, Y, +, -, L, R, ZL, ZR)
ends it, and - and + held together end it at any time. There is no setting to turn it off.

| File | Contents |
|---|---|
| `thorhax_intro.h`, `thorhax_intro.cpp` | The player: a decode thread on core 1 and a player thread on core 2, frames shown at the audio clock on a linear libnx framebuffer, the sound through audout |
| `thorhax_intro.mpg`, `thorhax_intro.pcm` | The video (MPEG-1, 1280x720) and its sound (raw 48 kHz stereo s16), linked into the executable |
| `make_intro.sh` | Makes both from a source video with FFmpeg |
| `pl_mpeg.h`, `pl_mpeg_impl.cpp` | [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (MIT, unmodified), the MPEG-1 decoder |

## How it fits this port

`main.c` calls `thorhax_intro_start()` once the game's files are found, before the modules are loaded. The player
holds the port's output (`source/sdt_output_hold.c`) until the video ends. While it is held:

- creating the game's Vulkan surface waits (`sdt_vulkan.c`): the window is the intro's;
- the game's sound goes out silent (`sdt_audio_out.c`);
- no buttons or sticks reach the game (`sdt_input.c`);
- the error and status text screens wait (`error.c`).

The `Makefile` adds this folder to the sources and gives the player the folder's path (`THORHAX_INTRO_DIR`) for
the two `.incbin` lines.
