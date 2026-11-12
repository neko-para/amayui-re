// emulator-lift · user32 菜单 / 模态（SCOPE §1：整体 no-op）
//
// ★ 用户口径（2026-11-04）：菜单 / 模态按 **no-op** 处理，不是硬 trap。判据（SCOPE §1）：
//   · `LoadMenuA`    → 返回**非空假句柄**（guest 只把它当不透明令牌；真去解引用它没有意义）；
//   · `SetMenu`      → TRUE（顺手把窗口的菜单句柄记下来，`GetMenu` 才自洽）；
//   · `GetMenu`      → 窗口的菜单句柄（**没有菜单 = 0**，这是 Win32 的真值；⛔ 不凭空造句柄）；
//   · `GetSubMenu`   → 0（假菜单没有项 ⇒ Win32 就是 NULL；⛔ 不造第二层假句柄）；
//   · `GetSystemMenu`→ **非空假句柄**（有 WS_SYSMENU 的窗口在真机上总有系统菜单）；
//   · `RemoveMenu` / `DestroyMenu` / `DrawMenuBar` → TRUE；
//   · `TrackPopupMenu` → 0（立即返回，不阻塞、不等待）；
//   · `MessageBoxA`  → ★ 2026-11-12 改判（用户口径）：**系统模态 + SDL 自行实现的窗口**。
//     ⛔ 旧口径「恒 IDOK(1) / 不建窗口 / 不阻塞」是**历史**（`SCOPE.md` §1 已改）：
//     窗口档进宿主模态循环并画一整幅自绘对话框；返回值按 `uType`（`MB_YESNO` 默认与 X/Esc
//     一律 `IDNO(7)`，见 `runtime/message_box.h`）。headless / 无 sink / feeder / 非主线程
//     ⇒ 立即默认值 + 一行 warn（CTest 因此不会挂）。正文/标题仍是 **warn** 级（默认可见）。
// ★ 每一条都打一行 `[shim]`（SCOPE §3 第 3 条：⛔ 不许静默 no-op）。菜单族条目仍经 `addStub`
//   登记 `data/lift/overrides.json`（`lift sync` 自动进清单）；`MessageBoxA` 已改成 `add`
//   （真实现，不在 stub 清单里）。
// ★ 它们**不属于** D6 七族（user32 窗口 / 消息族），所以不触发 `seven-family-no-stub`。
//
// ★ 实测调用点（`.lst`，锚 EA；都是"有菜单才做"的分支）：
//   · `0x004B9E1B LoadMenuA` → `0x004B9E6F GetSubMenu(hMenu, 2)` → `TrackPopupMenu` →
//     `0x004B9E7D DestroyMenu`（右键菜单：整条链都是 no-op）；
//   · `0x004B98C2 GetSystemMenu(hwnd, FALSE)` → `0x004B98D0 RemoveMenu(hMenu, 0xF030, 0)`
//     （WM_SYSCOMMAND 里去掉"最大化"项）；
//   · `0x004BB0ED GetMenu(hwnd)` → `SetMenu`（经函数指针）→ `0x004BB0FE DrawMenuBar(hwnd)`
//     （`display:ScreenMode` 分支）；另有 `0x004B9A35 / 0x00414239 DrawMenuBar`。
//   · `MessageBoxA`：`0x00406D38`（DSOUND 装载失败）· `0x00406D93`（DirectSound 不可用）·
//     另有 `j_MessageBoxA_4` 这条跳板的多处调用点。
#include <cstdint>
#include <string>

#include "runtime/audio.h"
#include "runtime/cp932.h"
#include "runtime/host_window.h"
#include "runtime/machine.h"
#include "runtime/message_box.h"
#include "runtime/present.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// 假菜单句柄：非 0、稳定、一眼能看出不是真 HANDLE 的值。
constexpr std::uint32_t kFakeMenu = 0x00010001u;
// 假系统菜单句柄（与 LoadMenuA 的假句柄区分开，日志里能看出用的是哪一条路径）。
constexpr std::uint32_t kFakeSystemMenu = 0x00010003u;

HostWindow* findWindow(X86& cpu, std::uint32_t hwnd) {
    return cpu.windows ? cpu.windows->find(hwnd) : nullptr;
}

void shimLoadMenuA(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hInstance / lpMenuName（2 个栈槽；不解析资源）
    shimLog("user32", "LoadMenuA",
            "菜单整体 no-op ⇒ 返回非空假句柄 0x%08X（SCOPE §1；不解析菜单资源）", kFakeMenu);
    call.result = kFakeMenu;
}

