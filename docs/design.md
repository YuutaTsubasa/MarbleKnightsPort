# 設計文件：iOS 版 Marble Knights → Android

2026-11-03，Claude（Opus 5.5）。第一版，寫在里程碑 1 的 IL2CPP 那一半跑通之後。

## 1. 結論（先講）

**可行，而且最難驗證的那一半已經在 Thor 上跑起來了。** iOS 版 `UnityFramework` 原封不動地在 Android 上載入，`il2cpp_init` 成功、讀進 metadata、執行遊戲編譯好的 C# 程式碼、GC 與受控例外都正常。這證明「把 iOS 的 IL2CPP 程式碼當成函式庫，在 Android 上換掉系統層」這條路走得通，而且 ABI 的坑是有限、可以一個一個補的。

還沒驗證、也最可能卡住的是**引擎那一半**：官方 Android 播放器 `libunity.so` 能不能接受 iOS 版的 `il2cpp_*`（同一個 Unity 版本，理論上 API 相同）、能不能讀 iOS 建置的資料檔，以及 **Metal 著色器要轉成 GLES／Vulkan**——這是最大的一塊工作。估計到能玩大約 **3–6 週**的集中工作（見第 7 節）。

## 2. 跟 Project Dream 的差別

| | Project Dream（Sonic Dream Team） | 我們（Marble Knights） |
| --- | --- | --- |
| 來源 | Mac 版，`GameAssembly.dylib` 是**獨立的** libil2cpp＋遊戲程式 | iOS 版，引擎、libil2cpp、遊戲程式、FMOD、外掛**全部靜態連在一個 `UnityFramework`** |
| 架構 | x86_64／arm64 都有 | **只有 arm64** → Android（arm64）最直接；Windows x64 需要模擬，先不做 |
| 系統層 | macOS → Windows | iOS（Darwin）→ Android（bionic，也是 Unix 系，比 Windows 接近） |
| 著色器 | Metal → 目標 API | 同（Metal → GLES3 或 Vulkan） |

因為 iOS 是一整包，我們把 `UnityFramework` 當成「libil2cpp＋遊戲」使用：它對外公開了 229 個 `il2cpp_*` 函式（含 `il2cpp_add_internal_call`），裡面的 iOS 引擎程式碼雖然也在，但我們不呼叫它（只讓它的靜態初始化跑完，因為 Unity 覆寫了 `operator new`，libil2cpp 也透過它配置記憶體）。

## 3. UnityFramework 的分析結果

細節與數字在 [`notes/2026-11-03-unityframework-analysis.md`](../notes/2026-11-03-unityframework-analysis.md)。重點：

- `__TEXT,__text` 17 MB（引擎＋libil2cpp＋FMOD＋外掛），`__TEXT,il2cpp` 23 MB（**IL2CPP 產生的遊戲程式碼獨立一個 section**，112,885 個函式），全部 224,657 個函式。
- 匯入 896 個符號，來自 24 個系統框架。用呼叫圖算過：**libil2cpp 與遊戲程式碼實際碰得到的只有** libSystem 約 215 個、libc++ 49 個、CoreFoundation 9 個、ObjC runtime 10 個；UIKit、Metal、AVFoundation 等都只有引擎在用。
- 遊戲程式碼不經過 icall、**直接呼叫**的原生函式（`DllImport("__Internal")`）：FMOD 553 個（`FMOD5_*`／`FMOD_Studio_*`，靜態連結在裡面）、zlib 包裝 5 個（`CreateZStream` 等），以及約 60 個沒有名字的 ObjC 外掛函式（GameController、NSCalendar、NSLog、Game Center、觸覺回饋、指南針）。
- 引擎相關的 icall 名稱約 1,200 個（字串表裡 2,402 條含簽章與不含簽章的版本）；其中 iOS 專用、Android 播放器不會註冊的約 30 個：`UnityEngine.iOS.Device::*`、`GameCenterPlatform::*`、`GcLeaderboard::*`、`UnhandledExceptionHandler::iOSNativeUnhandledExceptionHandler`。
- 素材：`Data/` 的序列化檔目標平台是 iOS（9）；著色器**只有 Metal**（MSL 原始碼，`resources.assets`、`sharedassets0`、`unity_builtin_extra` 裡都是 `metal_stdlib`，沒有 GLES）；`Data/Raw/AssetBundles` 約 850 個檔、3.3 GB；聲音是 FMOD Studio 的 `.bank`。
- 組件：62 個，包括 `MarbleQuest.dll`（2,609 類別）、`Rewired_Core`（輸入）、`FMODCSharp`、`Zenject`、`Cinemachine`、`AppleGameController`、`GameCenter`、`NiceVibrations`、`MagneticHeading`、`UnityEngine.AndroidJNIModule`（有，之後用得到）。

