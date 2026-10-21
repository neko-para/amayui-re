# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

core spec 的 `CORE-ISA` / `CORE-POOL` / `CORE-CONFIG` / `CORE-FRAME` 已落盘并有语料级 / pure 守卫；`apps/emulator-new`（只读 `data/spec`）走**真实入口**：安装目录 → `SYS4INI.BIN`（ALF 索引）→ 统一文件 id → 根脚本 **id 0**（`SYSTEM4.BIN`），并按 spec 执行。
**本单是交接快照**：状态以各子单 / `pnpm tools requirements plan` 为准；这里只记"换会话 / 换设备必须知道的事"。
★ **目标状态（2026-10-21）**：从 id 0 跑到 LOGO `0x20F` **仍未达成** —— 停在子脚本 `LOADCONFIG.BIN` 偏移 192（`0x192`，大函数，未取证到可实现粒度）；**本轮没有推进前沿**，做的是 spec 质量治理。剩余路径见 `REQ-01M5CYFJY8P1PQ0AANGE87K1VJ`。

## 本轮（2026-10-21）spec 治理（前沿未动）

- **storage 指针全部改挂 shape**：`bindings.json` 里 `shape:` 89 处 / `layout:` 0 处；解析器**删掉了 `layout:` 分支**（不是计数棘轮，是结构性拒绝），守卫 `tools/test/spec.test.mjs#storage 指针只认 shape：bindings 里 0 处 layout:` + 变异条目。收口单 `REQ-01M5D2Q214Y8949Q0PBP236K29`（done）。
- **两轮只读审稿**（60+45 条）逐条处置，清单与状态在 `docs/02-engine/spec-audit-2026-10.md` §八 / §九；可文本裁决的已改，12 条缺证据的转 `REQ-01M5D4NNZWAJVJFAHXQ9C5Z6GE` 并进 `audit.json` 残差。
- 关键修正：`0x3` **先**写父帧长度槽 = 3 **再**查深度；`0x2` 释放有条件（父帧号 + 1 == 当前帧号）；type 8 读 / 写全抛；读浮池内 32 位 / 返回 64 位；配置键 9 字节、命名键与脚本键**两张表**；ALF / AGF **同一个 LZSS**；ALF 头 268（300 属另一加载器）。
- 门禁：`spec validate` 13/13 · `pnpm test` **569/569** · `ledger` 8/8 · `requirements` 5/5 · `typecheck` 绿。

## 已完成（凭据 = 守卫 / 文件）

- **机制地图**：`data/spec/core/README.md` —— 14 个机制 + `CORE-VM` 五子机制（§2.2）+ `CORE-FS` / `CORE-BOOT` 显式延后（§1.1）。
- **CORE-ISA**：`10-format.md` 9 条；守卫 `spec-isa.assets`。
- **CORE-POOL**：`40-pools.md` / `50-engine-state.md`；19 条全部有归宿；守卫 `spec-pool.assets`。
- **CORE-OPS**：`60-ops.md` —— 数值 / 纹理 / 场景颜色 / 语音 / 引擎级标量 / 转发型 / 列表项 / 文本 / 子系统转发 / 字段对 / 条件跳转 / 多脚本（`0x3`/`0x2`/`0x9`）/ 等待输入 / 影片 / 空操作；守卫 `emulator-new-voice` + `emulator-new-scalars-thin`。
- **CORE-FRAME**：`15-frame.md` —— 帧栈（容量 40、父帧链、条件释放）+ `0x8f call` / `0x5 ret`（位置 = 同脚本 dword 序号、每帧返回栈、−1 哨兵）；守卫 `emulator-new-frame`。
- **CORE-CONFIG**：`55-config.md` 5 条 + `format/config-keys.json`；emulator-new 读索引 51 条默认 + 可选 `SYS4REG.INI`，`--init-config` 生成默认文件；守卫 `emulator-new-config{,.assets}`。
- **CORE-VM / CORE-TIMING**：`70-vm.md` —— 互斥分支链 / 门 / 状态位 / 时钟 / 计时窗 5 条。
- **真实入口**：`src/lzss.ts` / `src/alf.ts` / `src/host.ts` —— ALF 索引（S4IC，8 归档 / 21109 条目）、4 KiB 环形窗口 LZSS、统一文件 id == TOC 条目下标、松散文件优先、按 `(offset,length)` 切片。
- **参考实现**：`apps/emulator-new/`（只读 `data/spec`，无构建，已进 `pnpm typecheck`）；守卫 `emulator-new-isolation` · `decode` · `ops` · `sound` · `frame` · `texture` · `scene` · `alf` · `noop`（pure）+ `boot` / `config` / `logo`（assets）。
- **spec↔EA 映射**：`bindings.json` / `audit.json`（规则 / 章节数用 `pnpm tools spec chapters`）。

