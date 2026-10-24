# 残差第十三批：存档负载的块身份 · 场景表 · 帧记录

- id: REQ-01M5N8ZKZ69TKTH8QXDH9SWG9G
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-codec.test.mjs#★ 未压缩信封：前缀三字 = body 长度、body 原样、参数块两个 CRC 覆盖密文
- tags: [spec residual]

# 残差第十三批：存档负载的块身份 · 场景表 · 帧记录

## 一句话

把存档负载的六块身份、局部池持久化、帧记录数与 `rec[259]/rec[260]`、负载内 740 表一次读清；纠正上一批"740 表是只写表、无读者"的误判。实现本轮不改。

## 纠错 / 合同

- 六个块 = 六个全局池：c0 整型值池 / **c1 浮点值池** / c2 字符串 / **c3 整型引用池** / **c4 浮点引用池** / **c5 字符串引用表**（只写计数，样本 0 是残留）。原文"块 3 = 浮点池""块 5 写 0"都错；v2 ≥ 20 只门控**浮点族两块内容**。
- 局部池不持久化（帧记录只覆盖状态 / 脚本索引 / 位置表两格 / 位置 1 / 位置 2）；恢复后是 0 还是残留不规定。
- 帧记录数 = 当前帧下标 + 1。
- `rec[259]` / `rec[260]` = 帧自带位置表下标；返回点 = 表 3 表项 + 3 字；`rec[260]` 优先（推进 3 字）、否则 `rec[259]`（推进 0）；当前帧 `rec[260]` 恒 −1。
- 负载内 740/2964 表 = **场景记录表**（键 + 740B 记录；尾块 = 键窗 + 场景自身记录），有读取路径、参与恢复；与文本缓冲尾段是两张表。

## 残差

`RES-SAVE-POOLFAMILIES` / `-FRAME-COUNT` / `-TRAILING-TREE` 关闭；`LOCAL-POOLS` 窄化；`FRAMEREC` 窄化。

## 实现缺口（登记，未修）

浮点应写块 1 而非块 3；`tailBase` 漏 `4*(c1+c4+c5)` 保留洞；`rec[259]` 现写位置本身而非表下标。

## 判据

`pnpm test` 994/0 · `pnpm test:assets` 201/0（1 skip）· `pnpm test:mutation` 328/328 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show FMT-SAVE-CODEC` · `docs/02-engine/spec-audit-2026-10.md` §30。
