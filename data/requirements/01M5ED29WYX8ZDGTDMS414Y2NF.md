# spec：20-alf / 30-agf 拆成 FMT-* 规则并接证据体系

- id: REQ-01M5ED29WYX8ZDGTDMS414Y2NF
- type: req
- status: done
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- verify: tools/test/spec.test.mjs#★ 真源：data/spec/{spec,bindings,audit}.json + chapters/*.md 过全部不变量，且试点未登记 = 0
- tags: [spec, format]

## 做了什么

**先守卫、后条目**（因为 `spec validate` #4 要求每条规则有 `binding.evidence`，而两章原先两者都没有）：

1. **新守卫（`@env assets`，读安装目录）**
   - `tools/test/spec-alf.assets.test.mjs`：5 份真实 `*.AAI`（当前全是 `S4AC`）——头 268 / 四签名 / AC 的 12 字节块与 280 起点 /
     TOC 记录宽度（归档 256 · 文件 80）与四个 u32 字段 / 归档名存在且 `offset+length` 落在数据体内；
     外加合成流 LZSS 单测（环内下标 0..4095、重叠匹配、长度 = 低 4 位 + 3）。
   - `tools/test/spec-agf.assets.test.mjs`：8 份真实 `*.AGF`（version 2）——24 字节头 / 12 字节 body 头 /
     **v2 的 ACIF 紧接 body** / ACIF 宽高 = meta 宽高 / alpha 字节数 = 宽×高 / 行不填充（`bpp×w/8`）/ 8bpp 的 1024 字节调色板槽。
   - 共享的 LZSS 解压放在 `tools/test/spec-lzss.mjs`（纯函数）。
2. **变异条目 3 条**：flag 极性反 / 头长 268→300 / 头长 24→32 ⇒ 对应守卫全部当场红（已逐条实测）。
3. **条目化**：`20-alf` 4 条（`FMT-ALF-{HEADER,ADDON,LZSS,TOC}`）、`30-agf` 5 条（`FMT-AGF-{HEADER,BODY,LZSS,ACIF,PIXELS}`）；
   每条 `draft` + `handled` + 机器站点（EA 从 `format/alf-agf-verification.md` 搬）+ `guard:` 证据；
   章级残差按归属移进规则（S5 / S?IN / file_index / 码页 / 多索引 / 窗末 18 字节 / 非 ACGF / v1 / 写侧 / 0 宽高）。
4. `data/spec/README.md` §2 的"协议描述章暂不挂规则"豁免句已删除。

## 守卫抓到的 spec 歧义（已修）

原稿只写 "flag 字节管 8 个单位，低位先"，**没写极性**。用真实 `*.AAI` 反解：把 1 当匹配会把归档名解成 0；
⇒ 正文补 "flag 位为 1 ⇒ 字面量、为 0 ⇒ 匹配"，并写明"反过来解会把归档名解成 0"这条反证。

## 判据（已满足）

- `pnpm tools spec chapters`：alf 4 条 / agf 5 条；`pnpm tools spec validate` 13/13
- `pnpm test` 594/594；`pnpm test:assets` 见本轮记录（新守卫绿；仍只剩 2 条已知 mac 路径项）
- `node tools/mutate-check.mjs --match "flag 极性" | "头长 300" | "头长 32"`：各 1/1 变红

## 遗留（不在本单）

- 写侧（压得小才压 / 相等即原样）与多份索引叠加仍不规定（规则残差里各有一条）。
- `S5` / `S?IN` / 非 ACGF 签名 / version 1 仍无样本。
