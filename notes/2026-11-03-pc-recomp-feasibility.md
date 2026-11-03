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

## 方向調整：整份 UnityFramework 都轉，不重建 libil2cpp（同一天）

Android 版本來就是跑 iOS 自己的 IL2CPP 執行環境（在 `__text` 裡），外面包 Darwin 相容層。Windows 版照同一個架構，只是原生 ARM64 改成轉出來的 C：`__text` 裡的執行環境（從遊戲與 il2cpp API 走得到的約 6,800 個函式，函式指標造成的漏算另計）跟遊戲程式碼一起轉。好處：不用重建註冊表、不用逐一認出 4,444 個輔助函式；ABI 邊界只剩 il2cpp API（229 個，簽章已知）、Unity 註冊的 icall（Android 版的 icall adapter 已經有簽章）、Darwin 系統函式（Android 版的相容層要改寫成 Windows 版）。libil2cpp 原始碼當參考（結構、簽章）。

## Windows 第一個里程碑（同一天）

- `tools/build_win.py` 組出 `work/win/MarbleKnights/`：官方 `WindowsPlayer.exe`（改名）＋`UnityPlayer.dll`（附 PDB 符號，之後除錯、修補很有用）＋iOS 的 Data（序列化檔的平台改成 StandaloneWindows64＝19；引擎內建資源換成 Windows 版；`global-metadata.dat` 放到 `il2cpp_data/Metadata`；StreamingAssets 用 junction 連到 `work/raw-android`，不複製）。
- `loader/win/ga_main.c`＋`tools/gen_win_stubs.py`：先做一個只會記錄的 `GameAssembly.dll`（229 個 il2cpp API 都輸出，呼叫時寫進 `mk_ga.log`、回傳 0），用 VS 2022 Build Tools＋clang-cl 編。
- 實跑：Windows 播放器載入我們的 DLL，呼叫順序和 Android 一樣：`add_internal_call`（註冊 icall）→ `runtime_unhandled_exception_policy_set` → `set_commandline_arguments` → `set_config_dir` → `set_data_dir` → `debugger_set_agent_options` → `il2cpp_init`（stub 回 0，所以停在這裡）。
- 下一步：Windows 版的 Mach-O 載入器與 Darwin 相容層、x64 ↔ guest 的呼叫轉接，然後把整份程式碼轉出來編進 `GameAssembly.dll`，目標是讓 `il2cpp_init` 跑完。

## Windows 進度：il2cpp_init 成功（同一天）

整份 UnityFramework（224,657 個函式、約 1,000 萬條 ARM64 指令）轉成 C（151 個檔、685 MB），clang-cl 編成 157 MB 的 `GameAssembly.dll`（完整重建約 20 分鐘，拿 heavy-build 鎖）。官方 Windows 播放器載入後：

1. 553 個靜態初始化全部跑完；
2. `il2cpp_init` 回傳 **1**：iOS 的 IL2CPP 執行環境在 x64 上初始化成功（global-metadata.dat 載入）；
3. Unity 開始呼叫 icall（`ScriptableObject::CreateScriptableObject` 等）、載入場景。

途中修掉的問題（都記在 commit 裡）：bind 位址沒有 64 位元環繞（macho.py）、跳躍表比對位址錯、posix_memalign 配置的記憶體被 free（heap corruption；改成統一的配置器）、mmap 超過檔尾（Windows 不允許）、icall 指標被包兩次。

工具：`tools/run_win.sh`（跑、看兩份 log）、`MK_TRACE_IMPORTS=1`（記錄每個匯入函式呼叫）、`MK_WATCHDOG=秒`（定期取樣主執行緒的堆疊，名字從 PDB 來）。

