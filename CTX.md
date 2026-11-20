# CTX — 交接快照（2026-11-20 10:40，系统重启后）

> 本文件只在"换设备 / 看交接"时重建；平时不维护。真源仍是 git + `docs/` + 需求树。
> 设备：win32 · 两棵工作树（§14）：`E:\Projects\amayui-re` = main（真相）·
> `E:\Projects\amayui-re-dev` = dev（代码）。

## 0. 当前 HEAD / 工作树

| 树 | HEAD | 状态 |
|---|---|---|
| main | `b14b897` + 本 CTX 提交 | `git status` 空 |
| dev | `b642a76`（C2b-1 F2/F3 修复） | `git status` 空 |

- main 上有 merge commit `ee56984`（dev → main，C0/C1/C2a），之后 `b14b897` 只改需求单。
- dev 在 `b14b897` 上线性追加 C2b-1：`611abc9`（实现）/ `79e4c5f`（pure 用例）/
  `4eb6011`（变异条目）/ `d939480`（d.ts F1）/ `b642a76`（F2/F3 修复）。
- **C2b-1 尚未合入 main**；main 的 `main → dev` 已 ff 到 `b14b897`。
- 系统重启前最后一个 subagent 是 C2b-1 修复；它留下的未提交 WIP 已由父级补完
  （`inCallback()` 从占位的 `true` 改成 `in_callback_`）并提交为 `b642a76`。

## 1. 性能优化结论（main，全部有文档/提交）

| 项 | 结论 | 落点 |
|---|---|---|
| note 写钩子 | **OFF 胜**：写密集 map-reveal 3 对同向，点击后 60 帧中位 **−4.4%**、尖峰帧 **−5.0%**、`__text` **−15.6%**；AOT 里 note 本是 no-op。默认 OFF。 | 提交 `d1cafce`；`docs/03-emulator/` 无单独文档（读数在 `.tmp/note-bench/report.md`） |
| alloca 寄存器提升 | **判负，不启用**：Cut A `__text` 仅 **−0.84%**、栈引用 +81%、spill +151%、墙钟 −1.48%（<5% 分辨力）；主树已回退，patch 在 `.tmp/aot-alloca/cutA.patch`。Cut B 不做。 | `docs/03-emulator/aot-alloca-evaluation.md`（提交 `ede8671`） |
| router 死项清理 | **no-go**：5,312 是 `lift_dyn` 调用点数，不是死表项；300 帧基线 `lift_dyn misses=1,432,759` 仍走 shim + trampoline，不能删。真正无引用的 host-call resume 仅 353 个，删了省 ~15 KB、0 块代码。 | `docs/03-emulator/aot-router-dead-entries-evaluation.md`（提交 `f785157`） |
| 拆 router | **性能上 no-go**：单入口去 switch 只省 61–84 KB（<0.1% 墙钟）；TB 风格 +2.5–4.0 MB（+20–31%）；只有同分量 `bb_exit` 直连（1,891 处）值得小实验。真正成本是 52.0M 次/300 帧 trampoline 往返 + 18,494 次 dispatch call。 | `docs/03-emulator/aot-router-dismantle-evaluation.md`（提交 `f4305d4`） |
| 帧 614 shim 归因 | **具体逻辑 80.4% / 交接边界 10.0%**；边界 0.518 µs/call × 66,943 = 34.7 ms，解释不了 276.5 ms（需 8 倍）。top 3 API 占 0.56% 调用、60.2% shim：`D3DXCreateTexture`/`GetSurfaceLevel` 在 `ComRegistry::createObject`，`CreateFileA` 在 `Vfs` 查找。 | `docs/03-emulator/shim-614-attribution.md`（提交 `cd880a0`） |

## 2. 插件运行时（dev）

- **已合入 main**：C0（能力表/9 项同源）、C1（TriggerRunner/IntentQueue/Executor/EngineRead）、
  C2a（QJS loader / start·onTrigger / ctx 基础 / `--plugin` / 读点 observer / plugin-headless）。
  C2a 经 4 轮独立复审收口；`plugin-runtime` C2a 时 170 checks，C2b-1 后 **308 checks/0**。
- **C2b-1 已完成、未合入 main**：`PluginScript : UiScript`、`RenderCtx`（ui/draw/read/game/params/log）、
  `ctx.draw.rect/text/measure`（不支持的 style 字段 TypeError）、`ctx.ui` 读取、`measure` 的 FontEngine 语义、
  d.ts 的 undefined/pos?/buttons?、render 预算、存下来的 RunCtx 在 render 中拒绝（F2）。
- **C2b-2 未做**：`present_sdl::adoptOverlayScript` + `aot_main` 的窗口装配 + FontEngine 进 PluginHost +
  `game.mouse().buttons` 的生产读数 + `aot-overlay` 回归。
- **C2c 未做**：`scripts/plugins/lib/save.js` 的 `loadSave(slot)`、pure chain、load-save headless e2e。
- C3–C5 未做。

## 3. 构建 / 测试现状

- main：`out` 上次全量构建 **exit 0**；`ctest --test-dir out` **131/131 passed**（208.7 s）；
  `pnpm test` 604 pass；`pnpm verify` 500 pass / 2 skip；`requirements --validate` 6/6。
- dev：C2b-1 后 `cmake --build out --config RelWithDebInfo --target lift-plugin-runtime lift-plugin-headless
  emulator-lift-aot` exit 0；targeted ctest **7/7 passed**；`lift-plugin-runtime` **308 checks/0**；
  `core-shape` **6/6**；examples tsc exit 0。
- 系统重启后没有正在运行的 build / ctest / subagent / background job（`node` 只剩 DSH 服务）。
- `apps/emulator-lift/out` 是**唯一**构建树；⛔ 不要新建 `out-*`。

## 4. 下一步（按顺序）

1. **C2b-2**（dev）：窗口 adopt + FontEngine + `game.mouse().buttons`；跑 `aot-overlay` / `plugin/ui-*` 回归。
2. **C2c**（dev）：`loadSave(slot)` + pure chain + `plugin/load-save-e2e`。
3. 两片都独立复审后，再 `merge dev → main`；main 上更新需求单 C2 状态与锚点，跑全量 ctest。
4. 之后 C3（JS recipe driver parity）/ C4（ControlBus）/ C5（UI→Run）。

## 5. 常用命令

```bash
cd apps/emulator-lift
cmake --build out --config RelWithDebInfo --target lift-plugin-runtime lift-plugin-headless emulator-lift-aot
ctest --test-dir out -R "plugin-(runtime|headless)|ui-script-(quickjs|modules)|trigger-runner|input-plan-read-points|^cli-aot-flags$" --output-on-failure
node --test tools/test/core-shape.test.mjs          # 从仓根跑
```

- C2b-1 报告：`dev:.tmp/plugin-runtime/c2b1-report.md`
- C2b-1 复审：`dev:.tmp/plugin-runtime/c2b1-review.md`
- C2a 报告/复审：`dev:.tmp/plugin-runtime/c2a-report.md` / `c2a-review.md`
- 机械性能工具：`main:.tmp/note-bench/bench.mjs`（note A/B；scenario `save05-map-reveal`）
