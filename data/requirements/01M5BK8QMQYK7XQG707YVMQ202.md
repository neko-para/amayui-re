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
★ **目标状态（2026-10-20）**：从 id 0 跑到 LOGO `0x20F` **未达成** —— 当前在子脚本 `LOADCONFIG.BIN` 偏移 192（`0x192`，一个大函数，未取证到可实现的粒度）；剩余路径逐条见 `REQ-01M5CYFJY8P1PQ0AANGE87K1VJ`。

## 已完成（凭据 = 守卫 / 文件）

- **机制地图**：`data/spec/core/README.md` —— 14 个机制 + `CORE-VM` 五子机制（§2.2）+ `CORE-FS` / `CORE-BOOT` 显式延后（§1.1）。
- **CORE-ISA**：`data/spec/chapters/10-format.md` 9 条规则；`tools/test/spec-isa.assets.test.mjs`。
- **CORE-POOL**：`40-pools.md` / `50-engine-state.md`；19 条全部有归宿。守卫 `tools/test/spec-pool.assets.test.mjs`。
- **CORE-OPS**：`60-ops.md` —— 数值 3 条、纹理 5 条、场景/颜色 5 条、**语音族 2 条（0x2f6 停组 + 0x2f8 设音量）**、**引擎级标量 2 条（0x149/0x21b/0x252 + 0x88）**、**转发型 3 条（0x1ca/0x324/0x32f）**、**列表项 1 条（0x70）**、**文本请求 1 条（0x71）**、**子系统转发 2 条（0x73 / 0x78+0x2db）**、**列表项字段对 1 条（0x79/0x1c1）**、**列表项字段 2 条（0x212/0x213/0x25d、0x303）**、**条件跳转 1 条（0xa0）**、**多脚本（0x3/0x2/0x9：代码 + 守卫 + `15-frame.md` 三条规则 + accepted 台账）**、等待与输入 2 条、影片 1 条；守卫 `emulator-new-voice`（10 条）+ `emulator-new-scalars-thin`（4 条）。
- **CORE-FRAME**：`15-frame.md` 2 条 —— `0x8f call` / `0x5 ret`：**位置 = 同脚本内 dword 序号**（不是字节、不是文件 id）、返回栈每帧一份、控制转移把「操作数个数」槽清 0、两侧都有 **−1 哨兵**；守卫 `emulator-new-frame`。
- **CORE-CONFIG**：`55-config.md` 5 条（库 / 默认表 / 玩家文件 / 保存 / 脚本访问）+ `format/config-keys.json`；
  emulator-new 读索引 51 条默认 + 可选 `SYS4REG.INI`（`--config`；缺省按 `USEAPPDATAFOLDER` + `REGFILEPATH` 拼），`--init-config` 生成默认文件；守卫 `emulator-new-config{,.assets}`。
- **CORE-VM / CORE-TIMING**：`70-vm.md` —— 一次迭代 / 门 / 状态位 / 时钟 / 计时窗 5 条。
- **真实入口**：`src/lzss.ts` / `src/alf.ts` / `src/host.ts` —— ALF 索引（S4IC，8 归档 / 21109 条目）、
  4 KiB 环形窗口 LZSS（绝对写位置）、统一文件 id == TOC 条目下标、**松散文件优先**、归档按 `(offset,length)` 切片。
  根脚本 id 0 = `SYSTEM4.BIN`；当前启动前段执行 **85 条**：`0x3 call-script` → 子脚本 `LOADCONFIG.BIN` 内已跑到偏移 **192**（`0x192`）。
- **参考实现**：`apps/emulator-new/`（只读 `data/spec`，无构建，已进 `pnpm typecheck`）。守卫：
  `emulator-new-isolation` · `decode` · `ops` · `sound` · `frame` · `texture` · `scene` · `alf` · `noop`（pure），
  以及 `emulator-new-boot.assets`（启动棘轮）、`emulator-new-config.assets`、`emulator-new-logo.assets` 三条 assets。
- **spec↔EA 映射**：`data/spec/bindings.json` / `audit.json`（规则数见 `pnpm tools spec report`）。

## 换机器先跑这四条

```bash
pnpm install                     # 仅 workspace 链接；Windows 建符号链接要提权（AGENTS §3）
pnpm test                        # 纯文本档（当前 568 条）
pnpm tools spec validate         # #1–#13
pnpm tools requirements validate # 5/5
```

语料档：`pnpm test:assets`（需要游戏安装 / 反汇编语料；本机有 2 条**先前就红**的环境项，与本轮无关）。

