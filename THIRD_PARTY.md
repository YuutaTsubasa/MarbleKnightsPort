# Third-party components

Nothing third-party is included in this repository. Building from source
depends on the following, which you obtain yourself. Releases include the
components marked below. None of it is game data; see
[LEGAL.md](LEGAL.md) for that.

| Component | Used for | Where it comes from |
|---|---|---|
| Unity 2019.4.17f1 Windows player (IL2CPP) | The engine: `UnityPlayer.dll`, `WindowsPlayer.exe`, built-in resources (**included in releases**) | Unity's official downloads (Windows Build Support); Unity's terms |
| Unity 2019.4.17f1 Editor | libil2cpp headers (API signatures, structure layouts) | Unity's official downloads; Unity's terms |
| Unity 2019.4.17f1 Android player | The Android route (**included in releases**) | Unity's official downloads (Android Build Support); Unity's terms |
| [Python 3.12](https://www.python.org/) embeddable package (64-bit) | Runs the launcher's prepare step (`tools/prepare_win.py`) on the player's machine (**included in releases** as `python/`) | python.org (3.12.10, signed by the Python Software Foundation); PSF License |
| [LZ4](https://github.com/lz4/lz4) 1.10.0 | AssetBundle compression, built with our PVRTC decoder into `bin/mk_native.dll` (**included in releases**) | The official release's source (`lz4-1.10.0.tar.gz`); `lib/` is BSD-2-Clause |
| [capstone](https://www.capstone-engine.org/) | ARM64 disassembly in `tools/a64toc.py` and `tools/disasm.py` | `pip install capstone`; BSD-3-Clause |
| [LLVM](https://llvm.org/) | `clang-cl`, `lld-link`, `llvm-rc` | llvm.org; Apache-2.0 with LLVM exceptions |
| Visual Studio 2022 Build Tools | MSVC headers and libraries, Windows SDK | Microsoft; Microsoft's terms |
| Android NDK | The Android route only | Google; Android SDK terms |

The game's own FMOD build (inside `UnityFramework`) is what plays the sound;
no separate FMOD library is needed.
