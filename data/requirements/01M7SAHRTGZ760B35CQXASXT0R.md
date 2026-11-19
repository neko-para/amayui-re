# AOT 臂的块循环不持令牌：guest 回调续延在令牌外碰 guest 堆（--profile 实测 violations=2 first=GuestHeap::free）

- id: REQ-01M7SAHRTGZ760B35CQXASXT0R
- type: bug
- status: open
- parent: REQ-01M7JHSE82SRRH0QJ7G6GXRD0Y
- repro: apps/emulator-lift/src/aot/aot_run.cpp#pumpSyntheticEas
- severity: S2
- tags: [emulator-lift, aot, token]

## 现象（2026-11-19 实测，mac arm64 / RelWithDebInfo）

`--profile` 的 AOT 短跑已经是**可观测**的（观测面由 `REQ-01M7JFEA7J8AWQHPXTW6GB41H0` 补上），读数：

```bash
./out/emulator-lift-aot --present headless --audio off --max-frames 1 --profile \
    --game-dir ../../links/install --appdata <空目录>
# token     : violations=2 max-concurrent=1 first=GuestHeap::free
```

缺省档（不给 `--profile`）**没有**这一行 —— 说明这不是"打印口径"，是运行期真的记了 2 次越界。
同一命令连跑 3 次（`--max-frames 1/1/3`）都是 `violations=2 first=GuestHeap::free`（确定性）。

## 归因（临时在 `GuestHeap::free` 里打 backtrace，已还原）

```
GuestHeap::free
shimEnumFontFamiliesExA(...)::{lambda}   ← 续延：收尾时 free 共享的 LOGFONTA / TEXTMETRICA
GuestCallStack::resume
AotHost::pumpSyntheticEas
AotHost::run
```

`src/shims/gdi32_font.cpp` 的 `EnumFontFamiliesExA` 在 **shim 返回之后**的续延里 free 两个 guest 堆块
（`lf_ea` / `tm_ea`）。续延由 `AotHost::pumpSyntheticEas` → `GuestCallStack::resume` 在**块边界**调，
而 AOT 的块循环**不持令牌**（`guest_token.h`：绕过 shim 的容器入口必须显式 acquire）⇒ 这两次 free 计数。
解释器（从代码看）不出现：`Machine::run` 每块 `GuestToken::Guard token_guard`（`machine.cpp:1158`），
续延也在里面。

## 修法候选（未裁决；⛔ 本单不改代码）

① **窄**：`GuestCallStack::resume` 调 `f.cont(...)` 时包一层 `GuestToken::Guard`（续延是宿主代码，
   会碰 guest 堆、也会启动下一个 guest 回调）。代价 = 每块边界一次可能竞争的取令牌（有音频 feeder 时）。
② **宽**：学解释器，AOT 块循环每块持令牌。会改变 Phase-0 的 `tokenhold` 读数与 feeder 竞争面 ⇒ 先裁决。

★ 判据（分歧消失）：`--profile` 短跑 `token     : violations=0 max-concurrent=1`，且 `tokenhold` 不出现
"主线程几乎全程持有"（口径见 `REQ-01M75WWEDC20GEH7QVSHPHH4GP`）。
⚠ `apps/emulator-lift/tests/aot_token_summary.cpp` **故意不断言 0**（只钉"行在、形状对、缺省档没有"）
—— 本单修好后才加 `violations=0` 那条断言（⛔ 不许反过来先改弱 / 归零）。

## 边界

- ⛔ 不许把 `violations` 调成 0（改计数器 / 改打印）—— 那是掩盖，不是修。
- 与 `REQ-01M7JFEA7J8AWQHPXTW6GB41H0` 的关系：41H0 已交付 arm / disarm / 摘要 / 守卫；本单是它判据 2
  （"真跑出非 0 ⇒ 开新单修那个越界点"）要求的那张单。
