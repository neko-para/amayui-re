# emulator-lift · SetRenderState 白名单把一批合法 D3DRS 当非法拒绝（+ Get 空洞不对称）

- id: REQ-01M7A3M73R3XYMM7J4KNQ1VNV7
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, d3d9, render-state]

## 缺口（D3D9 四条复审的"可另开单"）

`knownRenderState()` 的 A∪B 白名单**拒掉了一批合法的 D3DRS 值**：163 / 165 / 172–173 / 178–183 / 198–205
（D3D9 官方枚举里有，本线既没实现也没归 B"只存"）。引擎实测只写 16 个状态 ⇒ 今天无可观测后果，
但口径是"除明确拒绝外都补"：这些值应当在 B 档**接受并存下**（或用 `recordUnsupported` 显式拒绝），而不是落进"非 D3DRS"的 INVALIDCALL。

另有一处不对称：`GetRenderState` 仍接受 1..6 等**空洞**值（测试钉住 `Get(2)==0`），而 `Set` 拒绝它们。

## 判据

- 逐值核对 D3D9 官方枚举：本线不实现的合法 D3DRS ⇒ 归 B（存 + 可读回）并**记账**；真的非法 ⇒ INVALIDCALL 不记账。
- `Get` 的接受域与 `Set` 一致（除文档明确允许的默认读）。
- 守卫：`d3d9p/render-state-domain` —— 表驱动：对每个"合法 D3DRS"断言 `Set` 不返回 INVALIDCALL（B 档）或返回 INVALIDCALL 且 `recordUnsupported` 命中（显式拒绝档）；
  对每个"非 D3DRS"断言 INVALIDCALL 且不记账。⛔ 不要写"当前 209 个值里只有 X 个"这类取值断言。
- 变异：把某个 B 档值挪进拒绝档 ⇒ 该用例红。

## 非目标

- 不实现这些状态的行为（只做接受域与记账）；不扩多 RT / 多 stream 相关的状态语义。
