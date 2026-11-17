# B · GPU 批的透视校正插值（逐顶点 1/w + uv/w、color/w）—— 202/透视批真正入 GPU

- id: REQ-01M7NC21S484PFYDAE7YTNX02J
- type: req
- status: done
- parent: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- verify: apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/perspective-interpolation
- tags: [emulator-lift, d3d9, gpu]

## 一句话

拆掉 `submitPrimitives` 对 `invw != 1` 的**整批拒绝**（位 21）：GPU 屏空间三角形改为携带**逐顶点 `1/w`**，
在片元里对 `uv/w`、`color/w` 做插值后再除回，使 `clip.w ≠ 1` 的批（202 的透视 `Proj`）能**正确入队并画对**。

## 为什么

- N3 已证：202 的 `Proj` 是 `D3DXMatrixPerspectiveFovLH` 的输出，顶点是视图空间 ⇒ `clip.w = z_view ≠ 1`。
- 修前 `submitPrimitives` 的 `gpu_invw_ok` 对任何一批只要某顶点 `invw != 1` ⇒ `recordUnsupported(kUnGpuPerspective)`
  + `pipeWarn` + `D3DERR_INVALIDCALL`。A 单能把 202 的 `SetMatrix/SetVector`、`recorded`、CPU `vs_1_0`、`ps_1_0`、
  binding、像素 oracle 全部做实，**但真 run 里雪批仍被这条闸整批拒**。
- 用户口径：正确提供 feature，⛔ 不是告诉引擎"不支持"，⛔ 不是给 202 开特判。

## 要做

1. 顶点属性带 `1/w`（或 `w`）；MSL + DXBC 双源在片元里做透视校正：
   `uv = (Σ uv_i/w_i) / (Σ 1/w_i)`、`color` 同理；深度/zw 按 D3D 规则。
2. **固定功能路径不变**：`w == 1` 的批逐像素/逐字节与今天相同（既有 `fade-pixels` / `blur-pixels` / `d3d9p/*` 全绿）。
3. 去掉"凡 w≠1 就整批拒"；真正不可表示的情形（`w <= 0` / 非有限）要**具名**，⛔ 不许保留笼统拒绝。
4. 测试：透视校正 vs 仿射的像素 oracle（同一四边形、两个深度不同的三角形、纹理坐标梯度已知）——
   参考实现按 `uv/w` 算；仿射实现必须**差出阈值**（控制行证明不是恒真）。
5. 202 的 run 级：雪批不再命中位 21，`skipped_draws` 不增。
   ★ **雪景的真 run 可达性（N1）归另单**（`REQ-01M7NNNMPFB1GDB52J662Y1CMZ`）：本单不负责找配方，也⛔ 不把"没走到"写成"雪已出现"。
6. 变异：漏乘 `1/w` · 用仿射代替透视 ⇒ 红（`--match` 单条）。

## 判据

- 一条 GPU 用例：透视四边形上 `uv` 采样与参考逐像素 ≤1 LSB；同用例的仿射版差 > 阈值。
- 固定功能既有像素用例全绿；`ctest` 全绿；两条变异实测红。
- 202 真 run（**能走到时**）：位 21 `kUnGpuVertexW` 0 次、`skipped_draws` 不增。
  ★ 走到 202 的配方归 N1；⛔ 本单不拿"没走到"当"雪已出现"。

## 前置 / 未做

- 前置：**A 单**（202 的 shim 状态 / `vs_1_0` / `ps_1_0` / binding）先合入；两边都改
  `d3d9_pipeline.cpp` / `d3d9_gpu.cpp` ⇒ 串行，⛔ 不并行改同一批文件。
- 本单只做 GPU 插值面，⛔ 不改 202 的 shim / binding / CPU VS。
- 未做 / 待核：`rhw != 1` 的 XYZRHW 线性语义（若真机是线性，改 `toScreen` 一处 + 判据）；MSL 分支本机无运行时读数
  （win32 只编 DXBC，两份源靠逐句同步）；202 的真 run 可达性 → N1 单。
