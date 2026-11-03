# 2026-11-03 UnityFramework 分析與里程碑 1 紀錄

只寫結論與重現方法；原始輸出（符號表、bind 表、呼叫圖 JSON）在 `work/an/`（gitignore）。

## 怎麼重現

```bash
# 解出 IPA（不含 Data/Raw），放 work/ipa/
L="/c/Program Files/LLVM/bin"; UF=work/ipa/Payload/reprovisioned.app/Frameworks/UnityFramework.framework/UnityFramework
"$L/llvm-objdump" --macho --bind --lazy-bind --weak-bind $UF > work/an/binds.txt
python -I tools/group_binds.py work/an/binds.txt            # 依框架分組的匯入數
python -I tools/callgraph.py $UF work/an                     # 呼叫圖 → work/an/callgraph.json
cd "$TEMP" && python -E <repo>/tools/disasm.py <UF> <hex 位址>   # 反組譯（要 capstone）
```

## Mach-O

- thin arm64 `MH_DYLIB`，`LC_DYLD_INFO_ONLY`（沒有 chained fixups），`cryptid=0`。
- 段：`__TEXT` 0–0x2cb8000、`__DATA_CONST` 0x2cb8000–0x3000000、`__DATA` 0x3000000–0x3684000（含 `__bss`／`__common` 約 4 MB）、`__LINKEDIT`。
- `__mod_init_func` 553 個；`__eh_frame` 只有 0xd4 bytes（4 個 DWARF 模式的函式），其他都是 compact unwind。
- 沒有 `__thread_vars`（不用 Darwin TLV）。
- 匯入（bind＋lazy bind）896 個：libSystem 351、libc++ 108、CoreFoundation 65、UIKit 59、libobjc 39、AVFoundation 38、Foundation 31、OpenGLES 28、AudioToolbox 27…
- 匯出：229 個 `_il2cpp_*`、559 個 FMOD、`operator new/delete`（Unity 覆寫）、`UnitySendMessage`、PLCrashReporter、zlib 包裝等。

## 呼叫圖（`tools/callgraph.py`）

只看 BL／B 與 ADRP+ADD 指向函式開頭的參照，會低估（虛擬函式、函式指標表抓不到），但夠用來判斷範圍：

- 從 `il2cpp_*` 匯出可達 1,516 個函式；從 `il2cpp` section（遊戲程式碼）可達 119,125 個（其中約 6,000 個在 `__text`，是 libil2cpp 執行環境與外掛）。
- 這些函式用到的匯入：libSystem 215、libc++ 49、CoreFoundation 9（locale、CFString）、libobjc 10、Foundation 1（NSLog）、GameController 1。
- 遊戲程式碼直接呼叫、而且不是從 il2cpp API 可達的 `__text` 函式 1,095 個：FMOD 553、zlib 5，其餘多數是引擎內部的 helper；位址 0x1fe000–0x20b000 一帶約 60 個函式用 ObjC（`objc_msgSend`），是 `__Internal` 外掛（GameController、NSCalendar、NSLog…）。

## 里程碑 1 的除錯紀錄（照時間）

1. 第一次執行：載入成功，`il2cpp_init` 報 `Could not open`（路徑是空的）→ libc++ 符號表的 mangling 打錯（`_ZNNSt3…`），`std::string` 全部變成陷阱。
2. data dir 要設 `Data/Managed`（libil2cpp 自己接 `/Metadata/global-metadata.dat`）。
3. 5 個靜態初始化 `abort`：Unity 的 semaphore 用 `dispatch_semaphore_create`，沒實作 → 補 `dispatch_semaphore_*`／`dispatch_time`。
4. `il2cpp_init` 在 `0xe3ac98`（建 `InternalArray__*` 的泛型陣列方法表）讀到 NULL：表裡的 `std::string` 都是垃圾。原因：**Apple arm64 的建構子回傳 `this`**，呼叫端用 x0 接著寫欄位；我們的複製建構子回傳 void。修正後 `il2cpp_init` 回傳 1。
5. `Int32.Parse("not a number")` → `libc++abi: terminating due to uncaught exception of type Il2CppExceptionWrapper`：Android libunwind 看不懂 compact unwind → 寫 `macho_unwind.c` 轉 DWARF，之後正確接到 `FormatException`。

最後一次的輸出（Thor，Android 13）：

```
unwind: 111927 functions, 111923 FDEs generated, 4 DWARF-mode, eh_frame 4501 KB
ran 553 initializers, 0 crashed
il2cpp_init returned 1
62 assemblies: mscorlib.dll 1530 classes … MarbleQuest.dll 2609 classes … Assembly-CSharp-firstpass.dll 88 classes
String.Concat -> "Marble Knights"
GC collect ok, used 188416 bytes
Int32.Parse threw: FormatException
```

還在用陷阱（呼叫時回傳 0）的匯入：初始化期間有 `objc_autoreleasePoolPush/Pop`、`objc_msgSend`、`objc_retain*`／`release`（iOS 引擎的 ObjC 初始化，不影響 IL2CPP）。綁定時共有 443 個函式、362 個資料符號沒有實作（幾乎都是 UIKit／Metal／AVFoundation，只有引擎用）。
