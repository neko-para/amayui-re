# emulator-lift · D3D9 SetRenderState：A/B 白名单 + C 档值敏感拒绝 + 未支持位可读（bit26）

- id: REQ-01M79PPA10JCKG4C9HY6QMV6PT
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/render-state-policy
- tags: [emulator-lift, d3d9, render-state]

## 缺口（索引第 6 行拆出）

`SetRenderState` 的 `default: break;` 把 0..255 全当合法并**存下**，其中 210..255 根本不是 D3DRS 值；sRGB 写（194）这样静默通过。

## 引擎实测（2026-11-13 只读普查）

扫 AGE.EXE + AGERC.DLL 全部 `[reg+0E4h]` 调用点（96 处，过滤到 **17 个状态**），**全部已实现**：
`{1,7,14,15,19,20,22,27,137,139,145,151,167,171,206,207,208:0x100}` ⇒ "拒绝档"对引擎**零风险**（复跑命令见 `.tmp/d3d9-left/brief.md` §7）。

## 已裁决

- **A 实现** / **B 只存**（合法且无可观察副作用，或被已记账的 master 门控）/ **C 值敏感拒绝** 三档；`knownRenderState()` 一处 switch 表达 A∪B。
- **C 档** = `175 SLOPESCALEDEPTHBIAS`、`176 ANTIALIASEDLINEENABLE`、`184 ENABLEADAPTIVETESSELLATION`、`194 SRGBWRITEENABLE`、`195 DEPTHBIAS`：
  惰性值（0）放行；有效值 ⇒ `recordUnsupported(bit26)` + `D3DERR_INVALIDCALL`，**不写状态块**。
- 其余（含 **210..255**）不是 D3DRS ⇒ INVALIDCALL，**不记 bit**（与今天的上界同类）。
- ★ **不能只把 `default` 改成"记账但仍 setRs"** —— 那还是静默通过。

## 共用前置（与 #3 一起做，一次改完）

1. `UnsupportedBit` 具名枚举（0..25 数值不变，追加 26=未分类 D3DRS、27=ValidateDevice），替掉散在 20 处的裸整数。
2. **位图今天只写不读** ⇒ `PipelineInfo` 加 `unsupported[8]` + `pipelineInfoOfState` 拷贝 + 运行摘要加一行 `unsupported : 0x…`（否则守卫只能断言"日志里出现过"）。
3. `recordUnsupported` 命中已置位就早退 ⇒ 多状态可辨识要加一次性 bitset 日志。
4. ⛔ 别用 `pipeWarn` 记这两类拒绝（`tests/d3d9_pipeline.cpp` 按类别断言精确计数）。

## 判据

守卫 `d3d9p/render-state-policy`：(1) 引擎 17 状态按实测值逐个 Set 都 S_OK（白名单漏项 ⇒ 红）；(2) `Set(194,1)` ⇒ INVALIDCALL + bit26 置位 + `Get(194)` 仍 0；
(3) `Set(214,1)` ⇒ INVALIDCALL 且不写；(4) B 样本 `Set(34,color)` ⇒ S_OK + 可读回 + bit26 不置位。**修前红：(2)(3)**。
变异：恢复 `default: break` ⇒ (2)(3) 红；bit26→25 ⇒ 同位断言红。

## 订正（2026-11-13 · 复审抓出的扫描伪值）

- 引擎实测状态是 **16 个**，不是 17：`{7,14,15,19,20,22,27,137,139,145,151,167,171,206,207,208}`。
- 原表里的 `1` 是扫描伪值（`.lst 0x4C1ED2 push 3 / 0x4C1ED6 push 1 / 0x4C1ED8 push 16h` ⇒ state `0x16`=22、值 `1|3`）；
  `138` 不在 D3D9 官方枚举里。两个都已从白名单与 `engineRenderStates()` 删除，并加断言钉住"收非 D3DRS 值 ⇒ INVALIDCALL"。
