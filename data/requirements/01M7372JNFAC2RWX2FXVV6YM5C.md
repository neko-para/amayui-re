# emulator-lift · D3D9 点/线图元假成功：DrawPrimitive(UP) 返回 S_OK 却一个像素都不写

- id: REQ-01M7372JNFAC2RWX2FXVV6YM5C
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/lines-points
- repro: apps/emulator-lift/src/runtime/d3d9_pipeline.cpp#submitPrimitives
- severity: S3
- tags: [emulator-lift d3d9 raster]

## 一句话
`submitPrimitives()` 对 `POINTLIST / LINELIST / LINESTRIP` 只 `recordUnsupported(bit16)` 然后 `break`：
**不写任何像素、也不报错，Draw 仍返回 `S_OK`**（`runtime/d3d9_pipeline.cpp:3460-3464`）。
而引擎有两条实测站点画线（`0x48E16D` = LINELIST 1 段、`0x48E1F5` = LINESTRIP 16 段），
且 `GetDeviceCaps` 的 `LineCaps=0x1F` **主动宣称支持线**（`runtime/d3d9.cpp:483`）。

## 已核（EA）
- `0x48E16D`：`[+0x14C]` DrawPrimitiveUP，`push 1Ch / data / 1 / 2` ⇒ `D3DPT_LINELIST`、1 个图元、stride 0x1C。
- `0x48E1F5`：`[+0x14C]`，`push 1Ch / data / 10h / 3` ⇒ `D3DPT_LINESTRIP`、16 段。
- 同函数 `0x48E0A9` 先 `SetFVF(0x42)` ⇒ 走 FVF 路，不依赖顶点声明。

## 修法（最小）
1. `submitPrimitives` 里加线/点光栅：线宽 1px、按像素中心半开区间逐像素写（diamond-exit 可后续再对）；
   点先按 1px 落（`D3DRS_POINTSIZE` 的方框另开单），不猜大尺寸语义。
2. `LineCaps` 与"线真的会画"同真同假（做不到就把 caps 位撤掉，⛔ 不许继续宣称）。

## 守卫（修前必须红）
`apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/lines-points`（真 GPU 臂）：后缓冲上线的非背景像素
`>= 8`、点那一格非背景，且 `fallback_draws == 0` / `queued >= 1`；把线/点分支短路回 `break`
（或让展开的 quad 退化）⇒ 红。另见 `apps/emulator-lift/tests/d3d9.cpp#d3d9/linestrip-16`
（`--gpu` 臂：16 段阶梯真的落成像素）。

★ 2026-11-14（`REQ-01M7A74YG08R65NJBFXF7DEJDK`）：CPU 光栅整族删除后，原来那条
`d3d9/linelist-writes-pixels` 的逐像素断言（49 px / t=0.25 插值 / x=7,57 列归属）**不再是本线的合同**
—— 点/线在 GPU 上展开成 1px quad，端点吸附到像素中心格（最多偏 0.5px），⛔ 不承诺 diamond-exit
的逐像素覆盖。那条用例与 `d3d9/line-axes-and-direction` 一并删除（"线真的会画"由上面的 GPU 臂守）。

## 未做
- 点尺寸（`D3DRS_POINTSIZE`）与抗锯齿线（`D3DRS_ANTIALIASEDLINEENABLE`）不在本单。
- ~~线在 GPU 后端仍回退 CPU（`gpuEligible` 不改）~~ —— 已过时：CPU 光栅已删，准入不足的批
  一律"记账 + 跳过"（`D3DERR_INVALIDCALL`），⛔ 没有回退。
