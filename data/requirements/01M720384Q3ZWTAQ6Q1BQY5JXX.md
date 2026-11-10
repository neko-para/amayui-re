# spike · 交互图是否改用 YAML/TOML/JSON 等标准数据结构：**不换**（只省 110 行词法，丢语义错行号 + 64 位 hash 变坑）

- id: REQ-01M720384Q3ZWTAQ6Q1BQY5JXX
- type: spike
- status: done
- parent: REQ-01M6XVYY5TCKSAS80WS7PADF8P
- done_reason: 结论：不换格式。换 YAML/TOML/JSON 只删得掉词法层 ~110 行（文件 1154 行的 9.5%），~250 行闭集合语义校验照样要写；且标准库不给语义错的行号（nlohmann 对重复键静默取最后一个、未知键放行 ⇒ 13 条 origin:行: 断言里两条失败类会无声消失）。最近一次真实改动（e4a5457 加按钮词）解析器只占 41/292 行、测试占 194 行 ⇒ 维护成本不在词法。64 位 hash（0x94CB492BF7F1AEEB = 10721743791077830379 > INT64_MAX 且 > 2^53）在 JSON 里十进制会被 JS JSON.parse 丢精度（差 277）、在 TOML 里 int64 装不下。判据与读数见 docs/03-emulator/design-notes.md 的 L3-graphfmt-01；替代路径 = 解析器 table 化（REQ-01M7202Y1JHSN2WSSZFSFG9EY8）。重开条件：图以生成物为主 / schema 长出嵌套 / 有第三方消费者 / 行号不再是硬约束。
- tags: [emulator-lift, interaction, format]

## 设问

用户提出：把 `--interaction-graph` 的**手写行格式 + 手写解析器**换成 YAML / TOML / JSON 等标准数据结构，
以减少 parse 代码的维护成本（"yaml / toml 之类会比较接近目前的样式"）。

## 结论（2026-11-10）：**不换**；要省维护就 table 化解析器（另开 `REQ-01M7202Y1JHSN2WSSZFSFG9EY8`）

判据与读数全文在 `repo:docs/03-emulator/design-notes.md` 的 `L3-graphfmt-01`，这里只留可判定的四条：

- 换格式只删得掉**词法层 ~110 行**（`interaction_graph.cpp` 1154 行的 9.5%）；剩下 ~250 行是**闭集合语义校验**
  （未知键 / 重复键 / `next` 不存在 / `click center` 无框），换格式照样要写。
- 标准库**不给语义错的行号**：nlohmann 对重复键**静默取最后一个**、未知键**直接放行** ⇒ 现有 13 条 `origin:行:`
  断言里 12 条是语义错，其中两条的失败类会**无声消失**（`parse-errors` 是这些坑唯一的守卫）。
- 最近一次真实改动（`e4a5457` 加一个按钮词）动了 7 个文件 +292/−30，**解析器只占 41 行**、测试占 194 行
  ⇒ 维护成本不在词法，在"闭集合语义 + 用例"。
- **64 位 hash 是硬伤**：`0x94CB492BF7F1AEEB` = 10721743791077830379，> INT64_MAX 且 > 2^53。
  JSON 里 `0x…` 非法；十进制在 nlohmann 精确、但 **JS `JSON.parse` 变成 …830000（差 277）**；TOML int64 装不下
  （Python tomllib 接受 / toml++ 拒绝 ⇒ 同一份文件不同实现语义不同）。唯一安全写法是带引号的 `"0x%016X"` 字符串。

## 重开条件（四条全真才值得换）

图以**生成物**为主（>50%）· schema 长出**嵌套** · 有**第三方**消费者 · **行号**不再是硬约束。今天四条全假。

## 边界（不在本单做）

- ⛔ 不加 JSON / YAML 入口：按 `interaction-model.md` 的原计划，JSON 只在**生成器落地**时作为**第二入口**加，
  且与行写法**共用同一张闭集合表**；⛔ 不取代行写法。
- ⛔ 不改 `interaction-model.md` 的 schema；⛔ 不因为"换格式"去改 3 份脚本与 79 处测试夹具。