下一步：
- CoreFoundation（`CFLocaleCopyCurrent` 等傳回 0 之後 strlen(NULL) 當掉）：照 Android 的 darwin_cf.c 補。
- 著色器：播放器說「shader is not supported on this GPU」——資料裡只有 Metal／GLES3 程式，要照計畫轉成 OpenGL Core 並用 `-force-glcore` 啟動。
- 外掛（手把、存檔、Game Center 區網…）在 Windows 上要換成 host 版本。
- GC 的執行緒掃描（host 暫存器／堆疊）與效能還沒驗證。

## Windows：第一個畫面（同一天）

`-force-gles30` 下，官方 Windows 播放器（NVIDIA 的 OpenGL ES 3.2 context）畫出了標題畫面的 Marble Knights 標誌與「© 2020 WayForward Technologies」。**Android 版轉好的 GLES3 著色器在 Windows 上直接能用**，暫時不需要轉 OpenGL Core；PVRTC 貼圖 Unity 自己用 CPU 解壓。

這一段修掉／補上的：
- CoreFoundation（locale、CFString、CFUUID、CFData）；未知的資料匯入每個符號只給一個位址（呼叫端比對位址）。
- `/dev/urandom`（Windows RNG）、`lstat`、`difftime`；`Application.platform` 回報 IPhonePlayer（資料是 iOS 版，AssetBundles/ios）。
- StreamingAssets 用 hard link 組成 iOS 的名字（813 個連結，不複製）。
- UnityPlayer.dll 的記憶體修補（PDB 找位址，`tools/win_sym.py`）：`InitializeGfxDevice` 允許強制的渲染器、`TestAssetBundleCompatibility` 兩個多載回傳 true、`SerializedFile::ReadMetadata` 的平台集合加上 iOS／Android。
- icall 依 metadata 的簽章轉成 Windows x64 呼叫（浮點參數、位置對應），選「internal call」的那個多載（`GameObject.GetComponent` 有好幾個）；`mk_win64_call` 有 unwind 資料，引擎丟出的託管例外能穿過它回到 guest。
- OpenGL 下 Windows 播放器不能解碼影片：`VideoPlayer.get_time` 回報已播完，開場影片直接跳過。
- 引擎用 NULL 方法呼叫（被 iOS 版剝掉的 Analytics 等）：照 Android 版回 0。

診斷工具：`MK_TRACE_THROW=1`（每次 throw 的訊息和堆疊位址，用 llvm-symbolizer 對 PDB 解）、`il2cpp_runtime_invoke` 回傳例外時記錄方法名稱。

還沒驗證：背景（Android 標題畫面是藍色格子背景，這裡是黑的）、輸入、聲音（FMOD）、存檔、效能（CPU 約一個核心）、GC 掃描。

## Windows：進到遊戲（同一天）

開場故事圖正常顯示，按 Enter（鍵盤模擬手把 A）就進到教學的第一個對話框（「Welcome to Marble Knights. Let's have a quick lesson.」，手把焦點在 Continue）。新存檔會先跑教學，所以還沒看到主選單本身。

