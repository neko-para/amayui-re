# P2 · Fade(201) 真画：每 draw 带 shader/常量/2 纹理（MSL+DXBC 双源 + NaN 用例），并翻面 P1 的临时拒绝

- id: REQ-01M7MSGRRHCA23RAX1QGDAYJ3X
- type: req
- status: open
- parent: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- tags: [emulator-lift, d3d9, shader]

## 一句话

把 `technique Fade`（资源 201）真正画出来：**每个 draw 带自己的 shader + 常量 + N 张纹理**（MSL / DXBC 双源），
并把 P1 的临时拒绝翻面。

## 要做什么

1. 解析 technique / pass（4 份文本每份恰 1 个 pass，pass 里的 state 块要按 D3D9 语义应用）。
2. 后端开一条路：**每次 effect draw 一个 shader + 常量表 + 最多 2 张纹理**（现有 `gpuEligible` 只允许 stage 0）。
3. 写 `FadePS`（`ret.a = c0.a*(1-Alpha)+c1.a*Alpha`；`ColRate = clamp(c1.a*Alpha/ret.a,0,1)`；`ret.rgb = lerp`）的 MSL + DXBC(SM5.1) 双源。
4. **NaN 用例**：两图全透明 ⇒ `ret.a = 0` ⇒ `ColRate = 0/0`。ps_2_0 与 Metal clamp 的行为不保证一致 ⇒ 必须把行为钉住并写清取舍。

## 判据

- 两张已知纹理 + `Alpha ∈ {0, 0.5, 1}` 的像素 oracle（含 NaN 那一档）；
- 账本：这批 draw 是 `recorded` 而**不是** `skipped`；
- P1 的临时拒绝被翻面（删 bit44 + 删判据 + 删除该用例的"必须拒绝"断言，改成"必须被记录"）。

## 未做

不含 203 的 10-tap 模糊（P3）、不含 caps 默认值（P4）。
