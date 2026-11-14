#include "runtime/host_cursor.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace lift {

HostCursorPlatform hostCursorPlatform() {
#if defined(_WIN32)
    return HostCursorPlatform::Win32;
#elif defined(__APPLE__)
    return HostCursorPlatform::MacOS;
#else
    return HostCursorPlatform::Unsupported;
#endif
}

bool hostCursorPlatformSupported() { return hostCursorPlatform() != HostCursorPlatform::Unsupported; }

const char* hostCursorPlatformName() {
    switch (hostCursorPlatform()) {
        case HostCursorPlatform::Win32: return "win32";
        case HostCursorPlatform::MacOS: return "macos";
        case HostCursorPlatform::Unsupported: break;
    }
    return "unsupported";
}

bool hostCursorWarpToScreen(int screen_x, int screen_y) {
#if defined(_WIN32)
    // Win32：只有"当前桌面不允许写光标位置"才返回 0（锁屏 / UAC secure desktop / 远程会话…）。
    return ::SetCursorPos(screen_x, screen_y) != 0;
#elif defined(__APPLE__)
    // macOS：`CGWarpMouseCursorPosition` 吃全局显示 point（左上原点、可负），不需要辅助功能授权。
    // ★ warp 之后立刻复位鼠标–光标关联：Apple 文档说明 warp 会让两者短暂脱钩（防巨大相对位移），
    //   不复位 ⇒ "脚本挪完光标后玩家推鼠标推不动"（旧仓实测）。
    const CGError err = ::CGWarpMouseCursorPosition(CGPointMake(static_cast<CGFloat>(screen_x),
                                                                 static_cast<CGFloat>(screen_y)));
    if (err != kCGErrorSuccess) return false;
    ::CGAssociateMouseAndMouseCursorPosition(true);
    return true;
#else
    (void)screen_x;
    (void)screen_y;
    return false;  // 平台不支持：第二道防线（启动期已经 exit 2）
#endif
}

bool hostCursorQueryScreen(int* screen_x, int* screen_y) {
    if (screen_x == nullptr || screen_y == nullptr) return false;
#if defined(_WIN32)
    POINT p{};
    if (!::GetCursorPos(&p)) return false;
    *screen_x = p.x;
    *screen_y = p.y;
    return true;
#elif defined(__APPLE__)
    // `CGEventCreate(NULL)` 的 location 就是当前光标位置（全局显示 point）。
    CGEventRef event = ::CGEventCreate(nullptr);
    if (event == nullptr) return false;
    const CGPoint p = ::CGEventGetLocation(event);
    ::CFRelease(event);
    *screen_x = static_cast<int>(p.x);
    *screen_y = static_cast<int>(p.y);
    return true;
#else
    (void)screen_x;
    (void)screen_y;
    return false;
#endif
}

}  // namespace lift
