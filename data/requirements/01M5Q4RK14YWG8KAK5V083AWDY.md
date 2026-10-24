# renderer 重建 + Electron smoke

- id: REQ-01M5Q4RK14YWG8KAK5V083AWDY
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: .tmp/emulator-new-electron-smoke.png
- tags: [emulator s2]

# 第三十批：renderer 重建 + Electron smoke

## 一句话

S2 / 存档改动后 renderer 构建与 Electron smoke 均通过；截图与 demo 场景一致，无白闪 / 崩溃。

## 证据

- `pnpm_priv run build-renderer`：退出 0，754 modules，`dist/renderer/index.html` 更新。
- `pnpm_priv run smoke`：退出 0；`backend=webgl 1280×721`；字体加载成功；`--smoke OK`；截图 `.tmp/emulator-new-electron-smoke.png`（1920×1080，21454 B）。
- 截图：左上标题 `天結いキャッスルマイスター / Amayui CN`，其余黑底；demo 场景 1 纹理 / 1 绘制项 / 目标 −1。

## 未做

`?commandExecKinds=...` 的真实安装目录像素对照；效果技法 / 像素等价。

## 判据

`pnpm test` 1068/0 · `pnpm test:assets` 204/0（1 skip）· `pnpm test:mutation` 361/361 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §62 · `.tmp/emulator-new-electron-smoke.png`。
