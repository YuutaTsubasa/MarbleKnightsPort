# Marble Knights Port（個人保存用）

把 WayForward 的 Apple Arcade 遊戲 **Marble Knights**（已下架）的 iOS 版，移植到 Android（ARM64）自己玩。做法參考 Project Dream（Sonic Dream Team 的 Mac → Windows／Android）：

- 不重新編譯遊戲。iOS 版的 `UnityFramework`（Mach-O，arm64）裡有 IL2CPP 執行環境＋遊戲程式，我們在 Android 上**直接載入這個 Mach-O**，把它需要的 iOS 系統呼叫換成 Android（bionic）的實作。
- 引擎改用同版本（Unity 2019.4.17f1）的**官方 Android 播放器 `libunity.so`**，讓它透過標準的 `il2cpp_*` API 驅動 iOS 版的遊戲程式。
- iOS 專用的東西（Metal 著色器、GameKit／Apple Arcade、GameController、觸覺回饋）另外轉換或做假的。

> **只做個人保存、自己玩，不公開發布**。IPA 和從裡面解出、產生的任何東西都不進 Git、不上雲端或 CI（見 [AGENTS.md](AGENTS.md)）。

## 現況（2026-11-03）

第一個里程碑的 **IL2CPP 那一半已經在 Thor 上跑通**（還沒接 `libunity.so`）：

- `loader/` 的 Mach-O 載入器在 Android 上載入 iOS 版 `UnityFramework`（54 MB），553 個靜態初始化全部跑完。
- `il2cpp_init` 成功，讀進 `global-metadata.dat`，列出 62 個組件（`MarbleQuest.dll` 2609 個類別…）。
- 真的執行 iOS 版編譯好的 C# 程式：`String.Concat` 回傳 "Marble Knights"；GC 正常回收。
- 受控例外可以拋出並接住（`Int32.Parse` → `FormatException`）：已把 Mach-O 的 compact unwind 轉成 DWARF 給 Android 的 libunwind 用。

下一步是接上官方 Android 播放器（需要下載 Unity 2019.4.17f1 的 Android 支援，**要先問使用者**）。完整的設計、風險與工作量估計在 [docs/design.md](docs/design.md)。

## 目錄

| 路徑 | 內容 |
| --- | --- |
| `loader/` | Android 端的 Mach-O 載入器與 Darwin→bionic 相容層（C／組合語言，NDK 建置） |
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
