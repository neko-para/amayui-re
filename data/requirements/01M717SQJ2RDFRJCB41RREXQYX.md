# room-exit 的终态判据：ESC = 退出游戏（ExitProcess）不是回 TITLE ⇒ room-exit.graph 的 exit 0 要另选出口

- id: REQ-01M717SQJ2RDFRJCB41RREXQYX
- type: req
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- done_reason: 走「戻る」的裁决已兑现：room-exit.graph 的 room 节点换回 click center 后真机 exit 0 + task done + 终态 TITLE hash 0x94CB492BF7F1AEEB（三次读数逐位相同，insns=2120728564）；右键版 room-exit-right.graph 同一终态（insns=2120726066）。图口径一个字没改（back 节点仍等 TITLE hash）。BUG-16 改判为量测窗 artifact（已 dropped）。
- tags: [emulator-lift, interaction, seh]

## 判据（存在性 + 观察）

`room-exit.graph` 的终态 = **回到 TITLE 的稳定帧 hash**；出口 = 房间右下角「戻る」的**真实点击**（不是 ESC）。
本单原为 `blocked`（等 `REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`）。2026-11-10 实测：**左键点「戻る」与右键都能回 TITLE**，
且那张 bug 单的"点不动"被证明是**量测窗 artifact**（`docs/03-emulator/defects.md` 的 BUG-16 已改判「不是缺陷」）。

## 裁决（2026-11-10）：走「戻る」、⛔ 不改图口径 —— 已兑现

- `room-exit.graph` 的 `room` 节点从临时的 `action key 0x1B` 换回 `action click center`；
  `back` 节点**一个字没改**，仍等 TITLE 稳定帧 hash `0x94CB492BF7F1AEEB`。
- **为什么不改成"等进程退出"**：这张图同时是 `REQ-01M6XVYY5TCKSAS80WS7PADF8P`（识别状态机）"出房间同一终态"的判据；
  换成"等进程退出"会把判据从"回到同一屏"退化成"进程没了"——那是把判据改弱（`AGENTS.md` §10）。

## 实测（2026-11-10，macOS arm64 / RelWithDebInfo / `--present headless --raster cpu` / `--skip-logo`）

| 出口 | 图 | 读数 |
|---|---|---|
| 左键点「戻る」 | `repo:apps/emulator-lift/scripts/interaction/room-exit.graph` | exit 0 · `detail: interaction: task done` · `transitions=11 actions=3 samples=249 changes=44` · 末帧第 249 帧 `0x94CB492BF7F1AEEB` · `insns=2,120,728,564`（三次读数逐位相同） |
| 右键点「戻る」 | `repo:apps/emulator-lift/scripts/interaction/room-exit-right.graph` | exit 0 · 同样的计数 · 末帧同 hash · `insns=2,120,726,066`；trace 与左键版只差三处：`origin=` 行 / `action click center right applied` / `node back enter` 的 insn 差 4 条 |

- **因果对照（同机同构建）**：`click 100 100` / 不点击 / 只 hover ⇒ 全部 exit 5（`expect-change` 或 `timeout(node back)`）；
  `click 1100 150`（CG 鑑賞）⇒ 画面变了但不是 TITLE ⇒ 成功可归因于"在按钮上的一次点击"。
- **★ 观测窗口径**：必须盖住一次 TITLE 重载帧（今天单帧 730,765,167 条 ≈73 s guest）。11-09 那三次 run 的 `quit`
  全落在它里面 ⇒ 那时读到的"hash 停在 hover 态"其实**已经是退场中**（`OBS-20261110-02`）。
- **★ 判据的坑**：`room` 节点的 `expect screen changed` 会被**抬起那一下的 hover 重绘**满足，证明不了"点击生效"；
  真正拍板的是 `back` 节点等到 TITLE hash（trace 里 `node back … samples=249 hit`）。

## 右键为什么算"同一个终态"

引擎侧：`WM_RBUTTONUP`→`0x4B9D3F`→`sub_4B8DC0(1)` ⇒ 输入字 **bit5 = 取消位**
（`0x411CFE test byte ptr [ebx],20h`），跳到**脚本登记**的取消标签 —— 房间里那个标签就是回 TITLE。
⇒ 不是"另一条私有路"，是引擎的取消动作。⛔ 别当通用模板：取消目标由脚本数据决定
（没登记取消标签的界面里右键什么都不做）。

## 为什么单独成单 / 与别的单的关系

- 它**不是** SEH 的缺陷（机制级已在 `REQ-01M708DF24KY0QZNEHY66S0HYW` 成立）；也**不是**点击缺陷（BUG-16 判"不是缺陷"）。
- 它**是**"终态判据选错了出口"这一类问题 —— 混在 EH 单里会让"SEH 做完了"和"图的口径不对"红在一起。
  与 `REQ-01M6WW9R6SJ7P8XA4522XCR48A`（`Stopped` 仍 exit 0 的口径）同族：**"停住了"≠"到达目标"**。
