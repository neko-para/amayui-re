# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

core spec 的 `CORE-ISA` / `CORE-POOL` / `CORE-CONFIG` 已落盘并有语料级 / pure 守卫；`apps/emulator-new`（只读 `data/spec`）现在走**真实入口**：安装目录 → `SYS4INI.BIN`（ALF 索引）→ 统一文件 id → 根脚本 **id 0**（`SYSTEM4.BIN`），并按 spec 执行。`LOGO.BIN` 仍可执行到 `0x20F play-movie` 前（46 条 / 314 格 / 5024ms），但它只是 `--root-id` 的诊断路径，**不再是启动入口**。
**本单是交接快照**：状态以各子单 / `pnpm tools requirements plan` 为准；这里只记"换会话 / 换设备必须知道的事"。

## 已完成（凭据 = 守卫 / 文件）

- **机制地图**：`data/spec/core/README.md` —— 14 个机制 + `CORE-VM` 五子机制（§2.2）+ `CORE-FS` / `CORE-BOOT` 显式延后（§1.1）。
- **CORE-ISA**：`data/spec/chapters/10-format.md` 9 条规则；`tools/test/spec-isa.assets.test.mjs`。
- **CORE-POOL**：`40-pools.md` / `50-engine-state.md`；19 条全部有归宿。守卫 `tools/test/spec-pool.assets.test.mjs`。
- **CORE-OPS**：`60-ops.md` —— 数值 3 条、纹理 5 条、场景/颜色 5 条、等待与输入 2 条、影片 1 条；`bindings.json` 有 EA 站点。
- **CORE-CONFIG**：`55-config.md` 5 条（库 / 默认表 / 玩家文件 / 保存 / 脚本访问）+ `format/config-keys.json`；
  emulator-new 读索引 51 条默认 + 可选 `SYS4REG.INI`（`--config`；缺省按 `USEAPPDATAFOLDER` + `REGFILEPATH` 拼），`--init-config` 生成默认文件；守卫 `emulator-new-config{,.assets}`。
- **CORE-VM / CORE-TIMING**：`70-vm.md` —— 一次迭代 / 门 / 状态位 / 时钟 / 计时窗 5 条。
- **真实入口**：`src/lzss.ts` / `src/alf.ts` / `src/host.ts` —— ALF 索引（S4IC，8 归档 / 21109 条目）、
  4 KiB 环形窗口 LZSS（绝对写位置）、统一文件 id == TOC 条目下标、**松散文件优先**、归档按 `(offset,length)` 切片。
  根脚本 id 0 = `SYSTEM4.BIN`；当前启动执行 `0x1a7` / `0x1a8` 后停在 `0x2f6`。
- **启动阶段记账**：`src/boot.ts` 把 `_WinMain@16` → 主循环之间的 12 个阶段逐条登记（2 建模 / 3 部分 / 0 未建模 / 7 宿主）；配置只到 partial（库已建立，显示副作用未建模），文本与 `--json` 都输出。
- **参考实现**：`apps/emulator-new/`（只读 `data/spec`，无构建，已进 `pnpm typecheck`）。守卫：
  `emulator-new-isolation` · `emulator-new-decode` · `emulator-new-ops` · `emulator-new-texture` · `emulator-new-scene` · `emulator-new-alf` · `emulator-new-noop`（pure），
  以及 `emulator-new-boot.assets`（启动棘轮 / 取件顺序）、`emulator-new-config.assets`（配置默认 / 覆盖 / 生成）与 `emulator-new-logo.assets`（LOGO 前沿棘轮）三条 assets。
- **spec↔EA 映射**：`data/spec/bindings.json` / `audit.json`（规则数见 `pnpm tools spec report`）。

## 换机器先跑这四条

```bash
pnpm install                     # 仅 workspace 链接；Windows 建符号链接要提权（AGENTS §3）
pnpm test                        # 纯文本档
pnpm tools spec validate         # #1–#13
pnpm tools requirements validate # 5/5
```

语料档：`pnpm test:assets`（需要游戏安装 / 反汇编语料）。★ 本机仍有**先前就红**的 2 条环境项（corpus 2）—— 与本轮无关；`emulator-new` 的三条 assets 用例通过（shape 的 5 条已收口：`REQ-01M5CBD13M3XQYKTA2YFA82YJC`）。

