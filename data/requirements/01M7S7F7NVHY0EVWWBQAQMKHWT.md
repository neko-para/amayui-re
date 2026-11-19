# 🐞S2 句柄表全表扫描：OPTION 文本重绘时 DeleteObject 216 µs/次（随会话线性涨、占墙钟 32%）

- id: REQ-01M7S7F7NVHY0EVWWBQAQMKHWT
- type: bug
- status: done
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- verify: apps/emulator-lift/tests/mechanism.cpp#handle-table-payload-refcount
- repro: docs/03-emulator/defects.md#BUG-36
- severity: S2
- tags: [emulator-lift, perf, gdi, handle-table]

## 现象

文本密集的 UI（实测：`TITLE → OPTION` 后拖动/点击滚动条重绘列表）出现可见卡顿；overlay PROFILE 读数里
`shim:gdi32!DeleteObject` 单次成本随**会话时长**线性上涨：

- 用户读数（长会话）：`DeleteObject 880.697 ms / N=4066`（**216 µs/次**，占墙钟 31.9%）；
- 本机复现（60 s 预热，句柄表约 9k 槽）：`99.229 ms / N=6142`（**16.2 µs/次**）；
- 同一场景**不预热**（表更小）：`DeleteObject` 根本不进 top-15。

⇒ 同一份代码、同一条路径，成本只随"表有多长"变。

## 根因（读代码 + 实测）

`HandleTable` 的槽位**只增不减**（`close()` 只置 `used=false`，`entries_` 从不回收；`clear()` 没有调用者），
而一次**成功**的 `DeleteObject` 要在这张表上跑三条 O(N) 扫描：

1. `selectedAnywhere()`（`src/shims/gdi32_object.cpp`）—— 遍历**全部句柄**找"有没有任何 DC 选中它"，
   每次迭代还付一次跨 TU 的 `HandleTable::get()`（`LIFT_AOT_LTO_RUNTIME` 缺省 OFF）；
2. `releasePayload()` 的"还有没有别的活句柄指着同一个 payload"扫描（`src/runtime/handles.cpp`）；
3. `close()` 的线性扫描（`handles.cpp`；没有用 `get()` 的下标反算）。

实测增长：OPTION 列表每重绘一次约 **+43 槽/帧**，每次滚条点击批量 **~2047 次** `DeleteObject`
（`TITLE→OPTION` 那一帧 2766 次）。⇒ 会话越长，N 越大，单次越贵（用户那台 ≈10 万槽对得上 216 µs/次）。
`CloseHandle`（`kernel32_sync.cpp`）与 `DeleteObject` **完全同构**，同一批修法一起受益。

## 修法

- **A**：`HandleTable::close()` 改下标反算（逐位等价，零风险）。
- **B**：`selectedAnywhere()` 只扫**活 DC**（`HandleTable` 维护一份 `live_dcs_` 派生索引；`add`/`close`/`clear` 三处维护）。
- **C**：`releasePayload()` 用 payload 引用计数（`unordered_map<void*,uint32_t>`；`add` ++ / `release` --，
  减一**只能**在 `releasePayload`，`DuplicateHandle` 的共享语义由 `sync-ext#duplicate-handle` 钉住）。
- ⛔ 不采用"GDI payload 独一无二 ⇒ 跳过扫描"：`DuplicateHandle` 不查种类（`kernel32_sync.cpp` 无条件复制 payload），
  复制一个 font 句柄就会 use-after-free。
- ⛔ 不做槽位复用（会破 `mechanism#handle-table-index-lookup` 的"只增不减"断言；属另一张语义单）。

## 读数（复现）

命令骨架见 `.tmp/deleteobject-profile/report.md` §1；关键点：

```bash
./out/emulator-lift-aot --present window --overlay \
  --game-dir ../../links/install --appdata ../../.tmp/deleteobject-profile/appdata \
  --skip-logo --audio off --time-limit 400 \
  --input-plan ../../.tmp/deleteobject-profile/scenario2.plan
# plan = TITLE → click 812 622（OPTION）→ await script CONFIG1 → wait 60000（预热）→
#        profile_begin scroll8 → 滚条轨道点击 3 次 → profile_end scroll8 → quit
```

⚠ `--present-hidden` 在这台机器上是病理档（实测 1.66 s/帧），⛔ 不要用它做性能读数。

## 判据

- 语义：`sync-ext#duplicate-handle` / `gdi#objects` / `gdi#delete-object-dc` / `mechanism#handle-table-index-lookup`
  必须全绿；新增 pure 用例 `handle-table/live-dcs` / `handle-table/payload-refcount`（⛔ 不断言墙钟）。
- 性能（observation，不进 CTest）：同一 `scenario2.plan` 修后 `DeleteObject` 单次应从 16.2 µs 降到 ~1 µs 量级
  （掉出 top-15 或 <2 ms / 6142 次）。
- 变异：`releasePayload` 改成无条件 destroy ⇒ `sync-ext` 当场红（点名共享 payload 机制）。

## 未做 / 边界

- 槽位仍**只增不减**（长跑内存与 `findNamed` 的 O(used) 是下一个候选；另开单）。
- `shim` 桶含**等 guest 令牌**的墙钟（`aot_run.cpp` 在 `lift_shim_id` 外包计时）；本单的判据用
  **同场景 A/B（预热 vs 不预热 / 修前 vs 修后）**，不单看绝对值。⛔ 不因此改计时口径。
- `Present` 的 readback（同一次 profile 里 821 ms/115 帧）是**另一条线**，不在本单。
