# M4-1b-1 · 推过 SYSTEM4 启动前段（16 个 opcode，handler 已机械点名）+ 前沿棘轮

- id: REQ-01M4B248NB8FWDN61E2NMK8N6V
- type: req
- status: open
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592

## 当前实测（可复算）

| 入口 | 结果 |
|---|---|
| 启动链根脚本 | 140 步 ⇒ `INITCONFIG4.BIN#7 = 0x61 lookup-array` |
| `LOADCONFIG.BIN`（直跑） | 32 步 ⇒ `#33 = 0x61 lookup-array` |
| `CHECKCONFIG.BIN`（直跑） | **37 步**（本轮 34 → 37）⇒ `#55 = 0x1a4` |
| `INITCONFIG.BIN` | 跑通（含 `INITCONFIG0..4`）⇒ `INITCONFIG4#7` |

## 本轮做完

* **`0x76`/`0x77`**：写 `Engine+0x15280` / `+0x15284` 两个槽，值是 **`bswap24(op1)`**（低 24 位字节序倒过来，
  `b0<<16|b1<<8|b2` —— 像 BGR↔RGB），且**都**在写完后调 `sub_459F40`。
* ★★ **修掉我自己的一处静默遗漏**：`0x78`/`0x2db` 的体里**也有** `sub_459F40(...)`，而我的标量 handler
  **没记录那次调用** ⇒ 保真欠账被**少算**（日志显得比实际干净）。
  做法：知识层的表加一列 **`callsAfter`**（写完标量之后还调了谁），handler 按它逐次发 `logged-only`。
  判据：`CHECKCONFIG` 直跑的**保真欠账从 0 变成 3**（`0x76`/`0x77`/`0x78` 各一次）—— 这就是修好的证据。
* 棘轮更新（CHECKCONFIG 34 → 37 步 / `0x1a4` / `#55`）、守卫 +1 例（bswap24 的值 + callsAfter 必须留痕）、
  变异条目 +1 条（`bswap24` 改成恒等 ⇒ 当场红）。

## 下一块

1. `0x1a4`（`CHECKCONFIG#55` 的新墙）。
2. **`0x61 lookup-array` + 指针/地址空间**（三条子脚本的共同墙）—— 前置是池基址冲突 `REQ-01M4B49MK0HMJ7NK00PN4SMDHB` 与容量来源。
3. 引擎侧配置门 `REQ-01M4B416EKPVZWHFWT283C1T3Q`。