## 4. 為什麼在 Android 上直接載入 Mach-O，而不是先轉成 ELF

選了**執行期 Mach-O 載入器**（`loader/src/macho_loader.c`）：

- 不管哪種做法，系統呼叫都要換、ABI 差異都要補；轉成 ELF 只是把「套用 rebase／bind」從執行期搬到離線，省不了工作，反而要多寫一個 ELF 產生器、還要處理 Mach-O 的 section 對齊與 16 KB 分頁假設。
- 載入器很小（約 350 行），完全掌握每個匯入綁到哪裡；沒實作的匯入自動綁到「陷阱」（呼叫時記下符號名、回傳 0），一次執行就能列出還缺什麼。
- 例外處理一樣要自己處理（見下），跟格式無關。
- `__TEXT` 直接從檔案 `mmap` 成 R+X（Android 允許 shell 與 App 執行自己資料夾裡的檔案對映），資料段複製一份再套用 rebase／bind。不改任何一個指令。

## 5. 架構

### 5.1 現在的原型（`loader/probe/il2cpp_probe`）

```
il2cpp_probe（Android 執行檔）
 ├─ macho_loader   對映 UnityFramework、rebase、bind（匯入 → Darwin 相容層）
 ├─ macho_unwind   compact unwind → DWARF .eh_frame + .eh_frame_hdr，假的 dl_iterate_phdr 模組
 ├─ darwin_libsystem / darwin_cxx / darwin_cf   iOS 系統函式的 Android 實作
 └─ trap.S         沒實作的匯入、可變參數轉接、setjmp、___chkstk_darwin
流程：載入 → 跑 553 個靜態初始化 → il2cpp_set_data_dir(Data/Managed) → il2cpp_init → 呼叫受控方法
```

### 5.2 目標（APK）

```
官方 Unity 2019.4.17f1 Android 播放器（Java UnityPlayerActivity、libmain.so、libunity.so）
 └─ dlopen("libil2cpp.so") ← 我們的代理函式庫
      ├─ 匯出 il2cpp-api-functions.h 的全部函式，轉呼叫 UnityFramework 裡同名的 _il2cpp_*
      ├─ 內含現在的載入器與 Darwin 相容層
      ├─ 補 iOS 專用 icall（iOS.Device、GameCenter…）與 ObjC 外掛函式的 Android 版
      └─ FMOD 的輸出改走 AAudio／OpenSL
assets/bin/Data   iOS 的 Data（序列化檔的平台欄位改成 Android、著色器補上 GLES3／Vulkan 程式）
大型 AssetBundle  放 /sdcard/Android/data/<pkg>/files（3.3 GB，不塞進 APK）
```

`libunity.so` 註冊自己的 icall 時會呼叫我們轉過去的 `il2cpp_add_internal_call`，存進 iOS libil2cpp 的表；遊戲程式碼呼叫 icall 時用名字查（`il2cpp_codegen_resolve_icall`），就會拿到 Android 引擎的實作。兩邊是同一個 Unity 版本，物件與 icall 的記憶體配置應該一致。

## 6. 找到並處理的 ABI 差異（Darwin arm64 vs Android AAPCS64）

這些都已經在相容層裡處理；之後碰到新的就補在這裡：