void shimSetMenu(X86& cpu, ShimCall& call) {
    // 参数：hWnd / hMenu
    // ★ SCOPE §1 判 TRUE；顺手记下窗口的菜单句柄（`GetMenu` 才自洽）。
    //   窗口句柄无效时**仍返回 TRUE**（SCOPE 的 stub 口径；`tests/shim_abi.cpp` 也钉了这一条）。
    HostWindow* w = findWindow(cpu, call.u32(0));
    if (w) w->menu = call.u32(1);
    shimLog("user32", "SetMenu",
            "hwnd=0x%08X hMenu=0x%08X ⇒ TRUE（SCOPE §1 菜单整体 no-op；%s）", call.u32(0), call.u32(1),
            w ? "已记到窗口状态" : "窗口不存在 ⇒ 只返回 TRUE、不记状态");
    call.result = 1;
}

void shimGetMenu(X86& cpu, ShimCall& call) {
    // 参数：hWnd
    // ★ Win32：窗口没有菜单 ⇒ NULL（0）。⛔ 不要为了"非 0 假句柄"给一个不存在的菜单：
    //   引擎拿它去 SetMenu / DrawMenuBar，真值 0 才能让它走"没有菜单"的分支。
    HostWindow* w = findWindow(cpu, call.u32(0));
    const std::uint32_t menu = w ? w->menu : 0;
    shimLog("user32", "GetMenu",
            "hwnd=0x%08X ⇒ HMENU=0x%08X（%s；菜单整体 no-op，不建真菜单）", call.u32(0), menu,
            w ? (menu ? "窗口上记着的菜单句柄" : "窗口没有菜单 ⇒ Win32 的真值 0") : "窗口不存在 ⇒ 0");
    call.result = menu;
}

void shimGetSubMenu(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hMenu / nPos
    // ★ 假菜单里没有任何项 ⇒ Win32 的真值就是 NULL(0)；随后 `TrackPopupMenu(0,…)` 立即返回 0。
    shimLog("user32", "GetSubMenu",
            "hMenu=0x%08X nPos=%d ⇒ NULL（假菜单没有子菜单项；SCOPE §1 菜单整体 no-op）", call.u32(0),
            call.i32(1));
    call.result = 0;
}

void shimGetSystemMenu(X86& cpu, ShimCall& call) {
    // 参数：hWnd / bRevert
    HostWindow* w = findWindow(cpu, call.u32(0));
    if (!w) {
        shimLog("user32", "GetSystemMenu", "hwnd=0x%08X 不存在 ⇒ NULL（Win32 失败值）", call.u32(0));
        call.result = 0;
        return;
    }
    shimLog("user32", "GetSystemMenu",
            "hwnd=0x%08X bRevert=%d ⇒ 非空假 HMENU 0x%08X（系统菜单 no-op；⛔ 不建真菜单）", call.u32(0),
            call.i32(1), kFakeSystemMenu);
    call.result = kFakeSystemMenu;
}

void shimDestroyMenu(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hMenu
    shimLog("user32", "DestroyMenu", "hMenu=0x%08X ⇒ TRUE（假句柄，没有资源要释放）", call.u32(0));
    call.result = 1;
}

void shimRemoveMenu(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hMenu / uPosition / uFlags
    shimLog("user32", "RemoveMenu",
            "hMenu=0x%08X uPosition=0x%04X uFlags=0x%04X ⇒ TRUE（假菜单没有项可删；SCOPE §1 no-op）",
            call.u32(0), call.u32(1), call.u32(2));
    call.result = 1;
}

void shimDrawMenuBar(X86& cpu, ShimCall& call) {
    // 参数：hWnd
    HostWindow* w = findWindow(cpu, call.u32(0));
    if (!w) {
        shimLog("user32", "DrawMenuBar", "hwnd=0x%08X 不存在 ⇒ FALSE（Win32 失败值）", call.u32(0));
        call.result = 0;
        return;
    }
    shimLog("user32", "DrawMenuBar", "hwnd=0x%08X ⇒ TRUE（没有菜单栏可重画；SCOPE §1 no-op）", call.u32(0));
    call.result = 1;
}

void shimTrackPopupMenu(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hMenu / uFlags / x / y / nReserved / hWnd / prcRect（7 个栈槽）
    shimLog("user32", "TrackPopupMenu", "菜单整体 no-op ⇒ 立即返回 0（SCOPE §1；⛔ 不阻塞）");
    call.result = 0;
}

