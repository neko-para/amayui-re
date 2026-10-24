# 残差第五批：分辨率判据 · 源矩形窗口 · 显示适配 · 0x229

- id: REQ-01M5MWCQX4MP6BV68A0BNY68F0
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-graphics-params.test.mjs#★★ pickResolution：六档样例对请求 1280×720 ⇒ 选 1280×720；逐档符号 = × × ○ × ― ―
- tags: [spec residual]

# 残差第五批：分辨率判据 · 源矩形窗口 · 显示适配 · 0x229

## 一句话

继续收口交接单里的可静态取证残差：图形设备初始化剩余三条、绘制消费者的源矩形步进与编号档、命令族的离屏/遮罩、以及上一轮"场景相机"的生产者问题。audio / text-system / Live2D 仍跳过。

## 已闭合 / 移出（6 条）

`RES-BOOT-GFX-RESOLUTION-PICK` · `-LEGACY-WINDOW-DOWNGRADE` · `-LOGSINK` · `RES-SCENE-COMMAND-DRAW-FLAG` · `-KIND1-OFFSCREEN` · `RES-TEX-DRAW-CONSUMER-RECTSTEP`。

## 合同变化

- `CORE-BOOT-GRAPHICS-INIT`：分辨率校验全量判据（请求对整对覆盖、0.7 两池、七符号、池只留最小一档、收口两支不对称、失败不写回、写回值用于绘制区域与全屏后缓冲）；旧式对象内部窗口化降级（色深<16：确定⇒降级、取消⇒失败；桌面高≤客户区高⇒直接降级；不回写形态字）；日志落点（`system:OutErrorLog`、覆盖语义、无效句柄静默丢弃且无第二落点）。
- `CORE-OPS-SCENE-COMMAND-RECORDS`：种类 1 第 9 操作数 = 影片纹理逐像素遮罩（写表/结束互补）；离屏结果 = 表面拷贝 36 → 影片纹理 + 每帧遮罩混合；"区间之下"只在第一帧路径 + 设备就绪 + 能力档 <2。
- `CORE-OPS-TEXTURE-DRAW-CONSUMER`：4.1 源矩形窗口（0x239 五字段、三档、步进公式、只在到期回写、逐帧标脏、清位由计时窗统一执行）；6.1 场景自身变换（键区间 + 0x229、命令档粗判）；越界非 0 ⇒ 经虚表第 6 项调用；编号档命名纪律。
- 新规则 `CORE-SCENE-DISPLAY-FIT`：显示适配参数来自显示配置，没有脚本 / 存档 / 场景资产写入者（上一轮"相机"是渲染器状态）。
- 新规则 `CORE-OPS-SCENE-SELF-TRANSFORM-229`：先重置，再写键区间 + 三元平移。

## emulator-new 落地

- `pickResolution` 纯函数 + 请求对整对覆盖 + 诊断逐档 + `logSink`。
- `SourceRectWindow` + `setter239` 具名写入 + 规划器 4.1（盖章 / 三档 / 步进 / 到期 `rectWriteBack` / perRow 0 诊断）。
- `scene.selfRecord` + `0x229` handler + 6.1（右开区间、计数 0 不乘、skip 参数）。
- 守卫：`emulator-new-graphics-params`（30）· `emulator-new-scene-draw-plan`（32）· `spec-sites-batch5.assets`（2）；变异 +9。

## 仍开着

`RES-SCENE-COMMAND-KIND1-FLAG`（遮罩极性）· `-BELOW-RANGE`（目的）· `-KIND2-LAYOUT` · `RES-BOOT-GFX-RESOLUTION-DIAG-FORMAT` / `-CACHE` / `LEGACY-DOWNGRADE-EFFECT` / `LOGSINK-PATH` / `-SINK2` · `RES-TEX-DRAW-CONSUMER-OOB` / `-SELFTARGET` / `-BELOW-RANGE` · `RES-SCENE-DISPLAY-FIT-FORMULA` / `-MATRIX`。命令档执行、显示适配公式、场景自身矩阵内容仍未闭合。

## 判据

`pnpm test` 920/0 · `pnpm test:assets` 见收口记录 · `pnpm test:mutation` 301/301 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。复审：只读 `data/spec` 三轮，S1 12 → 2 → 0。

## 怎么查

`pnpm tools spec show <rule>` · `docs/02-engine/spec-audit-2026-10.md` §22 · `tools/test/emulator-new-graphics-params.test.mjs` / `-scene-draw-plan`。
