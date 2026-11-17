# P1 · D3DXCreateEffectFromResourceA（9 参）+ ID3DXEffect 9 槽只记状态；effect draw 临时具名拒绝（bit44）

- id: REQ-01M7MSGRJCEPAE8NS323WEG0SG
- type: req
- status: done
- parent: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- verify: apps/emulator-lift/tests/d3dx9_effect.cpp#d3dx9-effect/resources-pinned
- tags: [emulator-lift, d3d9, effect]

## 一句话

GPU 路线 P0+P1 落地：4 个 effect 资源可读（PE `.rsrc` RT_RCDATA 的 ASCII HLSL），`D3DXCreateEffectFromResourceA`（9 参）
+ `ID3DXEffect` 9 槽 COM（**只记状态、不执行 shader**）；引擎那 718 个 `Begin..End` 循环不再停桩页（`7FFE0060` 消失）。

## 读数（2026-11-17 / mac arm64 / `links/raw` / prefix27）

- E2E（`LIFT_D3D9_PS_VERSION=0x200`）：`exit 0` · `pending=0` · `unimplemented=0` · `comUnsupported=0` · `7FFE0060=0`；
  资源 **201/203 在 frame 949 创建成功**；方法面计数 718/2684/718/718/718/718/718/718（与 S0 逐条吻合）。
- 缺省档 vs 改动前：差异只有 `shim: implemented 298→299 / gaps 12→11` + 抖动行。
- 资源 pin：`d3dx9-effect/resources-pinned`（4 份大小 + `//` 前缀 + 完整 sha256 + `RT_RCDATA` 恰好 4 项）。

## ★ 本段修掉的一处静默错（写进 `defects.md` 的那一类）

E2E 发现：引擎对 203 只绑 `Tex0` ⇒ 既有 `stage≥1`（bit33）不触发 ⇒ effect 内提交的 draw 被**当作固定功能接受**
（错误像素、账上无具名位）。⇒ 新增**具名位 44** `effect shader 尚未实现执行（P1 临时）` + `gpuEligible` 第一条判据，
整批拒绝且**每批一条** warn（点名资源 id / technique / pass）。读数：同一 E2E 拒绝 **1195** 批（SlideBlur 885 + ZoomBlur 310）。

## 未做（如实）

`Release` 收口只有 pure 用例（E2E 未到收尾）· `*pPasses = 1` 是结构性下限（P2 才解析）· `IID_ID3DXEffect` 只答 `IUnknown` ·
等级第 2 入口（`0x4A6FA2` 的 `arg_4`）未观测到覆盖。

## P2 翻面清单（⛔ 不要留成永久契约）

删 `gpuEligible` 里那条临时判据 + 删 bit44 + 删 `effectDrawWarn` + 把 `d3dx9-effect/effect-draw-refused` 改成 `recorded`。