// ★ MessageBoxA 的文本入参是 **A 版 API** ⇒ guest 内存里是 **CP932 字节**（日文工程）。
//   日志要能读 ⇒ 按 CP932 解成 UTF-16 再编成 UTF-8（与 `vfs.cpp` 的 `guestPathForLog` 同一条规则；
//   码表是自带的 `cp932.cpp`，⛔ 不借宿主的 MultiByteToWideChar —— 那会让两台机器各有一套表）。
//   ⛔ **不做"已经是 UTF-8 就原样"的猜测**：MessageBoxA 的字节只可能来自 guest 的 A 版表示，
//   而"合法 UTF-8"与"合法 CP932"有交集 ⇒ 猜错会把日文打成乱码（`guestPathForLog` 那样猜是因为
//   VFS 还会收到宿主侧的 UTF-8 路径；这里没有那一侧）。纯 ASCII 是 CP932 的子集，不受影响。
//   非法字节按 Windows 的宽松口径跳过，并**另起一行**报是哪一侧、多少字节（⛔ 不静默吞掉）。
std::string cp932TextToUtf8(const std::string& bytes, std::size_t* bad) {
    if (bytes.empty()) return std::string();
    return utf16ToUtf8(cp932ToWide(bytes, bad));
}

// 日志里的文本必须**留在一行内**：引擎的正文本身带 `\r\n`（例 `.lst` .data:0051E488 那条
// 「DSOUND.DLLの読み込みに失敗しました．\r\nDirectXを…」）⇒ 原样打会把一行日志撕成多行，
// 既难 grep 也看不出"这属于同一个框"。所以把控制字符转义成 `\r` / `\n` / `\t` / `\xNN`。
// ★ 只是**显示**层面的转义：guest 收到的返回值 / 任何写回都不受影响。
std::string escapeLogText(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '\r': out += "\\r"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\x%02X", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
        }
    }
    return out;
}

void shimMessageBoxA(X86& cpu, ShimCall& call) {
    // 参数：hWnd / lpText / lpCaption / uType
    // ★ 为什么要把文本打出来（2026-11-05 用户口径）：boot 上**唯一**能看见"引擎为什么停 / 为什么
    //   走错分支"的手段就是这个模态框的正文（真机上它是弹给人看的）⇒ 必须进日志。
    // ★ 2026-11-12（`REQ-01M75S724...` ②）：正文升 **warn** ⇒ 默认级别下可见。
    // ★ **在第一次 Yield 之前**把两段 guest 文本读成宿主 `std::string`（设计 §1.2 的硬纪律：
    //   让出令牌期间 feeder 有权改 guest 内存 ⇒ 跨 Yield 的 `call.str()` 一律作废）。
    const std::uint32_t hwnd = call.u32(0);
    const std::uint32_t text_ea = call.u32(1);
    const std::uint32_t cap_ea = call.u32(2);
    const std::uint32_t type = call.u32(3);
    std::size_t bad_text = 0;
    std::size_t bad_cap = 0;
    const std::string text_utf8 = cp932TextToUtf8(call.str(1), &bad_text);
    const std::string caption_utf8 = cp932TextToUtf8(call.str(2), &bad_cap);
    const std::string text = escapeLogText(text_utf8);
    const std::string caption = escapeLogText(caption_utf8);
    const ModalSpec spec = makeMessageBoxSpec(type, caption_utf8, text_utf8);
    const std::uint32_t fallback = modalDefaultResult(spec);
    // 正文是**错误串**时默认级别必须能看见（这是本单判据②的落点；⛔ 不是 Info）。
    shimLogWarn("user32", "MessageBoxA",
                "type=0x%08X caption=\"%s\" text=\"%s\" ⇒ 默认键值 %u（窗口档进宿主模态；headless "
                "立即返回）",
                type, caption.c_str(), text.c_str(), fallback);
    if (spec.unknown_type) {
        shimLogWarn("user32", "MessageBoxA",
                    "未知的 uType 低 4 位（0x%08X & 0x0F = 0x%X）⇒ 按 MB_OK 处理（⛔ 不静默）", type,
                    type & 0x0Fu);
    }
    if (spec.defbutton_overridden) {
        shimLogDebug("user32", "MessageBoxA",
                     "MB_YESNO 收到显式 DEFBUTTONn（0x%08X）⇒ 忽略、默认键强制 IDNO(7)"
                     "（答 YES 会弹启动设置并退出；`0x4BA82B cmp eax,6`）",
                     type);
    }
    if (bad_text != 0 || bad_cap != 0) {
        shimLogWarn("user32", "MessageBoxA",
                    "⚠ 文本里有非法 CP932（caption %u 字节 / text %u 字节）⇒ 已按 Windows 的宽松口径跳过；"
                    "上面那行可能少字符（text@0x%08X caption@0x%08X hwnd=0x%08X）",
                    static_cast<unsigned>(bad_cap), static_cast<unsigned>(bad_text), text_ea, cap_ea, hwnd);
    }

    // ★ 分支口径（设计 §1.3 / §4）：只有"主线程 + 有 sink + sink 给得出模态呈现缝"才进循环。
    //   feeder / headless / 无 presenter / 非主线程 ⇒ 立即默认值 + **一次** warn（⛔ 不阻塞）。
    Machine* machine = cpu.machine;
    std::uint32_t result = fallback;
    bool enter_modal = false;
    if (machine != nullptr) {
        if (cpu.audio != nullptr && cpu.audio->onFeederThread()) {
            shimLogWarn("user32", "MessageBoxA",
                        "在音频 feeder 线程上被调用 ⇒ headless 语义：立即返回默认值 %u（⛔ 不碰窗口）",
                        fallback);
        } else if (!machine->presenter().hasSink() ||
                   machine->presenter().sink()->modalPresenter() == nullptr) {
            shimLogWarn("user32", "MessageBoxA",
                        "headless / 没有模态呈现后端 ⇒ 立即返回默认值 %u（⛔ 不建窗口、不阻塞）",
                        fallback);
        } else {
            enter_modal = true;
        }
    } else {
        shimLogWarn("user32", "MessageBoxA",
                    "没有装配 Machine（纯夹具）⇒ 立即返回默认值 %u（⛔ 不阻塞）", fallback);
    }
    if (enter_modal) result = runModalDialog(*machine, cpu, spec);
    noteMessageBoxCall(type, result);
    call.result = result;
}

}  // namespace

