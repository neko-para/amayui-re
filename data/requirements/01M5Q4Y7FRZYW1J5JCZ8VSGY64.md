# Electron 启动参数：--command-exec-kinds

- id: REQ-01M5Q4Y7FRZYW1J5JCZ8VSGY64
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: .tmp/emulator-new-electron-smoke.png
- tags: [emulator s2]

# 第三十批：Electron 启动参数 `--command-exec-kinds`

## 一句话

把 S2 开闸从浏览器 URL 接到 Electron 启动参数：`--command-exec-kinds 0,1,2`。

## 代码

- `main.ts`：`Args.commandExecKinds` + 解析（只留 0..3）；传给 `openWindowHost`。
- `window-host.ts`：选项 `commandExecKinds` ⇒ renderer URL 拼 `?commandExecKinds=...`。
- renderer 的 `parseCommandExecKinds` 读取该查询串。
- 帮助文本加一行。

## 实证

`build-renderer` / `smoke` 均退出 0；无参数时 URL 不拼查询串，行为不变。

## 未做

带参数真实安装的像素对照（需人工看窗口）。

## 判据

`pnpm test` 1068/0 · `pnpm test:assets` 204/0（1 skip）· `pnpm test:mutation` 361/361 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §63 · `apps/emulator-new/src/platform/electron/main.ts`。
