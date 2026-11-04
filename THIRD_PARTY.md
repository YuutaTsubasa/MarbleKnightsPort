# Third-party components

Nothing third-party is included in this repository. Building from source
depends on the following, which you obtain yourself. Releases, when there are
any, will include Unity's players (marked below). None of it is game data; see
[LEGAL.md](LEGAL.md) for that.

| Component | Used for | Where it comes from |
|---|---|---|
| Unity 2019.4.17f1 Windows player (IL2CPP) | The engine: `UnityPlayer.dll`, `WindowsPlayer.exe`, built-in resources (**included in releases**) | Unity's official downloads (Windows Build Support); Unity's terms |
| Unity 2019.4.17f1 Editor | libil2cpp headers (API signatures, structure layouts) | Unity's official downloads; Unity's terms |
| Unity 2019.4.17f1 Android player | The Android route (**included in releases**) | Unity's official downloads (Android Build Support); Unity's terms |
| [capstone](https://www.capstone-engine.org/) | ARM64 disassembly in `tools/a64toc.py` and `tools/disasm.py` | `pip install capstone`; BSD-3-Clause |
| [LLVM](https://llvm.org/) | `clang-cl`, `lld-link`, `llvm-rc` | llvm.org; Apache-2.0 with LLVM exceptions |
| Visual Studio 2022 Build Tools | MSVC headers and libraries, Windows SDK | Microsoft; Microsoft's terms |
| Android NDK | The Android route only | Google; Android SDK terms |

The game's own FMOD build (inside `UnityFramework`) is what plays the sound;
no separate FMOD library is needed.
