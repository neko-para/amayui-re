# shim 效率：ComRegistry 槽计划缓存 + Vfs 目录索引/负缓存（SAVE05 尖峰帧判据）

- id: REQ-01M7VQNQ5E3ANQQR0RNSQRVX9K
- type: req
- status: doing
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- tags: [emulator-lift, shim, perf]

## 一句话

把 **SAVE05 移动**尖峰帧里宿主 shim 的"具体逻辑"成本压下去：`ComRegistry::createObject` 的**每槽登记**
（一次 `D3DXCreateTexture` 22 次 `registerSlot→addRuntime`，每次 O(`defs_`) 线性扫 + 每项 `normalizeModule`）
与 `Vfs` 的**逐段目录枚举 / 重复规范化**。判据 = **8 个尖峰帧 Σwall** 与单帧 862/886，
⛔ 不是窗口 wall（这两族合计只占窗口 2.8%，"窗口 +5%"数学不可能）。

## 读数（2026-11-20 / mac arm64 / SAVE05 移动窗口 7753 ms / 195 帧 / `.tmp/save05-bench/`）

- 8 尖峰帧（862/886/814/838/888/863/864/865）Σwall **1005.0 ms**；862 = 297.9 ms / 226.8 M insns，
  shim 95.5（`GetSurfaceLevel` 28.9 · `D3DXCreateTexture` 20.5 · `CreateFileA` 19.8 · `Present` 10.0）。
- COM 两族合计 **143.5 ms**、`CreateFileA` **71.7 ms**；两族合计 215 ms = 窗口 **2.8%**。
- 每槽成本 9.9–11.8 µs（mac）/ 33.6–35.9 µs（win32）；22:17 槽 = 1.29 vs 实测 1.09–1.21 ⇒ **随槽数线性**。
- `defs_` 已 `reserve`（`shims.cpp:214/462`）⇒ 739 µs **不是**扩容；主项 = `findMutable` + 每项 `normalizeModule`。
- 拆解与证据全文：`.tmp/perf-reassess/shim-createobject-vfs.md`（临时区，不入库）。

## 候选（预期按 8 尖峰帧口径）

| # | 候选 | 预期 | 风险 |
|---|---|---|---|
| C1 | `ComRegistry` 每 (iface, methods) 一份**槽计划缓存**（命中只 alloc + 写 stub EA + `installShim`） | −90…−115 ms（−9…−11%） | 低（guest 字节不变） |
| C2 | `ShimRegistry` 加 (module,name)→id 哈希（消掉 `findMutable` 线性扫） | −50…−70 ms | 低（纯查表） |
| C3 | 重复对象不再 `installShim`/`installFeederShim`（要 flush epoch） | −4…−8 ms | 中 |
| C5 | `Vfs` 每目录一次 `lower(name)→真实名` 索引（保留多候选 `[ambiguous]`） | −30…−45 ms | 中 |
| C6 | `Vfs` 路径缓存 + 负缓存（影子写入 +1 代失效） | −40…−55 ms | 中 |
| C7 | 日志侧先判可见性再 `snprintf`/转码（`comLog`/`d3dxLog`/`pathFailure`/`shimVPrintAt`） | −2…−4 ms | 无 |
| C4 | `GetSurfaceLevel` 同 (texture,0) 去重 = 返回同一对象 | −35…−50 ms | **高 ⇒ 另开语义单** |
| C8 | 对象头 / vtable 池化 | ≈0 | 高（破 arena 指纹）⇒ 不做 |

## 判据（能红）

1. 指纹（`exit` / `stop @` / arena 三元组 / `table misses`）逐位同。
2. A/B（同机、RelWithDebInfo、交错 ≥3 对）：**同帧号尖峰集 Σwall ≥5%**（C1 预期 −10%）；862/886 各 ≥5%；
   窗口 wall **只报不判**。
3. 每次调用 µs（prof60 同名行）：C1 目标 `createObject` 218→~40、`GetSurfaceLevel` 201→~35。
4. 守卫要能红：COM 槽计划复用（两对象 vtable 逐字节同 + 冲突表仍响亮拒绝）+ 变异一条；
   Vfs 索引的 `[ambiguous]` 用例。
5. ⛔ 不改 guest 可见语义：C4 另开语义单；Vfs 的 `L1-FILE-01` / `..` 越根 / 排序 / 写穿透 / 负缓存失效见
   `.tmp/perf-reassess/shim-createobject-vfs.md` §5。

## 落点

`src/runtime/com.{h,cpp}` · `src/runtime/shims.{h,cpp}` · `src/runtime/vfs.cpp` · `src/shims/kernel32_file.cpp`；
读数脚本在 `.tmp/perf-reassess/` 与 `.tmp/save05-bench/`（临时区，⛔ 不入库）。

## 未做

不预判 C1/C5 的命中率（先量）· 不做 C8/C4 · 不把"窗口 wall +5%"当判据（上限 2.8%）· 不动 `kCheckStride` / 泵频率。
