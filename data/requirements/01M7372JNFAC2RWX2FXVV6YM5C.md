# emulator-lift · D3D9 点/线图元假成功：DrawPrimitive(UP) 返回 S_OK 却一个像素都不写

- id: REQ-01M7372JNFAC2RWX2FXVV6YM5C
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/d3d9.cpp#d3d9/linelist-writes-pixels
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
`apps/emulator-lift/tests/d3d9.cpp#d3d9/linelist-writes-pixels`：
`SetFVF(XYZ|DIFFUSE)` + `DrawPrimitiveUP(LINELIST, 1, 两顶点)`，断言后缓冲上出现 ≥L 个非背景像素；
把 line 分支短路回 `break` ⇒ 红。

## 未做
- 点尺寸（`D3DRS_POINTSIZE`）与抗锯齿线（`D3DRS_ANTIALIASEDLINEENABLE`）不在本单。
- 线在 GPU 后端仍回退 CPU（`gpuEligible` 不改）。
