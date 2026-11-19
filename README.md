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

You need a Switch with Atmosphère and the game's APK (`sdt-1.10.3.apk`).

1. Make the folder `sdmc:/switch/sdt/` and copy `sdt_nx.nro` into it.
2. Copy the APK there as `base.apk`, unmodified.
3. Open the APK as a zip and copy the four files of `lib/arm64-v8a/` into `sdmc:/switch/sdt/lib/`:
   `libmain.so`, `libunity.so`, `libil2cpp.so`, `libUnityFramework.so`.
4. Start it with **title override**: hold R while launching an installed game to open the
   homebrew menu, then run `sdt_nx`. It does not have enough memory in album (applet) mode.

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
| `SDT_BOOT_CONFIG` | graphics jobs, 3 workers | Engine start-up settings added to the game's `boot.config`, as `key=value;key=value`. The default is `gfx-enable-gfx-jobs=1;gfx-enable-native-gfx-jobs=1;job-worker-count=3` (draw calls prepared on the worker threads, and three of them); `none` adds nothing |
| `SDT_SHADOW_DISTANCE` | the game's | How far real-time shadows are drawn, in metres (`0` = none) |
| `SDT_AUDIO` | own | `sdl` uses SDL's audio device instead of the port's own output loop |
| `SDT_AUDIO_TRACE` | off | `1` writes `audio.txt` once a second: queue counters, loudness, where the audio threads are if the queue stalls |
| `SDT_PROFILE` | off | Samples per second (e.g. `400`) for the built-in profiler; see Build |

## Known limits

- Measured only on an overclocked console (CPU about 1785 MHz, GPU 768 MHz): about 50 to 60 fps in
  the first level at 720p. The main thread is the limit, so stock clocks will be slower; try
  `SDT_MEMTOTAL_MB=512` there.
- Not yet tested: playing with no network, docked mode, sleep and resume, anything past the
  first level, and whether progress resumes after finishing a level.
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

The source is MIT (`LICENSE`). The built `.nro` statically links the Mesa NVK driver and FFmpeg,
whose own licences (including GPL terms) apply to the binary.
