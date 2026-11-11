# emulator-lift · D3DXCreateTextureFromFileInMemory 的 PNG 解码 + 上传（Live2D 图集页全 0 的根因）

- id: REQ-01M738VYG0DP3TNJKYDSA49J7H
- type: req
- status: done
- parent: REQ-01M735AWXRDZKCDT4124BCFF27
- verify: apps/emulator-lift/tests/d3dx9.cpp#d3dx9/texture-from-memory-png
- tags: [emulator-lift d3dx9 png texture]

## 一句话
把 `D3DXCreateTextureFromFileInMemory` 从"**只读文件头**、像素全 0"改成**真解码 + 上传**。
今天引擎的 Live2D 图集页（`0x478398`，三张 2048² PNG）就是这样被建出来、又永远是全 0 的 ⇒
所有采样它们的固定功能批次都不可见（父单 `REQ-01M735AWXRDZKCDT4124BCFF27` 的根因）。

## 已核
- stub 落点：`shims/d3dx9.cpp:884-890`（头注自注"纹理内容解码是下一层"）+ `:1018-1097`（只从 PNG IHDR 取宽高、建纹理）。
- 引擎调用点：`0x478398`（Live2D，3 页 2048²）；其它调用点用 `--trace-shims` 一次列全。
- **libpng 已在 vcpkg 依赖树里**：`apps/emulator-lift/out/vcpkg_installed/x64-windows/include/png.h`
  ⇒ ⛔ 不必自写 inflate；但要在 `CMakeLists.txt` 里**显式** `find_package(PNG REQUIRED)` + 链接
  （现在能编过是因为 freetype 的传递依赖，那是幽灵依赖）。

## 修法
1. `find_package(PNG REQUIRED)` + `PNG::PNG` 链接；解码落新文件（如 `src/runtime/png_decode.cpp`），
   用 libpng 的 `png_image` 简化 API 解成 **RGBA8**。
2. 像素写进纹理 arena **走既有路径**（`d3d9::lockRect` / `unlockRect`，与 native upload 同一份语义）；
   ⛔ 不要另造一条像素写路径（否则版本/脏区/GPU 上传缓存全部要重做一遍）。
3. 失败面：坏 PNG / 不支持的色型 ⇒ `E_FAIL`（**可继续**），⛔ 不 trap（资源耗尽仍按 `BUG-10` 的既有口径）。
4. 纹理格式仍只用现有接受集，PNG 一律 `A8R8G8B8`；`MipLevels` 保持 1（真机是 D3DX_DEFAULT，另记）。

## 守卫（修前必须红）
- **pure**：内嵌一张 8×8 RGBA PNG（带 α 梯度）⇒ 解码后逐像素等于期望（四角 + 中心），⛔ 不写"当前取值"断言。
- **pure**：截断 / 错 magic ⇒ `E_FAIL` 且**不 trap**（用 `stop_why` 断言机器没停）。
- **e2e**：同一 repro 下 `[draw]` 的 21/22/23 行 `texa` mean>0 **且** 覆盖区 RGB 不全黑（父单判据）。

## 未做
- BMP / DDS 的解码（它们现在同样只读头）。
- `MipLevels`（真机 D3DX_DEFAULT）；`D3DXLoadSurfaceFromSurface` 仍未实现（与本单无直接关系）。
- Live2D 绘制侧（`0x4AF459 → 0x49E37B`）的语义核对。
