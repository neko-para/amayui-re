# emulator-lift · 交互图解析：完备性交给编译器（漏一个枚举值 ⇒ 编译期红）；已裁决不建表

- id: REQ-01M7202Y1JHSN2WSSZFSFG9EY8
- type: req
- status: done
- parent: REQ-01M6XVYY5TCKSAS80WS7PADF8P
- verify: apps/emulator-lift/tests/interaction_graph.cpp#parse-errors
- tags: [emulator-lift, interaction, refactor]

## 为什么要它

`docs/03-emulator/design-notes.md` 的 `L3-graphfmt-01`：**换格式省不下维护**（只删得掉 ~110 行词法，还丢语义错的行号）。

真正的风险不是"加一个键要改 3 处 if"，而是**按 kind 的 `if` 链会静默漏改**：编译器原先只看得见两个穷举
`switch`（`condKindName` / `condHolds`）；其余判断（expect 是否要新样本 / `when` 是否产框 / 是否登记矩形 /
`click center` 的坐标来源合不合法 / `quit` 是否一发即收口 / 失败报告按哪些条件打候选）都是
`if (k == …)` —— 加一个枚举值漏了任何一处，**构建不红、用例也不红**。

★ 实测校正（2026-11-10，本轮复审）：原单拿 `e4a5457` 当成本证据，但那次提交**没有新增任何 `CondKind` /
`GraphActionKind`** —— 它只给 `click` 加了可选按钮词（292 行里解析器 41 行、测试 194 行，能落到这类分派上的
≤10 行）。⇒ 它证明的是"成本在语义与跨层用例"，**不支持**大改解析器。本单因此改成"**把完备性交给编译器**"
（最小、可机械验证），**不建表**。

## 判据（做完后）

1. **完备性由编译器守**：`CondKind` / `GraphActionKind` 的每个成员，在
   ① 落值（元数·字段）② 判定（`condHolds`）③ 分类（产框 / 登记矩形 / 盯整帧 / expect 是否等新样本 /
   动作是否要框 / 动作是否一发即收口）④ 执行（`issueAction`）⑤ 失败报告候选
   的**每一个落点**上，都必须由**不带 `default:` 的 `switch`** 表态；漏任一处 ⇒ **编译期红**
   （实测：临时加一个枚举值 ⇒ 上述每个落点各报一条 `-Wswitch`）。
   ★ "词 → kind" **不在本条的范围内**：它必须手写（判据 2 管它）。
   ⚠ 三平台预设用的编译器（clang-cl / GCC / Clang）都会报；裸 MSVC `cl` 的 C4061/C4062 **默认关闭** ——
   若将来改用 `cl` 配置，要补 `/w14061 /w14062`（未在 Windows 取证）。
   ⛔ 不把"必须存在一张描述符表"写进合同（那是实现取舍，见 §不做）。
2. **手写字符串分派只允许"词→枚举"这一种形态**：`condKindFrom` / `actionKindFrom`（词→kind）与
   `parseButtonWord`（词→`MouseButton`）。未知词必须**解析期**带 `origin:行:` 响亮报错，
   ⛔ 不静默放过、⛔ 不退化成 `always`。
3. **字节不变**（历史 trace 与错误断言都靠它）：`parse-errors` 的 `origin:行:` 断言**一条不少**；
   未知词回显**原文**（写 `FOO` 就在报错里看到 `FOO`，不被小写化）；`GraphCond::text` 的归一化
   （条件头小写 + `0x%016X`）与 `action <text> applied` 里的**原始词序**不变。
4. **全部 pure 用例绿**（⛔ 不写死条数），且必须有 `parse-uppercase-ok` 与 `parse-unknown-echoes-raw`。
   ★ 前者的覆盖口径是**每一个靠 `lowerCopy` 的词位**（键名 / 条件头 / `screen` 子词 / `rect` 的 `avg` /
   按钮词 / `wait` 的单位词 / `profile_begin` / `exit`）—— 去掉任何一处 `lowerCopy` 都必须让它红。
   ★ 解析失败时**不许**继续索引 `nodes()`（空 vector ⇒ UB：实测会把"一条断言红"变成 SEGFAULT，
   连汇总行都打不出来）。
5. **变异清单仍然有效**：`pnpm verify` 的清单静态检查绿；重构若改了行缩进，**同一次改动**里同步
   `from` / `to`。
6. **死代码清零**：`graphActionKindName`（全仓零调用者）与只写不读的 `trace_watch_frame_` 删除。

## 不做（边界）

- ⛔ **不建表、不拆 TU**（已裁决）：单键在表内的节省 ≤20 min，而回本要 ≥24 个新键；已知待加键只有 2 个
  （`when script`、`screen rect … hash`），且 `script` 的正确形状还没落地 —— 它的"等新样本"判据很可能是
  **marker 边沿**而不是采样计数（帧停住时采样计数不涨）⇒ 现在建表等于猜。
  **重开条件（两条任一）**：≥2 个新键同时落地，**或**出现一次"漏改静默点"缺陷。
- ⛔ 不改文件格式、不改 `interaction-model.md` 的 schema、不碰 trace 格式。
- ⛔ 不顺手实现 `when script`（父单的下一项；它还要解 `--anchor-script` 与 `--interaction-graph` 互斥）。
- ⛔ 不为了"省几行"补 `default:`：那等于关掉唯一的完备性护栏。

## 本次落点（2026-11-10）

- `apps/emulator-lift/src/runtime/interaction_graph.cpp`：6 个分类谓词 · `condKindFrom` / `actionKindFrom` ·
  两个 `switch (kind)` 落值段 · 删 `graphActionKindName` 与 `trace_watch_frame_`。
- `apps/emulator-lift/tests/interaction_graph.cpp`：新增 `parse-uppercase-ok`（覆盖全部 `lowerCopy` 词位）/
  `parse-unknown-echoes-raw`。
- `tools/mutate-check.mjs`：按钮词那条变异对象的缩进随新的 `case` 块同步（`from` 逐字）。
- 读数：`ctest -R interaction-graph` 绿 · `pnpm test` 全绿 · `pnpm verify` 绿 ·
  `node tools/mutate-check.mjs --match interaction-graph` 2/2 按预期变红 · 临时加一个枚举值 ⇒ 每个落点一条 `-Wswitch`。
