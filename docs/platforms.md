# 其他平台

這個專案只在兩台機器上實際測過：Windows 11 x64＋NVIDIA RTX 4090，以及作者的 Android 13 掌機 Thor（arm64，Adreno GPU）。這份文件寫的是其他平台**目前的判斷**，除非特別註明「已測」，都還沒驗證過。試過的人歡迎回報。

## 共通的限制

- **遊戲版本**：只支援 iOS 版 2.3.1（build 760642）。`GameAssembly.dll` 和 Android 代理裡的位址都是照這一版做的，啟動器會比對 `UnityFramework` 的 SHA-256。
- **畫面一律走 OpenGL ES 3**：資料裡只有從 Metal 轉出來的 GLES3 著色器，Windows 版也是用 `-force-gles30` 讓 Unity 播放器開 OpenGL ES context。這要靠顯示卡驅動支援 `WGL_EXT_create_context_es2_profile`。NVIDIA 已測可以；AMD、Intel、Qualcomm 的 Windows 驅動**還沒測**，不支援的話遊戲會開不起來或全黑。

## Windows

| 情況 | 判斷 |
|---|---|
| x64＋NVIDIA | **已測**，可以玩 |
| x64＋AMD／Intel 顯示卡 | 沒測；取決於驅動有沒有 OpenGL ES context（見上） |
| Windows on ARM（Snapdragon） | 沒測。`GameAssembly.dll` 與播放器都是 x64，要靠系統的 x64 模擬（Prism），速度會打折；OpenGL ES 同樣看驅動 |

## Linux、Steam Deck（Proton／Wine）

沒測。Windows 版理論上可以用 Proton 或 Wine 跑（啟動器、附帶的 Python 都是一般的 Win32 程式），但卡在 OpenGL ES：

- Wine 要把 `wglCreateContextAttribsARB` 的 ES profile 轉給 Linux 端（Mesa 的 GLX／EGL 有 ES context）。Wine 的 Wayland 驅動目前直接拒絕 ES context（[winewayland 的實作](https://list.winehq.org/archives/list/wine-commits@list.winehq.org/thread/DBXOYULGFMSEKCRK4564WIZODOGCINE4)），X11 驅動的情況還沒查證。
- 如果 Wine 這邊不行，可能的做法是讓 Windows 版改用桌面 OpenGL（GLCore）著色器，要另外轉一份著色器，工程不小。

Steam Deck 是 x86，Android 版（只有 arm64）不能用 Waydroid 之類的方式替代。

## macOS

- **Apple Silicon**：最直接的是不用這個專案，用 [PlayCover](https://applegamingwiki.com/wiki/PlayCover) 直接跑你自己的 IPA（它只收解密過的 IPA）。沒測；Apple Arcade 的訂閱檢查、Game Center 與 iCloud 存檔在 PlayCover 下能不能用都不確定。這個專案在 Windows／Android 上把 Game Center 與 CloudKit 換成本機實作，PlayCover 不會有這一層。
- **Intel Mac**：PlayCover 不支援 Intel。剩下 Windows 版加 CrossOver／Wine，跟上面 Linux 一樣卡在 OpenGL ES，而且 macOS 的 OpenGL 本身沒有 ES context，可能性很低。
- 要做原生 macOS 版的話，路線會跟 Windows 一樣（重編譯程式＋Unity 的 macOS 播放器），但要處理 Metal：macOS 播放器可以直接用原本的 Metal 著色器，反而比 Windows 簡單；還沒開始。

## iOS、iPadOS

原本就是 iOS 遊戲，用 AltStore、SideStore 之類的工具把自己的 IPA 重新簽名側載就是原版。沒測；免費 Apple ID 簽的 App 7 天要重簽一次，Apple Arcade 的訂閱檢查會不會擋也不確定。這部分跟這個專案無關。

## 其他 Android 裝置

| 條件 | 說明 |
|---|---|
| CPU | 只有 arm64-v8a（`UnityFramework` 是 arm64 的機器碼，在裝置上直接執行）。32 位元手機、x86 的模擬器與 Chromebook 不行 |
| 系統 | Android 7.0（API 24）以上。**已測**：Android 13（Thor） |
| 顯示 | OpenGL ES 3.0 以上 |
| 空間 | 約 4 GB：APK 87 MB，加上放在內部儲存空間最上層的 `MarbleKnights` 資料夾 3.9 GB |
| 權限 | 第一次開啟要允許「檔案和媒體」（targetSdk 29＋`requestLegacyExternalStorage`，Android 11 以後仍有效） |

Adreno 以外的 GPU（Mali、PowerVR、Xclipse）沒測。Android 版沒有先解開 PVRTC 貼圖（Adreno 沒有 PVRTC，在 Thor 上看起來是 Unity 自己處理掉了），其他 GPU 應該一樣，但沒驗證。
