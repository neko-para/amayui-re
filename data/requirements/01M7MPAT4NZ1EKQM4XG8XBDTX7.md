# caps 声明 ⇔ 实现能力：启动 capability-deviation 读数 + 一条默认档守卫（防'caps 报 0 导致静默改路'）

- id: REQ-01M7MPAT4NZ1EKQM4XG8XBDTX7
- type: req
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- tags: [emulator-lift, d3d9, caps, guard]

## 一句话

把"caps 报的值"与"我们到底实现了什么"绑成**机器可判**的两件事，避免再出现"caps 报 0 ⇒ 引擎静默改走软件路径、而没有任何信号"：

1. **启动一行 `capability-deviation`**（warn、一次性）：点名"`PixelShaderVersion=0` ⇒ 效果等级 0 ⇒ 资源 200/201/202/203 不会被建
   ⇒ 转场走软件模糊（单帧峰值 2e10 条）"。⛔ 不是调数值，是让后果可见。
2. **一条 CTest（默认档）**：`caps 声明的 shader 能力 ⇔ 实现能力` —— 若默认 caps 报 `PS >= 0xFFFF0100`，
   则 `d3dx9_43!D3DXCreateEffectFromResourceA` **必须已实现**（不在 `addUnimplemented` 表里）；反之若该导出仍未实现，
   默认 caps 必须报 0。这样"为了试 FX 改 caps"会**当场红**，而不是 12.7 s 后停在桩页。

## 依据（2026-11-17 取证，台账 `KN-01M7MPA94T7E7R0B0D2758465S` / `KN-01M7MPADXQ7N2H3T7Y73341V28`）

- 引擎只读 5 个 caps 字段（4 站点）；`+0xCC PixelShaderVersion` 是**唯一**有整条分路后果的项
  （`0x4A6FB7/0x4A6FCA` → 等级 0/1/2 → 10 个分支）。
- 其余 48 字段**零读点** ⇒ 它们取值只影响"声明 == 实现"这条口径，不影响行为（收紧缩值属于后续清理，不在本单）。
- 反例读数：PS=0 ⇒ 转场软件模糊（峰值帧 20,455,714,096 条 / 单段 37.9e9 / 7.39 s）；
  PS=0xFFFF0300 ⇒ 12.7 s 后停在 `0x004B0798`（`unimplemented`）。

## 判据

- `ctest -R caps-declaration` 绿：默认 caps 与 `addUnimplemented` 表一致；把默认 PS 改成 `0x200` 而导出仍未实现 ⇒ **红**（变异实测）。
- 启动 readout 在 `--audio off` 缺省档**多一行**（warn 级、一次性），且这行**只在**存在被清位的 shader 能力时出现。

## 未做

⛔ 不改默认 caps 值（那是路线单 P4）；⛔ 不收紧那 48 个零读点字段（行为惰性，另单）。