1. **C++ 建構子與解構子回傳 `this`**（x0）。呼叫端會直接用回傳值（`std::string` 複製建構後馬上 `stur q0, [x0, #0x18]`）。我們實作的 libc++ 建構子／解構子都要回傳物件。這是讓 `il2cpp_init` 過關的關鍵。
2. **可變參數全部放在堆疊**，不像 AAPCS64 先用暫存器；`va_list` 是指向堆疊的 `char *`。`open`、`fcntl`、`ioctl`、`printf` 系列用組合語言轉接（`trap.S`），`vsnprintf` 系列把 `char *` 包成「暫存器區是空的」AAPCS64 `va_list`。
3. 非可變參數的堆疊參數照**自然大小**排（不是每個 8 bytes）；目前碰到的函式都沒差，之後接外掛時要注意。
4. 呼叫端負責把 8／16 位元參數延伸到 32 位元；Android 端呼叫 Mach-O 函式（例如 `libunity.so` 經 invoker 呼叫遊戲程式）時理論上可能出問題，目前沒看到，列為風險。
5. **libc++ 用 alternate string layout**（`ldrsb [x, #0x17]` 判斷長短字串），命名空間是 `std::__1`；Unity 覆寫的 `operator new/delete` 在映像檔裡，我們的 `std::string` 實作也用它。
6. **errno**：1–34 跟 Linux 一樣，35 以後不同（`EAGAIN` 35、`ETIMEDOUT` 60…），在 `__error()` 與失敗路徑轉換。
7. 結構：`stat`（144 bytes）、`dirent`（`d_name[1024]`）、`utsname`（5×256）、`timeval`（`tv_usec` 是 32 位元＋未初始化的填充）、`sigaction`、`sockaddr`（BSD 有 `sa_len`）、`addrinfo`（`ai_addr`／`ai_canonname` 順序相反）。前四個已轉換；後三個要等用到網路時再做。
8. 旗標與編號：`open`／`mmap`／`fcntl`、`sysconf`、`sysctl`、`setlocale` 的類別、訊號編號、`PTHREAD_MUTEX_RECURSIVE`（Darwin 2／bionic 1）、`PTHREAD_CREATE_DETACHED`（2／1）。
9. **pthread 靜態初始化帶簽章**（`0x32AAABA7` 等）：第一次使用時換成 bionic 的物件；Darwin 的物件比 bionic 大，放得下。
10. **jmp_buf**：Darwin 192 bytes，bionic 的 `setjmp` 會寫 256 bytes → 自己實作。
11. **Mach thread／semaphore**：Boehm GC 在 Darwin 上用 `thread_suspend`／`thread_get_state` 停住其他執行緒 → 用即時訊號＋`ucontext` 模擬（已實作，還沒在多執行緒 GC 下壓力測試）。`dispatch_semaphore_*` 用 POSIX semaphore。
12. **例外處理**：Mach-O 用 compact unwind，Android 的 libunwind 只讀 DWARF。我們把 111,927 個函式的 compact unwind 轉成 DWARF FDE（4.5 MB），做成 `.eh_frame_hdr` 二分搜尋表，再在 `dl_iterate_phdr` 裡多回報一個假模組。personality 用 NDK 的 `__gxx_personality_v0`，LSDA（`__gcc_except_tab`）格式相同可以直接用。
13. `_dyld_*`、`getsectbynamefromheader_64`：讓 Boehm GC 把 `UnityFramework` 的 `__DATA` 當成根集合掃描。

## 7. 剩下的工作與估計

以「一個 Claude 工作階段集中做」的天數估計，範圍很寬是因為著色器與 `libunity.so` 還沒實際看過。

| # | 工作 | 估計 | 風險 |
| --- | --- | --- | --- |
| A | **接 `libunity.so`**：做 `libil2cpp.so` 代理、包成最小 APK（官方播放器＋iOS 的 Data），確認 API 集合一致、icall 註冊與解析、補 iOS 專用 icall 的假實作，讓引擎走到第一個場景的 `Awake`／`Start` | 3–6 天 | 中：要先下載 Unity 2019.4.17f1 Android Build Support（**要問使用者**）。`libunity.so` 若直接呼叫非 API 的 libil2cpp 內部函式就要另外找 |
| B | **資料檔**：序列化檔與 AssetBundle 的目標平台欄位、確認貼圖格式（ASTC 在 Adreno 可直接用；若有 PVRTC 要轉）、大型 bundle 放外部儲存 | 2–4 天 | 中：Unity 可能還會檢查其他平台相關的序列化欄位 |
| C | **著色器**：Metal（MSL 原始碼）→ GLES 3.0 或 Vulkan。做法是寫 Unity HLSLcc 產生的 MSL 的轉譯器（結構很固定），把 GLES3 子程式補進 Shader 物件；或在執行期攔截 `libunity.so` 的著色器建立 | 7–15 天 | **高**：這是最大的一塊。要先統計有幾個不同的著色器與變體 |
| D | **原生外掛**：FMOD 輸出（AudioUnit／AVAudioSession → AAudio，保留 iOS 版 FMOD，`.bank` 不用動）；約 60 個 ObjC 外掛函式改成 Android 版（手把用 Rewired／Unity 的 Android 輸入、觸覺回饋、指南針做空的）；需要一個把產生碼位址對回 C# 方法名的工具 | 3–5 天 | 低到中 |
| E | **系統整合**：存檔路徑、Game Center／Apple Arcade 登入做假的、CloudKit 存檔改本機、`Application.platform`（可能要讓遊戲以為還在 iOS）、觸控與螢幕比例、App 前後景切換 | 3–5 天 | 中 |
| F | 打包、在 Thor 上長時間遊玩測試、效能 | 2–4 天 | 低 |

