# 引擎逆向（常态化）

- id: REQ-01M3TCCVC01AWFEMP6GJM96KQ4
- type: req
- status: doing
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 30
- tags: [reverse-engineering, standing]

## 性质
**常态化工作，没有上界**：不追求"做完"，只追求"每条结论都能被再校验"。

## 纪律
锚点锚 **EA**（不锚语料行号）；每条结论必须绑定至少一条可再校验的观察；
观察失效 ⇒ 降级 `stale` 但不删除；冲突显式化，不许静默改写。

## 非目标
结论本身**不进这棵树**（树只放"还要做什么"）；已成立的结论进知识台账（`data/ledger/`）。
