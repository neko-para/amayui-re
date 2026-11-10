# spike · 待核：EU-ROOM 退出到底走不走 SEH —— 怀疑那次真机的"退出"其实是注入点击/ESC 落到了 Quit（abort）

- id: REQ-01M71P87BMTZK4MHT8468BMC3G
- type: spike
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- tags: [emulator-lift, seh, interaction]

## 假设（用户提出，2026-11-10，**未验证**）

**「EU-ROOM 退出并不经过 SEH；我们那次真机看到的 SEH 卸载，是注入的点击/ESC 被投递到了后面的 `Quit`（abort 路径）」。**

从 EU-ROOM 回 TITLE 是**进程内换屏**，没有理由抛 `Command_Exit_Exception`；而 `Quit`（退出游戏）本来就是 abort。

## 为什么这个怀疑站得住（本会话已确立的观察）

1. **throw 的唯一入口是 WM_QUIT**：`0x412399 jnz`（`GetMessageA` 返回 0 才继续）→ `0x4123B8 push pThrowInfo(0x544774)` → `0x4123C4 call __CxxThrowException@8`。
   `GetMessageA` 返回 0 意味着**有人 `PostQuitMessage`** —— 那是"退出消息循环"，不是"回上一屏"。
2. **`sub_428A60` 不是 EU-ROOM 退出**：它是"结束当前场景 + 重装根脚本（文件 id 0）"，**它自己的抛出点在 `0x428D9C`（装载失败才抛）**，与 `0x4123C4` 那处不是同一条。
3. **那几次退出运行用的是 ESC（`key 0x1B`）或固定坐标点击**：交互图 `room-exit.graph` 的 `back` 节点等 TITLE hash，**从来没命中**；跑完是 guest 自己调 `ExitProcess`
   ⇒ 观测到的就是"**退出游戏**"，而 `戻る`（回 TITLE）这条**一次都没有真正跑过**（它挂在 `REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`：按钮对注入点击无反应）。
4. ⇒ 我此前把 `0x4123C4` 这条 throw 当成"EU-ROOM 退出的前置"，是**沿用原始 bug 描述、未经独立验证**的推断。用户的解释用同一个观测、更少的假设。

## 成立/不成立各自意味着什么

- **成立（用户对）**：`RaiseException`/`RtlUnwind` 的实现仍然**必要且正确**（abort/quit 路径真的会走到，且原来确实是 unimplemented 桩），
  但**归属要改**：它是"**退出游戏（abort）**"的卸载，不是"回 TITLE"的卸载；`REQ-01M6XVYY5TCKSAS80WS7PADF8P`（识别状态机）的"出房间"前置**不是** SEH，
  而是 `REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`（戻る 点击）。相关两张单的措辞必须跟着改。
- **不成立**：`戻る` 真的会抛 ⇒ 记录它的 `code/flags/nargs` 与抛点 EA，与 abort 那次对比；若两者同一处 throw，则"换屏走异常"成立，本假设作废并写明反证。

## 判据（验证方式，⛔ 不要用 ESC / 不要用猜的坐标）

1. **先解决"点击真的落在戻る按钮上"**（`REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`）：要有**命中证据**（按钮高亮 / rect 均值变化 / `--expect-change-after click`），
   ⛔ 不接受"我点了它应该是"。
2. 用**真实戻る点击**跑一次，看 `[seh]` 行有没有出现、摘要里有没有 `origin`/`RaiseException`、以及是否 `exit 0 + task done`（终态 = TITLE hash）。
3. 交叉：同一次运行里打 `PostQuitMessage` / `ExitProcess` 的调用点（或看 `stop`/`detail` 是 `input script: quit` 还是 `ExitProcess`）。
4. 反证条件：在**确认命中按钮**的前提下出现 `RaiseException(0xE06D7363, …)` ⇒ 本假设作废，按"不成立"处置。

## 已确立 vs 待核（写清楚，别混）

- **已确立**：`RaiseException`/`RtlUnwind` 原本是 unimplemented 桩；本批实现后真机 trace 完整跑过 `RaiseException → handler 0x513B90 → RtlUnwind → catch 0x41389A → ExitProcess`；
  `0x4123C4` 的 throw 只在 `GetMessageA` 返回 0 的分支上。
- **待核**：那次运行的"点击/ESC"到底触发的是 `Quit` 还是 `戻る`；`戻る` 路径**是否**经过任何异常；两条路径的终态判据分别该是什么。

## 与别的单的关系

- 直接影响（措辞要改）：`REQ-01M6XVYY5TCKSAS80WS7PADF8P`（"出房间"终态）· `REQ-01M6XVZ8AXJNG492WA2QAMTA35`（把 `sub_428A60` 当 exit/reset 的说法已在本批被语义取证修正过）·
  `REQ-01M6XVZ8AXJNG492WA2QAMTA35` 里的 `0x4123C4` 归属（已在本批改正为 `sub_412290` 主循环）。
- 前置：`REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`（戻る 点击无反应，open）· `REQ-01M6ZDSGQY1RXX3N3WJKA8D26H`（后缓冲 LockRect，已收口）。
- 已收口但**归属待重写**：`REQ-01M708DF24KY0QZNEHY66S0HYW`（SEH 派发：机制对，"它是 EU-ROOM 退出的前置"这句话待核）。
