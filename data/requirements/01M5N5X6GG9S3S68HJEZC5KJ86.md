# 残差第十批：12 抹除图案 · 效果技法内部

- id: REQ-01M5N5X6GG9S3S68HJEZC5KJ86
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-wipe-pattern.test.mjs#★ 12 模式各一例：模式 0 用 S=8 / 宽 800 / 经过时间 = 步长×10 ⇒ 让出 80 像素
- tags: [spec residual]

# 残差第十批：12 抹除图案 · 效果技法内部

## 一句话

把种类 2 的 **12 种抹除图案**逐模式写成条件表并落地纯函数；把种类 0/3 的**效果技法内部**（Fade 两档 / ZoomBlur / SlideBlur / 退路）读清并写进正文。图案**尚未接渲染**。

## 合同

- **12 图案**：每个单元两趟落笔（先 36=A、后 37=B）；12 种都用两块画布。一族 0..3 整幅按道跳；二族 4..7 条带同相逐像素；三族 8..11 条带错相；网格余数 / 多出 `S−余数` / 提前走完。
- **效果技法**：Fade 低档 200 / 高档 201（1 pass、不声明混合）；ZoomBlur / SlideBlur 同一资源 203，各 1 pass，抽数与权重已写；退路 CPU 逐像素另一套；不写"像素等价"。
- **残差**：`WIPE` → `WIPE-BOUNDARY`（D=0 / 矩形外覆盖 / 画布领域名 / 晚 1 像素意图）；`EFFECT-TECHNIQUE` → `EFFECT-EQUIVALENCE`（含 Fade `a_ret=0` 的 0/0）；新增 `BLEND-STATE`。

## emulator-new 落地

- 新纯函数 `renderer/wipe-pattern.ts`：`wipePattern({mode, rect, split, elapsed, duration}) → {strokes, diagnostics}`；一族 2 笔、二/三族每带 2 笔；`duration≤0` / `split≤0` 只记诊断（不定义成"立即完成"）。
- 守卫 `emulator-new-wipe-pattern`（12 条）+ 站点守卫 `spec-sites-batch10.assets`（36 站点）；变异 +5（325 条）。
- **未接渲染**：`scene-command-plan` 的种类 2 通道仍只记 `scene-command-wipe-rect-missing`，`pixi-host` 未调用。

## 仍开着

图案接渲染（落点 / 视野偏移 / 时间戳盖章 + 延迟 / `mode<0` 强制完成副作用在调用方）· `RES-SCENE-COMMAND-WIPE-BOUNDARY` · `-EFFECT-EQUIVALENCE` · `-BLEND-STATE` · 命令档真实绘制 S2+。

## 判据

`pnpm test` 987/0 · `pnpm test:assets` 201/0（1 skip）· `pnpm test:mutation` 325/325 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。复审：一轮 S1 8，已修 1–6、补 falsifier 与站点。

## 怎么查

`pnpm tools spec show CORE-OPS-SCENE-COMMAND-RECORDS` · `docs/02-engine/spec-audit-2026-10.md` §27 · `tools/test/emulator-new-wipe-pattern.test.mjs`。