**合計約 20–39 天（3–6 週）**。比較早能看到畫面的路線：A → B → C 先做最少量的著色器（例如 UI 與不透明物件）→ 第一個場景能畫出來，大約 2 週。

### 7.1 Windows x64

iOS 版只有 arm64 機器碼，Windows x64 要嘛做 arm64 → x64 的指令轉譯（等於寫模擬器），要嘛在 Windows 上再跑一層 Android（WSA 已停止支援），都不划算。sonicmachine 是 Windows on ARM，但 Unity 2019.4 沒有 Windows arm64 播放器。**建議 Windows 先不做**；Android 版完成後，用 Thor 或其他 Android 裝置玩。

## 8. 下一步

1. ~~下載 Unity 2019.4.17f1 Android Build Support~~（2026-11-03 使用者同意，已完成）。
2. ~~做 A：`libil2cpp.so` 代理＋最小 APK~~（已完成，見第 9 節）。
3. **C：著色器**。先統計數量、變體與用到的 MSL 功能，再決定寫轉譯器還是執行期攔截。
4. 片頭影片（Apple Arcade 標誌）放在壓縮的 AssetBundle 裡，Android 的影片播放器讀不到（`AndroidVideoMedia::OpenExtractor could not translate archive:/CAB-…resource`）→ 跳過，或解出來放成檔案。

## 9. 接上官方 Android 播放器（2026-11-03）

- 從 download.unity3d.com 下載 `UnitySetup-Android-Support-for-Editor-2019.4.17f1.exe`（239 MB，簽章 Unity Technologies ApS 有效），用 7-Zip 解開，不執行安裝程式。放在 `work/unity/`。
- `libunity.so` 用 dlsym 找的 222 個 `il2cpp_*` 函式，**全部**在 iOS 版 `UnityFramework` 的 229 個匯出裡。
- 代理 `libil2cpp.so`（`loader/proxy/`）：被載入時從 App 的 native lib 資料夾載入 `libmkframework.so`（就是 `UnityFramework` 改名，讓安裝程式把它解出來），跑初始化，把 229 個匯出用跳板轉進去。另外用 C 包了幾個：
  - `il2cpp_runtime_invoke*`、`il2cpp_method_get_param_count` 收到 NULL 時不當機：iOS 版剝掉了 `UnityAnalyticsModule`，但 Android 引擎載入場景時還是會呼叫 `AnalyticsSessionInfo.CallSessionStateChanged`。
  - `il2cpp_add_internal_call`：換掉 `Application.get_streamingAssetsPath`，指到外部儲存（見下）。
  - `il2cpp_set_data_dir`：記下 App 的外部 files 資料夾。
- **在記憶體裡改 `libunity.so` 兩個指令**（不改檔案）：`SerializedFile::ReadMetadata<false/true>` 讀出檔案的目標平台後跟 13（Android）比較，不同就拒絕載入。把 `ldr w1, [sp, #0xd8]` 改成 `mov w1, #13`（位移 0x6207e4、0x621560，用 Unity 自己的 `libunity.sym.so` 找到；改之前先比對原指令）。這樣 iOS 建置的 `globalgamemanagers`、場景，**以及 3.3 GB 的 AssetBundle 都不用重新打包**。
- APK（`tools/build_apk.py`，產物在 `out/`）：官方的 `classes.dex`＋從 `Source/` 編的 `UnityPlayerActivity`（`classes2.dex`）、`libmain.so`、`libunity.so`、我們的 `libil2cpp.so`、`libmkframework.so`、`assets/bin/Data`（iOS 的 Data，不含 Raw，不壓縮）。用本機 debug 金鑰簽。
- StreamingAssets（`Data/Raw`：AssetBundle＋FMOD `.bank`）放在 Thor 的 `/sdcard/Android/data/com.yuutatsubasa.marbleknights/files/Raw`（`tools/thor-push-raw.sh`）。遊戲用平台名稱決定資料夾，所以 `AssetBundles/ios/ios` 改名成 `AssetBundles/android/android`。adbd 在這裡建不了多層資料夾、一次推整個資料夾也會中斷，所以先用 `adb shell mkdir -p` 再逐檔推。
- 結果（Thor）：`il2cpp_init` 在 `libunity.so` 裡回傳 1 → 讀 `globalgamemanagers` → GLES3 初始化 → `PlayerStartFirstScene` → 遊戲腳本開始跑（Apple Game Controller 管理器、AchievementTracker、「Initialized data!」、Bootstrap、AssetBundle 載入）。畫面是**全螢幕洋紅色**：Unity 的錯誤著色器，因為著色器只有 Metal 版（log：`Desired shader compiler platform 5 is not available in shader blob`）。

