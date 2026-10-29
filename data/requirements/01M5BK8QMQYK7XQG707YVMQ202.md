# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

本节点**保持 open**，作为「当前实现总览与交接」持续更新，不按收口处理（用户口径 2026-10-29）。
2026-10-29：文本门 / 计时器、记录标志位 2、`0x205` / `0x20e` / `0x1d2` / `0x1d3` 的成篇与纠正已收口；
当前剩余集中在 **`0x82` 绘制、`0x2CF`、`0x1D0`** 与「其余已实现 opcode 的 spec 条目」。

## 交付物在哪

- **规范**：`repo:data/spec`（真源）· 工具 `pnpm tools spec validate|report|describe`。
- **C++ 实现线**：`repo:apps/emulator-cxx`（preset `windows-x64`，SDL3 + SDL_GPU + FreeType；`out/` 是 binaryDir）；
  **参考实现**（只作旁证）`repo:apps/emulator-new`。
- **取证记录**（锚 EA、含不确定项，⛔ 不是 spec）：`repo:docs/02-engine/ops-forensics-2026-10-title-option.md`。
- **本线需求单**：`REQ-01M5WD5Q5HDE246W3T97F3NA8C`。

## 宿主观测面（不是引擎语义）

- 播放：`--input-script` / `--play-headless` / `--tick-ms` / `--anchor-script` / `--expect-script` / `--require-script` / `--bmp`。
- 诊断：`--watch-locals` · `--trace-script` / `--trace-opcodes` · `--dump-scene-at N` · `--dump-texture <id> --dump-texture-out <bmp>`。

## 当前需要处理（2026-10-29）

1. `REQ-01M630YKF34EVZPMMBAJVMDDQW`（**done**）：第 29 位的服务者 = 主循环文本泵（`0x41341F` → `sub_409400`），
   启动参数 `Font+0x560` = **每字间隔毫秒**；emulator 已实现"两档写入 + 每 D 毫秒一格 + 抽干清位"，
   守卫 `emulator-core-adv-pacing` / `-adv-drain`（均变异验证）。第 27 位服务者仍单独跟踪（`REQ-01M60CFPK2X36ZCQD21M3E5G7E`）。
2. `REQ-01M633QTR9VXK1CA1AYJCJPNN8`（**done**）：位 2 由两个排版调用方置起（层模式=1 支 / 振り仮名 run），
   主线正文 run 不置起；字号支收敛到 `record_font_size`，守卫 `emulator-core-text-record-flags`（变异验证）。
3. **取证齐未实现**：`0x82` 的绘制（只记请求）、`0x2CF` 画面模式、`0x1D0` 步进算法（`0xC2` 已实现，B 支守卫已补）。
   ★ `0x1D0` 依赖字体对象那张**对表**的建造者（布局提交侧，本模型没有）⇒ 先建模对表再落它。
4. **静态出现未走到**：`0x1BA`、`0xBA`/`0xB6`/`0xB7`/`0xC4`/`0x194`/`0x1B5`/`0x1B8`/`0x1B9`/`0x2F5`/`0x2FF`/`0x307`/`0xB8`/`0x215`。
5. **spec 条目**：其余已实现 opcode 多数只有取证记录（`0x205` / `0x20e` / `0x1d2` / `0x1d3` 已成篇）。

## 本轮已收口（2026-10-29）

- `0x205` / `0x20e`：selftest 断言（`emulator-core-render-prims`，变异验证）+ **成篇进 spec**
  （`CORE-OPS-RENDER-NUMTEXT-205` / `-CLEAR-20E`），并从 `GAP-OPS-0200-02FF` 移出；两轮 spec 复审 + 一轮实现面复审已处置。
- **实现纠正**：`0x205` 的"放不下"从 `#` 改回**丢高位数字**；X 公式改成引擎的"先乘后除"；`0x20e` 改为清当前目标纹理
  （非空操作）；守卫升级为整张纹理逐字节相等 + 正向控制（字体缺失当场红）。
