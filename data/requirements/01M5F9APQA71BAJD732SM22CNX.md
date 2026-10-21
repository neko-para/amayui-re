# emulator-new：`0x2de` 字体名查表（宿主字体名表 + 命中/未命中语义）

- id: REQ-01M5F9APQA71BAJD732SM22CNX
- type: req
- status: done
- parent: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- verify: tools/test/emulator-new-prologue.test.mjs#★★ `0x2de` 字体名查表
- tags: [emulator, host, font]

## 一句话

`0x2de`（`CHECKCONFIG.BIN` 的字体名查表）已落地：宿主字体名表是**外部输入**（`Machine.fontNames`），本仓不伪造系统字体枚举；缺省空表 ⇒ 查表一律未命中 ⇒ 走引擎自己的**回退分支**（把找不到的字体名换成回退名并 `save-string`）。

## 已做

- `0x2de` handler：读 op2 文本（`readText`，支持 type 5 / 内联 type 2）→ 宿主表线性查（**长度相等 + 逐字节相等**；键首 `@` 跳过）→ 0 基下标 / 未命中 **−1**，写回 op1。
- `Machine.fontNames`（宿主输入，缺省空表）+ `analyzeInstall` 透传 `fontNames`。
- 守卫：`tools/test/emulator-new-prologue.test.mjs` 的两条 `0x2de` 用例（命中 / `@` / 长度不等不命中 / 空表）。
- 变异：`mutate-check` 两条（长度相等改成前缀匹配 / 不跳过 `@`），各 1/1 红。

## 与"宿主边界"的裁决

不引入"伪造系统字体表"：空表 ⇒ 未命中 ⇒ 回退分支（引擎真实会走的一条路）。这条路径已由启动链棘轮覆盖 —— CHECKCONFIG 之后链继续走到 LOGO.BIN 入口。

## 遗留（开在看板上的问题，不在本单）

- 引擎里那张字体名表**是否会被 `0x1a5 set-font`（`sub_4328F0`）填充**没有取证；若是，则"宿主表"其实应是"引擎侧表 + set-font 写入"。本层把 `0x1a5` 记为转发 + 诊断（⛔ 不猜）。
- 真实系统字体枚举（跨平台不可复现）只应作 assets / external 档的夹具，不进 pure 判据。
