# 帧记录基址口径改写：layout.mts 的 FRAME_LAYOUT.base 0x5D880 → 记录基址 0x5D894（含 off.* 编号 / "至少 0x8C" 用例 / slot-census 一族）

- id: REQ-01M4H0Q2TQMKHQ35X69DG0FPGP
- type: req
- status: done
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- verify: tools/test/engine-frame.test.mjs#★★ 帧记录 = `0x5D894` 起 `0x78` 字节
- tags: [engine, ledger]

## 已收口（2026-10 三次订正落地）

台账：**`KN-01M4H0MZGD4K4P4F0J1E7E5G79`**（本轮的"模型已对齐"观察另落一条，锚 = 本单 `verify`）。

### 1. `packages/age-format/src/engine/layout.mts`
* `FRAME_LAYOUT.base` `0x5d880` → **`0x5d894`**（记录基址）；`stride = 0x78` **同时就是记录大小**（`0x74 + 4`）。
* `off.*` **统一减 `0x14`**；`strTable` 删除（与 `strBase` 重复）；`state6C/state70` → **`position58/position5c`**
  （名字随基址走，注释写明"**位置**，不是下标"）；新增 `grids: [0x64, 0x68, 0x6c]`（那三格是**记录内**三格）。
* 那五个绝对地址槽（`0x5D880/0x5D884/0x5D888/0x5D88C/0x5D890`）**移出记录**，单独登记成
  **`FRAME_ENGINE_SCALARS`**（注释写清：全语料**零个 `*8` 形态** ⇒ 不是 per-cur 字段；`0x5D880` = `cur`）。
* `FRAME_SLOTS_OBSERVED` = 记录内 `0x00..0x74` **逐 dword 连续 30 格**；`LOCAL_POOL_SLOTS` 的 `count/base`
  跟着减 `0x14`（计数 `0x08..0x1C`、基址 `0x20/0x24/0x28/0x2C/0x30/0x34`）。
* 头注把**两种以 cur 索引的写法**各自的正当归属写清：`Engine[0x5D880]`（`cur` 自身）→ 装载器算 `120*cur`
  → 落到 `0x5D894 + 0x78*cur`（**记录**）；`0x5D880 + 0x78*cur` **不是任何记录地址**（落在记录前 `0x14` 字节的标量里）。

### 2. 守卫（用例名 = 收口凭据）
* `tools/test/engine-frame.test.mjs`：
  * 新判据 `★★ 帧记录 = 0x5D894 起 0x78 字节…`（算式全走模型常量 ⇒ 改回 `0x5D880`/`0x8C` 当场红）；
  * 普查用例改成"**`≥ stride` 的槽 0 个** + 负偏移恰好是那五个 Engine 级标量"；
  * 旧「至少 0x8C」用例**改写成负例**（保留分歧对照：假偏移怎么来的、真读数落在 `0x78` 之内）；
  * 新增边界判据：`0x5D894 + 40*0x78 = 0x5EB54`（语料 94 处）而旧算术 `0x5EB40` 在 `.text` **零处**。
* `tools/test/emulator-model.test.mjs`：`frameCensus` 的折叠形落点 `+0x38` → `+0x24`；记录区尾部三格 → `+0x64/+0x68/+0x6C`；
  「两个不同的量」那条用例改成「**索引步长就是记录大小**」；关键槽点名一律改走模型常量。
* `tools/test/emulator-iterate.test.mjs`：推进式那两格 → 记录 `+0x60` / `+0x04`。
* `tools/mutate-check.mjs`：加两条（基址 `0x5D894 → 0x5D880`、步长 `0x78 → 0x8C` ⇒ `engine-frame` 当场红），
  并把三条随偏移改写的旧条目同步到新编号。

### 3. 连带（口径不许并存）
`apps/emulator/src/{model/address-space.ts,model/iterate.ts,vm/{ops,machine,script}.ts}`、
`apps/emulator/README.md`、`docs/03-emulator/address-space.md`、
`tools/test/emulator-frame-tables.assets.test.mjs`、`packages/age-format/src/engine/handlers.mts`
里所有 `帧+0xNN` 记法都改成 `记录+0xNN`（旧值仍然随文保留为"旧基址记法"，不做静默改写）。
