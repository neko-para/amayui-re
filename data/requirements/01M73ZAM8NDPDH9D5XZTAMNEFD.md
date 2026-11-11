# emulator-lift · CheckDeviceFormat 恒 S_OK：假支持会把

- id: REQ-01M73ZAM8NDPDH9D5XZTAMNEFD
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/d3d9.cpp#d3d9/checkdeviceformat-truthful
- repro: apps/emulator-lift/src/shims/d3d9.cpp#shimCheckDeviceFormat
- severity: S3
- tags: [emulator-lift d3d9 caps]

## 一句话
`IDirect3D9::CheckDeviceFormat` **恒返回 `S_OK`**（`shims/d3d9.cpp` 的 `shimCheckDeviceFormat`），只把参数记进环形诊断。
引擎 boot **真的会问一次**（`0x49918D`，实参 `Adapter=0 · RType=3(TEXTURE) · Usage=0 · CheckFormat=0x16(22=X8R8G8B8)`，
结果经 `setns` 存进 `[esi+464h]`）——这一次恰好答对；换成 `DXT1` / `D24S8` / `USAGE_RENDERTARGET` 就会**假支持**。

## 为什么是缺陷
假支持 ⇒ 引擎"问过就建" ⇒ 走到创建失败；而本线对不支持的格式/`MipLevels≠1` 是 **trap 停机器**
⇒ 停因会被记成"我们没实现"，而不是"能力不匹配"。这是把"能力问题"伪装成"实现缺口"。

## 修法
1. 用**同一份支持集**回答（⛔ 不另写一张表）：纹理侧 = `runtime/d3d9.cpp` 的 `formatForD3dFormat` / 那份 6 格式列表；深度侧 = 本线真的建的那个。
2. 规则：`RType=3` 只接受 `Usage ∈ {0, RENDERTARGET}`，且 `RENDERTARGET` 只接受 32bpp；
   `RType=1` 只接受 `Usage=DEPTHSTENCIL` 且格式是深度格式；其它一律不支持。
3. 不支持 ⇒ `D3DERR_NOTAVAILABLE`；**未知格式一律不支持**（⛔ 不放行未知）。
4. 保留既有 `recordCheckFormatQuery` 诊断。

## 守卫（修前必须红）
`apps/emulator-lift/tests/d3d9.cpp#d3d9/checkdeviceformat-truthful`：
`(3,0,22) ⇒ S_OK`（**引擎那次真实查询必须仍通过**）· `(3,0,DXT1) ⇒ 非 S_OK` ·
`(3,RENDERTARGET,22) ⇒ S_OK` · `(3,RENDERTARGET,DXT1) ⇒ 非 S_OK` · `(1,DEPTHSTENCIL,D24S8) ⇒ S_OK` · `(3,0,D24S8) ⇒ 非 S_OK`。
把实现改回恒 `S_OK` ⇒ 至少 3 条红；变异条目实测。

## 交付（2026-11-11）
- 实现：`runtime/d3d9.cpp` 的 `checkDeviceFormat`，**唯一支持集** = `formatForD3dFormat` 的 6 格式（不另写表）。
  `TEXTURE` 只收 `Usage ∈ {0, RENDERTARGET}`（RT 限 32bpp）；`SURFACE` 仅 `DEPTHSTENCIL + D24S8`（`createForDevice` 建的是 32bpp 深度面，D16 不建）；
  其它 `RType`/`Usage`/未知格式 ⇒ `kNotAvailable(0x8876086A)`；`recordCheckFormatQuery` 诊断保留（答 no 也记）。
- 守卫 `d3d9/checkdeviceformat-truthful`：修前 **13 failures**，修后 `d3d9: 1056 checks, 0 failures`。变异 `--match checkdeviceformat` **1/1 红**。
- 门禁：全量 ctest **79/79** · `pnpm test` 546/0 · validate 6/6 · `diff --check` 干净。
- 顺带修正：既有 `caseCreateDeviceAndCaps` 的 #10 实参从 `(RType=1, Usage=1)` 改成 `(3,0)`——旧组合在新判据下本就该答 no。
- 勘误：本单初稿把 `D24S8` 写成 `0x5A`（=90）是笔误；代码取枚举值 **75**（正确）。

## 未做
- **两侧不对称**：`createTextureState` 还没加"`RENDERTARGET` 必须 32bpp"的拒绝（查询侧已答 no，创建侧仍可能接受）。
- 不实现其它 `RType`（vertexbuffer / indexbuffer 等）。
- 不改 `recordCheckFormatQuery` 的记录格式；未校验 `Adapter`/`DeviceType`/`AdapterFormat`；未跑真机 boot 验证后续查询。
