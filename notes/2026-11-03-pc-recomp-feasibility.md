# 電腦版（Windows／Linux）可行性評估：靜態重編譯遊戲程式（2026-11-03）

使用者想要電腦版。iOS 的遊戲程式是 ARM64，Unity 2019.4 沒有可用的 ARM64 電腦版播放器，所以照 Recompiled 的思路：**只轉 IL2CPP 產生的遊戲程式碼**，引擎用官方 Windows 版 `UnityPlayer.dll`，IL2CPP 執行環境用 Unity 附的 libil2cpp 原始碼重新編譯，`global-metadata.dat` 原樣沿用。圖形先用 OpenGL Core（沿用現有的 Metal → GLSL 轉換），之後再研究 Vulkan（見 tailsmachine-ops `docs/decisions.md`）。

工具：`tools/recomp_survey.py`（統計）、`tools/a64toc.py`（ARM64 → C 原型）。輸出都在 `work/`（從遊戲產生，不進 Git）。

## 數字

- `UnityFramework`：`__text`（引擎＋IL2CPP 執行環境＋外掛）16.6 MB；**`__TEXT,il2cpp`（產生的程式碼）22.3 MB、585 萬條指令、112,885 個函式**，其中 73,360 個是有名字的 C# 方法（378 萬條指令），其餘是泛型實例、invoker 等。
- 方法大小：中位數 27 條、p90 121 條、最大 11,636 條。
- 指令種類很集中：`mov` 20%、`ldr` 15%、`bl` 11%、`adrp` 6%……共 138 種。間接呼叫（`blr`，虛擬呼叫）24,385 個；間接跳躍（`br`，switch 表）7,038 個；看起來是 SIMD 的指令約 6 萬（多數是 `movi`／`fmov`／`ld1` 類的搬移）。
- **例外**：只有 2,357 個方法（3.2%）有 landing pad；它們都呼叫 `_Unwind_Resume`，接住例外用 `__cxa_begin_catch`／`__cxa_end_catch`。
- **呼叫執行環境**：遊戲方法呼叫的非方法目標有 4,444 個，但集中在少數幾個：前 10 個就涵蓋 70% 的方法，前 100 個涵蓋 88.5%，前 1,000 個涵蓋 95.4%。最常見的是 `0xe22484`（38,441 個方法，應該是 `il2cpp_codegen_initialize_method` 類的 metadata 初始化）、`0xe22620`、`0xe22590`……（都在 `__text`，是 IL2CPP 的 codegen 輔助函式，名字被 strip 了，要對照 libil2cpp 原始碼認出來）。

## 原型轉換器 `a64toc.py`

- 每個函式變成 `void f_<位址>(cpu *c)`，操作一份暫存器檔（x0–x30、sp、NZCV、v0–v31）。記憶體直接存取（guest 指標＝host 指標，映像的資料段映射在 `IMG`）；`bl` 到已轉換的函式是直接的 C 呼叫，到執行環境的走 `host_call`，`blr` 走 `dispatch`，`br`（switch 表）在函式內用 `switch` 跳回 label。
- **覆蓋率：112,885 個函式中 107,214 個（95.0%）完全轉得出來；沒轉的指令只佔 0.87%**，幾乎都是向量 `mov` 的幾種寫法，其餘是零星的 NEON／浮點指令（`movi`、`neg`、`fabd`、`fccmp`、`dup`、`frintm`…）。都是補表的工作。
- 轉出來的 C（例如 `CustomAppleController.AdjustControllerIndexForDisconnect`）用 NDK 的 clang 編成 x86_64 沒有錯誤。可讀性不錯，跟原本的 ARM64 一對一。

## 剩下的工作與做法