## 已知红 / 已知限制（别追错方向）

- **`pnpm test:assets` 不是全绿**（2 条，都是**本机环境**，不是产品缺陷）：corpus 的两条本机写入口（临时清单按入库的 Windows 盘符根解析）——先前状态。shape 与 spec-isa 档已全绿。
- **`0x20F play-movie` 已实现但 LOGO 运行停在它前面**（停止条件 = "停在它前面"）；`0x1fa` 清共享对象槽而**引用计数未建模** ⇒ 先补引用计数再去掉 stop-at。
- `CORE-OPS` 的其余 opcode 仍是缺口（"执行到第几条 / 停在哪"由棘轮与 `--json` 回答）；且**引擎缺省 handler（抛异常）**与**spec 还没写**当前只区分出后者（缺口报 `unimplemented`）。
- 浮点池"未写槽"的初值规范不说：参考实现要跑真实脚本必须显式 `--uninitialized-float zero`，每次读进 `diagnostics`。
- **`CORE-FS` / `CORE-BOOT` 的 spec 规则还没成篇**：入口实现与守卫先落（`REQ-01M5C0G36…`），把"松散优先 / 根脚本 id 0"
  写成带锚的合同是后续单。⛔ 别把"实现里有"当成"规范里有"。
- 只读 `SYS4INI.BIN` 一份索引：`APPEND*.AAI` 的叠加索引未实现；配置脚本访问只支持 type 3 / 5（引用族响亮失败，等 `CORE-POOL`）；ALF 尾部严格解析、`REGKEY` 按低 32 位。
- 7 条既有规则仍用 `SPC-*` id（并号是机械改名，见治理二批）。

## 未提交（★ 换设备的前提）

**工作树包含全部改动，未 `git add` / `commit`。** 另一台设备要看到这些，必须先由用户提交并推送。
未入库但仍在本机的草稿：`.tmp/spec-draft/{core-isa,core-pool,core-vm-scan,tidy}/` 与 `ops-texture` / `ops-scene` / `ops-timing`（历史批次）。

## 待裁决（已登记，未定）

- `REQ-01M5AM4M5…`：`reviewed` 要求 `@env pure` 守卫 vs 语料型机制只能挂 assets；机制地图是否进分发清单；`engine-operand-types` 守卫误标；codec 无诚实 storage 指针。
- `REQ-01M5AKDR…`：`audit.planned[]` 与机制对齐（数据层）/ 键名复述 / 需求树 `old:`·`repo:` 路径标记 / 机制表机器形态暂缓。

## 下一步（建议顺序）

1. **SYSTEM4 启动前段**：从 `0x2f6` 继续（台账已有 `0x2f6` / `0x149` / `0x21b` / `0x88` / `0x1ca` / `0x252` / `0x324` / `0x32f` / `0x70` … 的 accepted 观察），把启动棘轮往后推；配置服务已补（`CORE-CONFIG`）。
2. `CORE-FS` / `CORE-BOOT` 成篇：松散优先、根脚本 id 0、索引/归档失败路径、宿主启动阶段的边界写成带锚的合同（解除 §1.1 的显式延后）。
3. `0x20F` 之后的执行路径：引用计数（`0x1fa` 清共享对象槽）+ `0x2 exit`（帧返回/中止）+ `CORE-FRAME` 成篇。
4. 把 `60-ops.md` / `70-vm.md` 的规则从 `draft` 推到 `reviewed`（补 accepted 台账锚；assets 守卫不算 reviewed 的强证据）。
5. 治理二批（并号迁移、planned 对齐、路径标记）。
6. `TABLE-RECON`（argc 对账）需要独立守卫；其余残差见 `audit.json`。

## 新会话从这读

- 技能：`amayui-spec`（判据）/ `amayui-emulator`（实现闭环）/ `amayui-re-engine`（取证）。
- 组织真源：`data/spec/core/README.md`；schema：`pnpm tools spec describe`。
- 进度：`pnpm tools requirements plan`；审计：`pnpm tools spec report`。
- 旧实现：`apps/emulator/**` 已废弃（只读 README 的架构段，不读代码）。
