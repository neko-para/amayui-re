# P2 · Fade(201) 真画：每 draw 带 shader/常量/2 纹理（MSL+DXBC 双源 + NaN 用例），并翻面 P1 的临时拒绝

- id: REQ-01M7MSGRRHCA23RAX1QGDAYJ3X
- type: req
- status: done
- parent: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- verify: apps/emulator-lift/tests/d3dx9_effect.cpp#d3dx9-effect/fade-pixels
- tags: [emulator-lift, d3d9, shader]

## 一句话

`technique Fade`（资源 201）**真的画出来了**：每个 effect draw 带自己的 shader（MSL + DXBC 双源）+ 常量 + 2 张纹理；
203/202 仍按 technique **具名拒绝**（P3/顶点着色）。

## 读数（2026-11-17 / mac arm64 / `links/raw` / prefix27）

- **pass 解析**：4 份文本的 pass 块只有 `PixelShader` / `VertexShader` 两条赋值 ⇒ 没有 D3D9 渲染状态要应用；
  未建模的名字 ⇒ `UnsupportedState` + 点名。`*pPasses` 从 P1 的结构性常数 1 改成解析结果。
- **像素 oracle**（真 D3D9 + 纹理 RT + 读回）：`Alpha = 0/0.5/1` ⇒ `(R255,A128)/(R255,G170,A191)/(R255,G255,A255)`，
  与 C++ 参考**最大误差 1 LSB/通道**。
- **NaN 档**（两图全透明 ⇒ `ColRate = 0/0`）：输出与未画前**逐字节相同**，先跑**控制行**证明这条 draw 会画
  ⇒ 结论「**不可观测**」（SRCALPHA/INVSRCALPHA 下 `ret.a=0` ⇒ 源因子 0），⛔ 不是定义 NaN（shader 无特判）。
- **翻面按 technique 分流**：Fade(201) ⇒ `recorded`（后端收到 program=`FadePs20` / Alpha / 2 纹理 / uv1）；
  203 SlideBlur、202 ⇒ `INVALIDCALL` + 具名位 `kUnEffectProgramNotExecuted` + 原因串（id/technique/pass）。
  E2E 里 `尚未实现执行` **0 条**（改前 1232）；`pending=0` / `unimplemented=0` / `comUnsupported=0`。
- 缺省档 vs 基线二进制：stdout 58/58、stderr 9/9 行，归一化后 0 差异（原始只有 `shim implemented 298→299 / gaps 12→11` + 抖动）。
- 变异：`--match '常量表'`、`--match '1-Alpha'` 各实测红。

## 顺手修的既有缺陷

`effectResourcePins()` 的 `kPins[]` 缺终止项 ⇒ `pins[4].id == 0` **一直越界读**；补终止项 + 在终止项 break。

## 未做

203/202 的 shader（P3 / 顶点着色）· DXBC 分支在 mac 上无运行时验证（同一语义的第二份源，逐句对齐；变异用带上下文的 `from` 防误命中）·
转场窗口的 hash 是 **8 帧周期循环**（不是单调灰阶推进），所以"多级灰阶"这条判据**不成立于该窗口**——它被撤到 P3/正文段去查。
