# M2 P4 · GDI region 7 条 + GetUpdateRgn/ValidateRgn

- id: REQ-01M6KSW0VWHTZ18M05WNW4CDT9
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/user32_window.cpp#region-data-roundtrip
- tags: [emulator-lift, m2]

## 一句话

把 GDI32 的 **region 七条**（`CreateRectRgn` / `CreateEllipticRgnIndirect` / `CreatePolygonRgn` / `CombineRgn` /
`GetRegionData` / `GetRgnBox` / `PtInRegion`）与 USER32 的 **`GetUpdateRgn` / `ValidateRgn`** 从显式 stub
变成真语义：区域是**真数据类型**（规范化矩形表 + 椭圆 / 多边形扫描线 + 布尔运算），句柄走 `HandleTable`，
窗口的无效区（update region）与 `InvalidateRect` / `ValidateRect` / `UpdateWindow` 共用同一份状态。

## 落点（`repo:` 相对仓库根）

- `repo:apps/emulator-lift/src/runtime/region.h` · `repo:apps/emulator-lift/src/runtime/region.cpp`
  —— `GdiRegion`：矩形表（互不重叠、右/下不含、能合并已合并）+ 椭圆 / 多边形光栅化 + AND/OR/XOR/DIFF/COPY +
  `RGNDATA` 写出与解析。**纯计算**，不碰 guest 内存 / 句柄 / 日志。
- `repo:apps/emulator-lift/src/shims/gdi32_region.cpp` —— 七条 shim（句柄、Win32 返回值、日志）。
- `repo:apps/emulator-lift/src/shims/user32_window.cpp` —— `GetUpdateRgn` / `ValidateRgn`；`InvalidateRect` /
  `ValidateRect` 改成维护真无效区。
- `repo:apps/emulator-lift/src/runtime/host_window.h` —— `HostWindow::update_region`（客户区坐标）+ 四个
  `WindowManager` 方法；`dirty` 保留为 `!update_region.empty()` 的**布尔镜像**。
- `HandleKind::GdiRegion` + `DeleteObject` 认 region（`repo:apps/emulator-lift/src/runtime/handles.h` ·
  `repo:apps/emulator-lift/src/shims/gdi32_object.cpp`）。
- 派生文件由 `node tools/lift.mjs --sync --write` 重生（`register_import_gaps.cpp` 少了这 7 条）。

## 判据（每条都有会红的守卫）

1. 矩形 `CreateRectRgn(0,0,10,10)`：内部 / 最后一格 `(9,9)` 在内，**`(10,5)` 与 `(5,10)` 不在**（右/下不含），
   `GetRgnBox` = SIMPLEREGION + `(0,0,10,10)`。
2. **空矩形**（`x1==x2`）是**合法区域**：`CreateRectRgn(5,5,5,9)` 返回**非 0 句柄**，`GetRgnBox` = NULLREGION +
   `(0,0,0,0)`，`PtInRegion` 永远 0 —— ⛔ 不是"没创建"。
3. 椭圆 `(0,0)-(20,20)`：圆心在内、包围盒角不在、`GetRgnBox` 撑满包围盒、类型 = COMPLEXREGION。
4. 多边形（L 形 6 点，ALTERNATE 奇偶）：横条 / 竖条内的点在内、缺口 `(7,7)` 不在、包围盒正确；
   **WINDING 返回 NULL + 日志**（未实现形态响亮失败，⛔ 不拿 ALTERNATE 冒充）。
5. `CombineRgn` 五种模式各一条用例：A=(0,0,10,10) · B=(5,0,15,10)；
   AND=`[5,10)`、OR=`[0,15)`、**DIFF=`[0,5)`（A−B）**、COPY=整体替换目标、XOR=对称差（两块 ⇒ COMPLEXREGION）。
6. `GetRegionData`：`nCount=0` / 缓冲不足 ⇒ 返回**所需字节数**（48 / 64），够 ⇒ 写 `RGNDATA`
   （`dwSize` / `iType=RDH_RECTANGLES` / `rcBound` / RECT 数组）+ 返回非零；解析回来与**原区域逐矩形相等**
   （多边形那块用 `operator==` 整表比对）；坏头（`iType=0`）必须被拒绝。
7. 无效区：`InvalidateRect(hwnd,(10,20,30,40))` ⇒ `GetUpdateRgn` = SIMPLEREGION 且包围盒**完全相同**；
   `ValidateRgn` 之后 ⇒ NULLREGION + `(0,0,0,0)`；`InvalidateRect(hwnd,NULL)` ⇒ 整客户区 1280×720；
   `ValidateRect(hwnd,NULL)` ⇒ 空。
8. `DeleteObject(region)` 返回 TRUE、句柄项消失、之后 `PtInRegion` 按无效句柄返回 FALSE。

## 变异验证（实测"改坏就红"）

- `region.cpp` 的 `RegionCombine::Diff` 改成 `andIntervals` ⇒ **`combine-diff` 红**
  （`R_DIFF_IN` 1→0、`R_DIFF_OUT` 0→1），连带 `update-region-validated` 红（`ValidateRgn` 走 Diff）。
- `GdiRegion::contains` 的 `x < r.right` 改成 `x <= r.right`（`y` 同理）⇒ **`region-rect` 红**
  （右/下边界那两个断言 0→1），连带 `region-data-roundtrip` 红（同样的半开判据在那里再查一遍）。

## 没做的（与原因）

- **WINDING（非零环绕）多边形**：只做 ALTERNATE（奇偶）。WINDING 对自交多边形给出的填充不同，
  目前没有可校验的需求 ⇒ 明确返回 NULL + 一行日志，⛔ 不静默降级（`CreatePolygonRgn` 的 mode 只有 1/2 合法）。
- **`RGN_XOR`**：实现了（对称差 = 在 A 或 B 里、不同时在），语义写在 `RegionCombine` 的注释与用例里。
- 光栅化的坐标上限 ±16384（整数精确扫描线的前提，见 `region.h` 的推导）；超出 ⇒ Win32 失败值 + 日志。
  矩形区域不受此限（只比较 / 复制，没有乘法）。
- 椭圆 / 多边形是**本模拟器的确定性近似**（像素中心求交），不是真机 GDI 的逐像素等价 —— 真机 GDI 的
  椭圆算法不可复现，这里只保证"内部 / 外部 / 包围盒"三条机械判据（plan §5 B7 的口径）。
- 把 region 选进 DC 做**裁剪**（`SelectObject(hdc, hrgn)`）—— 不在本单范围（当前没有导入点用得上）。