估計更新：A 與 B 大部分已經做完。剩下最大的是 **C（著色器）**，其次是 D、E。到能玩估計再 **2.5–5 週**。

## 10. 著色器：Metal → GLES3（2026-11-03）

**做法（離線）**：在每個 Shader 物件加上 GLES3（shader compiler platform 9）程式，跟一般 Android 建置的資料一樣，執行期不用攔截。不選執行期攔截的原因：Unity 的 GLES 程式是**一段合併的 GLSL**（`#ifdef VERTEX … #ifdef FRAGMENT …`，放在 vertex subprogram，fragment subprogram 是空的），而 Metal 的 VS、FS 是分開建立的，執行期配不成對。

- **調查**（`tools/shader_survey.py`）：151 個 Shader、1,481 段不同的 Metal 程式（421 VS、1,060 FS，8 MB MSL）。MSL 是 HLSLcc 產生的，非常規律。注意：blob 的 subprogram 表每筆 12 bytes（offset、length、segment），不是 8 bytes；調查用的是舊算法，數字偏少。
- **轉譯器**（`tools/msl2glsl.py`）：cbuffer → 一般 uniform、`Mtl_VertexIn` → `in_<語意>`、stage 介面 → `vs_<語意>`、`SV_TargetN` → `layout(location=N) out`、`.sample/.sample_compare/.read` → `texture/textureLod/texelFetch`、型別與內建函式改名、bool 向量比較 → `lessThan()` 等。`tools/test_msl2glsl.py` 用 glslangValidator 編全部：1,447/1,481 單獨可編。
- **blob 格式**：照 `libunity.so` 的 `LoadGpuProgramFromData` 讀出來的順序寫（記在 `tools/shader_gles.py` 檔頭）：參數表一定先有一個空名字的 cbuffer；GLES 的鬆散 uniform 放在 `$Globals`；資源表裡貼圖多一個 dim 欄位。
- **產生**（`tools/shader_gles.py`、`tools/patch_shaders.py`）：每個 pass 把每個 Metal VS 變體配上關鍵字最接近（FS 關鍵字是 VS 的子集合、最多者）的 FS，翻成合併的 GLSL，建 blob 與 parsed form（`$Globals` 用新的 float 排列、貼圖重新編 unit、名稱補進 pass 的 `m_NameIndices`），加上平台 9。結果：1,891 個 Shader 實例、198,312 筆 GLES3 變體、225 個檔案；**5,299 段合併程式裡 5,298 段能被 glslang 編譯並連結**。沒做：GPU instancing（cbuffer 是 struct 陣列，9,222 筆）與粒子的 device buffer（1,500 筆）。
- 工具：`unity_assets.py` 能讀寫 UnityFS／SerializedFile／type tree，原樣讀寫回去逐 byte 相同。AssetBundle 區塊旗標要用 3（LZ4HC），不能多加 0x40，否則 Unity 解壓失敗。
- `unity default resources` 直接換成 Android 播放器內附的版本。
- **圖形 API**：iOS 的 BuildSettings 只列 Metal，Android 播放器會退回 GLES 2.0（要 platform 5）。改成啟動參數 `-force-gles30`（編進我們的 `UnityPlayerActivity`），並在記憶體裡改 `InitializeGfxDevice` 的檢查（0x88ba00，`b.ne` → `b`），否則會出現「Unable to initialize the Unity Engine Gfx API」。
- **Thor 上的結果**：GLES3 啟動，`Desired shader compiler platform … not available` 的錯誤全部消失，log 裡沒有任何 GLSL 編譯或連結錯誤，畫面也不再是洋紅色。但還**沒看到畫面內容**：片頭影片之後遊戲出現 NullReferenceException 就停在黑畫面。iOS 外掛（Game Center、Apple Arcade）的 ObjC 呼叫目前都被陷阱回傳 0，最可能是這裡。下一步是第 7 節的 D、E（原生外掛、系統整合），之後才能用眼睛確認著色器。
- 片頭影片：Android 的 VideoPlayer 只能播放 bundle 裡沒壓縮的影片。`tools/repack_videos.py` 改成不壓縮重新打包，但仍然「bundle had some problem while loading」（未解決，遊戲會略過片頭）。

