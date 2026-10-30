# 四个提交回归修复：剩余 spec P1（0x90/0x97/0x91-92/0x72/0x1ce/0x32a/0x32/0x20a）

- id: REQ-01M66W5F242YMCSER2ETG7DYCQ
- type: req
- status: open
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- tags: [spec regression]

## 背景

四个提交 `323b5a5..6821e19` 的 spec 审计已完成 P0 与大部分 P1；本单只跟踪尚未收口的项。

## 已在本轮完成

- `0x90` / `0x97` 读序补齐两次重读；`0x90` 深度档改为**表级单格**（`0x403BB8`，读 `0x4206BD` / 深度校验 `0x4083BF`）。
- `0x91` / `0x92`：改为“只有 arm 那一次读平台光标（`0x404029/40/51`）”，并补“arm 闸已置时不得再取光标”的 falsifier。
- `0x72`：起播（跳过位未置）采一次输入并清输入字（`0x41F033/4C/55`）；补进 `CORE-INPUT-WORD` 写者枚举。

## 剩余项

1. `0x1ce` 的“乱数启用”身份存疑：非 0 支实际起 500ms 计时器（`0x4202ED→0x420301→0x420308 jmp sub_453A90`）；若确认，`CORE-OPS-RNG-ENABLE` 整体重写。需要一次身份裁决。
2. `0x32a` 槽空间：语料 `0x4AB229` 扫 1000 格，正文只写 `0..2`（需补“为什么是 3”或改 `0..999`）。
3. `0x32` 混合支实现仍按单趟放大（`vm_ops_texture.cpp`）；spec 已写“先放大后缩小”，实现面待对齐（`RES-TEX-STRETCH-MIXED` 只管等价公式）。
4. `0x20a` 新失败形态的 binding 站点未补（`0x423640/42367C` 与抛点 `0x41C218/23C`）。

## 判据

每项改完后：`pnpm tools spec validate` 13/13、`pnpm test` 全绿；正文 falsifier 能推翻该条；`audit` / `bindings` 同步。
