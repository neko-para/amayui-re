# emulator-lift · D3D9 DrawIndexedPrimitive：Base 用 int32 + 合法索引区间 [min,min+n)，越界拒绝不 clamp

- id: REQ-01M79PP9S4BFBPKDPBQBDN5GGR
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/indexed-range
- tags: [emulator-lift, d3d9, indexed]

## 缺口（索引第 5 行拆出）

`DrawIndexedPrimitive` 的 `BaseVertexIndex` 是 **INT**，今天按 `u32` 收：负 base 变成 2^32−x；
`MinVertexIndex` 被 `(void)min_index;` 丢掉，越界判据用 `vertex_index + 1u` —— 当 `vi == 0xFFFFFFFF` 时**回绕绕过检查**，
随后 `(size_t)vi * stride` 直接做指针加 ⇒ **越界读（可能崩）**。声明路还把顶点表建成 `[0, N)`，Base≠0 时映射错。
引擎两处都是 `0/0` ⇒ 今天没暴露。

## 判据

- `base_vertex` 改 `std::int32_t`（shim 处 `static_cast<int32_t>(call.u32(2))`，头文件同步）。
- 合法**索引值**区间 `[min, min+n)`；流内顶点号 = `base + idx`（int64 全程，先判区间再碰指针）；越界 ⇒ `D3DERR_INVALIDCALL`，⛔ 不 clamp。
- 声明路用 `first = base+min` 建、`idx-min` 取；UP 变体同样按 `[min, min+n)` 校验（UP 无 Base）。
- 守卫 `d3d9p/indexed-range`（`tests/d3d9_pipeline.cpp`）四条：
  (a) `base=-2,min=2,n=3,IB{2,3,4}`、(b) `base=-1,min=1,n=3,IB{1,2,3}` ⇒ S_OK + 像素 = 顶点 0/1/2 的颜色（修前假 INVALIDCALL）；
  (c) `base=0,min=2,n=4,IB{0,1,3}` ⇒ INVALIDCALL + 像素不变（修前静默画错）；(d) `base=-1,min=0,n=1,IB{0,1,2}` ⇒ INVALIDCALL（`vi=-1`；修前是越界读）。
- 变异：`idx+base` 改回忽略 min ⇒ (a)(b) 红；删 `idx<min` ⇒ (c) 红。

## 风险

引擎 0/0 ⇒ TITLE / 存档 / 实时零影响；别处若有负 Base，修前是假拒或越界读，修后是明确 INVALIDCALL（更好）。