- `0x1d2` / `0x1d3` **成篇进 spec**（`CORE-OPS-TEXT-RECORD-REGISTER-1D2` / `-QUERY-1D3`），并从 `GAP-OPS-0100-01FF` 移出；
  复审的 H1–H3 / M1–M5 / L1–L4 已处置（同表关系、"位 29 ⇒ 0x82 跳过"、`0x1d2` 守卫与 62 的定义冲突、位号统一、组边界指代、写序、列表号口径）。
- **`0x1d3` 实现纠正**：引擎**命中不中断扫描**（同组后命中覆盖前面的 ⇒ 取最后一条）；旧实现命中即 `break` ⇒ 已改，
  `core_selftest` 夹具扩成"同组两条同 key ⇒ 取 9（不是 7）"，变异验证（加回 `break` ⇒ 红）。
- 文本门 / 计时器 / 记录标志位 2 的取证落进两张单（含对旧结论的两处订正）；两张单都已 done。
- 设置界面单 `REQ-01M5Z5D786WF65YWHFK7K8Z7Q8`、ADV 文本层残差单 `REQ-01M618MSP80J9FHXB4JS4VE1J5`、
  文本记录生产链 `REQ-01M630YH3RVEJJEGYKZJW2VQE7`、`config-toggle` bug `REQ-01M631SB4CRGK48ZXC34XC0SR4` → done。

## 本轮结束状态（2026-10-29）

- 用户**暂停了目标**；工作区全绿，没有半成品文件（见下「当前门禁」）。
- 未尽事项都在「当前需要处理」里；`0x1D0` 的前置（对表建造者）已写在条目 3。
- 未做：`mutate-check` 清单只跑 node 守卫（C++ 侧变异靠手动，本轮做了 2 条）；`0x82` 的真实绘制仍未做。

## 已打通（实测）

TITLE：`0x12e` 命中框表 → `0xa1/a2/a3` 菜单分派 → key 3 → `0x3 call-script 52` ⇒ CONFIG.BIN → `0x3 call-script 20940`。
点击在**抬起**时生效；左键 = 引擎位 0。实现指令：`0xC0`/`0x142`/`0x1BB`/`0x1A6`/`0x1D2`/`0x1D3`/`0x300`/`0x301`/`0x6E`/`0x6F`/`0x196`/`0x217`/`0x12F`/`0x2CC`/`0x2CD`/`0x2CE`/`0x2E6`/`0x2E7`/`0x2E8`/`0x2EA`/`0x10D`/`0x205`/`0x20E`/`0xC5`/`0xC6`/`0xC7`/`0x306`。

## 当前门禁（实测）

`ctest --test-dir apps/emulator-cxx/out` 24/24 · `pnpm test` 1103 通过 · `pnpm tools spec validate` 13/13 ·
`pnpm tools requirements validate` 6/6 · `pnpm tools corpus validate` 8 通过 / 0 失败 / 1 警告。

## 工作口径（2026-10-29）

- 本节点**不关闭**：它是"当前实现总览与交接"，随工作更新；进度仍以 `pnpm tools requirements plan` 为准。
- **spec 完整性判据**：所有影响可观察行为的字段 / 触发 / 转换都写全，独立实现者**只凭 spec**能写出唯一实现。
  ⇒ 需要回看 `.lst` 才能补的字段 / 触发 = **spec 缺口**（进正文或 residual/gap），不是"原则上定不了"。
- **静态优先**：单线程确定性程序，分支 / 触发都可静态枚举；"需要真机采样"只在静态读完后仍有两个在 spec tick 边界上
  不可区分的模型时才成立，并要写明采什么。
- **spec vs emulator**：spec 写领域状态机（字段 / 触发 / 请求），机器布局走 `storage` 指针；emulator 是示范与第二实现证据。
- 详细口径：`repo:docs/02-engine/spec-proposal.md` §9；技能 `amayui-spec` §0.5。
