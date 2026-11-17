# P4 · 取消 psver opt-in：caps 固定报 2.0 + 删掉 psver=0 的测试面 + 'caps 报 2.0 ⇒ 会被用到的 effect 程序必须都实现' 守卫

- id: REQ-01M7MXHYR1M72JW77J136GT3ZG
- type: req
- status: open
- parent: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- tags: [emulator-lift, d3d9, caps]

## 一句话

**取消 `LIFT_D3D9_PS_VERSION` 这个开发开关**，把 `D3DCAPS9.PixelShaderVersion` 固定报**真实支持能力**（目标 `0xFFFF0200`），
并把所有"psver=0 时怎样 / 改了之后 0x200 仍怎样"的测试与断言**删掉** —— 用户口径（2026-11-17）：
**不再考虑 `PixelShaderVersion = 0` 的配置**。

## 为什么

- 用户实测：`PS=0x200` 下转场与真机一致（除 203 的 effect 层未实现）；缺省档（caps=0）会让引擎走软件模糊、
  转场长达 40+ s 的黑屏 ⇒ **caps=0 这条配置不是产品形态**，没有必要为它维持"逐字节不变"的测试面。
- 这也和 ADR（`REQ-01M7MNP9K8T81SP2HNXQ9TTXPG`：目标是真正的 GPU 渲染）一致：caps 报的值应当**等于实现能力**。

## 要做

1. 删 `LIFT_D3D9_PS_VERSION` 的 env 解析 / 值域校验 / readout（`runtime/d3d9.{h,cpp}` 里那一块）；
   `fillCaps` 直接把 PS 报 `0xFFFF0200`（`MaxPixelShaderValue` 等自洽值一起）。
2. 删/改测试面：
   - `d3dx9-effect/caps-ps-version` 里针对 `0/0x100/0x300` 的档位断言 ⇒ 改成"固定报 2.0"一条；
   - 任何"不给 env 时逐字节不变"的判据（P1–P3 报告里的那一类）⇒ 删除；
   - `--audio off` 那类与 caps 无关的缺省输出判据**保留**（那是另一件事）。
3. **新守卫（替代旧的"声明⇔实现"）**：caps 报 `PS=0xFFFF0200` ⇒ 引擎会建的 effect 程序（等级 1/2 会用到 200/201/202/203）
   **必须全部在实现集里**；少一个 ⇒ 启动即红（不是运行到那一帧才停）。
   ★ 这条会在 202（Snow，`vs_1_0`）未实现时**直接红** ⇒ 所以本单的前置是"202 也有结论"（实现，或明确裁决它缺失并登记成已知偏离）。
4. 启动 readout：把"caps 报 2.0 ⇒ 会建 200/201/202/203"打成一行（一次性），让"哪些 effect 会被用"在默认日志里可见。

## 判据

- `ctest` 全绿；`grep -r LIFT_D3D9_PS_VERSION` 在 `apps/` 与 `tests/` 零命中。
- 一条守卫：caps 报 2.0 且实现集缺任一会被用到的 effect 程序 ⇒ 红（变异实测）。
- E2E（不给任何 env）：转场与真机一致（用户口径），`尚未实现执行` 0 条。

## 前置 / 未做

- 前置：P3（203）落地 + 202 的裁决（实现 vs 登记缺失）。
- ⛔ 不在本单实现 202 的 shader（若裁决为"实现"，另开单）。
