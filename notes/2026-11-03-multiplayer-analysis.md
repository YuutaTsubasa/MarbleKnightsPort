# 多人模式分析與重作計畫（2026-11-03）

## 遊戲裡有三種多人

1. **同一台裝置、多支手把**（Multiplayer → Player Setup）：Rewired 加上我們的 AppleGameController 替代品（`loader/proxy/gamepad.c`，最多 4 支）。Thor 內建手把已經能在 Player Setup 指派成 P1；第二支以上還沒實測（要再接一支藍牙手把）。這部分大概不用重作，只要驗證。
2. **Game Center 連線**（Multiplayer 開房、Join Party 輸入房號）：
   - 傳輸：`MarbleQuest.Networking.GameCenterWrapper.GameCenterMessageBus`，經 `NetworkBusWrapper`／`MessageBusManager`（Zenject 綁定）。
   - 配對：`GameCenterRemoteControllerHost.CreateMultiplayerRoom` → `ControllerSelection.SetPartyCodeSlots`；外掛 `GameCenter.GameCenter` 的原生函式 `_GCStartMatchmaking`、`_GCGetStartingRoomNumber`、`_GCSendMatchData`／`_GCSendMatchDataToPlayers`、`_GCDisconnectMatch`、`_GCGetPlayersInMatch`、`_GCCancelMatchmaking`、`_GCFinishMatchmaking`、`_GCAddPlayersToMatch`、`_GCStartEmptyMatch`，回呼 `_OnMatchmakingFoundMatch`、`_OnMatchStarted`、`_OnMatchPlayerReceivedData`、`_OnMatchPlayerConnectionStateChanged`、`_OnMatchDisconnected`、`_OnInviteRoomCodeUpdated`。看起來是用房號當 GKMatchRequest 的 player group：輸入同一個房號的人被配在一起。
   - 同步：`MultiplayerManager`、`NetworkOwnedObject`、`RPCBroadcaster`、`PlayerNetworkData`、`EnemyNetworkData`、`MultiplayerLoadSyncManager`，都在 bus 之上，跟 Game Center 無關。
3. **手機當手把**（`RemotePhoneReceiver`、`GameCenterRemoteControllerHost`）：iPhone 跑同一個 app 當遙控器，也走 Game Center match。Android 上要有第二台裝置跑同一個 APK 才有意義，優先度最低。

另外 build 裡留著開發者在編輯器測試用的 **socket 傳輸**：`EditorWrapper.EditorMessageBus`（UDP、可靠重送、host migration）、`EditorHost`／`EditorJoin`（區網找 host、邀請、握手），UI 是 `ControllerSelection_EditorInviteMenu`（`ActivateClient` 解析兩個整數後 `EditorJoin.StartSearchingForHost`）和 `ControllerSelection.OpenNearbyControllerListEditor`（生假玩家）。

## 做法比較

- **A. 改走編輯器的 socket 傳輸**：傳輸是現成而且開發者用過的，但進入點是編輯器專用的選單（可能藏著或很陽春），要改遊戲的分支去選它，遊戲的房號 UI 用不到。
- **B. 自己實作 Game Center 外掛的 match 原生函式**（建議）：跟 Free Riders 把 Xbox LIVE 換成自己的伺服器同一個思路。遊戲自己的開房／房號 UI 和同步層都不動，我們只換最底層：
  - 區網：UDP 廣播「房號 N 有 host 在 ip:port」，加入的人收到就連上，不需要伺服器；
  - 跨網路（之後）：一支小的 relay 伺服器（類似 Free Riders 的 `live_server.py`），兩邊都連出去，用房號配對、轉送封包；
  - `_GCSendMatchData` 的可靠／不可靠兩種模式：可靠的走 TCP 或自己的 ack 重送；
  - 回呼要在主執行緒送（照 CloudKit／GameCenter 替代品的做法：先排隊，`runtime_invoke` 時 pump）。
  - Manifest 要加 `INTERNET`（Free Riders 踩過：沒有這個權限 bind 直接失敗）。

## 估計

- 同一台多支手把：驗證半天（要第二支手把，或用 `input gamepad` 注入第二個裝置）。
- 方案 B 區網版：讀懂 `GameCenterMessageBus` 用到哪些原生函式和資料格式 1 天、原生函式＋UDP 2–3 天、兩台實機（Thor＋另一台 Android）測開房→加入→一起玩一關 1–2 天，合計約一週。
- relay 伺服器版：再 2–3 天。
- 手機當手把：再 2–3 天，看需求。

## 下一步

1. ~~先實測同一台多支手把~~：2026-11-03 用虛擬手把驗證完成（設計文件第 22 節）。
2. 方案 B：把 `GameCenterMessageBus` 呼叫的 `_GC*` 函式和回呼參數格式（player ID 字串、byte[] 資料）整理出來，寫進 `docs/design.md`。
3. 需要第二台 Android 裝置做兩機測試，開始前先問使用者用哪一台。