這一段修掉／補上的：
- **轉換器補齊 NEON**：整份映像沒轉的指令從 2,518 降到 92（剩下多半是 `udf`，本來就是陷阱）。補了純量 pairwise（`faddp s, v.2s` 等）、post-index 的 `ld1`／`ld1r`、多暫存器單 lane 的 `ld2..4`／`st2..4`、向量立即值 `orr`／`bic`、`fcvtn`／`fcvtl`（含 f16）、向量整數↔浮點（含定點）、整數 pairwise min/max、飽和加減、`sqdmulh`、`srshr`、`rev16/32/64`、窄化家族（`addhn`、`xtn`、`shrn` 與飽和／捨入版）、加寬家族（`saddw`、`uaddl`、`ushll`、`sxtl`…）、`fcvtzu w, s, #fbits`、無號的 `fcvtmu`／`fcvtpu` 等。capstone 把大於 9 的 lane 印成十六進位（`v0.b[0xf]`），先轉成十進位。
- `a64toc.py --all` 只重寫內容有變的 chunk，之後小修只重編幾個 chunk（這次 4 個，約 2 分鐘）。
- **原生外掛替代**（`loader/win/plugins_win.c`，用 `mk_override_function` 蓋掉轉好的函式，位址跟 Android 版相同）：CloudKit 存成遊戲資料夾的 `save\icloud\*.bin`（結果延遲 1.5 秒、在主執行緒送回）；Game Center 回報已登入的本機玩家，Windows 還沒有區網配對，所以 `IsMultiplayerRestricted` 回 1；AppleGameController 用 XInput，鍵盤也接到 0 號手把（方向鍵／WASD、Enter／空白＝A、Esc／Backspace＝B、X、C＝Y、Q／E 肩鍵、1／3 扳機、Tab、P）；觸覺回饋與指南針不做事。回呼在 `il2cpp_runtime_invoke` 進入時於主執行緒處理。
- **Rewired 初始化失敗的原因**：Rewired 記錄一則錯誤時，`System.Console` 的靜態建構子要開 stdout，但 GUI 程式沒有 fd 0–2（`_fstat64` 失敗 →「Invalid handle to path …/[Unknown]」），整個 `InputManager_Base.Initialize` 跟著失敗，後面 `CustomController`、`CustomTiltControl` 等一連串 NullReference。現在 fd 0–2 的 `fstat` 回報字元裝置，寫到 1／2 的文字進 `mk_ga.log`。
- FMOD Studio 用 NOSOUND 輸出啟動成功（還沒有聲音）。

Rewired 會印「ControllerDataFiles is missing critical data!」，要和 Android 版的 log 比對是不是本來就有。

還沒驗證：主選單之後的流程、實際關卡、XInput 手把（只測了鍵盤）、聲音、存檔讀回、效能、GC 掃描。

## Windows：聲音、3D、觸控（2026-11-03 深夜～11-04）

- **聲音**：`loader/win/fmod_win.c`，跟 Android 一樣讓 FMOD 用 NOSOUND 混音，主 channel group 開頭放一個擷取 DSP 把混音寫進環形緩衝區，waveOut 執行緒播放（FMOD 2.00.10，24 kHz）。DSP 的 read 是 host callback（只有整數／指標參數）。使用者確認有聲音。
- **3D 全黑**：場景設定正常（`MK_DUMP_RENDER=1` 用引擎 icall 印霧、環境光、太陽光），原因是貼圖：iOS 資料大多是 PVRTC 4bpp，桌機 GPU 不支援，Windows 播放器在 CPU 解壓時有 mipmap 的貼圖會變黑（只剩高光，角色像一堆點點）。`tools/win_textures.py` 在建置時把 bundle 裡的 PVRTC 全部解成 RGBA32（含每一層 mip，解碼器 `tools/pvrtc_decode.c`，用 ctypes 呼叫）：210 個 bundle、1,818 張；`resources.assets`／`sharedassets0.assets` 另外 28 張（沒有 type tree，借 bundle 的 Texture2D 樹），放在 `work/win/patched-data`，`build_win.py` 會優先用。StreamingAssets 原本是硬連結到 Android 資料，轉換時寫新檔取代連結，原檔不動。教學關卡、標題的藍色格子背景都正常了。
- **觸控**：遊戲讀 `Input.touchCount`／`GetTouch`，Windows 的滑鼠不會產生觸控。在註冊 icall 時把這兩個換成包裝：有真的觸控就照引擎的，否則滑鼠左鍵當 0 號手指（每格推進 Began／Moved／Stationary／Ended），`touchSupported` 回 true。log 確認觸控事件有送出；遊戲裡實際效果待使用者測。
- **手把**：XInput 看得到手把（user 0），遊戲每格都有呼叫 `AGCGetController`，並把 0 號手把認成「Xbox Wireless Controller」；使用者回報沒反應，加了狀態變化的 log 待查。
- 測試時要注意：自動送滑鼠／鍵盤會搶到使用者正在用的游標，使用者在用電腦時不要做。
- 存檔（PlayerPrefs）在登錄檔 `HKCU\Software\WayForward Technologies\Marble Knights`；測教學時清掉，原本的備份在 `work/win/2026-11-03-playerprefs-backup.reg`。

