# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

本节点**保持 open**，是**一次性快照**：只写"现在是什么状态"，不存历史、不堆叠 —— 历史看 `git log`，
待办看 `pnpm tools requirements plan`（本单不抄）。用户口径 2026-10-29 / 2026-11-01（SCOPE 见 `repo:apps/emulator-cxx/SCOPE.md` §0）。

## 现在能跑到哪

- TITLE 可进设置界面；GameStart → ADV 跑通（`0x72` 等推进、一句一停）。
- `apps/emulator-cxx` 已按 2026-11-01 的 spec 收口轮对齐：输入合成 · 文本容器与字体 · 纹理 ·
  帧流程 · `0xfd/0x10c` 三张表 · ADV 模式门 · 错误通道顺序；并在 11-01 第二轮清掉
  `0x324` / `0x25b` / `0x205` / 字体度量 / 表 B（93 条）/ 数值族与数组回绕等（顶层 CTest 见 `ctest -N`）。

## 实现线

- 规范真源 `repo:data/spec`；唯一在维护的模拟器 `repo:apps/emulator-cxx`（preset `windows-x64`，`out/` 是 binaryDir）。
- `apps/emulator`、`apps/emulator-new` 已废弃（只读遗产）：⛔ 不新增功能 / 不补守卫 / 不为它们改 spec。
- 取证记录（锚 EA，⛔ 不是 spec）：`repo:docs/02-engine/ops-forensics-2026-10-title-option.md`。
- 宿主观测面（不是引擎语义）：`--input-script` / `--play-headless` / `--tick-ms` / `--anchor-script` /
  `--expect-script` / `--require-script` / `--bmp`；诊断 `--watch-locals` / `--trace-*` / `--dump-scene-at` / `--dump-texture`。
- **SCOPE 四条核心决策**（`repo:apps/emulator-cxx/SCOPE.md` §0，用户 2026-11-01；字体与字型已合并成一条）：
  总是 Amayui 字体 + FreeType 模拟 `GetGlyphOutline`（**宿主补偿允许改变 AA / 两趟合成的输出**，格式判据仍是引擎合同；
  任意名字都路由到 Amayui、两次度量保留）· 配置隔离（默认不写）· 不支持全屏 ·
  **摇杆整套输入不实现（VM 边界按无对应设备拦截）**。

## 待做（emulator 侧）

- `0x205` 的格宽还有两支未实现（缩放档 / `set:BlankExtentMode=1`），见残差 `RES-RENDER-NUMTEXT-CELLWIDTH`。
- 触点坐标的「客户区 / 显示模式」换算**产品路径未接线**（`set_touch_client_mapping` 只有自测调用）。
- 字体名字表按 SCOPE 简化：任意名字解析到 Amayui；**两次度量保留**、只在名称类行为上做特殊处理（⛔ 不跳渲染逻辑）。
- 摇杆（轴 / 帽子 / 按钮）：**不实现**，产品路径不打开设备（SCOPE §0 D4）。
- 灰阶 8 位档已按 spec 可达（启动期应用设置入口抄入级数键）——若本模型仍只做 4 位，需按 `CORE-OPS-RENDER-TEXT-204` 的判据补。

## 已知偏差（实现侧选择，不是引擎语义）

- 鼠标左右键交换没有配置键（`set_mouse_swap` 注入口，缺省 false）。
- `0x1f9` 共享对象槽未建模；`0x20e` 门控支缺虚拟全屏类型格；`0x2` 的 −11 位置表 / 返回点表走宿主接缝；
  `0x2`「整个执行结束」用 halt 表达。
- 打字机的"跳过"只清第 29 位、不置第 27 位（模型没有第 27 位服务者）。
- ADV 旧路径下 `0x6e/0x196` 仍经宿主兜底建画布、`kTextLayerFallbackBase` 兜底基数。
- 摇杆 / 名字表 / 保存闸三处按 SCOPE 简化（见上「实现线」），⛔ 不是引擎语义。

## 待基建

- spec 工具缺「正文 / audit 里引用的 `RES-…` / `GAP-…` 必须存在」的不变量；历史悬空 id 仍有：
  `RES-23D-SCENE-UNBIND`、`RES-AUDIO-BGM-FADE-SERVICE`、`RES-PARAM-21668-INIT`（代码注释里各一处）。
- `data/shape` 缺若干载体成员（`T_FadeTimer`、`T_Scene_42c` 的 `+8/+c/+10`、`T_Command` 的 1200 字节宽度等）——
  已另开需求单跟踪（shape 子树）。

## 门禁（怎么查，不记数字）

`pnpm test` · `ctest --test-dir apps/emulator-cxx/out` · `pnpm typecheck` · `pnpm tools spec validate` ·
`pnpm tools requirements validate` · `git diff --check`。

## 工作口径

- **残差只登记在** `data/spec/audit.json`（`pnpm tools spec residuals` 看），⛔ 不在需求单里重复。
- **`apps/emulator-cxx/SCOPE.md` 的「显式不做」只由用户拍板追加**，⛔ 不拿它当待办回收站。
- spec 完整性判据：所有影响可观察行为的字段 / 触发 / 转换都写全，独立实现者只凭 spec 能写出唯一实现。
- spec vs emulator：spec 写领域状态机；emulator 是示范与第二实现证据；分歧按 `amayui-spec` §9.2 归因。
