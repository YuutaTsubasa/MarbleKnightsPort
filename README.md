# Marble Knights Recompiled

[繁體中文](README.zh-TW.md)

An unofficial port of WayForward's *Marble Knights* — an Apple Arcade game that
is no longer available — from iOS to Windows, built through static
recompilation. The ARM64 code in the iOS app's `UnityFramework` (the IL2CPP
runtime and the game's compiled C#, well over 100,000 functions) is translated
ahead of time into C and compiled into a `GameAssembly.dll` that the official
Unity 2019.4.17f1 Windows player loads. A small compatibility layer stands in
for the iOS system calls, Game Center, CloudKit saves, game controllers and
haptics.

**No game data is included.** You supply your own copy of the iOS app
(*Marble Knights* 2.3.1, build 760642). Unity's players are not included
either: they come from Unity's own downloads. Read [LEGAL.md](LEGAL.md) before
building or sharing anything.

> [!IMPORTANT]
> This is a work in progress, and there are no releases. The game boots and
> its levels can be played on Windows, but several things are still broken
> (see [Status](#status)).

## Table of Contents

- [Status](#status)
- [How it works](#how-it-works)
- [How to Build](#how-to-build)
- [Repository Layout](#repository-layout)
- [License](#license)

## Status

What works on Windows:

- Opening videos, title screen, saves, main menu and Quest Mode levels, with
  sound (the game's own FMOD mixer, played through waveOut).
- Controllers (XInput) and keyboard.
- The game's art: the iOS Metal shaders are translated to GLSL and the player
  runs on OpenGL ES 3; PVRTC textures, which desktop GPUs cannot sample, are
  decoded at build time.

Known problems:

- The mouse does not yet work as touch input (title screen, the on-screen
  joystick in levels).
- An intermittent crash during garbage collection between the title screen
  and the menu.
- A broken frame that only appears in full screen; z-fighting on the plates'
  glow.

| Platform | State |
|---|---|
| Windows (x64) | builds and plays, with the problems above |
| Android (ARM64) | an earlier route: the iOS `UnityFramework` loaded directly (Mach-O loader) under Unity's Android player; reaches the first scene |
| Linux, macOS | not started |

## How it works

- `tools/prelink_win.py` lays the Mach-O image of `UnityFramework` out in
  memory, resolves its imports and records what each one binds to.
- `tools/a64toc.py` translates every function into C over a register file
  (`x0`–`x30`, `sp`, flags, `v0`–`v31`). Guest pointers are host pointers;
  direct calls become C calls, indirect calls go through a dispatch table,
  switch tables become `switch` statements. Exceptions are handled per call
  site from the image's LSDA. All but a handful of instructions (mostly `udf` traps) translate.
- `loader/win/` is the host runtime: the 229 `il2cpp_*` entry points the Unity
  player calls, the calling-convention bridge between Windows x64 and the
  translated code, and Darwin's libSystem, CoreFoundation and libc++ written
  against Windows.
- `tools/build_win.py` assembles a player folder from the official Windows
  IL2CPP player and the iOS app's data (the serialized files are retargeted to
  StandaloneWindows64; `global-metadata.dat` is used as is).

The design, the Darwin ABI differences that matter, and the numbers behind
each step are in [docs/design.md](docs/design.md) and
[docs/notes/](docs/notes/) (both written in Traditional Chinese).

## How to Build

Windows only for now. Requirements:

- Visual Studio 2022 Build Tools (for the MSVC headers and libraries), LLVM
  (`clang-cl`, `lld-link`, `llvm-rc`), Python 3 with
  [capstone](https://www.capstone-engine.org/).
- Unity 2019.4.17f1: the Editor (for the libil2cpp headers) and Windows Build
  Support (IL2CPP), from Unity's official downloads, unpacked into
  `work/unity/editor` and `work/unity/winil2cpp`.
- Your copy of the iOS app, unpacked into `work/ipa/`.

```bash
python -I tools/build_win.py              # player folder: work/win/MarbleKnights/
python -I tools/win_textures.py           # PVRTC -> RGBA32
python -I tools/prelink_win.py            # Mach-O image -> work/win/image
python -E -X utf8 tools/a64toc.py --all   # ARM64 -> C (work/win/gen)
bash tools/build_win_ga.sh                # GameAssembly.dll (about 5 minutes)
```

Then run `work/win/MarbleKnights/MarbleKnights.exe`. Everything the build
produces is derived from the game and stays in `work/`, which is ignored by
Git.

## Repository Layout

| Path | Contents |
|---|---|
| `tools/` | The translator (`a64toc.py`), Mach-O and Unity asset readers, build scripts, analysis scripts |
| `loader/win/` | Windows runtime for the translated code, Darwin compatibility layer, plugins (FMOD output, controllers, saves) and the launcher |
| `loader/src/`, `loader/proxy/` | Android route: Mach-O loader, Darwin → bionic layer, `libil2cpp.so` proxy |
| `docs/` | Design document and development notes |
| `__ROM__/`, `work/`, `private/`, `generated/`, `out/` | Game files and everything built from them (ignored by Git) |

## License

The code in this repository is released under the [MIT License](LICENSE).
*Marble Knights* is © WayForward Technologies, Inc.; this project is not
affiliated with or endorsed by WayForward, Apple or Unity Technologies. See
[THIRD_PARTY.md](THIRD_PARTY.md) for what the build depends on.
