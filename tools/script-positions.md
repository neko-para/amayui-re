# tools/script-positions.md —— 脚本内位置（`pos`）锚表的说明书

> **JSON 是不透明数据**：字段语义 / 枚举 / 不变量（含谁在守）/ 操作，唯一真源是
> `node tools/script-positions.mjs --describe`（= `pnpm tools script-positions describe`）。
> 本文件只写"它是什么 / 为什么这么定 / 怎么查 / 怎么改"，⛔ 不复述 schema。

## 它是什么

| | 落点 | 读写 |
|---|---|---|
| **锚表（真源）** | `tools/script-positions.json` | 人工编辑的文本；`gen` **不回写**它 |
| **生成物** | `apps/emulator-lift/scripts/plugins/lib/script-positions.js` | 唯一写入口 = `gen --write`；⛔ 别手改（`check` 会红） |
| 输入（只读） | `data/translations/patch.json` + 基线安装树（清单 `roots.gameInstall`） | 补丁后视图由 `lib/patch.mjs` 的 `buildView('src', …)` 现算 |

**一条锚 = 基线里的一段指令序列 + 基线行序**，它绑一个消费方 `import` 的常量名。生成物里的数值是
**补丁后坐标**（插件运行时读的是汉化 install 的 BIN）—— `pos = 0x3C + Σ(4+argc*8)`，
与 `dist/views/data/<NAME>.BIN.txt` 的 `label_%08x` 同坐标系。

## 为什么锚长这样（三条都来自实测）

1. **译文不改坐标**：译文只改字符串字面量，字符串进"代码之后的字符串池"；指令长度只由 `argc` 定
   ⇒ 译文变长**不移动**任何 `pos`。所以 `window` 里的字符串一律**屏蔽**成 `"?"`；
2. **只有 `insert-after` / `delete` 会移动坐标**：每条 ±`4+argc*8` 字节（patch 的 op 是行级的）⇒
   地址与 `label_XXXXXXXX` 都是**派生量**，不能当锚；`window` 里的 label 引用一律屏蔽成 `label_?`；
3. **基线不可变**、补丁可变：锚钉在**基线行序**（`baselineIndex`）上，`pos` 由"命中的补丁后指令"**现算**。
   ⇒ patch 里加了 `insert-after` / `delete`，不要手改生成物，重跑 `gen --write` 就行。

## 怎么查

```bash
# 一页纸：字段语义 / 不变量 / 操作（schema 的唯一真源）
pnpm tools script-positions describe

# 生成物还新不新：锚唯一性 + 锚表 pos 与基线网格一致 + 逐字比较生成物（只读）
pnpm tools script-positions check

# 人审一条：锚 → 基线窗口 → 命中的补丁后指令行 → 算出的 pos（含平移来源的 op）
pnpm tools script-positions explain FIELD_MAIN_LOOP_LO
```

★ 直接跑也行（不经 pnpm）：`node tools/script-positions.mjs check` / `gen` / `explain <NAME>`。

人怎么审（生成物 diff 一眼看出是哪条锚动的手）：

* `window` 变了 ⇒ **代码序列**真变了（译文被 `"?"` 吃掉，不会响）；
* `pos` 变了而 `window` 没变 ⇒ 之前有 `insert-after` / `delete`（`explain` 会点名是哪个 op、±多少字节）；
* `baselineIndex` 变了 ⇒ **基线**（原版语料）变了 —— 那是更大件事，停下来查。

## 怎么改

* **新增一条位置**：先定锚（脚本名 + 基线指令行序 + 最小可唯一窗口；字符串写 `"?"`、label 引用写
  `label_?`）⇒ 加进 `tools/script-positions.json` 的 `positions`，并把它放进 `layout` 里生成物该出现的位置
  （`note` 写用途 / 读数出处，会**逐字**搬进生成物）⇒ `pnpm tools script-positions gen --write` ⇒ `check`。
  窗口不够唯一时 `check` / `gen` 会点名"命中 N 次 + 候选地址"，**加宽窗口**，⛔ 不要"取第一个"。
* **patch 改了之后**：`patch.json` 改完（尤其带 `insert-after` / `delete`）跑
  `pnpm tools script-positions check`；红 = 忘了 `gen`，按点名的常量重跑 `gen --write`。
* ⛔ **不要手改生成物**：`check` 与它逐字比较；头的注释也会被覆盖。
* ⛔ **不要在消费方裸写 `0x…`**：位置值只在生成物里出现（守卫 `tools/test/script-map.test.mjs` 的
  `HEX_ALLOWED` 按**文件名**豁免这份生成物 —— 是既有机制，⛔ 不是这次新放宽的口子）。

## 未做 / 待核

* **`SN0000.BIN` 的整节常量已从本表移出**（原表里那一节，10 条）。原因：该脚本有 33 个 `delete` + 1 个
  `insert-after`，按指令累计前移 ≈ −236 B ⇒ 那 10 条里 **8 条不在补丁后指令网格上**（另 2 条是"区间上界
  一过"的值），而**当前没有 lib 消费方** ⇒ 属于**潜伏**的静默失效。重锚要**对着补丁后视图**重取窗口
  （⛔ 不要为凑"逐字相同"把它硬算成旧值）；在重锚之前，任何 `.plan` 里的 SN0000 字面量都只当**历史读数**。
* **锚歧义**：窗口在补丁后视图里命中 ≠ 1 次时，工具**拒绝生成并点名候选地址**（用 `explain` 看命中的
  是哪几处）。健康脚本的窗口很短就唯一；页块互相重复的脚本（如 SN0000）需要更长的窗口或换锚点。
* **多份视图不一致**：本工具按"基线 + patch"现算，不读 `dist/install`。若 `dist/install` 是另一份
  产物（例如没跑 `pnpm tools release install`），插件运行时看到的 `pos` 可能与这里不同 —— 两边不一致时
  以 `patch.json` 为准并重跑 `pnpm tools release install`，⛔ 不要把 `dist/install` 的字节写回锚表。
