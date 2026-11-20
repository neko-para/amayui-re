# shim 效率：ComRegistry 槽计划缓存 + Vfs 目录索引/负缓存（SAVE05 尖峰帧判据）

- id: REQ-01M7VQNQ5E3ANQQR0RNSQRVX9K
- type: req
- status: doing
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- tags: [emulator-lift, shim, perf]

## 一句话

把 **SAVE05 移动**尖峰帧里宿主 shim 的"具体逻辑"成本压下去：`ComRegistry::createObject` 的**每槽登记**
（C1，已落地）与 `Vfs` 的**重复解析**（C6 负缓存，已落地）。判据 = **8 个尖峰帧 Σwall** 与单帧 862/886，
⛔ 不是窗口 wall（两族合计只占窗口 2.8%）。

## 已落地

### C1 · `ComRegistry` 槽计划缓存（`b48fdad`）

- 每 (iface, methods) 一份 `SlotPlan{ iface, methods, stubs, def_ids }`（key = 表地址 + size + 内容逐字段）；
  命中只 alloc vtable + 逐槽写缓存 stub + `installShim`；命中率 **100%**（SAVE05 窗口 712/0）。
- A/B（3 对交错，**visible** 档）：8 尖峰帧 Σwall **959.1 → 804.9 ms（−16.08%）**、862 位 **−17.54%**、
  886 位 **−11.80%**；`D3DXCreateTexture` 213.8 → **<10.5 µs/次**、`GetSurfaceLevel` 196.7 → **<6.9 µs/次**；
  窗口 wall 只报不判（7745 → 7595 ms，−1.93%）。
- 指纹 12 次 run 全同；守卫 `mechanism` 的 `com-slot-plan-hit-and-conflict`（**`8b015f6` 起逐槽对已知 stub 比，
  变异在本 case 就红**）+ 变异 `--match '命中路径'` 1/1 红。

### C6 · `Vfs` 读路径负缓存（`7f47235`）

- 键 = 已折叠段序列 + `must_exist` + `resolve_case`；值 = 完整错误串；**影子代**失效（写/删/mkdir/copy-up
  **成功后** +1 代；无条件 +1 会让窗口内代 143/命中 197，只在真创建时 +1 ⇒ 代 7/命中 363）；上限 8192；
  只有在 `--appdata`（写覆盖层）启用时生效。**显式假设**已写进 `vfs.h`：写覆盖层启用时安装树只读。
- 命中率（临时计数器，已还原）：可缓存读解析 1511、重复 **1119（74.1%）**；负缓存命中 **363（24.0%）**。
- hidden A/B（**全 hidden**，干净 3 对 + prof60 2 对；before `c1857693…` → after `fe8d744e…`）：
  8 事件 Σwall **820.2 → 756.5 ms（−7.8%）**；886 位 `CreateFileA` **151.1 → 38.3 µs/次（−74.7%）**；
  862 位 **−61.6%**；窗口 wall 只报不判（7598 → 7556 ms，−0.55%）。
- 指纹 10 次 run 全同（exit 0 / `stop @ 00412383` / arena `89668632,119683880,182` / `table misses=0`）。
- 守卫 `vfs-negative-cache-invalidated-by-create`（负 → 影子建 → 找到 → 删 → 失败；ambiguous 子判据在 macOS
  大小写不敏感 FS 上如实跳过，Linux 真跑）+ 变异 `--match '代号检查反了'` 1/1 红。

### 正缓存 / 内容缓存：显式 no-go（已实测）

- **正缓存 = no-go**：正结果重复 756/1511、全缓存命中 1119/1511，但 `CreateFileA` 37.0 vs 38.4 µs/次、
  8 尖峰 Σ 在噪声内。原因：安装树命中段解析只是一次精确 stat（~2 µs），贵的是失败探测的整目录枚举
  （根 542 项）——负缓存已吃掉。不值得引入"安装树运行期不变"的额外承诺与内存。
- **内容缓存 = 本轮不需要**：同一 8 事件 `ReadFile` 仅 0.5–1.1 ms、内容类 shim 不在 top-8；瓶颈是解析不是读/解码。
  若将来读数翻转，另开提案（先测窗口读入字节数，⛔ 不静默改热更新语义）。

## 剩余候选（预期按 8 尖峰帧口径）

| # | 候选 | 预期 | 风险 |
|---|---|---|---|
| C5 | `Vfs` 每目录一次 `lower(name)→真实名` 索引（保留 `[ambiguous]`） | 待量（正命中已便宜，可能 <5%） | 中 |
| C7 | 日志侧先判可见性再 `snprintf`/转码 | −2…−4 ms | 无 |
| C2 | `ShimRegistry` (module,name)→id 哈希 | SAVE05 上≈0（只利首次登记 / 装弹帧） | 低 |
| C4 | `GetSurfaceLevel` 同 (texture,0) 去重 | −35…−50 ms | **高 ⇒ 另开语义单** |
| C8 | 对象头 / vtable 池化 | ≈0 | 高（破 arena 指纹）⇒ 不做 |

## 判据（能红）

1. 指纹（`exit` / `stop @` / arena 三元组 / `table misses`）逐位同。
2. A/B：同机、RelWithDebInfo、**≥3 对交错、hidden 档**（`.tmp/save05-bench/protocol.md`）⇒ 同事件顺序
   尖峰集 Σwall ≥5%；862/886 各 ≥5%；窗口 wall **只报不判**；⛔ 不混臂。
3. Vfs：`L1-FILE-01` 全条 + 影子写入后负缓存必须失效；守卫 `file_io/case-insensitive-read` /
   `vfs-negative-cache-invalidated-by-create`。
4. ⛔ 不改 guest 可见语义：C4 另开语义单；⛔ 不做 C8。

## 落点

`src/runtime/vfs.{h,cpp}` · `src/shims/kernel32_file.cpp` · `src/runtime/com.{h,cpp}` · `src/runtime/shims.{h,cpp}`；
读数脚本 `.tmp/save05-bench/` / `.tmp/perf-reassess/`（临时区，⛔ 不入库）。

## 未做

C5/C7 未做（C5 待先量）· C2 在 C1 后优先级下调 · C4 语义单未开 · 不把"窗口 wall +5%"当判据（上限 2.8%）。

## 2026-11-20 win32 复核（合臂，⛔ 不是单变量）

win32 / hidden / 3 对交错（before = `b14b897`，after = HEAD 含 P/C1/A′/C6/Phase A）：尖峰只有 **3 帧**过 50M 门（非 8），Σwall 973.0/866.6 → 662.4/649.4 ms；174M 事件 `CreateFileA` 38.3/30.2 → 15.6/16.6 ms；指纹 14/14 逐位同。全文 `docs/03-emulator/perf-bench-save05.md` §3.1。
