# spec 成篇 · L2D 载入与参数（0x341/0x345/0x34e/0x352）

- id: REQ-01M63XE2NE5V71RST5SKV0NZCA
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- order: 5
- verify: tools/test/spec-gaps.test.mjs#★ 精确划分
- tags: [engine spec]

## 判据（可判定）

`audit.gaps[]` 里不再出现：`0x341` `0x345` `0x34e` `0x352`；两条规则各有标题、`bindings`、`audit.rules`；
`pnpm tools spec validate` 13/13 绿 + 精确划分守卫绿（收口凭据）。★ 站点也进了
`repo:tools/test/spec-ops-100-200-sites.assets.test.mjs` 的 RULES 清单（站点必须是指令行首）。

## 已落（2026-10-29）

`data/spec/chapters/60-ops-list.md`：

- `CORE-OPS-L2D-LOAD`（0x341 / 0x345 / 0x34e）：三条同形兄弟；文件名最先读、其余**从后往前**读；
  失败写错误文本并抛「显示消息」型异常；不写操作数。
- `CORE-OPS-L2D-MARK`（0x352）：按对象下标取对象，方向 = 0 / 非 0 两支互斥；空槽静默。

## 独立复审后的修复（第 2 批复审）

- 章首导语补上 `0x12f`（既有的漏列，与本批无关但顺手）。

## 已知欠账

- 参考实现里这四条**共用 `op_341` 且是空返回**（不是按条目实现）⇒ 已登记成
  `REQ-01M63YS3T54VYCXCBJ6RPCHDAY`（"spec 已成篇、实现仍是桩"的清单）。
- 证据字段引的是站点守卫（只证锚位置），语义证据是 `.lst` 逐 handler 的读取（记录在本单）。
