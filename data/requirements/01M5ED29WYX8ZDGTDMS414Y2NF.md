# spec：20-alf / 30-agf 拆成 FMT-* 规则并接证据体系

- id: REQ-01M5ED29WYX8ZDGTDMS414Y2NF
- type: req
- status: open
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- tags: [spec, format]

## 一句话

`20-alf` / `30-agf` 目前是"协议描述章"：正文有可执行命题，但**没有 `<!-- rule -->`、没有 bindings / audit 条目**，因此不进入证据计量（`pnpm tools spec chapters` 显示 partial 但零规则）。本轮只在 `data/spec/README.md` §2 显式记录这个决定，不在本轮补规则。

## 要做什么

1. 把两章拆成规则：`FMT-ALF-*`（头 / 加装档 / TOC / LZSS / 取文件路径）、`FMT-AGF-*`（头 / meta / body / ACIF / 像素），每条一个 `###` + `<!-- rule -->`。
2. 每条在 `bindings.json` 挂 EA（本轮 `format/alf-agf-verification.md` 已有逐条 EA，直接搬）、在 `audit.json` 给 status / disposition / residual。
3. 状态词统一：正文用 `已核验 / 部分 / 未取证 / 无样本 / 不属于 ALF|AGF`，与 `format/alf-agf-verification.md` 的口径表一一对应（该表已在本轮建立）。

## 判据

- `pnpm tools spec chapters` 里两章不再是"零规则"；每条规则的证据可解析。
- 不得把 `data/spec/README.md` §2 的"协议描述不挂规则"留成永久豁免（拆完就删那句）。

## 不做

- 不重写正文内容（本轮已按 `.lst` 修过一轮）；只做"条目化 + 挂证据"。