## 11. 開機到 3D 場景（2026-11-03，除錯）

依序解掉的問題（每一項都在 `loader/proxy/proxy.c`）：

1. **受控例外追蹤**：`__cxa_throw` 綁到我們的包裝，每個受控例外記下型別、訊息、回溯；回溯的方法名稱來自執行期建的方法表（IL2CPP API，含巢狀型別，寫到裝置上的 `files/methods.txt`），另外把每個 frame 的 x19–x28 裡的物件型別和字串值印出來（泛型共用程式碼在 API 裡查不到名稱，看暫存器最快）。
2. CoreFoundation 的 locale key 原本都是 NULL，語言和國家撞在一起變成 `us-us-us`；改成各自不同的常數字串。
3. 自己寫的 bundle 把 `.resS`／`.resource` 節點標成 flag 4（序列化檔），應為 0；俄文字型與片頭影片因此載入失敗。改正後片頭影片可以播放。
4. **FMOD**：iOS 的音訊輸出走 AudioUnit／AVAudioSession（都是空的），初始化失敗後一連串例外。把匯出的 `FMOD_Studio_System_Initialize`（16 bytes 的跳板）換成我們的函式：先把 core system 設成 `FMOD_OUTPUTTYPE_NOSOUND` 再呼叫原本的實作。目前**沒有聲音**，之後要換成 AAudio 輸出。
5. iOS 專用 icall（`iOS.Device`、Unity 的 GameCenterPlatform）自己註冊假的實作。
6. **iCloud（CloudKit 外掛）**：把 `_CKInitialize` 等 8 個原生函式換成「本機檔案版 iCloud」：帳號可用，紀錄存在 `files/icloud/<record>.bin`，照樣呼叫 C# 的回呼。
7. **Game Center（遊戲自己的 GameCenter.dll 外掛）**：`_GCInitialize` 立刻回呼 `OnAuthentication(true)`，`_GCLocalPlayer` 回傳本機玩家。沒有這個，存檔系統等不到登入，標題畫面一直在「載入中」、不接受按鍵。
8. **icall 的 ABI 轉接（重要）**：Apple arm64 的堆疊參數照自然大小緊密排列、而且要求被呼叫端把小於 32 位元的回傳值延伸；Android 的 AAPCS64 每個堆疊參數占 8 bytes、回傳值上半部未定義。`TextGenerator.Populate_Internal` 有 9 個堆疊參數，libunity 把兩個 int 讀成一個指標（`0x5000000004`）而當機。做法：`il2cpp_init` 之後，對引擎註冊的每個 icall 用 IL2CPP API 查出受控簽章，需要時改註冊成一段轉接程式（`trap.S` 的 `mk_icall_adapter`：把 Apple 排列的堆疊參數搬成 AAPCS 排列、延伸 bool／byte／short 回傳值）。7,638 個 icall 裡包了 191 個（190 個是小型回傳值，1 個有堆疊參數）。

結果（Thor）：片頭影片 → 標題畫面（UI、TextMeshPro 文字、觸控與手把提示都正確）→ 點一下開始 → 存檔版本更新對話框 → 「Yes」→ **3D 場景正確繪出**（光照、貼圖、植物、水面動畫），截圖在 `work/2026-11-03-first-3d-scene.png`。

接著要處理：教學關卡 `TutorialEventManager.Start` 裡 `iOSAttackButton`（螢幕上的觸控按鈕）的 NullReferenceException；聲音（FMOD → AAudio）；俄文 bundle；GPU instancing 的著色器變體。

## 12. 進入可操作的教學關卡（2026-11-03）

