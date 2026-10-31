# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

本节点**保持 open**，是**一次性快照**：只写"现在是什么状态"，不存历史、不堆叠 —— 历史看 `git log`，
待办看 `pnpm tools requirements plan`（本单不抄）。用户口径 2026-10-29 / 2026-11-01。

## 现在能跑到哪

- TITLE 可进设置界面；GameStart → ADV 跑通（`0x72` 等推进、一句一停）。
- `apps/emulator-cxx` 已按 2026-11-01 的 spec 收口轮对齐 6 批：输入合成 · 文本容器与字体 · 纹理 ·
  帧流程 · `0xfd/0x10c` 三张表 · ADV 模式门 · 错误通道顺序（各挂顶层 CTest）。

## 实现线

- 规范真源 `repo:data/spec`；唯一在维护的模拟器 `repo:apps/emulator-cxx`（preset `windows-x64`，`out/` 是 binaryDir）。
- `apps/emulator`、`apps/emulator-new` 已废弃（只读遗产）：⛔ 不新增功能 / 不补守卫 / 不为它们改 spec。
- 取证记录（锚 EA，⛔ 不是 spec）：`repo:docs/02-engine/ops-forensics-2026-10-title-option.md`。
- 宿主观测面（不是引擎语义）：`--input-script` / `--play-headless` / `--tick-ms` / `--anchor-script` /
  `--expect-script` / `--require-script` / `--bmp`；诊断 `--watch-locals` / `--trace-*` / `--dump-scene-at` / `--dump-texture`。

## 待做（emulator 侧）

- `0x1ca` 的「设命名键」与 `0x324` 的接收者复位**未落实现**（模型没有命名键存储 / 接收者对象；落点需裁决）。
- 字体度量后端接真（`set_font_measurer` 缺省走回退 ⇒ `0x75/0x197/0x1a5/0x2fe` 的派生不改值）。
- `0x25b` 只落状态 `2`（外枠纹理 / 状态 `1` 未实现）。
- `0x205` 的 CTest 只判"停在 20940 + 命中项数量"，缺**文本 / 落点**断言。
- 可执行守卫：数组地址算术 32 位回绕（`0x61/0x12c`）、数值族 `0x52–0x5f`（截断 / 符号 / 位数掩码 / 比较方向）。

## 已知偏差（实现侧选择，不是引擎语义）

- 表 B 是稀疏表：未知逻辑键号只诊断（引擎侧未定见 `RES-INPUT-LOGICAL-KEY-CODES`）。
- 鼠标左右键交换没有配置键（`set_mouse_swap` 注入口，缺省 false）；摇杆轴约定 X 正 = 右 / Y 负 = 上、
  阈值注入初值 0（引擎侧未定见 `RES-INPUT-JOY-AXIS-CONVENTION`）。
- 「再缩一次」取"对位模式 = 1"、半格阈值取 `字号 ÷ 2`（见 `RES-OPS-FORWARD-PROLOGUE-RESCALE`）。
- `display:TextureBit` 缺键按 32 位（见 `RES-TEX-LOAD-TEXTUREBIT-DEFAULT`）。
- `0x1f9` 共享对象槽未建模；`0x20e` 门控支缺虚拟全屏类型格；`0x2` 的 −11 位置表 / 返回点表走宿主接缝；
  `0x2`「整个执行结束」用 halt 表达。
- 打字机的"跳过"只清第 29 位、不置第 27 位（模型没有第 27 位服务者）。
- ADV 旧路径下 `0x6e/0x196` 仍经宿主兜底建画布、`kTextLayerFallbackBase` 兜底基数（等 `RES-ADV-TEXT-LEGACY-PATH` 定）。

## 待基建

- spec 工具缺「正文 / audit 里引用的 `RES-…` / `GAP-…` 必须存在」的不变量；已发现历史悬空 4 个：
  `RES-23D-SCENE-UNBIND`、`RES-AUDIO-BGM-FADE-SERVICE`、`RES-OPS-SCALAR-VARIANT-REBUILD`、`RES-PARAM-21668-INIT`。

## 门禁（怎么查，不记数字）

`pnpm test` · `ctest --test-dir apps/emulator-cxx/out` · `pnpm typecheck` · `pnpm tools spec validate` ·
`pnpm tools requirements validate` · `git diff --check`。

## 工作口径

- **残差只登记在** `data/spec/audit.json`（`pnpm tools spec residuals` 看），⛔ 不在需求单里重复。
- **`apps/emulator-cxx/SCOPE.md` 的「显式不做」只由用户拍板追加**，⛔ 不拿它当待办回收站。
- spec 完整性判据：所有影响可观察行为的字段 / 触发 / 转换都写全，独立实现者只凭 spec 能写出唯一实现。
- spec vs emulator：spec 写领域状态机；emulator 是示范与第二实现证据；分歧按 `amayui-spec` §9.2 归因。
