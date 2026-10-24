# 残差第四批：场景命令字段 · 绘制消费者档位 · 图形重试/旧式路线

- id: REQ-01M5MSYCVWK0HC9WR9F5ZBGF9S
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-draw-plan.test.mjs#★ 空纹理槽 ⇒ 上报（带纹理编号）并跳过：不产出绘制实参（于是调用方没有东西可建，⛔ 不建占位块）
- tags: [spec residual]

# 残差第四批：场景命令字段 · 绘制消费者档位 · 图形重试/旧式路线

## 一句话

把第三批后仍开着的四条主线残差收口：场景命令族（种类 1/3 字段 + 命令档请求 + 种类 0 失败）、纹理消费者（位置四步 / 颜色 / 附加组派生 / 越界 / 自引用）、图形设备初始化（重试序列 / 旧式实参 / 失败双通道）、池与 AGF 的数据事实。audio / text-system / Live2D 仍跳过。

## 已闭合（13 条旧残差）

`RES-SCENE-COMMAND-KIND1-LAYOUT` · `-KIND3-LAYOUT` · `-VARIANT` · `-DRAW-REQUESTS` · `-TEXTURE-FAIL` · `RES-BOOT-GFX-RETRY-ORDER` · `-FULLSCREEN-COMBO` · `-TEXT` · `RES-TEX-DRAW-CONSUMER-OFFSET` · `-COLOR` · `-BLEND` · `RES-POOL-MEMFLIP-DEFAULT` · `RES-AGF-ALPHA-32`。另移除 `RES-TEX-DRAW-OOB`（与消费者 OOB 重复）与 `RES-TEX-DETACH-FIFTH-TABLE`（合同已接住）。

## 合同变化（都在既有规则内）

- `CORE-OPS-SCENE-COMMAND-RECORDS`：种类 1 = **影片覆盖命令**（12 操作数布局、时间戳判第一帧、之后只标记不画）；种类 3 两变体（滑移 / 缩放模糊）的起终对与推进（不夹取）；命令档请求（条目自己的纹理、走消费者、每帧一次、辅助 B 不参与、离屏目标恢复）；种类 0 请求在写表前、失败仍写表并上报（不带脚本位置）。
- `CORE-OPS-TEXTURE-DRAW-CONSUMER`：1000 格槽表 / 消费路径不检查 / 越界读相邻格；自引用不跳过；**5.1 位置四步**；本步派生色只写出参、插值不落表、到期写回；位 2 = **附加组派生**（改输入，不是第二次绘制）。
- `CORE-BOOT-GRAPHICS-INIT`：尝试序列 6/2/3/1（设备类型外层、顶点内层、每轮重置）；旧式路线实参表；失败双通道 + 逐步骤表（对话框模式错误但不中止、32 位格式只警告、旧式只发用户消息）。
- `SPC-POOL-MEMFLIP`：发行默认表把 `ENABLEMEMFLIP` 置 0（数据事实，不是合同）。
- `FMT-AGF-PIXELS`：32bpp 不使用 alpha 段（四条装载路径 / 两处 32bpp 实现穷尽；18 份 AGF 中 0 份 32bpp）。

## emulator-new 落地

- 场景命令：`SceneCommandRecord` 全字段 + `TextureObject.creationFailed`；0x223 失败仍写表/置脏/上报；0x24d 槽空抛且旧记录不动；0x250/251 两变体字段互不共用。**命令档执行仍未建模**（诊断保留）。
- 绘制消费者：纯规划器 `renderer/scene-draw-plan.ts`（空槽上报并跳过、夹空静默、位置四步、自引用不跳过、位 2 只派生一次）；`pixi-host` 删占位块与自引用跳过。
- 图形：`deviceCreationAttempts` 真重试（null ⇒ 下一次）；旧式行为位 / `FullScreenBit` / 客户区；两个独立路线位；`graphicsLog{category,step,text}` + 逐步骤 outcome 表。
- 守卫：新增 `emulator-new-scene-draw-plan`、`spec-sites-batch4.assets`；更新 `emulator-new-scene-commands` / `-graphics-params` / `-window` / `-config.assets`；变异 +12。

## 仍开着（诚实项）

`RES-SCENE-COMMAND-KIND1-FLAG` / `-DRAW-FLAG` / `-KIND1-BELOW-RANGE` / `-KIND1-OFFSCREEN` / `-TEXTURE-RETRY` · `RES-BOOT-GFX-LEGACY-WINDOW-DOWNGRADE` / `-RESOLUTION-PICK` / `-LOGSINK` · `RES-TEX-DRAW-CONSUMER-OOB` / `-SELFTARGET` / `-RECTSTEP` / `-BELOW-RANGE`。命令档**执行**、相机与附加组字段的生产者、分辨率枚举判据、32bpp 端到端样本仍需后续批次或真机。

## 判据

`pnpm test` 897/0 · `pnpm test:assets` 192/0（1 skip）· `pnpm test:mutation` 292/292 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。复审：只读 `data/spec` 三轮，S1 7 → 2 → 0。

## 怎么查

`pnpm tools spec show <rule>` · `pnpm tools spec report` · `docs/02-engine/spec-audit-2026-10.md` §21 · `tools/test/emulator-new-scene-draw-plan.test.mjs` / `-scene-commands` / `-graphics-params`。
