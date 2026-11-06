# emulator-lift · 真线程 / 完全 override 喂料线程可行性（含 arm64 内存序）

- id: REQ-01M6RWGHB9YXDS6K9QAZTATZC5
- type: spike
- status: done
- parent: REQ-01M6K84KHPA1XF3B1HSHXME6W0
- done_reason: 直接 std::thread 不可行：等待/临界区/Interlocked/线程句柄/裸指针并发均缺同步层。推荐 D（完全 native override 喂料线程，11–17 人日）：sub_4B5AA0 只有 0x4B5B03 一处 guest 调用可搬回主线程；D 的三条硬要求与 C/A/B 对比见报告 repo:apps/emulator-lift/docs/thread-model-feasibility.md。需裁决 V1 路线 D/C。
- tags: [emulator-lift thread audio arm64 spike]

## 结论

**“直接简单实现成真线程”不可行；“实现成真线程”可行，但必须先补同步对象层。** 当前
`WaitForSingleObject` / `WaitForMultipleObjects` 在未 signaled 且 timeout≠0 时直接
`shimAbort`（`kernel32_sync.cpp`）；临界区只改 guest 字段、跨线程拥有者 abort、
`TryEnterCriticalSection` 恒 TRUE；`Interlocked*` 与 `lock` 前缀是裸 `rd+wr`；线程句柄不是
可等待对象；`BlockCache` / `ShimRegistry` / `HandleTable` 返回的裸指针在并发下会悬垂。
⇒ 直接 `std::thread` 起步会在这些点同时错，且大多**静默**。

## 方案

| 方案 | 说明 | 工作量 |
|---|---|---|
| A 真并行 | 每线程 guest CPU + 全访存原子化 + x86 TSO store buffer；arm64 风险最高 | 30–55 人日 |
| A′ 真线程 + 单令牌 | 任意时刻只一个线程跑 guest，令牌交接是唯一同步点；无需内存序模拟 | 22–35 人日 |
| B 协作式 guest 线程 | 第二 guest 栈、单宿主线程；自己写上下文切换 | 17–28 人日 |
| C 混合 | 真宿主线程 + 单令牌 + 只为音频开一个线程 | 12–20 人日 |
| **D 完全 native override 喂料线程** | **推荐先做**：`sub_4B5AA0` 循环体只有 `0x4B5B03 call sub_4B6C30` 一处必须 guest；其余 12 段可 native；把唯一 guest 调用搬到主线程确定性边界，用现成 `startNestedGuestCall` | **11–17 人日** |

## D 的三条硬要求

1. 泵前后整份交换 guest 上下文（GPR / EFLAGS / x87 / XMM / EIP / ESP / FS / thread_id）；
   只存 GPR 会污染主线程的浮点上下文。
2. 主线程持槽临界区时泵必须 **defer**（真机 feeder 会阻塞；同线程可重入会插进半更新状态）。
3. `WaitForSingleObject(feeder 句柄)` 必须 **pump 到退出**，否则 `Release pDS` 后泵还在写 = UAF。

## 报告

repo:apps/emulator-lift/docs/thread-model-feasibility.md

报告含逐段 EA 表、x86/arm64 内存序、五方案总表、D 的 P0/P1/P2 里程碑与守卫、以及
“每 0.5 s 一次 `sub_4B6C30` 是否超一帧预算”的零代码先量方法。

## 需裁决

- **V1 路线**：D（完全 override，推荐）还是 C（真线程 + 令牌）。
- **V2 调度**（若走 C）：单令牌的换手点 / 量子口径。
- **V3 线程范围**：只音频，还是通用 `CreateThread`。
- **V4 复现边界**：`threads≠off` 时 boot-trace 守卫是红还是 skip。