## Windows：操作與顯示（11-04）

- `MarbleKnights.exe` 換成自己的啟動程式（`loader/win/launcher.c`）：呼叫 UnityPlayer 的 `UnityMain` 並加上 `-force-gles30`。直接點兩下原版播放器會用 D3D11，整個畫面變粉紅（只有 GLES3 著色器）。
- 關閉遊戲：`il2cpp_shutdown` 不會返回（執行環境停 GC 與 guest 執行緒時卡住）；播放器最後才呼叫它，PlayerPrefs 已經寫好，所以直接結束行程。
- 游標：iOS 版會隱藏、鎖定游標，Windows 上忽略 `Cursor.visible`／`lockState`。
- **手把**：Rewired（iOS 版，平台寫死成 iOS）是從 Unity 的搖桿輸入讀手把、用 Unity iOS 的按鍵配置對應。現在 `GetJoystickNames`／`GetKeyInt`（搖桿鍵碼）／`GetAxis(Raw)("JoyNAxisM")` 都照 iOS 播放器回報 extended gamepad 的方式從 XInput 回答：名字 `[extended,wireless] joystick N by Xbox Wireless Controller`，A=14、B=13、X=15、Y=12、十字鍵 4–7、肩鍵 8/9、扳機 10/11、暫停=0，搖桿軸 1–4（Y 向下為正）。使用者確認可以玩。
- Alt+Enter／F4：`Screen.SetResolution` 在全螢幕（螢幕大小）與 1280×720 視窗之間切換（使用者確認）。
- AppleGameController 替代品不再把鍵盤當成永遠連著的 0 號手把（鍵盤由 Rewired 自己讀）；一直有手把可能讓遊戲停在手把模式、不接受觸控。觸控是否因此恢復待測。
- 待查：傳送板的藍色光和地板有 Z-fighting 般的閃爍（推測：iOS 是 Metal 的浮點深度＋reversed Z，這裡是 GL 的 24-bit 深度，貼花的深度差不夠）；只在全螢幕時，第一關 Loading 前會閃一格壞掉的畫面。

## Windows：開頭影片、觸控追查（11-04）

- **開頭 Apple Arcade／WayForward 標誌一片黑**：這些是 VideoClip（H.264 mp4）。Windows 播放器只在 D3D11／12 下用 Media Foundation 解碼影片，這個版本跑 GLES3，Player.log 寫「Microsoft Media Foundation video decoding to texture disabled」。Unity 內建的 VP8 解碼器不挑圖形 API，所以 `tools/win_videos.py` 用 ffmpeg 把 11 段影片（9 段 Apple Arcade 各比例、WF 標誌、`Intro_Video`）轉成 VP8＋Vorbis 的 WebM（最高 1080 行），改 VideoClip 的大小、尺寸與路徑副檔名。bundle 直接改寫；`sharedassets0` 的放進 `work/win/patched-data`。實際畫面待使用者確認。
- **觸控**：滑鼠觸控事件確實送進遊戲（log 有拖曳），但關卡裡沒有呼叫到 `RectTransformUtility.ScreenPointToLocalPointInRectangle`，表示畫面上的搖桿（`UIJoystick`）沒收到 pointer 事件。遊戲的觸控 HUD（`iOSMobileHUD`／`iOSAttackButton`）只在「傾斜／觸控控制器」被玩家使用時（`UI_ControllerWidget::IsTiltControllerUsed`，比對使用中控制器的名字）才作用；存檔是 `controlMethod: Touch`。使用者測試那次手把開遊戲時是連著的，中途才斷線，所以手把可能一直佔著玩家。下一步：手把關掉再開遊戲測；仍不行就在關卡裡記錄使用中的控制器。