1. `iOSAttackButton.ToggleTutorialAttackVisual` 的例外：引擎回報 `Application.platform` = Android，遊戲的 `PerPlatformDisable` 就把 iOS 的觸控按鈕刪掉，其他 iOS 程式碼又還在用。**改成對 C# 回報 `IPhonePlayer`**（覆寫 `Application.get_platform` icall）。連帶 AssetBundle 要放回 iOS 的資料夾名稱 `AssetBundles/ios/ios`（推檔腳本會自動對應，本機的 `work/raw-android` 不用改）。
2. iOS 模式下存檔走 iCloud。我們的本機 CloudKit 回報「找不到紀錄」時，遊戲還沒建好存檔物件，載入永遠不會完成（照遊戲自己的 log：「failed iCloud load while not logged in」）。**改成回報沒有 iCloud 帳號**（`_CKAccountStatus` → NoAccount），遊戲就改用 `SSPlayerPrefs`，也就是 Android 的 SharedPreferences，存檔會留在 App 裡。CloudKit 的回呼也改成排隊、延遲約 1.5 秒、在主執行緒送出，比較接近真的 CloudKit。
3. 測試腳本可以送點擊、長按、拖曳、按鍵（`TAPS="62:960,800 74:1085,710 112:960,700 128:drag=400,800,250,600"`），並且指定在第 0 號螢幕：Thor 有兩個螢幕，系統的焦點常在第二個螢幕的啟動器上。

結果（Thor）：標題 → 存檔版本對話框 → 教學關卡「Welcome to Marble Knights」→「Tap and drag your Left Thumb to move.」→ 拖曳之後騎士移動、教學前進到下一步（「Some treasure is strewn about」）。這一輪沒有任何受控例外。截圖：`work/2026-11-03-tutorial.png`、`work/2026-11-03-gameplay.png`。

還沒做：聲音（FMOD 目前無聲）、有一個 Shader 沒有 GLES3 程式（log 出現一次 `platform 9 is not available`，多半是 instancing 變體）、片頭影片的解碼著色器（`VideoDecoder::Blit failed`，畫面可能是黑的）、俄文 bundle、手把與長時間遊玩測試。

## 13. 聲音：FMOD → AAudio（2026-11-03）

iOS 版 FMOD（2.00.10）只會輸出到 AudioUnit／AVAudioSession。做法（`loader/proxy/fmod_audio.c`）：FMOD 仍用 no-sound 輸出（它照樣以實際時間混音），在 master channel group 的最前端（`FMOD_CHANNELCONTROL_DSP_HEAD`）插一個自己的 DSP（`FMOD_DSP_DESCRIPTION`，plugin SDK 110），把最終混音複製到環狀緩衝區，再由 AAudio 串流（float、立體聲、FMOD 的取樣率 24 kHz、低延遲、`USAGE_GAME`）播放。兩邊時脈的小差距由環狀緩衝區吸收（過多就跳過、不足就補靜音）。不用 FMOD 的輸出外掛 API，是因為那個結構體跟 FMOD 版本綁得很緊。

驗證：AAudio 串流啟動成功，`dumpsys media.audio_flinger` 顯示這條音軌有實際訊號（約 -13 dB，不是 -inf）。**還沒用耳朵確認**。App 切到背景時的暫停（`mk_audio_pause`）還沒接上。

另外跑了 3 分多鐘的自動操作（拖曳、點 Continue）：騎士移動、吃掉大部分金幣，沒有例外、沒有當機。

## 14. 修 Bug 一輪（2026-11-03，使用者在遠端）

- **GPU instancing**：還不會翻的 instancing 變體（cbuffer 是 struct 陣列）與 procedural instancing（device buffer），改成把該 pass 的 `m_HasInstancingVariant`／`m_HasProceduralInstancingVariant` 設成 false（421／250 個 pass），Unity 就不會對這些物件用 instancing，改用已經翻好的一般程式。
- **完全沒有程式的 Shader**（例如 `unity_builtin_extra` 的 `UI/Default Font`）：也補一個空的 GLES3 平台項目。`Desired shader compiler platform 9 is not available` 的錯誤因此消失。
- **背景／前景**：攔截引擎送出的 `OnApplicationPause(bool)`，暫停或重新啟動 AAudio。按 Home 再回來，遊戲會顯示它自己的「Paused」選單，正常。
- **除錯追蹤改成選用**：只有 App 的外部 files 資料夾裡有 `mkdebug` 檔案時才追蹤呼叫、印完整回溯（原本每次引擎呼叫都做線性搜尋，也會在第一個例外時建 10 萬筆的方法表，拖慢開機）。平常每個例外只印一行。
- **FPS**：`FMODUnity.RuntimeManager.Update` 每格跑一次，proxy 每 10 秒印一次 FPS。Thor 上教學關卡穩定 **約 60 fps**（59.8–59.9，偶爾 57）。
- 俄文 bundle：上一輪修好 bundle 節點旗標後已經正常。
- 片頭影片：需要 Unity 的 `Hidden/VideoDecodeAndroid` 著色器，它只在 Android 建置的內建資源裡（播放器套件沒有），先不處理（只影響 Apple Arcade 標誌那段影片）。
- 還沒驗證：手把（Thor 內建）、聲音實際聽起來、長時間遊玩。

