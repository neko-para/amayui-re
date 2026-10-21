# CORE-POOL 批次：数据池与值取证、条目与 EA 映射

- id: REQ-01M5AJWX9RN845VXFTX79Y1J9D
- type: req
- status: done
- parent: REQ-01M5AJWPM4DXWG3XR6W7JR6XHD
- order: 20
- done_reason: 草稿 19 条全部有归宿：12 条新规则合入（含标量表拆 4 条）+ 既有 7 条按草稿订正/改写；19 个类型标签 × 5 原语全有条目；证据 = KN 记录 + spec-pool.assets.test.mjs 6 条结构断言。既有 7 条的 SPC→CORE 并号是纯机械改名，留作后续
- tags: [engine spec core pool]

## 目标

池族 / 槽 / 五种访问原语 / 类型标签支持集 / 值编解码 / 容量与越界 / 字符串记录。
迁移自 `repo:data/spec/chapters/40-pools.md`（`repo:data/spec/chapters/50-engine-state.md` 的唯一规则归 `CORE-OPS`，不归本机制）。

## 判据

1. 闭集 = 操作数类型标签；每个标签三选一：被回答 / 明确不支持 / 残差。
2. 五种原语（取址 / 读整 / 读浮 / 写整 / 写浮）的站点 EA 齐，每条有 `evidence`。
3. 容量与越界可判：至少区分"静默跳过 / 抛 / 未定义"三种行为，各自有条目。
4. 字符串记录：内联阈值、记录尺寸、读写路径都有映射（EA）。

## 合并状态（每次更新这一段）

* 已订正 5 处条文（全部对过语料，见 `40-pools.md` / `50-engine-state.md`）：
  1. `GLOBAL-REF`：删掉 `type 8 → 解引用 → 字符串记录`（读整 / 读浮对 8 落缺省抛；8 只有取址）；
  2. `UNSUPPORTED`：支持表重写为「接受 / 明确不支持」——补读的 0–2、写不接受 0–2、`0x8003` 与 `0x8009` 被五原语接受、`0x8005` / `0x800B` 只在取址里接受；
  3. `0x10b`：检查的是**值**（≤ 0x1F），**索引完全不检查**（越界写 = 未定义）；
  4. `0x107`：检查的是**索引**（值不检查）——原表把它写成"值 ≤ 0x1F"，错；
  5. `0x107` / `0x10b` 的操作数角色相反；三条都先把操作数个数槽置 5（`argc = 2`）；
  6. 句柄 `0x8003` 的读分支：复核后确认草稿的「容器」模型**成立**——`[操作数地址]` 是 raw（池槽下标），`pool[raw]` 是**编码后的容器指针**，解码后按头/尾指针取第一个元素（空/0 ⇒ 0）。⇒ 两条句柄规则已按此合入。
* 已合 12 条新规则：`TAG-IMMEDIATE` · `HANDLE-INT` · `HANDLE-STRING` · `STRING-WRITE` · `INDEX-UNCHECKED` ·
  `SCALAR-ARRAY-107/10B/30A` · `SCALAR-FRAME-FLAG` · `PRIMITIVE-ENTRY` · `TAG-DISPATCH` · `FLOAT-VALUE`
  （`SPC-POOL-CAPACITY-AND-UB` 改写为"容量与分配"；`SPC-ENGINE-SCALAR-ARRAYS` 已拆成 4 条并移除）。
* 既有 7 条（`GLOBAL-VALUE` / `GLOBAL-REF` / `LOCAL` / `UNSUPPORTED` / `INT-CODEC` / `STRING-RECORD` / `CAPACITY`）
  已按草稿订正或改写，但仍用 `SPC-*` id；与草稿同义的并号迁移未做（纯机械改名，不急）。
* 草稿 19 条全部有归宿（12 条新规则 + 7 条既有规则）；池的闭集（19 个标签 × 5 原语）已全有条目。
* 证据：能挂 `KN:` 的用 `KN:`；其余用 `spec-pool.assets.test.mjs` 的结构断言（6 条用例）。

## 非目标

* 不写 opcode 语义（那是 `CORE-OPS`）。
* 容器句柄（`0x8003/5/9/B`）归 `CORE-CONTAINERS`（不在本机制里展开）。