void registerUser32MenuShims(ShimRegistry& s) {
    // 顺序 = 原来的 4 条 addUnimplemented 的顺序 + 其余菜单族（trace 的 id 顺序不变）。
    s.addStub("user32", "LoadMenuA", Conv::Stdcall, 2, &shimLoadMenuA,
              "SCOPE §1 菜单整体 no-op：LoadMenuA 返回非空假句柄（不解析菜单资源）");
    s.addStub("user32", "SetMenu", Conv::Stdcall, 2, &shimSetMenu,
              "SCOPE §1 菜单整体 no-op：SetMenu 返回 TRUE（没有 Win32 菜单栏）");
    s.addStub("user32", "TrackPopupMenu", Conv::Stdcall, 7, &shimTrackPopupMenu,
              "SCOPE §1 菜单整体 no-op：TrackPopupMenu 立即返回 0（不阻塞）");
    // ★ 2026-11-12（M3 复审）：MessageBoxA 已是**实现**（宿主自绘模态 + 按 uType 返回），
    //   不是"按 SCOPE 偏差整体 stub"⇒ 用 `add` 登记（`data/lift/overrides.json` 不再是 stub 条目）。
    s.add("user32", "MessageBoxA", Conv::Stdcall, 4, &shimMessageBoxA);
    // ★ 菜单族其余条目（`.lst` 里有调用点，见文件头）：同样按 SCOPE §1 的 no-op 口径。
    s.addStub("user32", "GetMenu", Conv::Stdcall, 1, &shimGetMenu,
              "SCOPE §1 菜单整体 no-op：GetMenu 返回窗口的菜单句柄（没有菜单 = 0，Win32 真值）");
    s.addStub("user32", "GetSubMenu", Conv::Stdcall, 2, &shimGetSubMenu,
              "SCOPE §1 菜单整体 no-op：GetSubMenu 返回 NULL（假菜单没有子菜单项）");
    s.addStub("user32", "GetSystemMenu", Conv::Stdcall, 2, &shimGetSystemMenu,
              "SCOPE §1 菜单整体 no-op：GetSystemMenu 返回非空假句柄（不建真系统菜单）");
    s.addStub("user32", "RemoveMenu", Conv::Stdcall, 3, &shimRemoveMenu,
              "SCOPE §1 菜单整体 no-op：RemoveMenu 返回 TRUE（假菜单没有项可删）");
    s.addStub("user32", "DestroyMenu", Conv::Stdcall, 1, &shimDestroyMenu,
              "SCOPE §1 菜单整体 no-op：DestroyMenu 返回 TRUE（假句柄，没有资源要释放）");
    s.addStub("user32", "DrawMenuBar", Conv::Stdcall, 1, &shimDrawMenuBar,
              "SCOPE §1 菜单整体 no-op：DrawMenuBar 返回 TRUE（没有菜单栏可重画）");
}

}  // namespace lift
