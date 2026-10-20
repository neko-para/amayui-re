# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

core spec 的首批两个机制（`CORE-ISA` / `CORE-POOL`）已落盘并有语料级守卫；`apps/emulator-new` 能只读 `data/spec` 解到第一个未实现 opcode。
**本单是交接快照**：状态以各子单 / `pnpm tools requirements plan` 为准；这里只记"换会话 / 换设备必须知道的事"。

## 已完成（凭据 = 守卫 / 文件）

- **机制地图**：`data/spec/core/README.md` —— 14 个机制 + `CORE-VM` 五子机制（§2.2）+ `CORE-FS` / `CORE-BOOT` 显式延后（§1.1）。
- **CORE-ISA**：`data/spec/chapters/10-format.md` 9 条规则；`tools/test/spec-isa.assets.test.mjs`（真实脚本 564 个，0 失败）。
- **CORE-POOL**：`40-pools.md` / `50-engine-state.md`；草稿 19 条全部有归宿（12 条新规则 + 7 条既有规则）。守卫 `tools/test/spec-pool.assets.test.mjs`。
- **spec↔EA 映射**：`data/spec/bindings.json` / `audit.json`（规则数见 `pnpm tools spec report`；每条带站点 EA / 函数 / storage / 证据）。
- **参考实现**：`apps/emulator-new/`（只读 `data/spec`，无构建）；守卫 `tools/test/emulator-new-isolation.test.mjs` + `emulator-new-decode.test.mjs`。
- **文档整理**：`data/spec/{README,chapters/README,spec}.md` 的失效指针与口径矛盾已修；`apps/emulator/README.md` 标"已废弃，仅架构参考"。

## 换机器先跑这四条

```bash
pnpm install                     # 仅 workspace 链接；Windows 建符号链接要提权（AGENTS §3）
pnpm test                        # 纯文本档
pnpm tools spec validate         # #1–#13
pnpm tools requirements validate # 5/5
```

语料档：`pnpm test:assets`（需要游戏安装 / 反汇编语料）。

## 已知红 / 已知限制（别追错方向）

- **shape 的 5 条 assets 测试是先前就红的**（二级对象下钻 / 层级 ≥3 / 入库模型守恒 / 回归钉 / 改坏一处）；`data/shape`、`corpus`、`tools/lib` 未被本轮改动。
- `.tmp/spec-draft/**` **不入库**：ISA / POOL 的原始扫描、报告、feedback 只在本机；内容已并入 `data/spec`，但统计类结论不可从仓库复现（正文已删掉这类数字）。
- `CORE-OPS` 未成篇 ⇒ `emulator-new` 的 handler 表为空，缺口停在第 0 条（设计使然，不是 bug）。
- 7 条既有规则仍用 `SPC-*` id（并号是机械改名，见治理二批）。

## 未提交（★ 换设备的前提）

**工作树包含全部改动，未 `git add` / `commit`。** 另一台设备要看到这些，必须先由用户提交并推送。
未入库但仍在本机的草稿：`.tmp/spec-draft/{core-isa,core-pool,core-vm-scan,tidy}/`。

## 待裁决（已登记，未定）

- `REQ-01M5AM4M5…`：`reviewed` 要求 `@env pure` 守卫 vs 语料型机制只能挂 assets；机制地图是否进分发清单；`engine-operand-types` 守卫误标；codec 无诚实 storage 指针。
- `REQ-01M5AKDR…`：`audit.planned[]` 与机制对齐（数据层）/ 键名复述 / 需求树 `old:`·`repo:` 路径标记 / 机制表机器形态暂缓。

## 下一步（建议顺序）

1. `CORE-OPS` 先做一小族（控制流 + 纯数值），让 `emulator-new` 能执行一段真实脚本前缀。
2. `CORE-VM`（按 `core/README.md` §2.2 五子机制）与 `CORE-FRAME`。
3. `CORE-BOOT` / `CORE-FS`（解除 §1.1 的显式延后）。
4. 治理二批（并号迁移、planned 对齐、路径标记）。
5. `TABLE-RECON`（argc 对账）需要独立守卫；其余残差见 `audit.json`。

## 新会话从这读

- 技能：`amayui-spec`（判据）/ `amayui-emulator`（实现闭环）/ `amayui-re-engine`（取证）。
- 组织真源：`data/spec/core/README.md`；schema：`pnpm tools spec describe`。
- 进度：`pnpm tools requirements plan`；审计：`pnpm tools spec report`。
- 旧实现：`apps/emulator/**` 已废弃（只读 README 的架构段，不读代码）。
