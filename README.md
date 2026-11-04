# Marble Knights Port（個人保存用）

把 WayForward 的 Apple Arcade 遊戲 **Marble Knights**（已下架）的 iOS 版，移植到 Android（ARM64）與 Windows（x64）自己玩。有兩條路線：

**Android（Mach-O 載入）**：做法參考 Project Dream（Sonic Dream Team 的 Mac → Windows／Android）：

- 不重新編譯遊戲。iOS 版的 `UnityFramework`（Mach-O，arm64）裡有 IL2CPP 執行環境＋遊戲程式，我們在 Android 上**直接載入這個 Mach-O**，把它需要的 iOS 系統呼叫換成 Android（bionic）的實作。
- 引擎改用同版本（Unity 2019.4.17f1）的**官方 Android 播放器 `libunity.so`**，讓它透過標準的 `il2cpp_*` API 驅動 iOS 版的遊戲程式。
- iOS 專用的東西（Metal 著色器、GameKit／Apple Arcade、GameController、觸覺回饋）另外轉換或做假的。

**Windows（靜態重編譯）**

- 照 Recompiled 的思路，`tools/a64toc.py` 把整份 `UnityFramework` 的 ARM64 程式（IL2CPP 執行環境＋遊戲程式，約 11 萬個函式）轉成 C，用 clang-cl 編成 `GameAssembly.dll`。
- 引擎用同版本的官方 Windows IL2CPP 播放器 `UnityPlayer.dll`，`global-metadata.dat` 原樣沿用；Darwin 系統呼叫換成 Windows 的實作（`loader/win/`）。
- 著色器沿用 Metal → GLSL 的轉換，以 `-force-gles30` 執行；PVRTC 貼圖在建置時解成 RGBA32。

> 這個 repo 只有我們自己寫的載入器、轉換器、工具與文件，**不含任何遊戲檔案**，也不提供遊戲或 Unity 播放器的下載。遊戲本體要自己準備；IPA 和從裡面解出、產生的任何東西都不進 Git、不上雲端或 CI（見 [AGENTS.md](AGENTS.md)）。Marble Knights 是 WayForward 的作品，本專案與 WayForward、Apple、Unity 無關。

## 現況（2026-11-04）

- **Windows**：可以從頭玩到關卡裡（手把、鍵盤）。還在修的：滑鼠當觸控（標題畫面、關卡的虛擬搖桿）、標題 → 選單偶爾在 GC 時閃退、只有全螢幕才有的破圖、地板光暈的 z-fighting。細節在 [notes/2026-11-03-pc-recomp-feasibility.md](notes/2026-11-03-pc-recomp-feasibility.md)。
- **Android**：在 Thor 上跑到第一個場景；後來重心移到 Windows。

### Android 的進度（2026-11-03）

第一個里程碑的 **IL2CPP 那一半已經在 Thor 上跑通**（還沒接 `libunity.so`）：

- `loader/` 的 Mach-O 載入器在 Android 上載入 iOS 版 `UnityFramework`（54 MB），553 個靜態初始化全部跑完。
- `il2cpp_init` 成功，讀進 `global-metadata.dat`，列出 62 個組件（`MarbleQuest.dll` 2609 個類別…）。
- 真的執行 iOS 版編譯好的 C# 程式：`String.Concat` 回傳 "Marble Knights"；GC 正常回收。
- 受控例外可以拋出並接住（`Int32.Parse` → `FormatException`）：已把 Mach-O 的 compact unwind 轉成 DWARF 給 Android 的 libunwind 用。

同一天也接上了**官方 Unity 2019.4.17f1 Android 播放器**：代理 `libil2cpp.so` 把 API 轉進 iOS 的程式碼，引擎直接讀 iOS 建置的資料（在記憶體裡關掉 `libunity.so` 的目標平台檢查），跑到第一個場景，遊戲腳本與 AssetBundle 載入都在動。畫面目前是全螢幕洋紅色（錯誤著色器）：**下一步是把 Metal 著色器轉成 GLES3**。設計、風險與工作量估計在 [docs/design.md](docs/design.md)。

## 目錄

| 路徑 | 內容 |
| --- | --- |
| `loader/` | Android 端的 Mach-O 載入器與 Darwin→bionic 相容層（C／組合語言，NDK 建置） |
| `loader/win/` | Windows 版 `GameAssembly.dll` 的執行環境、Darwin→Windows 相容層、外掛（FMOD、手把…）與啟動器 |
| `loader/probe/` | 里程碑 1 的測試程式：載入 → 初始化 → `il2cpp_init` → 跑受控程式碼 |
| `tools/` | 分析 `UnityFramework` 的腳本（不需第三方套件的 Mach-O 解析、呼叫圖、反組譯）、Thor 測試腳本 |
| `docs/design.md` | 設計文件：架構、ABI 差異、剩下的工作與估計 |
| `notes/` | 分析紀錄 |
| `__ROM__/`、`work/`、`private/`、`generated/`、`out/` | 遊戲檔與產生物（gitignore，不進 Git） |

## 建置與測試（tailsmachine）

```bash
NDK="$LOCALAPPDATA/Android/Sdk/ndk/29.0.13599879"
cmake -S loader -B work/build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build work/build-android
```

先把 IPA 解到 `work/ipa/`（不含 `Data/Raw` 也可以），再：

```bash
bash tools/thor-run.sh --sync MK_TEST_EXCEPTION=1
```

`--sync` 會把 `UnityFramework` 和 `Data/Managed` 推到 Thor 的 `/data/local/tmp/mk`。腳本會拿 `thor` 鎖。

完整 APK（需要 `work/unity/AndroidPlayer`：用 7-Zip 解開 Unity 官方的 Android Support 安裝程式；`work/raw-android`：IPA 的 `Data/Raw`，`AssetBundles/ios` 改名 `android`）：

```bash
python -I tools/build_apk.py      # → out/marbleknights-private.apk（只裝在自己的裝置）
bash tools/thor-push-raw.sh       # 第一次：把 Data/Raw（3.4 GB）推到 App 的外部 files 資料夾
bash tools/thor-apk.sh 45         # 安裝、啟動、抓 logcat 與截圖到 work/
```

## Windows 建置（tailsmachine）

需要 VS 2022 Build Tools、LLVM（clang-cl、lld-link）、Python 3；Unity 2019.4.17f1 Editor 與 Windows IL2CPP 模組解到 `work/unity/`（從 Unity 官方下載，用 7-Zip 解開，不執行安裝程式）。IPA 解到 `work/ipa/`。

```bash
python -I tools/build_win.py         # 組出 work/win/MarbleKnights/（官方播放器＋iOS 的 Data）
python -I tools/win_textures.py      # PVRTC → RGBA32（bundle 就地改寫，其餘放 work/win/patched-data）
python -I tools/prelink_win.py       # Mach-O 映像 → work/win/image
python -E -X utf8 tools/a64toc.py --all   # ARM64 → C（work/win/gen），拿 heavy-build 鎖
bash tools/build_win_ga.sh           # 編 GameAssembly.dll 並放進 work/win/MarbleKnights（約 5 分鐘；--runtime-only 只重編執行環境）
```

產生的東西全部在 `work/`，不進 Git。
