# 批 M0 · 骨架与纪律（目录 / 根配置 / 清单守卫 / 旧仓盘点）

- id: REQ-01M3TCGGJ0AVHPJTF1KG9R0BA7
- type: req
- status: done
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- order: 10
- verify: tools/test/corpus-manifest.assets.test.mjs#端到端：真实 corpus/assets.json 过全部断言

## 判据（已完成）
`corpus/assets.json` 过 9 条守卫；`.gitattributes` 首行是 `* text=auto eol=lf`；
根目录有 `AGENTS.md` / `decisions.md` / 旧仓盘点生成物。

## 凭据
`tools/test/corpus-manifest.test.mjs` 的端到端用例（真实清单必须全绿）。
