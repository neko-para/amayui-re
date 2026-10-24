# 残差第十五批：混合状态闭合 · 存档尾段字段

- id: REQ-01M5NAGW9JQ4T5K403VD706HGP
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-codec.test.mjs#★ CRC-32：反射 = 0xCBF43926（标准向量），非反射 = 0xFC891918
- tags: [spec residual]

# 残差第十五批：混合状态闭合 · 存档尾段字段

## 一句话

闭合命令档 / 技法进入时的混合状态，并把存档尾段从"文本库快照"改成"消息窗口显示 / 音声命令日志 + 检查点"，收窄余下字段语义。实现均不改。

## 混合状态

- 命令档入口前无混合写入；每条条目画完立刻复位成**中立值**（混合开 / 源因子 1 / 目标因子 0 / 运算加 = 覆盖式写入）。
- 种类 0 出口 A、种类 3 技法、种类 3 CPU 退路进入时都是中立值；**只有种类 0 默认退路不同**：不写因子、无条件关 alpha 测试、条件下写一次 (源=1,目标=0)。
- `BLEND-STATE` 关闭；新增 `BLEND-INTERFRAME`（渲染步之外未逐个读）；`KIND0-ALPHA` 收窄为"淡入来自哪里"未定。

## 尾段

- 不是文本库快照，而是**消息窗口显示 / 音声命令日志 + 检查点**。
- 8 字节对 = `(槽号, 记录下标)`（组首条 bit0=1；末对是导航哨兵）。
- 44 字节记录 = 槽号 / 矩形四边 / 族载荷 / 字距 / 标志；字符串区连续、整段 32 位取反、尾部 1..4 字节未定义。
- 写总长 `12+8P+44N+4W`；读时从尾部弹出到第一条 bit0（含）⇒ N = 末对下标、不恢复游标、不校验计数。
- `TRAILER` 关闭归属，收窄为 `TRAILER-FIELDS`。

## 实现

不改（无技法 pass / 无尾段实现；⛔ 不写"逐字节相同"守卫）。

## 判据

`pnpm test` 998/0 · `pnpm test:assets` 201/0（1 skip）· `pnpm test:mutation` 331/331 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show CORE-OPS-SCENE-COMMAND-RECORDS` / `FMT-SAVE-CODEC` · `docs/02-engine/spec-audit-2026-10.md` §32。
