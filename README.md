# Sonic Dream Team — Nintendo Switch port (Unity 2022.3 wrapper)

A homebrew loader that runs the Android build of **Sonic Dream Team** (`com.sdt.port` 1.10.3)
natively on a Nintendo Switch with custom firmware. It ships **no game code and no game
assets**: you supply the APK.

Status: **0.1.1, first playable.** Boots to the menu, plays the intro video and the story
cutscenes, and runs the first level with a controller. See [Known limits](#known-limits).

## How it works

The Android build is unusual: its `libunity.so` is the stock Android Unity 2022.3.54f1 player,
but its `libil2cpp.so` is a small layer that loads the game's **iOS** code image
(`libUnityFramework.so`, a Mach-O) and answers its Darwin calls. It renders with Vulkan only.

This port loads those libraries with a `.so` loader and stands in for Android:

| Part | File | What it does |
|---|---|---|
| Vulkan | `source/sdt_vulkan.c` | Answers `dlopen("libvulkan.so")` with the statically linked Mesa NVK driver; turns the Android surface into the Switch's; lists sRGB surface formats |
| Executable memory | `source/sdt_codemem.c` | Lets the layer map the iOS image writable and then make it executable in place, which Horizon does not allow directly |
| Engine memory | `source/libc_shim.c` | Unity's 511 MB address-space reservations, committed on demand from the heap |
| Layer support | `source/sdt_shim.c` | Environment variables, AAudio, thread names, thread suspension by signal (for the garbage collector) |
| Assets | `source/asset_pack.c` | Serves `assets/...` straight out of the APK (its entries are stored, not compressed) |
| Controller | `source/sdt_input.c` | Feeds the pad to the layer's own gamepad interface |
| Video | `source/sdt_video.c` | Plays the intro with FFmpeg and draws it over the swapchain image |
| Tuning | `source/sdt_tune.c` | Full 720p render scale |

It is a fork of [battd_nx](https://github.com/ChanseyIsTheBest/battd_nx) (MIT), which provides the
loader, the fake JNI layer and the libc shims.

## Install

You need a Switch with Atmosphère and the game's APK.

**Which APK:** the Android build of Sonic Dream Team, package `com.sdt.port`, **version 1.10.3**
(usually named `sdt-1.10.3.apk`). This is the only version the port was built and tested against;
other versions are not expected to work.

| | |
|---|---|
| Size | 2,245,078,225 bytes (about 2.1 GB) |
| SHA-256 | `d4e6fb090277a4ca92f9645f3f5c80e215fe63c56382efa1b937332b7c6356f6` |

The APK is not part of this project and is not distributed with it.

1. Make the folder `sdmc:/switch/sdt/` and copy `sdt_nx.nro` into it.
2. Copy the APK there as `base.apk`, unmodified.
3. Open the APK as a zip and copy the four files of `lib/arm64-v8a/` into `sdmc:/switch/sdt/lib/`:
   `libmain.so`, `libunity.so`, `libil2cpp.so`, `libUnityFramework.so`.
4. Start it with **title override**: hold R while launching an installed game to open the
   homebrew menu, then run `sdt_nx`. It does not have enough memory in album (applet) mode.

For the best frame rate, start it from a **forwarder that allows four CPU cores** instead (for
example sphaira: select `sdt_nx`, Options, Install Forwarder, Address Space 39-bit, CPU Cores 4).
The title override gives a homebrew three cores; with the fourth the port starts a fourth engine
worker by itself. In the heaviest level measured that is the difference between about 57 fps and
a steady 60 (see Known limits).

The Thorhax intro plays at startup while the game loads (`thorhax_intro/`): any button skips it
after 3 seconds, and - and + together skip it at once.

The first boot builds `apk.idx` (a list of the APK's files) and takes a little longer. Saves go
to `sdmc:/switch/sdt/com.sdt.port/files/`.

## Controls

The game shows Xbox button pictures. Buttons are mapped by **label**: a prompt showing "A" means
the Switch's A button. A / B / + skips the intro video.

## Settings

Optional `sdmc:/switch/sdt/env.txt`, one `NAME=value` a line:

| Name | Default | Meaning |
|---|---|---|
| `SDT_RENDER_SCALE` | `1.0` | 3D render scale, 0.25 to 1.0 (`0` = the game's own, 0.5) |
| `SDT_MEMTOTAL_MB` | `4096` | Memory reported to the game, which picks its quality class from it: 512 = lowest (30 fps cap), 4096 = middle (60 fps target), 8192 = highest |
| `SDT_BUTTONS` | label | `position` maps buttons by position instead (the Switch's B becomes the game's A) |
| `SDT_PSO_WARMUP` | on | `0` skips building the game's list of shader pipelines at boot |
| `SDT_VSYNC` | on | `0` presents without waiting for the display (the game's own choice; can tear) |
| `SDT_BOOT_CONFIG` | graphics jobs, a worker per core | Engine start-up settings added to the game's `boot.config`, as `key=value;key=value`. The default is `gfx-enable-gfx-jobs=1;job-worker-count=3` (draw calls prepared on the worker threads), with `4` workers when four cores are allowed; `none` adds nothing |
| `SDT_SHADOW_DISTANCE` | the game's | How far real-time shadows are drawn, in metres (`0` = none) |
| `SDT_AUDIO` | own | `sdl` uses SDL's audio device instead of the port's own output loop |
| `SDT_AUDIO_TRACE` | off | `1` writes `audio.txt` once a second: queue counters, loudness, where the audio threads are if the queue stalls |
| `SDT_FPS` | off | `1` writes `fps.txt`: every 300 frames the rate, the longest frame and how many frames took more than one refresh |
| `SDT_FUTEX` | kernel | `bucket` goes back to the older wait/wake emulation for the engine's threads (0.1.4 and before) |
| `SDT_FIXED_STEP` | the game's | Physics step in seconds (the game's is 1/60). `0.02` saves a little CPU below 60 fps; it can change how movement feels |
| `SDT_LOD_BIAS` | the game's | Detail distance factor; below `1` switches to simpler models nearer the camera |
| `SDT_URP` | the game's | Properties of the render pipeline asset, as `name=value;name=value`, e.g. `supportsMainLightShadows=0;maxAdditionalLightsCount=0` |
| `SDT_THREADS` | off | Cores and priority of engine threads, as `name:cores:priority;...` (hex; `0` keeps a value), e.g. `Job.Worker 3:8:0` |
| `SDT_PROFILE` | off | Samples per second (e.g. `400`) for the built-in profiler; `SDT_PROFILE_THREADS` = the thread names to sample, comma-separated; see Build |

## Known limits

- Measured only on an overclocked console (CPU about 1785 MHz, GPU 768 MHz), at 720p. The heaviest
  level found (Ego City Nightmare) holds 60 fps from a four-core forwarder and runs at about 57 fps
  from the title override (three cores). The main thread is the limit, so stock clocks will be
  slower; try `SDT_MEMTOTAL_MB=512` there.
- The game starts with no network (tested in airplane mode).
- Docked mode, sleep and resume, and HOME and return work (the game pauses itself).
- Game Center features (leaderboards, cloud saves) do not exist here.

## Build

Needs devkitA64 with libnx, `switch-sdl2 switch-zlib switch-libpng switch-libexpat switch-libzstd
switch-ffmpeg`, and the Mesa Horizon SDK (NVK), 26.2.4 or later:

```sh
make NVK_SDK=/path/to/sdk/opt/devkitpro/portlibs/switch            # with debug.log
make BUILD=build-release DEFINES=-DDEBUG_LOG=0 NVK_SDK=...          # release
python3 tools/symcheck.py <folder with the four .so files> --source source   # imports nothing answers
```

Compiled shader pipelines are kept in `sdmc:/switch/sdt/cache/pipelines.bin`; delete it after changing the
NVK driver.

With `SDT_PROFILE` set, the port writes `prof.map`, `prof.bin` and `methods.txt` (the game's managed
methods) beside the `.nro`. Read them with
`python3 tools/prof.py prof.map prof.bin sdt_nx.elf --libs <folder with the .so files> --methods methods.txt`
(needs `pyelftools`) and the `.elf` of the same build. To name the engine's own functions, add
`--unity-syms libunity.sym.so` with the symbol file of the same engine build, which ships in the Unity
Editor's Android support (`Variations/il2cpp/Release/Symbols/arm64-v8a`).

`probe/` is a small test program that reports which code-memory permission changes the console
allows and what the NVK driver offers.

## Credits

- [battd_nx](https://github.com/ChanseyIsTheBest/battd_nx) by ChanseyIsTheBest and the Switch
  `.so`-loader lineage it descends from (TheFloW, fgsfds, Rinnegatamante and others; see `LICENSE`).
- The Mesa NVK driver and its Horizon port; FFmpeg; SDL2; libnx and devkitPro.
- The author of the Android conversion of the game, whose layer this port runs.

## Disclaimer

Not affiliated with or endorsed by SEGA, Hardlight, Apple, Nintendo or Unity. Sonic Dream Team is
© SEGA. This repository contains none of the game; use it only with a copy you are entitled to use.

## License

The source is MIT (`LICENSE`). The intro's MPEG-1 decoder is [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (MIT). The built `.nro` statically links the Mesa NVK driver and FFmpeg,
whose own licences (including GPL terms) apply to the binary.
