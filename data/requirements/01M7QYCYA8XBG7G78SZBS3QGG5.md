# emu-ctl typed global 读写原语（global-get / global-set；id + 显式 kind，哑工具）

- id: REQ-01M7QYCYA8XBG7G78SZBS3QGG5
- type: req
- status: open
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- verify: apps/emulator-lift/tests/control_globals.cpp#lift-control-globals
- tags: [emulator-lift, control, global, emu-ctl]

## 目标

`emu-ctl` 现在**没有任何读 guest 内存 / 寄存器 / global 的能力**（现有 op 只有
`list/pause/resume/sample/screenshot/click/wait/quit/gc`）。本单加 **typed global 原语**：
`global-get` / `global-set`（按 **id + 显式 kind** 读写 script global）。

★ 只做**哑原语**：⛔ 不猜类型、⛔ 不挑时机、⛔ 不重试、⛔ 不写后校验、⛔ 不为任何具体 global / 选项开特例。
"什么时候写、写哪个 global"是**调用方的知识**（配方落 `docs/03-emulator/control-plane.md`）。

## 为什么需要（可再校验的观察）

- 复现 SAVE05 的"闪光黑方块"时，每 32 ms 的回调在 `2fb7 ≥ 100`（≈3.2 s 静止）且 `(b1b6 & 0x10) == 0` 时
  画出全屏 `SO033A`（"ナビゲーション画像"覆盖图），把画面盖住 —— 见 `.tmp/so033a/analysis.md` 与
  `.tmp/emu-ctl-globals/analysis.md`。要按"知识"而不是"进游戏点选项"关掉它，就需要能直接改 guest 的 global。
- 既有的两条**只读**需求单（`REQ-01M7J97QY66KMP5SBPCTR0S3TP` 的 await global、
  `REQ-01M7M7C7JFYHQ587TP75HVQZQN` 的读槽）都要"与 guest 同一份解码寻址"；读写共用这份 codec
  ⇒ 本单是它们的**前置**，**不取代**它们。

## 已核事实（锚）

- spec 覆盖：global id = **raw 池下标**（`b1b6` 就是 hex raw；**没有名→id 映射**，`disassemble.mts:92`）、
  类型族（`3` = global-int，`4`/`5` 其它值族，`6/7/8` 引用族）、宽度（int 4 B / float 4 B / string 28 B）、
  **int 过 ENC/DEC**（float 是裸 32 位）。
- 存储面 = **guest 内存**：AOT 臂没有宿主数组 / 影子副本；地址 = `rd32(Engine+0x5D800) + 4*id`
  （`0x6d` 会把 `5D800 ↔ 5D804` 对调 ⇒ **每次读 `+0x5D800`**，⛔ 不缓存基址）。
- 控制命令在**块边界**由主线程 `serve()` 执行 ⇒ 写完下一块就对引擎可见。
  ⚠ `serve()` 有 `leave_paused = !paused` 语义：运行态处理完要放行，⛔ 别把进程卡在暂停。

## 形状

- 请求 `{op, kind, index, value}`；`kind` 由调用方显式给（`int` / `uint` / `float`；**string 留二期**）；
  返回 `data.global{kind, index, value, raw}`。
- 错误只三类：`id_out_of_range` / `type_mismatch`（**只在能判定时**报，判不了就放过）/ `encode_failed`。
- ⛔ 不动 `--until` / `click` / `sample` 的语义；新 op 走同一条链路（client → server → 主线程 `serve`）。

## 守卫

- pure：ENC→DEC 往返回原值（含负数 / `0x7FFFFFFF` / `0x80000000`）、float **只断言位模式**、越界 id 拒绝、
  `kind` 与类型族不符、JSON 往返。夹具用假 Engine / 假基址，**⛔ 不断言某个 global 的当前值**。
- 变异：把 `ENC` 换成明文写 ⇒ 往返用例红。
- 端到端（写进 guest 真生效）只能人工：读回 + 静止 ≥ 3.2 s 截图 A/B + 反向对照。

## 未做 / 边界

- `string` 类型（28 B 记录布局在 shape `T_stdstring`，spec 明确排除在 spec 之外）。
- 池容量数值（运行期计数）未进 spec ⇒ 越界判定按运行期容量读，**值本身不作断言**。
- ⛔ 不做"等 TITLE / 判断配置加载完 / 写后校验 / 自动重试"——那是调用方的知识。
