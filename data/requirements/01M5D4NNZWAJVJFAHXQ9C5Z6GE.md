# spec 残差补证：12 条缺证据的合同缺口（容器下标 / 字段身份 / 默认组 / 颜色 / 边界算术…）

- id: REQ-01M5D4NNZWAJVJFAHXQ9C5Z6GE
- type: req
- status: done
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- verify: tools/test/spec.test.mjs#★ 真源：data/spec/{spec,bindings,audit}.json + chapters/*.md 过全部不变量，且试点未登记 = 0
- tags: [engine spec, evidence]

## 结果（2026-10-21/22）

12 条里 **5 条在本次之前已修**（1 检查对象 = "元素个数为 0"、2/3 两章认同一份来源、10 调色板条目数由 `2^bpp` 导出、11 ACIF 紧接 body 无 2048 搜索），其余 **7 条本次逐条取证并落盘**：

| # | 结论 | 落点 |
|---|---|---|
| 4 字段对身份 | `0x70` 的第三对 == `0x1c1` 的对（唯一重叠）；`0x79` `+0x1C/+0x20`、`0x303` `+0x120/+0x124` 与它都不同 | `60-ops-list` 三条规则 + audit 残差改写 |
| 5 默认组 | 组号 0 落在一个**选择子**上，初值 **1**，由 `0x80` 改写 | 新规则 `CORE-OPS-LIST-DEFAULT-GROUP`；0x70/0x79/0x1c1/0x303 升回 `handled` |
| 6 转发身份 | 入口身份能点名（函数 / 消息号 `/` vtable 槽 / 命名键）；类型名语料里没有，**不编** | `60-ops-forward` 六条 + 章首重写 |
| 7 两格 | `0x71` 与 `0x3` 写的是同一帧记录里**相邻的两个不同字段**，互不复用 | 两条规则各加一句（正文不写偏移） |
| 8 位身份 | 「有效」==「已存在」= 绘制项 bit 0；等待位 bit 10；`0x71` 条件位 bit 27；列表登记每组一个 dword | `0x1fb` / `0x202` / `0x21c` / `0x71` / `0x88` |
| 12 `0x30a` 的 31 | 31 是字面接受上界，与 8 格表无关；⛔ 不许实现成 `& 0x1F`；表零读者、表义未定 | `50-engine-state` 的 `0x30a` + 残差 |
| 9 配置第二标记 | 两个标记只判符号，不是长度；N 是标记之后的独立 32 位字 | `55-config` + 删 `RES-CONFIG-MARKERS` |

## 台账

新增 7 条 observation（均 `proposed`，锚 `reference` + `len`/`sha256`）：`0x80` 默认组 · `0x1ca` 命名键写入口 · `0x73` 十字段写 · `0x71`/`0x3` 两格 · `0x1fb`/`0x202` bit0 · `0x30a` 上界 · 配置默认表尾读序列。
`0x73` 首版观察有一行逐字表写错，已 `retract` 并重发（append-only，未改历史）。

## 边界

* 7 条规则的证据是 proposed（弱证据）⇒ 全部保持 `draft`，没有升 `reviewed`。
* 发现的参考实现口径差（`0x1ca` / `0x324` / `0x73`）已记残差，**未改 emulator**（不在本单范围）。
* 单末"另有（已在别处登记）"的 5 项（窗边界算术 / 版本修补键 / 颜色位段 / 版本阈值 / 字符串内联阈值）不在本单。

## 怎么查

`pnpm tools spec show <规则 id>` · `pnpm tools ledger show <新 KN>` · `pnpm tools spec validate`。
