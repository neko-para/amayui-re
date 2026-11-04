# emulator-lift · M3 · 3D 与 Live2D（D3D9/D3DX；外包即换路线）

- id: REQ-01M6H6G7FN5BAAJPPQYQEB2M27
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- tags: [emulator-lift m3]

## 一句话
3D（D3D9 + D3DX）与 Live2D。★ **主停手点**：自写 D3D9 是 XL（3–6 月，约占平台总工作量 30%）。

## 范围（真源：`repo:docs/03-emulator/platform-effort.md`）
- **§3.7 D3D9 + D3DX**：21 导入 / 147 站点，其中 D3DX 数学占 128 站点；真正的 API 面在**设备 vtable**（≤2200 站点 / 83 offset，**无类型证据**，不能按 offset 推断接口）。1 个 effect（雪，资源 id 202）+ `.x` 网格层级 + `D3DXSprite`。
- **§3.8 Live2D**：静态链接的 SDK（182 个函数，平台面 ≈0，导入站点 0）⇒ 本体**原样编译**、不 shim；它的「画」发生在**游戏 renderer** 里（走 D3D 还是 DDraw 属 §6-2 待核）。

## 必须先裁决的两件事
1. **自写 vs 外包**：外包（wined3d / DXVK + MoltenVK）本质是「用 Wine 系组件跑」= **第三条路线**（`§5.4`）⇒ 要单独立项、**不计入本线成本**（`§3.7` 把它记作「换路线」）。
2. **Live2D 是否在 ADV 主线**（`§6-12`）：若在，则「砍掉 D3D9」与「保留 Live2D」互斥；`§6-2` 未定之前不能承诺。

## 判据（轮廓，开工时写实）
- 若自写：设备状态机（渲染状态 / 纹理阶段 / 采样器 / 变换）可复现；雪 effect 能跑；`.x` 能载入 + 算帧 + 找骨骼。
- 若外包：外部依赖的许可与「换路线」裁决必须回写 `platform-effort.md`，并重算总量级（10–20 人月那张表的前提是「自写」）。

## 不做 / 待核
`§6-1`（DDraw vs D3D9 谁是 2D 主路径）与 `§6-2`（Live2D 后端）未定之前，本单只做**前置判定**，不做交付承诺。