補充：存檔現在會保留，第二次開啟直接進主選單（Single Player／Multiplayer／Join Party），點 Single Player 進到 Player Setup。**手把還不能用**：遊戲用自己的 `AppleGameController` 外掛（Apple GCController，ObjC）讀手把，目前是空的；而且 `adb shell input gamepad` 注入的按鍵沒有真的輸入裝置，Unity 不會當成搖桿。要支援 Thor 內建手把，得把這個外掛換成 Android 版（例如用 Unity 的 `Input` 搖桿 API 實作它的原生函式），並且要拿實機手把測。

## 15. 手把（2026-11-03）

遊戲用自己的 `AppleGameController` 外掛讀手把（Apple GCController）：C# 每格呼叫 `GetController(i)` 拿一個 40 bytes 的結構（是否有效、左右搖桿與十字鍵 6 個 float、12 個按鍵 byte），另有連線／斷線回呼 `(bool isMicro, bool isExtended, int index)`。

替代實作（`loader/proxy/gamepad.c` 與 `proxy.c` 的 `patch_agc`）：把 8 個原生函式換掉，資料來源是 **Unity 自己在 Android 上的搖桿輸入**：直接呼叫 libunity 的 icall `Input.GetAxisRaw("JoyNAxisM")`（遊戲的 InputManager 已經有 Rewired 產生的 `Joy1Axis1…` 軸）和 `Input.GetKeyInt(JoystickNButtonM)`。連線狀態每隔一段時間用 `Input.GetJoystickNames()` 比對，在主執行緒送出連線／斷線事件。glyph 名稱回傳 Apple 的 SF Symbols 名稱（遊戲找不到對應圖檔時會用通用圖示）。

對應表預設照 Unity Android 的 Xbox 配置：左搖桿 軸 1/2（Y 反向）、右搖桿 3/4、十字鍵 5/6、LT/RT 軸 13/12、A/B/X/Y = 按鍵 0–3、LB/RB = 4/5、Menu = 10、Options = 11。可以用 App 外部 files 資料夾的 `gamepad.txt` 覆蓋（例如 `ry=-4 dy=6 menu=7`，負數 = 反向，-99 = 不用），不用重新打包。有 `mkdebug` 時，每個軸與按鍵的變化都會印在 logcat（`mkpad`），方便校正。

結果（Thor）：遊戲偵測到 Thor 內建手把（Unity 回報的名稱是 `Xbox Wireless Controller`），log 顯示「Controller Manager Starting … Hardware: [Xbox Wireless Controller]」。用 `adb shell input gamepad` 注入的 A 鍵（會變成第二個虛擬手把）被讀成按鍵 0，主選單進入手把模式（選取框出現在 Single Player）。**實體手把的搖桿、十字鍵、肩鍵與扳機的對應還沒用實機確認**。

## 16. 手把按鈕圖示（2026-11-03）

`CustomAppleController.GetGlyphSprite` 把外掛回傳的字串當成 PNG 路徑：`File.Exists` → `File.ReadAllBytes` → `ImageConversion.LoadImage` → `Sprite.Create`（iOS 上是外掛把 SF Symbol 畫成圖檔）。`tools/make_glyphs.py`（Pillow）畫 13 個 Xbox 風格的圖示（A/B/X/Y、LB/RB/LT/RT、十字鍵、左右搖桿、Menu、Options；白色加深色描邊，是我們自己畫的），建置時產生 `glyphs.inc` 編進 `libil2cpp.so`；第一次用到時寫到 `files/glyphs/<名稱>.png` 並回傳路徑。「Image Data Does not exist」的 log 已消失。測試時手把也成功在 Player Setup 指派成 P1。
