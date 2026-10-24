# 残差第八批：显示适配公式 · 矩阵乘入门

- id: REQ-01M5N3DJ9ZHK5331EMXFDEVR2W
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-display-fit.test.mjs#★ VF 关：画布比后缓冲**小且差为奇数** ⇒ 原点**向零**截断（-11，⛔ 不是 floor 的 -12）
- tags: [spec residual]

# 残差第八批：显示适配公式 · 矩阵乘入门

## 一句话

把 `CORE-SCENE-DISPLAY-FIT` 的两条残差读死并落地：显示适配的精确算术、两张适配矩阵的构造与**互斥乘入门**；同时修掉 5.1 与实现里"视图矩阵门写反"的错。命令档执行的结构取证留给下一批。

## 已闭合（2 条）

- `RES-SCENE-DISPLAY-FIT-FORMULA`：输入（BW/BH、vfOn、T、A、refW/refH、固定分辨率模式）、trunc 向零、half=trunc(x*0.5)、双精度、`canvasW=trunc(canvasH*(1/t))`、`rH≤rW` 含等号、VF 关 1:1 居中、T=0/1/2 三支、A!=0、退化出口、三条不变量。
- `RES-SCENE-DISPLAY-FIT-MATRIX`：视图矩阵 = `Scaling`；逆缩放矩阵现场构造 `Scaling(1/sx,1/sy,1)`；**乘入门互斥**：视图 ⇐ `F==0 && (D<0||D==39) && tex∉[20,30)`；逆缩放 ⇐ `F==0 && D>=0 && D!=39 && tex∈[20,30)`；与 6.1 是"0x229 在左、适配在右"。
- 新增 `RES-SCENE-DISPLAY-FIT-SCALE-DIRECTION`（缩放方向需截帧）。

## 纠错

- 5.1 原"目标 ≥ 0 且 ≠ 39 乘视图矩阵"是**反的**：目标 38 与纹理目标不乘视图矩阵；实现同步修正并补 `F` 门。
- 非 `[20,30)` 档位置**不缩放、也不取整**。
- 折回坐标：位置**加上**偏移，偏移 = 负的视口原点 / 内容缩放。
- 固定分辨率模式 = `display:ScreenMode` 生效值（不进公式，只在取值器短路）。
- `T ∉ {0,1,2}` 退化出口：只写参考尺寸，其余格保持旧值。

## emulator-new 落地

- 新纯函数 `renderer/display-fit.ts` 的 `computeDisplayFit`；`session.ts` 去掉 `view: null`，按 config 计算并投影 `SceneViewSnapshot`（含 `fixedResolution` / `screenForm`）。
- `scene-draw-plan.ts` 矩阵门纠正（38/39、band 内外、F 门、固定分辨率短路），新增 `adaptMatrixCount`。
- 守卫：新增 `emulator-new-display-fit`（12 条）· `-scene-draw-plan` 加 8 条矩阵门；变异 +4。

## 仍开着

`RES-SCENE-DISPLAY-FIT-SCALE-DIRECTION` · `RES-SCENE-COMMAND-KIND0-ALPHA` · `KIND1-FLAG` · `RES-INPUT-103-NEGATIVE-MODULUS` / `-104-CURSOR-INIT` · `RES-BOOT-GFX-LOGSINK-PATH-DATA` / `-SINK2` · `RES-TEX-...-OOB` / `-SELFTARGET` / `-BELOW-RANGE`。命令档执行仍未建模；`createDevice` 忽略 backBuffer 宽高。

## 判据

`pnpm test` 963/0 · `pnpm test:assets` 200/0（1 skip）· `pnpm test:mutation` 315/315 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。复审：只读 `data/spec` 一轮 6 S1（已逐条修订）。

## 怎么查

`pnpm tools spec show CORE-SCENE-DISPLAY-FIT` · `docs/02-engine/spec-audit-2026-10.md` §25 · `tools/test/emulator-new-display-fit.test.mjs`。
