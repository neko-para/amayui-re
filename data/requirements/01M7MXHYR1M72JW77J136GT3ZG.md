# P4 · 取消 psver opt-in：caps 固定报 2.0/1.0 + 删 psver 档位测试 + 'required == implemented' 硬等式守卫（缺一个启动 Trap）

- id: REQ-01M7MXHYR1M72JW77J136GT3ZG
- type: req
- status: open
- parent: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- tags: [emulator-lift, d3d9, caps]

## 一句话

**取消 `LIFT_D3D9_PS_VERSION` 这个开发开关**，`fillCaps` 按**真实实现能力**报：

- `PixelShaderVersion = 0xFFFF0200`（ps_2_0）
- `VertexShaderVersion = 0xFFFE0100`（vs_1_0）
- `MaxPixelShaderValue = 8.0f`
- `MaxVertexShaderConst = 96`

并删掉所有 psver 档位的测试面。⛔ 不再有"报低一档 / 登记已知偏离 / declared_gap"这类回退：
不支持就 `comUnsupported` / Trap **响亮停**，⛔ 不许静默降级或继续走错路。

## 为什么

- 用户实测：`PS=0x200` 下转场与真机一致（除 203 的 effect 层未实现）；缺省档（caps=0）会让引擎走软件模糊、
  转场长达 40+ s 黑屏 ⇒ caps=0 不是产品形态。
- **VS 必须报 1.0**（用户口径 2026-11-18「按 vs=1 汇报，防止错误」）：202 的 FX 是 `vs_1_0 + ps_1_0`；
  报 `VS=0` 会让引擎对 202 的 VS pass 走错分支，比"缺能力"更糟。VS=1.0 的**兑现者** = CPU `vs_1_0` 程序
  （A 单 `REQ-01M7NAW2QT3TG9RCFMKR052C83`）；
  设备 vtable 的 `Create/SetVertexShader` 槽仍未实现 ⇒ 真被调用照旧 `comUnsupported` 响停。
- 与 ADR（`REQ-01M7MNP9K8T81SP2HNXQ9TTXPG`：真正的 GPU 渲染）一致：caps 报的值 = 实现能力。

## 要做

1. 删 `LIFT_D3D9_PS_VERSION` 的 env 解析 / 值域校验 / readout（`runtime/d3d9.{h,cpp}`）；`fillCaps` 固定报上面四个值。
2. 删/改测试面：
   - `d3dx9-effect/caps-ps-version` 里针对 `0/0x100/0x300` 的档位断言 ⇒ 改成"固定报 2.0 / 1.0"；
   - 任何"不给 env 时逐字节不变"的判据 ⇒ 删除；
   - `--audio off` 那类与 caps 无关的缺省输出判据**保留**。
3. **新守卫 = 硬等式 `required == implemented`**（⛔ 没有 gap / 已知偏离这一项）：caps 报 2.0/1.0 ⇒ 引擎会建的
   effect 程序（等级 1/2 的 200/201/202/203，以及 202 的 `SnowVS`）**必须全部在实现集里**；少一个 ⇒ **启动即 Trap**
   （不是运行到那一帧才停，也不是 warn）。
4. **运行期闸**：effect draw 命中"未实现的 program" ⇒ **响亮 `pipelineTrap()`**；⛔ 不许 warn + 画 0 像素 + 打 bit 44
   继续跑（P1 的临时形态必须随本单消失）。
5. 启动 readout：一行打出"caps 2.0/1.0 ⇒ 会建 200/201/202/203 + 各自 program"，让"哪些 effect 会被用"默认可见。

## 判据

- `ctest` 全绿；`grep -r LIFT_D3D9_PS_VERSION` 在 `apps/` 与 `tests/` 零命中。
- 守卫变异实测红：删 202 binding / 把 `SnowVS` 从实现集拿掉 ⇒ **启动 Trap**（不是运行期才红）。
- E2E（不给任何 env）：转场与真机一致，`尚未实现执行` 0 条；已实现的 program 不许 Trap（负控）。

## 前置 / 未做

- 前置：P3（203）+ **A 单（`REQ-01M7NAW2QT3TG9RCFMKR052C83`：202 的 `vs_1_0` / `ps_1_0` 实现）**。
  ⛔ "把 202 登记为已知缺失"**不再**是 P4 的合法前置替代。
- 本单不实现任何 shader 程序（202 在 A 单，203 在 P3）。
