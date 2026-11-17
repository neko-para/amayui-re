# SC0000 渲染接口：D3DX surface 拷贝族（分步补，第 1 步已落地）

- id: REQ-01M7JZKNDXZF5FM2WXTAQ319KG
- type: req
- status: doing
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- verify: apps/emulator-lift/tests/d3dx9_load_surface.cpp#d3dx9/load-surface+rect-semantics
- tags: [emulator-lift, d3dx9, sc0000]

## 一句话

把 SC0000（开局脚本，配方的第一个屏）路上**缺的渲染接口**逐个补齐，每一步一个可挂守卫的单元。
第 1 步已落地：`D3DXLoadSurfaceFromSurface` 的整条族（矩形语义 / 1:1 / 色键 / 通道序）。

## 为什么单独立这一节点

`SC0000.BIN` 是配方第一个要过的屏，而它的渲染路径会撞上**没有登记的 D3DX 导出**（`register_import_gaps.cpp` 里的
base gap 桩）⇒ guest 视角就是"这个 API 不存在"，停机原因是 `Unimplemented`，配方只能在那之前停住。
评估与路线见 `.tmp/sc0000/assessment.md`（只读普查，父级已裁决）。

## 第 1 步 · D3DX surface 拷贝族（2026-11-17 / dev `82bcb84` / 合并 `b7c00c5`）

落点：`src/runtime/d3d9.{h,cpp}` 的 `surfaceCopyPlan`（**唯一一份**格式判据）+ `copySurfaceRect`；
`src/shims/d3dx9.cpp` 的 `shimD3DXLoadSurfaceFromSurface`（8 参 stdcall）+ 登记；缺口行随 `pnpm tools lift sync --write` 消失。

判据（MS 文档口径，⛔ 不靠"观察到引擎这么用"）：

1. `rect == NULL` ⇒ **整幅**（⛔ 不是左上角）；矩形**半开** `[lo, hi)`。
2. ⛔ **不隐式裁剪**：源右/下越界、负坐标、空宽高、目标越界、rect 指针不可读 ⇒ 全 `D3DERR_INVALIDCALL` + 目标一字节不变。
3. **只做 1:1**：源/目标尺寸不等 ⇒ 响亮停（⛔ 不按左上角硬拷）。
4. **不支持 colorKey**：非 0 ⇒ 响亮停。
5. **只允许 RGBA↔BGRA 通道交换**：同序 32bpp / 同格式 ⇒ 恒等；跨序 ⇒ `{2,1,0,3}`；16↔32、不同 16bpp、未知、D24S8 ⇒ 拒绝。

守卫：顶层 `add_test(NAME d3dx9-load-surface)`（pure）；5 段 `g_current_case` =
`d3dx9/load-surface+{rect-semantics,contract,format-pairs,one-to-one,color-key}`。
**改动前实测红**（只退回登记与缺口行 = guest 视角仍是桩）：`67 checks, 94 failures`，现场
`=== unimplemented import (base gap) === d3dx9_43!D3DXLoadSurfaceFromSurface` + 逐像素 `dst(2,2)=0xDEADBEEF, expected 0xA1000102`。
变异 3 条（半开当闭区间 / 1:1 判据失效 / 跨序不换 R/B），`--list` 全绿、抽检 1 条实测红。

## 裁决（2026-11-17）

1. **同序 32bpp（`21↔22`、`32↔33`）按恒等直拷**，不是"只赞同 D3DFORMAT"。理由：两者**布局逐字节相同**，
   差的只是"顶字节算不算 alpha"，而设计已定"X8 的顶字节不属于颜色 ⇒ 照抄不改" ⇒ 这是**拷贝，不是转换**。
   ⛔ 但**没有**为任何组合猜转换语义；将来若引擎真要转格式，另开单。
2. **`kFmtA8B8G8R8=32` / `kFmtX8B8G8R8=33` 只作 `surfaceCopyPlan` 的判据值，不开创建路径**：
   `d3d9_gpu.cpp` 规定 32bpp 内存布局必须 BGRA（纹理上传按 B8G8R8A8 逐字节搬）⇒ 放行 32/33 会让被采样纹理
   **静默红蓝互换**。代价：RGBA↔BGRA 这条臂目前**没有可达的 surface**（只由判据表逐对断言，端到端由同格式 32/16bpp 覆盖）。
3. 评测顺序按 `assessment.md`；第 2/3 步不在这张单的第 1 步里。

## 还没做

- **第 2/3 步**（`assessment.md` 的后续缺口）：未开工；每一步要独立判据 + 守卫。
- **`--import-gaps` 一次性读数**（把剩余缺口清单打成读数）：不在第 1 步判据面，属第 2/3 步的前置工具，单列。
- 32/33 的**创建路径 + GPU 上传通道转换**：与裁决 2 绑在一起，真要开就得一起做（另开单）。
- 跑真游戏到 SC0000 第 0 屏：属验收，不是本单第 1 步。

## 判据

1. 每条接口的语义按**微软文档**，⛔ 不按观察；文档没写的写 `<待核>`，不许猜。
2. 每条新增接口必须有一条**顶层** CTest（pure 档，§12.4）+ 至少一条**能红**的变异条目。
3. ⛔ 不改 guest 可见语义的既有部分：`exit` / `frames` / `stop @` / arena 三项 / `table misses` 逐位不变。

## 第 2/3 步评估（2026-11-17 / macOS 只读）

- 完整：`.tmp/sc0000/assessment-mac.md`（390 行）；脚本级落点 `docs/03-emulator/scripts/SC0000.md` 的「第 2/3 步评估」。
- 第 1 步之后最可能仍停 `D3DXLoadSurfaceFromSurface` 本身（5 站点 `ColorKey=0` / `Filter∈{1,-1}` ⇒ 只剩缩放或 16↔32/不同 16bpp 对）；`0x4B4D6E` 可静态排除（先撞 `+0x70 CreateRenderTarget`）。
- `D3DXCreateEffectFromResourceA` 被 `PixelShaderVersion` 门控（`Engine+46668=0`）⇒ 当前不可达；补 caps 会点亮 FX 执行，另单。
- `i238` 是 AGE 脚本 opcode（handler `sub_4248C0`），不是 lift 缺口。
- 第 2 步先决 = 一次读数 run（`LIFT_AOT_SHIM_TRACE=1`；`--import-gaps` 不存在）；本机 `links/install` 是另一修订版，未到达 SC0000 ⇒ 等原版树/可对齐读数。
- A-1/A-2 与第 1 步「只做 1:1 / 不猜转换」冲突；未实现，等裁决。

## 第 3 轮（2026-11-17）：确认框已过，SC0000 到达

- `.tmp/sc0000/load24.plan`（三次回调形状）`rc=0` · `steps=12 fired=4 pending=0` · `script=SC0000.BIN`；
  根因/读数见 `docs/03-emulator/scripts/SC0000.md`「第 3 轮」。
- 到 SC0000 后 3371 帧、`pos` 到 `0x6B28`/`0xC68`，**无 `unimplemented` 停点** ⇒ headless 没撞
  `D3DXLoadSurfaceFromSurface`，与 win32 `frames=523` 不同形（未定）。
- 下一步：挂 `await position SC0000 0x6AB4 0x6B90` 推进到下一屏，等停点再选单元（⛔ 不猜）。
