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
    shims.addUnimplemented("advapi32", "RegEnumValueA", "注册表族：M1b 之后（配置读写走 S6 之后的批次）");
    shims.addUnimplemented("d3d9", "Direct3DCreate9Ex", "M3 D3D9 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXComputeNormals", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXCreateEffectFromResourceA", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXCreateSprite", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXCreateTexture", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXCreateTextureFromFileInMemory", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXFrameFind", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXLoadMeshHierarchyFromXInMemory", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXLoadSurfaceFromSurface", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixDecompose", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixLookAtLH", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixMultiply", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixOrthoLH", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixPerspectiveFovLH", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixRotationAxis", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixRotationYawPitchRoll", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixScaling", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXMatrixTranslation", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXVec3Normalize", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("d3dx9_43", "D3DXVec4Transform", "M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）");
    shims.addUnimplemented("dxva2", "DXVA2CreateDirect3DDeviceManager9", "M3 视频簇（SCOPE §1：skip）");
    shims.addUnimplemented("evr", "MFCreateVideoSampleFromSurface", "M3 视频簇（SCOPE §1：skip）");
    shims.addUnimplemented("gdi32", "AddFontMemResourceEx", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "CombineRgn", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "CreateEllipticRgnIndirect", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "CreateFontIndirectA", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "CreateICA", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "CreatePolygonRgn", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "CreateRectRgn", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "EnumFontFamiliesExA", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "GetCurrentObject", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "GetGlyphOutlineA", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "GetRegionData", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "GetRgnBox", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "GetStockObject", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "GetTextExtentPoint32A", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "GetTextMetricsA", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "PtInRegion", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "RemoveFontMemResourceEx", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("gdi32", "TextOutA", "M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）");
    shims.addUnimplemented("imagehlp", "MakeSureDirectoryPathExists", "imagehlp：M1 未实现（崩溃转储不在本线）");
    shims.addUnimplemented("imm32", "ImmAssociateContext", "输入法族：M2（窗口 / 输入）");
    shims.addUnimplemented("mf", "MFCreateAudioRendererActivate", "M3 视频簇（SCOPE §1：skip，只保留影片 opcode 推进）");
    shims.addUnimplemented("mf", "MFCreateMediaSession", "M3 视频簇（SCOPE §1：skip，只保留影片 opcode 推进）");
    shims.addUnimplemented("mf", "MFCreateSourceResolver", "M3 视频簇（SCOPE §1：skip，只保留影片 opcode 推进）");
    shims.addUnimplemented("mf", "MFCreateTopology", "M3 视频簇（SCOPE §1：skip，只保留影片 opcode 推进）");
    shims.addUnimplemented("mf", "MFCreateTopologyNode", "M3 视频簇（SCOPE §1：skip，只保留影片 opcode 推进）");
    shims.addUnimplemented("mf", "MFCreateVideoRendererActivate", "M3 视频簇（SCOPE §1：skip，只保留影片 opcode 推进）");
    shims.addUnimplemented("mf", "MFGetService", "M3 视频簇（SCOPE §1：skip，只保留影片 opcode 推进）");
    shims.addUnimplemented("mfplat", "MFCreateAsyncResult", "M3 视频簇（SCOPE §1：skip）");
    shims.addUnimplemented("mfplat", "MFCreateAttributes", "M3 视频簇（SCOPE §1：skip）");
    shims.addUnimplemented("mfplat", "MFCreateMediaType", "M3 视频簇（SCOPE §1：skip）");
    shims.addUnimplemented("mfplat", "MFInvokeCallback", "M3 视频簇（SCOPE §1：skip）");
    shims.addUnimplemented("mfplat", "MFShutdown", "M3 视频簇（SCOPE §1：skip）");
    shims.addUnimplemented("mfplat", "MFStartup", "M3 视频簇（SCOPE §1：skip）");
    shims.addUnimplemented("ole32", "CoCreateInstance", "COM 内存族已实现（同一个 guest 堆）；其余 ole32 导入属 M2/M3");
    shims.addUnimplemented("ole32", "PropVariantClear", "COM 内存族已实现（同一个 guest 堆）；其余 ole32 导入属 M2/M3");
    shims.addUnimplemented("oleaut32", "#7", "oleaut32：M2/M3（VARIANT / 自动化）");
    shims.addUnimplemented("oleaut32", "#8", "oleaut32：M2/M3（VARIANT / 自动化）");
    shims.addUnimplemented("oleaut32", "#9", "oleaut32：M2/M3（VARIANT / 自动化）");
    shims.addUnimplemented("shell32", "SHGetSpecialFolderPathA", "shell32：M1 未实现（CRT / 启动路径不需要）");
    shims.addUnimplemented("shlwapi", "PathRemoveFileSpecA", "shlwapi：M1 未实现（CRT / 启动路径不需要）");
    shims.addUnimplemented("user32", "ClientToScreen", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "DestroyMenu", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "DrawMenuBar", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetAsyncKeyState", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetCursorPos", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetForegroundWindow", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetMenu", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetMonitorInfoA", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetParent", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetQueueStatus", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetSubMenu", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetSystemMenu", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetUpdateRgn", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetWindowLongA", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetWindowPlacement", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "GetWindowRect", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "InvalidateRect", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "IsDialogMessageA", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "IsIconic", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "LoadCursorFromFileA", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "MonitorFromWindow", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "MsgWaitForMultipleObjects", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "PostThreadMessageA", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "RegisterClassExA", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "RegisterWindowMessageA", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "RemoveMenu", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "ScreenToClient", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SetCursor", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SetCursorPos", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SetRect", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SetRectEmpty", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SetWindowLongA", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SetWindowPos", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "ShowCursor", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "SystemParametersInfoA", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "UnhookWindowsHookEx", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "ValidateRect", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "ValidateRgn", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("user32", "WaitMessage", "S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）");
    shims.addUnimplemented("winmm", "joyGetDevCapsA", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "joyGetNumDevs", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "joyGetPosEx", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "mciGetErrorStringA", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "mciSendCommandA", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "mciSendStringA", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "mixerClose", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "mixerOpen", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "mixerSetControlDetails", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "mmioAscend", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "mmioClose", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "mmioDescend", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "mmioOpenA", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
    shims.addUnimplemented("winmm", "mmioRead", "winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）");
}

}  // namespace lift
