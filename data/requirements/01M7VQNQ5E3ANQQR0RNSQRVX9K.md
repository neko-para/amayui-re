# shim 效率：ComRegistry 槽计划缓存 + Vfs 目录索引/负缓存（SAVE05 尖峰帧判据）

- id: REQ-01M7VQNQ5E3ANQQR0RNSQRVX9K
- type: req
- status: doing
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- tags: [emulator-lift, shim, perf]

## 一句话

把 **SAVE05 移动**尖峰帧里宿主 shim 的"具体逻辑"成本压下去：`ComRegistry::createObject` 的**每槽登记**
（C1，已落地：一次 `D3DXCreateTexture` 22 次 `addRuntime` + 每项 `normalizeModule`）与
`Vfs` 的**逐段目录枚举 / 重复规范化**（C5/C6，未做）。判据 = **8 个尖峰帧 Σwall** 与单帧 862/886，
⛔ 不是窗口 wall（两族合计只占窗口 2.8%）。

## 已落地（C1 · `b48fdad`）

- `ComRegistry` 每 (iface, methods) 一份槽计划缓存（`SlotPlan{ iface, methods, stubs, def_ids }`，
  key = 表地址 + size + 内容逐字段比对）；命中只 alloc vtable + 逐槽写缓存 stub + `installShim`。
- 命中率 **100%**（SAVE05 窗口 712/0）；guest 字节 / refcount / 分配顺序不变。
- A/B（3 对交错，visible 档）：8 尖峰帧 Σwall **959.1 → 804.9 ms（−16.08%）**、862 位 **−17.54%**、
  886 位 **−11.80%**；`D3DXCreateTexture` 213.8 → **<10.5 µs/次**、`GetSurfaceLevel` 196.7 → **<6.9 µs/次**；
  窗口 wall 只报不判（7745 → 7595 ms，−1.93%）。
- 指纹 12 次 run 全同（exit 0 / `stop @ 00412383` / arena `89668632,119683880,182` / `table misses=0`）。
- 守卫 `mechanism` 的 `com-slot-plan-hit-and-conflict`（顶层 pure）+ 变异 `--match '命中路径'` 1/1 红。

## 剩余候选（预期按 8 尖峰帧口径；C1 后基线已降到 ~805 ms）

| # | 候选 | 预期 | 风险 |
|---|---|---|---|
| C5 | `Vfs` 每目录一次 `lower(name)→真实名` 索引（保留多候选 `[ambiguous]`） | −30…−45 ms | 中 |
| C6 | `Vfs` 路径缓存 + 负缓存（影子写入 +1 代失效） | −40…−55 ms | 中 |
| C7 | 日志侧先判可见性再 `snprintf`/转码（`comLog`/`d3dxLog`/`pathFailure`/`shimVPrintAt`） | −2…−4 ms | 无 |
| C2 | `ShimRegistry` (module,name)→id 哈希 | C1 后 SAVE05 上≈0（只利首次登记 / 装弹帧） | 低 |
| C4 | `GetSurfaceLevel` 同 (texture,0) 去重 | −35…−50 ms | **高 ⇒ 另开语义单** |
| C8 | 对象头 / vtable 池化 | ≈0 | 高（破 arena 指纹）⇒ 不做 |

## 判据（能红）

1. 指纹（`exit` / `stop @` / arena 三元组 / `table misses`）逐位同。
2. A/B：同机、RelWithDebInfo、**≥3 对交错、hidden 档**（`--present window --present-hidden`；
   `.tmp/save05-bench/protocol.md`）⇒ 同事件顺序尖峰集 Σwall ≥5%；862/886 各 ≥5%；窗口 wall **只报不判**。
3. Vfs：`L1-FILE-01`（逐段大小写不敏感且唯一 / 多候选 `[ambiguous]` 失败 / `..` 越根 / 排序确定）、
   影子写入后负缓存必须失效；守卫 `file_io/case-insensitive-read` + 新 `vfs/dir-index-ambiguity`。
4. ⛔ 不改 guest 可见语义：C4 另开语义单；⛔ 不做 C8。

## 落点

`src/runtime/vfs.cpp` · `src/shims/kernel32_file.cpp` · `src/runtime/com.{h,cpp}` · `src/runtime/shims.{h,cpp}`；
读数脚本 `.tmp/save05-bench/`（临时区，⛔ 不入库）。

## 未做

C5/C6/C7 均未做；C2 在 C1 后优先级下调（SAVE05 命中 100%）· 不把"窗口 wall +5%"当判据（上限 2.8%）。
