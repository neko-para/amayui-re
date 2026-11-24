# AOT · S7 覆盖面：间接 call 的 esp 清理字节（IAT 槽解析；004BBAB0/00412290 仍降级）

- id: REQ-01M86NMT0YJ9V78XS2DZZV364Q
- type: req
- status: done
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- done_reason: IAT 槽形态的 esp 清理字节已可证建模并测试：OFF 8/8 .ll 逐字节同基线；间接 call 首因 703→508、structurable 6411→6704、host-call 2967→3315；004B6130/0048DA20 转绿；lift-s7-iat 9 case + 2 变异实测红；ctest 141/141。默认 OFF：端到端 A/B 不可判定（机器漂移 ±50%，配对中位 +3.79%），结构量仅 +1.37% 站点。004B60C0/004E8C21/004BBAB0/00412290/004122E0 被寄存器间接 call（指向分析）挡住，负锁定。IAT 表保留作 shim 直呼的基础设施。
- tags: [emulator-lift, aot, s7]

## 结论（2026-11-24 收口：IAT 槽形态的机制已落，默认 OFF）

- **已落**：build 期 `IatModel`（`IAT 槽 EA → cleanup`）+ `lift_struct` 的 esp 记账 + `ret_cleanup` 沿尾跳 thunk
  传递；开关 `-DLIFT_S7_IAT` **缺省 OFF**。OFF 档 8 份 `.ll` 与基线 **8/8 逐字节相同**（split 清单逐位同）。
- **数字**（OFF → Step2 → Step3）：`indirect-call` 首因 **703 → 501 → 508**；`structurable`
  **6,411 → 6,685 → 6,704**；`host-call` 站点 **2,967 → 3,286 → 3,315**；`lift_dyn` 恒 5,312（发射器未改）。
- **变绿**：`004B6130` / `0048DA20` 由 Step 3 转 structured（before/after 均有 `[S7 downgrade]` 行）。
- **负锁定**（仍降级，且 reason 不是「间接 call」）：`004B60C0`（闭包 `004B6B60` 的 `call *%eax` @`4B6B9F`）、
  `004E8C21`（`004EE1EB` 的寄存器间接 call）、`004BBAB0` / `00412290` / `004122E0`。
  ⇒ 它们的最后一环是**寄存器间接 call（指向分析）**，IAT 表 + thunk 传递都解不了。
- **A/B**（12 趟 / 6 对，全程无并发）：配对中位 **+3.79%**、极差 −5.87…+15.92%；同臂漂移 ±50%、
  符号与臂序相关 ⇒ **不可判定（机器漂移）**，⛔ 不写「无收益」。结构量只 +1.37% 站点
  （D4 那轮 +5.5% 换 −9.4%）。
- **⇒ 默认保持 OFF**：收益低于本机分辨力，且 `take_control` 名单漏一条会静默误判（只影响派发密度）。
  IAT 表保留，作为「shim 直呼」（窗口 shim 调用对 ≈25–31% 派发）的基础设施。

## 判据（2026-11-24 第三次修正，按实测收口）

1. **正**：Step 3 后 `004B6130` / `0048DA20` 不在 `[S7 downgrade]`（`structurable` 相应增加）。
2. **数量**：`indirect-call` 703 → 508；`structurable` 6,411 → 6,704；`host-call` 2,967 → 3,315。
3. **负锁定**：`004B60C0` / `004E8C21` / `004BBAB0` / `00412290` / `004122E0` 仍降级，reason 不得是
   「间接 call」；`004B60C0` 的阻塞点已点名到 `004B6B60` 的 `call *%eax`（指向分析，不在本单）。
4. 五项指纹逐位同；`ctest` **141/141**；`lift-s7-iat`（纯档，9 case）+ **2 条变异 2/2 实测红**。
5. 读数：`iter` / `insns/iter` / `lift` ns/guest；⛔ 读数不进 test。

## 机制（Step 1–3）

- build 期 `lift-gen` 实例化 `ShimRegistry`（与 AOT 臂共用 `registerAotShims`），槽 EA =
  `image_base + PeImportInfo.iat_rva`，`cleanup = (conv==Stdcall ? argc*4 : 0)`（与 `shims.cpp` 同式）。
  ⛔ 不手写微软文档表、⛔ 不解析 `add(...)`；`[shim-meta] imports=310 modelled=290 unmodelled=20`。
- `lift_struct`：`modelled` ⇒ `espAdd(s.esp, cleanup)`；未知导入 / 表缺槽 / 寄存器 / 基址 / 段 /
  `fn==nullptr` / `take_control` ⇒ 仍 `kUnknown`。
- Step 3：`cleanup(C) = cleanup(owner(T))`（C 无 `ret` 且全部静态出口是同一条跨分量 jmp）；单调不动点，
  环 / 多出口 / 未解落 0。**发射器一行不改**。

## 测试

- `tests/s7_iat.cpp` + `tests/ir/s7_iat_roots.txt`（A=`0x418F80` / F=`0x4B8660` / G=`0x485F90` / G'=`0x47CC80`），
  顶层 `lift-s7-iat`（pure）：9 case（stdcall-1 记账 / 缺槽 / conv 写错 / 寄存器 / 绝对非槽 / `modelled=false` /
  发射形状 / 尾跳壳传递 / 形状回归）；生成期 ON `structurable=3 indirect-call=0` vs OFF `2/1`。
- 变异：清理字节恒 0（case 1/3/7 红）· 不沿 jmp 壳传递（case 8 红）⇒ 2/2 红。

## 未做

- **Step 0 上界实验**（设计 §8.3 的"假设 cdecl"一行）没做；本轮 A/B 是下界形态（只 208/501 绝对槽）。
- **运行期形态 A/B**（design §5.1 的 `lookups==1` 差分）没做。
- **指向分析**（`call *%reg`）—— 解开 `004B60C0` / `004E8C21` / `004122E0` / `004BBAB0` 的唯一剩下的前置。
- **shim 直呼**（IAT 表复用，25–31% 派发）—— 本单只保留表，未做直呼。
- `ShimDef::callback_capable`（`take_control` 名单的生成期化）仍 `<待核>`；274+19 个新单元未逐根复核；
  pricing 4 脚本未重跑；只有 win32 读数。