## 换机器先跑这四条

```bash
pnpm install                     # 仅 workspace 链接；Windows 建符号链接要提权（AGENTS §3）
pnpm test                        # 纯文本档（当前 569 条）
pnpm tools spec validate         # #1–#13
pnpm tools requirements validate # 5/5
```
语料档：`pnpm test:assets`（需要游戏安装 / 反汇编语料；本机有 2 条**先前就红**的环境项，与本轮无关）。

## 已知红 / 已知限制（别追错方向）

- **`pnpm test:assets` 不是全绿**（2 条本机环境项，与本轮无关）；未取证路径见各规则残差。
- **启动前段不是直线**：`#5` 是 `0x8f call`（不是"第六个标量写"）；`opcodes-handlers.assets.test.mjs` 的 16 项是**静态点名**，不是执行顺序。
- **`0x20F play-movie` 已实现但 LOGO 运行停在它前面**；`0x1fa` 清共享对象槽而**引用计数未建模** ⇒ 先补引用计数再去掉 stop-at。
- **`CORE-FS` / `CORE-BOOT` 的 spec 规则还没成篇**：已开单 `REQ-01M5CWFJTK…`。⛔ 别把"实现里有"当成"规范里有"。
- **`0x88` 的一条知识层记录与字节不符**（`ENGINE_SCALAR_BITS` 把 `0x71` 记成 `when:'always'`）：bug 单（S3）。
- 只读 `SYS4INI.BIN` 一份索引：`APPEND*.AAI` 叠加索引未实现；配置脚本访问只支持 type 3 / 5；ALF 尾部严格解析、`REGKEY` 按低 32 位。

## 入库状态（★ 换设备的前提）

**工作树有未提交改动**（2026-10-21 的 spec 治理；按长期纪律不 `git add` / `commit`）。HEAD = `689d487 feat: review`（2026-10-21）⇒ 另一台设备 `git pull` 只看到**提交时**的内容；本轮的 spec 改动在**工作树里**，换设备前要么提交（由你决定），要么先导出 diff。
2026-10-21 本机复跑：`pnpm test` 569/569、`spec validate` 13/13、`ledger validate` 8/8、`requirements validate` 5/5、`typecheck` 绿。只在本地的是 `.tmp/`（草稿与诊断脚本，可重建）。

## 待裁决（已登记，未定）

- `REQ-01M5AM4M5…`：`reviewed` 要求 `@env pure` 守卫 vs 语料型机制只能挂 assets；机制地图是否进分发清单；`engine-operand-types` 守卫误标；codec 无诚实 storage 指针。
- `REQ-01M5AKDR…`：`audit.planned[]` 与机制对齐（数据层）/ 键名复述 / 需求树 `old:`·`repo:` 路径标记 / 机制表机器形态暂缓。

## 下一步（建议顺序）

1. 从**子脚本 `LOADCONFIG.BIN` 偏移 192 的 `0x192`**（argc 2）继续。★ `0x55` 已落（`CORE-OPS-NUMERIC-MOV-INT`）。三条控制流规则在 `15-frame.md`，台账 accepted = `KN-01M5CZSPC45T6K524Z616R6S3B`。
2. `REQ-01M5D4NNZ…`：12 条缺证据的合同缺口（容器下标 / 字段身份 / 默认组 / 颜色 / 边界算术…）逐条取证。
3. `CORE-FS` / `CORE-BOOT` 成篇（`REQ-01M5CWFJTK…`）：取件 5 条 + BOOT 只写有强锚的 5 条。
4. 把 `60-ops.md` / `70-vm.md` / `15-frame.md` 的规则从 `draft` 推到 `reviewed`（要 accepted 台账锚或 `@env pure` 守卫）。
5. 治理二批（并号迁移、planned 对齐、路径标记、`CORE-POOL-SCALAR-*` 前缀改名）。
6. `TABLE-RECON`（argc 对账）需要独立守卫；其余残差见 `audit.json`。

## 新会话从这读

- 技能：`amayui-spec`（判据 + 编辑后必须独立复审）/ `amayui-emulator`（实现闭环）/ `amayui-re-engine`（取证）。
- 组织真源：`data/spec/core/README.md`；schema：`pnpm tools spec describe`。
- 进度：`pnpm tools requirements plan`；审计：`pnpm tools spec report`。
- 旧实现：`apps/emulator/**` 已废弃（只读 README 的架构段，不读代码）。
