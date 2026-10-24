# 残差第二十五批：真机/采样 29 条分诊

- id: REQ-01M5PNQ4EDCFS5AGY38YPTPN0T
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/spec.test.mjs#★ 最小合法样本：0 条问题，且未登记 = 0
- tags: [spec residual]

# 残差第二十五批：「真机/采样」29 条分诊

## 一句话

把 29 条笼统“需要真机”的残差按“引擎是否定义”分诊：真 UB 标记 `discarded` 并建议显式报错；代码已定义的改写成条件合同；平台边界延后到输入系统。

## 处置

- **真 UB 8 条 + 时钟回绕** → `discarded`：越界 / 未初始化 / 类型混淆 / 平台返回值；写 `reason/impact/range`，`next` = 实现显式报错、不复刻内存布局。
- **子头变体关闭**：短于 28 补 0、长于 28 忽略，已是合同；样本不是 spec 义务。
- **像素叠放合并**：负键 + 同键叠放 → `RES-SCENE-DRAW-STACK-SAMPLE`（引擎次序已定，只剩截图）。
- **样本缺失 9 条改写**：SYS3 / 坏编码 / ALF S5 / AGF 变体 / S3 族 / BMP 低位深 / STH 解码 —— 分支按代码定义，样本只影响数据覆盖。
- **代码确定 4 条改写**：帧栈 / 异常重试 / 窗口清理 / 退出码 —— 静态读或定向实验即可。
- **输入平台 3 条**：输入系统批次处理。
- **STH 捕捉**：保留（配置后端 + 实现偏差）。

## spec

`CORE-BOOT-LOOP-EXC` 增加：语言层未定义输入不规定可观察值；实现建议显式报错（诊断通道）。

## 账

`unknown` 441 → 430；`discarded` 2 → 11（残差总记录仍 441）。

## 判据

`pnpm test` 1024/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 338/338 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec report` · `docs/02-engine/spec-audit-2026-10.md` §42 · `pnpm tools spec show CORE-BOOT-LOOP-EXC`。
