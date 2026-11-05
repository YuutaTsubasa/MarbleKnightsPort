# Marble Knights Recompiled

[English](README.md)

把 WayForward 的 Apple Arcade 遊戲 *Marble Knights*（已下架）從 iOS 移植到 Windows 的非官方專案，做法是靜態重編譯：iOS 版 `UnityFramework` 裡的 ARM64 程式（IL2CPP 執行環境＋遊戲編譯好的 C#，超過 10 萬個函式）事先轉成 C，編成 `GameAssembly.dll`，交給官方 Unity 2019.4.17f1 Windows 播放器載入。iOS 的系統呼叫、Game Center、CloudKit 存檔、手把與震動由一層小的相容層代替。

**本專案不含任何遊戲資料。** 遊戲要自己準備（iOS 版 *Marble Knights* 2.3.1，build 760642），啟動器在你的電腦上把它轉成能玩的資料夾或 Android APK。建置或分享任何東西之前，請先讀 [LEGAL.md](LEGAL.md)。

> [!IMPORTANT]
> 還在開發中，沒有發布版本。Windows 上已經能開機、進關卡玩，但還有幾個地方是壞的（見[現況](#現況)）。

## 現況

Windows 上能用的：

- 開頭標誌、標題、存檔、主選單、Quest Mode 關卡；有聲音（遊戲自己的 FMOD 混音，經 waveOut 播放）。
- 手把（XInput）、鍵盤，滑鼠當觸控（標題畫面、關卡裡的畫面搖桿）；單人時 1P 跟著最後用的裝置。
- 畫面：iOS 的 Metal 著色器轉成 GLSL，播放器用 OpenGL ES 3 跑；桌機 GPU 不支援的 PVRTC 貼圖在準備時解開。
- 全螢幕（Alt+Enter 或 F4）是無邊框視窗，錄影軟體擷取得到。

已知問題：

- 地板漩渦附近偶爾冒出淺藍色亮點（著色器動畫，還沒查）。

| 平台 | 狀態 |
|---|---|
| Windows（x64） | 能玩 |
| Android（ARM64） | 能玩（作者的裝置上約 60 fps）：用 Mach-O 載入器直接載入 iOS 的 `UnityFramework`，配 Unity 官方 Android 播放器 |
| Linux、macOS | 還沒開始；Proton、PlayCover、側載與其他 Android 裝置的判斷見 [docs/platforms.md](docs/platforms.md) |

只在 Windows 11＋NVIDIA 顯示卡與一台 arm64 Android 裝置上測過。Windows 版要顯示卡驅動提供 OpenGL ES 3 context，AMD、Intel 的驅動還沒測。

## 遊玩（啟動器）

Release 的資料夾（`tools/package_win.py` 做的）裡沒有遊戲資料，打開 `MarbleKnightsLauncher.exe`：

1. 選你自己的 IPA，按「準備」（約 1–2 分鐘，要 3.5 GB 空間）。啟動器用附帶的 Python 跑 `tools/prepare_win.py`，中斷了再按一次會接著做。
2. 按「開始遊戲」。選項有「以全螢幕開始」與測試用的「關閉記憶體回收」。
3. Android：按「製作 APK」，`tools/prepare_android.py` 在 `android\` 做出 `MarbleKnights.apk` 和 3.4 GB 的 `MarbleKnights` 資料夾。把資料夾整個複製到手機內部儲存空間的最上層，再安裝 APK；第一次開啟時允許存取檔案。APK 用啟動器第一次做 APK 時產生的金鑰（`android\signing-key.json`）簽，之後的 APK 要靠同一把金鑰才能直接覆蓋安裝。

## 做法

- `tools/prelink_win.py` 把 `UnityFramework` 的 Mach-O 映像排進記憶體、解出匯入，記下每個匯入要接到哪裡。
- `tools/a64toc.py` 把每個函式轉成操作暫存器檔（`x0`–`x30`、`sp`、旗標、`v0`–`v31`）的 C。guest 指標就是 host 指標；直接呼叫變成 C 呼叫，間接呼叫查表分派，switch 表變成 `switch`。例外依映像的 LSDA 逐個呼叫點處理。只剩極少數指令（多半是 `udf` 陷阱）沒轉。
- `loader/win/` 是 host 端執行環境：Unity 播放器呼叫的 229 個 `il2cpp_*`、Windows x64 與轉換後程式之間的呼叫慣例轉接、照 Windows 重寫的 Darwin libSystem、CoreFoundation、libc++。
- `tools/build_win.py` 用官方 Windows IL2CPP 播放器和 iOS 版的資料組出播放器資料夾（序列化檔的平台改成 StandaloneWindows64，`global-metadata.dat` 原樣沿用）。

設計、要注意的 Darwin ABI 差異、每一步的數字在 [docs/design.md](docs/design.md) 與 [docs/notes/](docs/notes/)。

## 建置

目前只有 Windows。需要：

- Visual Studio 2022 Build Tools（MSVC 標頭與函式庫）、LLVM（`clang-cl`、`lld-link`、`llvm-rc`）、Python 3 與 [capstone](https://www.capstone-engine.org/)。
- Unity 2019.4.17f1：Editor（用它的 libil2cpp 標頭）與 Windows Build Support（IL2CPP），從 Unity 官方下載，解到 `work/unity/editor` 與 `work/unity/winil2cpp`。
- 自己的 iOS 版遊戲，解到 `work/ipa/`。

```bash
python -I tools/build_win.py              # 播放器資料夾：work/win/MarbleKnights/
python -I tools/win_textures.py           # PVRTC → RGBA32
python -I tools/prelink_win.py            # Mach-O 映像 → work/win/image
python -E -X utf8 tools/a64toc.py --all   # ARM64 → C（work/win/gen）
bash tools/build_win_ga.sh                # GameAssembly.dll（約 5 分鐘；--runtime-only 只重編執行環境）
```

之後執行 `work/win/MarbleKnights/MarbleKnights.exe`。建置產生的東西都是從遊戲來的，全部留在 `work/`（Git 會忽略）。

### Android 路線

```bash
NDK="$LOCALAPPDATA/Android/Sdk/ndk/29.0.13599879"
cmake -S loader -B work/build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build work/build-android
python -I tools/build_apk.py      # 需要 work/unity/AndroidPlayer 與 work/raw-android；APK 在 out/，只裝自己的裝置
```

### Release 資料夾

```bash
# Android 代理另外建一份不含驗證用 batch.c（從遊戲產生）的
cmake -S loader -B work/build-android-release -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DMK_RECOMP_BATCH=OFF
python -I tools/package_win.py    # out/MarbleKnightsRecompiled-win64；會檢查裡面沒有遊戲資料
```

`tools/thor-*.sh` 是作者在自己的測試裝置上用的腳本。

## 目錄

| 路徑 | 內容 |
|---|---|
| `tools/` | 轉換器（`a64toc.py`）、Mach-O 與 Unity 資產讀取、建置腳本、分析腳本 |
| `loader/win/` | 轉換後程式的 Windows 執行環境、Darwin 相容層、外掛（FMOD 輸出、手把、存檔）與啟動器 |
| `loader/src/`、`loader/proxy/` | Android 路線：Mach-O 載入器、Darwin → bionic 相容層、`libil2cpp.so` 代理 |
| `docs/` | 設計文件與開發紀錄 |
| `__ROM__/`、`work/`、`private/`、`generated/`、`out/` | 遊戲檔與從它產生的一切（Git 忽略） |

## 授權

本 repo 的程式碼以 [MIT License](LICENSE) 授權。*Marble Knights* © WayForward Technologies, Inc.；本專案與 WayForward、Apple、Unity Technologies 無關，也未經它們認可。建置用到的東西見 [THIRD_PARTY.md](THIRD_PARTY.md)。
