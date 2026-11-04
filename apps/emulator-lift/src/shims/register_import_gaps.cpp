// emulator-lift · PE 导入表的**显式缺口登记**（派生文件）
//
// ⛔ **不要手改**：本文件由 `pnpm tools lift sync --write` 生成。
// 它登记的是**导入表里有、但既没有实现也没有 addUnimplemented 的条目** ——
// 每条都会分到唯一桩地址，执行到就响亮失败并带上这里的 reason（SCOPE §3：⛔ 不许静默跳过）。
//
// 怎么改：实现它（在族文件里 add，删掉这里的行由 sync 负责）· 改理由（tools/lift.mjs 的
// MODULE_REASONS）· 判据：`pnpm tools lift check`（红 = 本文件与导入表/注册表不一致）。
#include "shims/register.h"

namespace lift {
namespace {
// 理由按**模块**分组（见 tools/lift.mjs 的 MODULE_REASONS）：这里不写引擎语义，只写批次归属。
}  // namespace

void registerImportGaps(ShimRegistry& shims) {
    shims.addUnimplemented("d3d9", "Direct3DCreate9Ex", "M3 D3D9 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXComputeNormals", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXCreateEffectFromResourceA", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXCreateSprite", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXCreateTexture", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXCreateTextureFromFileInMemory", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXFrameFind", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXLoadMeshHierarchyFromXInMemory", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXLoadSurfaceFromSurface", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixDecompose", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixLookAtLH", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixMultiply", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixOrthoLH", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixPerspectiveFovLH", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixRotationAxis", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixRotationYawPitchRoll", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixScaling", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixTranslation", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXVec3Normalize", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("d3dx9_43", "D3DXVec4Transform", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");
    shims.addUnimplemented("gdi32", "AddFontMemResourceEx", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "CombineRgn", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "CreateEllipticRgnIndirect", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "CreateFontIndirectA", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "CreateICA", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "CreatePolygonRgn", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "CreateRectRgn", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "EnumFontFamiliesExA", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "GetCurrentObject", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "GetGlyphOutlineA", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "GetRegionData", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "GetRgnBox", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "GetStockObject", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "GetTextExtentPoint32A", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "GetTextMetricsA", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "PtInRegion", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "RemoveFontMemResourceEx", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("gdi32", "TextOutA", "M2 GDI 字体 / 2D 图形簇（不属 M1）");
    shims.addUnimplemented("imagehlp", "MakeSureDirectoryPathExists", "imagehlp：不属 M1（崩溃转储不在本线；归属待裁决）");
    shims.addUnimplemented("imm32", "ImmAssociateContext", "输入法族：M2（窗口 / 输入），不属 M1");
    shims.addUnimplemented("ole32", "CoCreateInstance", "COM 内存族已实现（同一个 guest 堆）；其余 ole32 导入属 M2/M3，不属 M1");
    shims.addUnimplemented("ole32", "PropVariantClear", "COM 内存族已实现（同一个 guest 堆）；其余 ole32 导入属 M2/M3，不属 M1");
    shims.addUnimplemented("oleaut32", "#7", "oleaut32：M2/M3（VARIANT / 自动化），不属 M1");
    shims.addUnimplemented("oleaut32", "#8", "oleaut32：M2/M3（VARIANT / 自动化），不属 M1");
    shims.addUnimplemented("oleaut32", "#9", "oleaut32：M2/M3（VARIANT / 自动化），不属 M1");
    shims.addUnimplemented("shell32", "SHGetSpecialFolderPathA", "shell32：不属 M1（启动路径不需要；归属待裁决）");
    shims.addUnimplemented("shlwapi", "PathRemoveFileSpecA", "shlwapi：不属 M1（启动路径不需要；归属待裁决）");
    shims.addUnimplemented("user32", "ClientToScreen", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "DestroyMenu", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "DrawMenuBar", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetAsyncKeyState", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetCursorPos", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetForegroundWindow", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetMenu", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetMonitorInfoA", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetParent", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetQueueStatus", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetSubMenu", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetSystemMenu", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetUpdateRgn", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetWindowLongA", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetWindowPlacement", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetWindowRect", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "InvalidateRect", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "IsDialogMessageA", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "IsIconic", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "LoadCursorFromFileA", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "MonitorFromWindow", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "MsgWaitForMultipleObjects", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "PostThreadMessageA", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "RegisterClassExA", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "RegisterWindowMessageA", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "RemoveMenu", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "ScreenToClient", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SetCursor", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SetCursorPos", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SetRect", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SetRectEmpty", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SetWindowLongA", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SetWindowPos", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "ShowCursor", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SystemParametersInfoA", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "UnhookWindowsHookEx", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "ValidateRect", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "ValidateRgn", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "WaitMessage", "M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）");
}

}  // namespace lift
