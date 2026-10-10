# 三张表的存档写入侧与剩余语义（0x71 语义名 / sub_header≠28 分支 / 0x64 与 type-2 同形表 / sub_48F000）

- id: REQ-01M4GW5NTCWG9KQTTHHVYAT1JG
- type: req
- status: open
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 16
- tags: [engine, emulator]

## 范围（从 `REQ-01M48FRMKME8VERSH42M7MQMYP` 收口时移出的"仍未查明"）
1. **存档写入侧怎么产生 `table_2`/`table_3` 的下标** —— 全语料里记录 `+0x1C`/`+0x24`（旧基址记法 `帧+0x60`/`帧+0x68`）的读点只有 `sub_40F750`
   （恢复：表项 `+3` 后压进 `Engine+0x5EEA4`）与 `sub_4192F0`（恢复：`PC = 记录+0x00 + 4*表项`）两类，
   **没有**像 `table_1` 的 `sub_48E870` 那样的"位置 → 下标"查找例程 ⇒ 存档里那两个下标从哪来**未查明**。
2. **`sub_header_length ≠ 28` 的分支** —— 语料 491/491 都是 28（头 `+32`），无 >28 / <28 的样本
   ⇒ 无法区分"引擎的动态算式 `32 + u32@32`"与"本仓对 v4 硬编码的 `headerLen = 60`"。
3. **`0x71` 的正式语义名** —— `instruction-set.json` 里它的 `name` 是空串；"消息表"是旧仓口径
   （它确实调 `message:ReadTextSkip`），本轮**没有**定名。
4. **同形表有没有第二族** —— `0x64` 的数组块 / type-2 字符串块是否也有"位置表"；
   以及 `sub_48F000(消息引擎, 记录+0x3C（旧记法 帧+0x50）, 下标)` 拿到查找下标之后的语义。
5. **恢复路径的调用者与存档字段** —— `sub_40F750` 的 4 个 `0x410xxx` 调用点、`exit`（`0x2`）的
   记录 `+0x38`（旧记法 `帧+0x4C`）`== −11 / −10` 两个分支、`[Engine+0x7F25C / 0x89B98h / 0x991BCh]` 那族 per-cur 存档数组
   ⇒ 要往 `sub_4559C0` / `sub_455C60` 与 `SAVE*.DAT` 侧取证。
6. **命名欠账（★ 2026-10 三次订正：一半已解）** —— `FRAME_LAYOUT.off.state6C` / `state70` 这两个名字里的
   `6C`/`70` 是**旧基址（`0x5D880`）记法**；`REQ-01M4H0Q2TQMKHQ35X69DG0FPGP` 把记录基址改成 `0x5D894` 时
   **必须**跟着改（否则"名字说 6C、值却是 58" = 新的自相矛盾），已改名 `position58` / `position5c`
   （注释里写明"**位置**，不是下标"）。★ **仍有欠账**：`tools/test/emulator-model.test.mjs` 里那条用例名
   曾写「`+0x6C` 索引」（实测是**位置**）；本轮该用例已随基址订正改名，但"位置 vs 下标"这条**语义命名**的
   独立裁决仍归本单。
7. **记录 `+0x04`（旧记法 `帧+0x18`）的叫法（本轮**没有**动它）** —— 已登记的两条把它叫「操作数基址」
   （`Engine+0x5D880/frame-fields` 与 `Engine+0x5D880/local-pool-bases` 一族），而**同一批记录里的算式**
   只在「记录 `+0x04` = 本条指令的 **opcode 地址**」下自洽：`sub_41BF50` 的 type 落点 `[PC + 8·i − 4]`
   （i=1 ⇒ PC+4，正是紧跟 opcode 之后的 type 字）、
   以及 `0x8f` 压返回点的 `(PC − 记录+0x00) >> 2 + 3`（**`+3` 正好等于 argc=1 指令的 dword 长度** ⇒ PC 只能是 opcode 地址，
   否则每次 `ret` 都会落到返回点的下一格）。⇒ "操作数基址"这个叫法与算式**不能同时成立**，留待复核（不要在本单之外顺手改）。

## 已知（本轮已落台账，这里不复述结论）
见 `data/ledger/`：`Engine+0x5D880/frame-tables/entry-structure`（accepted）、
`Engine+0x5D880/header-subheader-length-and-table-fields`（accepted）与
`Engine+0x5D880/frame-0x54-group1-0x71-table` / `frame-0x5C-group2-0x03-table` /
`frame-0x64-group3-0x8f-table` / `script-base-and-position-numbering` /
`frame-0x6C-0x70-position-not-index`（proposed）。
★ 上面这些 subject 与正文里的 `帧+0xNN` 是**旧基址（`0x5D880`）记法**：换成现在的记录内偏移要 **−0x14**
（`帧+0x54` → 记录 `+0x40`、`帧+0x6C` → 记录 `+0x58` …）；台账是 append-only，旧记法不就地改。
守卫 = `tools/test/emulator-frame-tables.assets.test.mjs`（`@env assets`）。

## 判据
每条结论按准入门登记进 `data/ledger/`（锚到二进制 EA 或守卫用例）；本单不做收口，做到哪一步就登记哪一步。
