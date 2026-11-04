# BlockCache 的 SMC 失效会连带丢掉 shim 块（写已解码代码页后调 shim ⇒ decode-error）

- id: REQ-01M6KJWSAVXZ2MHS8E4JA5YBAN
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/dispatch.cpp#smc-preserves-shim-blocks
- repro: apps/emulator-lift/tests/dispatch.cpp#smc-preserves-shim-blocks
- severity: S2
- tags: [emulator-lift, m2]

## 一句话
`BlockCache` 的 SMC（自修改代码）失效走 `dropBlocks()`，而它是 `blocks_.clear()` —— shim 块与 guest 指令块同住这张表 ⇒ guest 每写一次**已解码的代码页**，`installShim()` 装进去的 shim 块就一起没了；随后 `call 桩地址` 在缓存里找不到那条记录，解码器转去**解码桩页字节**：桩页未提交 ⇒ `decode-error`（看着像"解码器不认识这条指令"，归因误导），桩页有字节 ⇒ 静默把数据当代码跑。

## 根因（哪一行）
`repo:apps/emulator-lift/src/runtime/x86.cpp` 里的 `BlockCache::dropBlocks()`：

```cpp
blocks_.clear();   // ← shim 块也在这张表里，一起丢
```

`installShim()` 只往 `blocks_` 塞一条 `is_shim = true` 的记录，`dropBlocks()` 从不区分两类块。

## 修法（为什么最省且不掩盖问题）
shim 块**单独一张表** `shim_blocks_`（`repo:apps/emulator-lift/src/runtime/x86.h`）：
- `get()` 先查 `shim_blocks_`、再查 `blocks_`；`peek()` 两张都查；`blockCount()` 仍是两类之和。
- `dropBlocks()` 只清 `blocks_`（guest 指令块 + 代码页位图）⇒ 失效对 shim 是**无操作**，没有"失效后把整张 shim 表重装一遍"的 O(n) 兜底。
- `clear()` 是**显式全清**（夹具换机器码用，`file_io.cpp` 原有语义），两张表都清 —— 语义不变。
- 顺带修同根因的另一面：`installShim()` 以前见 `blocks_` 里有同 EA 的块就原样返回（可能返回一条 `is_shim == false` 的 guest 块，把"桩当代码跑"永久化）；现在同 EA 上已有的 guest 块会被换掉，"这个 EA 是 shim"以 `installShim` 为准。

## 复现（修前红 / 修后绿）
- `apps/emulator-lift/tests/dispatch.cpp` 的 `smc-preserves-shim-blocks`：桩页**不提交**（与真实夹具 / DirectDraw 夹具同形），guest 写自己的代码页后调 shim ⇒ 修前 `StopReason::DecodeError`（停在桩 EA `7FFE0000`）、shim 调用数 0；修后 `until-ea`、shim 返回值 `0xBEEF0001`。
- 同文件的 `smc-preserves-shim-blocks-same-page`：guest 指令与 shim 桩**同处一页**（4 KiB 失效粒度下最容易被"整页一刀切"掉的形态），桩槽 4 字节是 `90 90 90 C3` ⇒ 修前解成 `nop;nop;nop;ret`，**静默跳过 shim**（返回 42 而不是 `0xBEEF0001`，`until-ea` 照常到达，只有断言抓得住）。
- 两条用例同时断言 **guest 块仍然失效**（写进 helper 立即数 `0x2A→0x63`，第二次调用得到 99）⇒ 绿不是"把失效关掉"换来的。

## 变异验证
把 `dropBlocks()` 改回"整表清空"（加一行 `shim_blocks_.clear()`）⇒ 重编 `lift-dispatch` 实测 **8 条断言全红**（两条新用例的 `UntilEa` / `g_bump_calls` / 返回值 / `is_shim` 全失败）；改回复修后 57 checks / 0 failures。

## 同根因的其它表现
- 桩页已提交时**不再响亮失败**，而是把桩槽字节当指令跑（静默走错路）—— 比 `decode-error` 更危险，由 `smc-preserves-shim-blocks-same-page` 覆盖。
- `installShim()` 被同一 EA 上已解出的 guest 块挡掉（见"修法"最后一条），由 `install-shim-replaces-guest-block` 覆盖（同时钉住 `installShim` 的幂等：同 EA 再装返回同一块、不改 id）。
- `--trace-blocks` 的顺序**不是**这个根因：shim 调用走 `ShimRegistry::invoke` 的 `--trace-shims`，块级 trace 只打 guest 指令，两条路径互不影响（未改）。

## 影响面
DirectDraw 线为绕开它把 guest 回调代码挪进了独立代码页；修好后**消息泵回调 / WndProc / EnumDisplayModes 回调**不必再为这个缺陷让路。