1. **補完轉換器＋正確性**：補上剩下的指令（含 NEON 向量運算）。驗證有個很好的捷徑：轉出來的 C 也能編成 **arm64**，直接在現在的 Android 版（Thor）裡把原本的函式換成轉換版，遊戲照常跑就代表轉對了（可以一批一批換、出錯就二分）。
2. **例外**：只有 3.2% 的方法有。例外只會從呼叫發生，所以對 LSDA 裡有 landing pad 的每個呼叫各包一層 `try { … } catch (…) { 設 x0/x1; goto landing pad; }`（產生的程式碼以 C++ 編譯），landing pad 裡的 `__cxa_begin_catch` 等換成 host 版。
3. **host 端執行環境**：
   - 用 Unity 2019.4.17f1 附的 libil2cpp 原始碼編 Windows x64 版；`global-metadata.dat` 沿用。
   - 從 `__DATA` 重建 `g_CodeRegistration`／`g_MetadataRegistration`（方法指標表、泛型表、RGCTX），指標改成指到 host 端的包裝函式。每個方法依 metadata 的簽章產生一個 host 呼叫慣例的包裝（把參數放進 `cpu` 再呼叫 `f_…`），反方向（遊戲呼叫引擎的 icall、委派）也依簽章產生 thunk。
   - 認出約 100 個最常用的 codegen 輔助函式，對應到 libil2cpp 的同名函式；其餘的執行環境函式也一併對應。
   - GC：每條 guest 執行緒的堆疊與暫存器檔要登記給 Boehm GC 掃描。
   - 確認 iOS（LP64）與 Windows（LLP64）的 libil2cpp 結構位移一致（產生的程式碼寫死了 `Il2CppClass`、`MethodInfo` 等的欄位位移）。
4. **Windows 播放器整合**：官方 `UnityPlayer.dll` 載入我們的 `GameAssembly.dll`；序列化檔的平台標記、著色器轉 GLCore（沿用 GLSL ES 轉換，改版本宣告與精度）、確認 ASTC 貼圖會不會被 CPU 解壓；外掛換成電腦版（FMOD 用 Windows 版 `fmod.dll`；Game Center 區網、CloudKit 存檔、手把、震動等 proxy 的 C 程式大多能直接沿用）。

## 估計

| 階段 | 時間 |
| --- | --- |
| 1. 轉換器補完＋在 Thor 上逐批替換驗證 | 1–1.5 週 |
| 2–3. 例外、host 執行環境、註冊表、包裝函式、GC | 2–3 週 |
| 4. Windows 播放器整合、著色器、外掛 | 1–2 週 |
| 合計（Windows 版，OpenGL Core） | **約 4–6.5 週** |
| Linux 版（同一套轉出的程式碼，換播放器與系統相容層） | 再約 1 週 |
| Vulkan | 之後另外研究 |

最大的不確定在第 3 步（libil2cpp 結構位移是否一致、註冊表重建）。第 1 步做完、能在 Thor 上跑轉換版之後，估計會再更準。

## 需要先問使用者的下載

- Unity Editor 2019.4.17f1（Windows，含 libil2cpp 原始碼）與 Windows Build Support（IL2CPP）模組：從 Unity Hub／官方下載。
- FMOD 的 Windows 函式庫：fmod.com（要帳號）。

## 進度（同一天，第一階段開工）

- 使用者同意下載。Unity Editor 2019.4.17f1 與 Windows IL2CPP 模組從 Unity 官方 CDN 下載（簽章確認是 Unity Technologies），用 7-Zip 解到 `work/unity/editor`、`work/unity/winil2cpp`，沒有執行安裝程式。libil2cpp 原始碼在 `editor/Editor/Data/il2cpp/libil2cpp`，Windows 64 位元 IL2CPP 播放器在 `winil2cpp/…/Variations/win64_nondevelopment_il2cpp/UnityPlayer.dll`。
- 轉換器補上向量／NEON 與其餘指令：**112,885 個函式只剩 1 個有 2 條指令沒轉（100.0%）**。隨機 3,000 個函式（10 MB 的 C）編成 x86_64 沒有錯誤。
- **結構位移**：用 clang 的 record layout 比對 libil2cpp 的 35 個結構（`Il2CppClass`、`MethodInfo`、`Il2CppObject`、`Il2CppArray`、註冊表…）在 LP64 與 LLP64 下的位移，全部一致（唯一的差異來自測試時借用的 Android 標頭把 `uintptr_t` 定成 `unsigned long`，真正的 Windows 上是 8 bytes）。原本最大的風險之一排除。
- **Thor 驗證工具**：`a64toc.py --batch REGEX` 產生 `work/recomp/batch.c`（轉好的函式＋每個函式一個進入 thunk＋對照表）；`loader/proxy/recomp_rt.{c,S,h}` 是執行環境：`mk_rc_enter` 把參數暫存器存進 `cpu` 結構、在真正的堆疊上給一塊 guest 堆疊（GC 掃得到）；`mk_rc_host` 用 guest 暫存器呼叫原生程式碼（複製 256 bytes 的堆疊參數）；`dispatch` 查表決定走轉換版或原生版。CMake 加 `-DMK_RECOMP_BATCH=ON` 才會把 batch 編進 proxy。第一批：遊戲自己的 `Update`／`LateUpdate`／`FixedUpdate`（275 個，沒有例外區段）已經編好，等 Thor 鎖。
