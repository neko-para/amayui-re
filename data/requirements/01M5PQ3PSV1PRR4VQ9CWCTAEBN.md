# 残差第二十六批：四条代码确定残差 · STH-CAPTURE

- id: REQ-01M5PQ3PSV1PRR4VQ9CWCTAEBN
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-frame.test.mjs#★ 0x8f：位置 × 4 = 字节偏移
- tags: [spec residual]

# 第二十六批：四条代码确定残差 + STH-CAPTURE

## 一句话

关闭四条“代码确定、只缺观测”残差与 `RES-SAVE-STH-CAPTURE`；修正 `0x3` / `0x6` 装载失败与异常重试的合同错误，并把 STH 边界改造登记为独立实现单。

## 四条

- `RES-FRAME-CALL-FRAME-STACK`：`0x8` 只写目标帧三格，不动返回栈；计数只由装载 / 构造 / `0x9` 清，`0x2` 与释放不清 ⇒ 重入未释放槽会复用旧返回点；count=0 读相邻数组属 UB。正文补条件。
- `RES-EXC-RETRY-FRAME`：重试只写日志 + 跳入口，不动帧/PC；但 `0x3` / `0x6` 先换帧后装载，打开失败抛类别 7（可重试）⇒ 重试落在新帧当前位置；头坏 / 返回 0 才退出。**正文 0x3/0x6 共 4 处改正**（原“已恢复 / 不重试 / 三支”与代码不符）；深度失败是类别 1，只能终止。
- `RES-BOOT-CLEANUP-WINDOW`：`DestroyWindow` 全语料唯一在 WndProc 关闭分支；图形初始化失败路径不销毁窗口、直接返回 0。正文已写，关闭。
- `RES-BOOT-FAIL-RETURN-OBS`：返回值只有两个写点：重复实例 `-1`、其余 `0`（含索引失败）；进程退出码属 CRT。正文已写，关闭。

## STH-CAPTURE

- 生效实现 = `set:DrawMode` 缓存，缺省 0、发行配置未覆盖 ⇒ **屏幕捕捉、恒 24 位**；纹理路径仅 `DrawMode == 1`。
- 屏幕路径把第 3 操作数当**表面选择**，0/1/2 被拒（不读文件内容、错误通道、状态 2）⇒ 本作合同。
- 32 位布局代码已定、零样本：源带 alpha ⇒ 32 位，B/G/R/A、alpha 左移、行宽 32 位对齐、声明长度 +40、自下而上。
- `RES-SAVE-STH-CAPTURE` 关闭；新 `RES-SAVE-STH-CAPTURE-32BIT-SAMPLE`（32 位样本）。
- **实现边界**（core 自建/自解 BMP、平台只给像素+行宽+掩码）另开实现单，不在本批只读范围。

## 账

`unknown` 430 → **426**。

## 判据

`pnpm test` 1024/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 338/338 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show CORE-OPS-FRAME-CALL-SCRIPT` / `CORE-OPS-FRAME-LOAD-FRAME` / `FMT-SAVE-STH` · `docs/02-engine/spec-audit-2026-10.md` §43。