## 已知红 / 已知限制（别追错方向）

- **`pnpm test:assets` 不是全绿**（2 条本机环境项，与本轮无关）；启动前段/空栈等**未取证路径**见各自规则的残差。
- **启动前段不是直线**：`#5` 是 `0x8f call`（不是"第六个标量写"）；`opcodes-handlers.assets.test.mjs` 里那份 16 项清单是**静态点名**，不是执行顺序 —— 别拿它当"下一条该实现谁"。
- **`0x20F play-movie` 已实现但 LOGO 运行停在它前面**；`0x1fa` 清共享对象槽而**引用计数未建模** ⇒ 先补引用计数再去掉 stop-at。
- `CORE-OPS` 的其余 opcode 仍是缺口（"执行到第几条 / 停在哪"由棘轮与 `--json` 回答）；**引擎缺省 handler（抛异常）**与**spec 还没写**当前只区分出后者（缺口报 `unimplemented`）。
- 浮点池"未写槽"的初值规范不说：参考实现要跑真实脚本必须显式 `--uninitialized-float zero`，每次读进 `diagnostics`。
- **`CORE-FS` / `CORE-BOOT` 的 spec 规则还没成篇**：已开单 `REQ-01M5CWFJTK…`（FS 5 条取件合同今天可写；BOOT 只能写有强锚的 5 条 + 3 条残差）。⛔ 别把"实现里有"当成"规范里有"。
- **`0x88` 的一条知识层记录与字节不符**（`ENGINE_SCALAR_BITS` 把 `0x71` 记成 `when:'always'`）：已开 bug 单（S3）。
- 只读 `SYS4INI.BIN` 一份索引：`APPEND*.AAI` 的叠加索引未实现；配置脚本访问只支持 type 3 / 5；ALF 尾部严格解析、`REGKEY` 按低 32 位。

## 入库状态（★ 换设备的前提）

**已提交**：工作树干净，HEAD = `68ca6d6 feat: config loads`（2026-10-20）⇒ 另一台设备 `git pull` 即能看到全部实现。2026-10-20 本机复跑：`pnpm test` 568/568、`pnpm tools spec validate` 13/13、`pnpm tools ledger validate` 8/8、`pnpm tools requirements validate` 5/5、`pnpm typecheck` 绿。只在本地的是 `.tmp/`（草稿与诊断脚本，可重建）。

## 待裁决（已登记，未定）

- `REQ-01M5AM4M5…`：`reviewed` 要求 `@env pure` 守卫 vs 语料型机制只能挂 assets；机制地图是否进分发清单；`engine-operand-types` 守卫误标；codec 无诚实 storage 指针。
- `REQ-01M5AKDR…`：`audit.planned[]` 与机制对齐（数据层）/ 键名复述 / 需求树 `old:`·`repo:` 路径标记 / 机制表机器形态暂缓。

## 下一步（建议顺序）

1. 从**子脚本 `LOADCONFIG.BIN` 偏移 192 的 `0x192`**（argc 2）继续。★ `0x55` 已落（`CORE-OPS-NUMERIC-MOV-INT`）。三条控制流规则在 `15-frame.md`，台账 accepted = `KN-01M5CZSPC45T6K524Z616R6S3B`。
2. `CORE-FS` / `CORE-BOOT` 成篇（`REQ-01M5CWFJTK…`）：取件 5 条 + BOOT 只写有强锚的那 5 条，锚清单见该单。
3. `0x149` / `0x21b` / `0x252` / `0x88`（call 返回后才执行）：台账已有逐字观察、**7 个槽的领域角色全部未定** ⇒ 规则最多写"写入形态"，别写"含义"。
4. 把 `60-ops.md` / `70-vm.md` / `15-frame.md` 的规则从 `draft` 推到 `reviewed`（要 accepted 台账锚或 `@env pure` 守卫）。
5. 治理二批（并号迁移、planned 对齐、路径标记）。
6. `TABLE-RECON`（argc 对账）需要独立守卫；其余残差见 `audit.json`。

## 新会话从这读

- 技能：`amayui-spec`（判据）/ `amayui-emulator`（实现闭环）/ `amayui-re-engine`（取证）。
- 组织真源：`data/spec/core/README.md`；schema：`pnpm tools spec describe`。
- 进度：`pnpm tools requirements plan`；审计：`pnpm tools spec report`。
- 旧实现：`apps/emulator/**` 已废弃（只读 README 的架构段，不读代码）。
