# 帧 614 的 shim 耗时归属：具体逻辑 80%，交接边界 10%

> 读数日期 2026-11-20 / win32 / main `f4305d4`（note OFF；alloca 已回退）。
> 证据与原始日志见 `.tmp/shim-attrib/report.md` / `plan.md` / `logs/`（临时 instrumentation，已还原）。
> 复现：`.tmp/note-bench/bench.mjs` 的 `save05-map-reveal`；slow-frame `insns=226,789,207` /
> `shim_calls=66,943`（与任务书帧 614 的 226,792,527 / 66,940 同一逻辑帧）。

## 0. 一句话

**276.5 ms 集中在具体逻辑，不在 shim 交接边界。** 同一逻辑帧的 instrumented 分解逐位闭合到
`shim_ms=348.442`：逻辑 **280.037 ms（80.4%）**、边界 **34.681 ms（10.0%，0.518 µs/调用）**、
仪器税 29.238 ms（8.4%）、`lift_shim_id` 圈外 4.486 ms（1.3%）。

三个 top API 只占 **0.56% 的调用**（376 / 66,943），却吃 **209.849 ms（60.2%）**：

| API | calls | per-call | 主要子步 |
|---|---:|---:|---|
| `D3DXCreateTexture` | 94 | **739 µs** | `ComRegistry::createObject` **98.8%** |
| `IDirect3DTexture9::GetSurfaceLevel` | 144 | **610 µs** | `createD3dxSurfaceObject` **99.9%**（同一笔 `createObject`） |
| `CreateFileA` | 138 | **381 µs** | `Vfs` 查找 **77%** |

`ct_obj` + `gs_surface` + `cf_resolve` 三点 = **196.8 ms = 帧的 56.5%**。

## 1. 反证算术

- 边界单价 **0.518 µs/调用** × 66,943 = **34.7 ms**；
- 要用边界解释 276.5 ms 需要 **4.13 µs/调用（实测的 8.0×）**；解释 348.4 ms 需要 5.21 µs（10.1×）。
- 第二组独立读数（window 轮，帧 329，calls 66,944）：边界 10.3% / 逻辑 80.8% / 税 8.9% —— 同向稳定。

## 2. 边界内部

70% 是「两把 `shims` 锁 + 两次 `ShimDef` 拷贝 + 两次 `GuestToken`」：

| 子项 | µs/调用 |
|---|---:|
| `lookup` | 0.101 |
| `def_copy` | 0.094 |
| `token_acq` | 0.088 |
| `token_rel` | 0.088 |
| `id`（AOT 快路径） | 0.031 |

⇒ 边界不是“每调用几 µs 的固定税”；即使把边界全删，也只省 ~35 ms（instrumented 帧），
且还要付 regs 物化/回读。

## 3. 结论与下一步

- **三个 top API 的耗时是具体逻辑**，不是交接边界：`D3DXCreateTexture` / `GetSurfaceLevel`
  都落在 `ComRegistry::createObject`（guest COM 对象 + 每槽运行期 shim 注册），`CreateFileA`
  落在 `Vfs` 查找。
- 要优化 614 帧，应查 `ComRegistry::createObject` 的**每槽 `registerSlot→addRuntime`**
  与 `Vfs` 查找；⛔ 不是先改 `lift_shim` 的 regs/id/锁（那只有 ~10%）。
- 未拆：`createObject` 内部分三块（guest vtable 分配 / 每槽 `addRuntime` / 对象头+表插入）；
  `cf_resolve` 内部（大小写解析 vs copy-up）；feeder 线程（`--audio off`，不在本表）。
  这些是下一轮归因的入口。

## 4. 口径

- `shim_ms` 来自 `aot_run.cpp:892-901`：`lift_shim_id` 整段（含 regs 物化/回读、id、`invokeById`）；
- instrumentation 只在 `LIFT_SHIM_BREAKDOWN=1` 下累计，默认零行为；收尾后 `src/` 已还原、
  `out/` 已用还原源码重建（14,962,176 B，与原 OFF 同字节数；sha256 因链接期不可复现而不同）。
- instrumented run 的 `shim_ms=348.442` 比原 276.5 高 17.7%，其中 8.4% 是仪器税，其余是同帧
  run-to-run 方差；本报告用**同一帧内的比例**下结论，不拿 instrumented 绝对值当原帧读数。
