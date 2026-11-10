# spike · 已确认：EU-ROOM 的「戻る」与 ESC 是两条路 —— 那次 SEH 是退出游戏（关窗），不是出房间

- id: REQ-01M71P87BMTZK4MHT8468BMC3G
- type: spike
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- done_reason: 成立（归属改判）：0x4123C4 只可能是主循环取到 WM_QUIT；引擎侧 WM_QUIT 的唯一来源是 WM_CLOSE→DestroyWindow→WM_DESTROY→PostQuitMessage（0x4B967B / 0x4B9691）；ESC 在 WndProc 里无条件 PostMessage(WM_CLOSE)（0x4B9367）。真机复跑（room-exit 的 room 节点 = key 0x1B）拿到 PostQuitMessage→Destroyed→Exit→RaiseException，key_up samples=0。⇒ 那次 SEH 是退出游戏，不是回 TITLE。SEH 机制仍必要（0x2 根帧结束 / 数据错误 / 0x9 重装失败也抛同型异常）。戻る 点击仍 open；OBS-20261108-03/04 的点击→TITLE 读数可作'不抛'旁证。
- tags: [emulator-lift, seh, interaction]

## 结论（2026-11-10，已核）

**成立：那次真机的 `RaiseException` 属于"退出游戏（窗口关闭）"，不属于"从 EU-ROOM 回到 TITLE"。**

证据链（锚 = EA；逐字见 `.lst`，行号是派生值）：

1. `0x412391 GetMessageA` → `0x412397 test eax,eax` → `0x412399 jnz` —— 只有 `GetMessageA` 返回 0（WM_QUIT）才走
   `0x4123B8 push pThrowInfo(0x544774)` → `0x4123C4 call __CxxThrowException@8`。⇒ 这个抛点**只**代表"主循环取到退出消息"。
2. `PostQuitMessage` 全语料只有一处：`0x4B9691`，在窗口过程的"窗口已销毁"分支
   （`Msg==2`：`0x4B9689 ShowWindow(0)` → `0x4B9691 PostQuitMessage(0)`）。
3. `DestroyWindow` 全语料只有一处调用：`0x4B967B`，在窗口过程的"关闭"分支（`Msg==16`）。
   ⇒ 引擎侧 WM_QUIT 的唯一来源是"关闭 → 销毁 → 投递退出消息"。
4. ESC 在同一个窗口过程里被**无条件**转成关闭消息：`0x4B9367 cmp ebx,1Bh` →
   `0x4B9373 PostMessageA(hWnd, 0x10, 0, 0)`。⇒ ESC 不是"返回上一屏"，是"请求关闭窗口"。
5. 真机复跑（2026-11-10，macOS arm64，`room-exit.graph`，`--present headless --raster cpu`；room 节点 = `key 0x1B`）：
   `[shim] user32!PostQuitMessage: exit_code=0` → `OutputDebugString "Destroyed"`（WM_DESTROY）→
   `OutputDebugString "Exit"`（GetMessage==0 分支）→ RaiseException / ExitProcess；同一次 `key_up` 自报
   `samples=0`（ESC 从未作为输入被 guest 采样到）。⇒ 观测到的 SEH 是退出链，不是房间退出链。

## 对「戻る」这条路的判定（写清边界）

- `戻る` 的点击今天仍点不动（`REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`，open）；本轮**没有**新跑通一次真实点击。
- 但 `OBS-20261108-03`/`04` 记录过一条**点击 → TITLE** 的完整读数（151M 条重载帧、9.17 s 中位）；
  那时 SEH 还是桩、任何 `Command_Exit_Exception` 都会当场停 ⇒ 这条历史读数本身就是"`戻る` 不抛"的旁证。
  ⛔ 不要把它当成"今天在 macOS 上点击可用"的证据（BUG-16 是另一件事）。
- 因此本 spike 的结论只到"**观测到的那次 SEH 不是出房间**"；"`戻る` 在今天的 macOS 上到底走哪条路"
  仍挂在点击单上。

## 机制仍必要（别把本结论读成"SEH 白做"）

`Command_Exit_Exception` 是**引擎级终止异常**：除 WM_QUIT 外，根的 `0x2 exit`（父帧号 < 0，
handler `sub_41A820` 的抛点）、数据损坏对话框选"终止"（`0x412491`）、`0x9 exit-script` 重装根脚本失败
（`0x428D9C`）都抛同一种异常。⇒ `RaiseException`/`RtlUnwind` 的实现必要且正确；要改的只是**归属与引用口径**。

## 处置

- `REQ-01M708DF24KY0QZNEHY66S0HYW`（SEH 机制，done）：标题 / 正文改为"退出游戏（ESC / 关窗）的 C++ EH"，
  不再称"EU-ROOM 退出的前置"。
- `REQ-01M717SQJ2RDFRJCB41RREXQYX`（room-exit 终态，blocked）：保留"等 `戻る`"，但写明"SEH 不是这条路的判据"。
- `REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`（戻る 点击，open）：删掉"点中它就走退出路径 ⇒ 停在 0x4123C4"这句（错误归属）。
- `repo:apps/emulator-lift/scripts/interaction/room-exit.graph` 的 room 节点注释：ESC 不是 `戻る` 的同一条路。
- spec `CORE-BOOT-EVENT-LOOP` 补一条"ESC ⇒ 关闭消息"（`0x4B9367` / `0x4B9373`）—— 这条缺口正是本次误归属的根。
