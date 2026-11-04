// emulator-lift · user32 资源族：LoadIconA / LoadCursorA（AGE.EXE 与真实 AGERC.DLL 的 .rsrc）
//
// ★ 口径（plan §4 D6 / §6 P0 + 用户 2026-11-04 改）：
//   · `LoadIconA(0, id)`       → stock 图标：返回**确定性非空**伪 HICON（⛔ 不读任何模块的资源）。
//   · `LoadIconA(主模块句柄, …)` → 用 `cpu.image`（AGE.EXE）的 `RT_GROUP_ICON`：按名字 / 序号找一组，
//     再解析组内的 `RT_ICON` ID 列表。窗口图标**不需要真的渲染** ⇒ 返回确定性伪 HICON。
//   · `LoadIconA(AGERC 句柄, …)` → ★ 2026-11-04 改口径：AGERC.DLL 是**真实装载的 guest 模块**，
//     它的 HINSTANCE = 真实模块基址（`cpu.shim.agerc_base`），资源从 `cpu.shim.agerc_image` 解析。
//     按 `"IDI_ICON1"` 找 `RT_GROUP_ICON`（找不到可回退第一组）⇒ 伪 HICON。窗口图标不渲染是平台偏差。
//     ★ 资源数据 RVA 读不到（原件 AGERC.DLL 的 `.rsrc` 虚拟尾部在文件里没有字节）⇒
//        返回伪 HICON + 日志（窗口图标不渲染；平台偏差，SCOPE §1）；`--agerc` 拆壳件可读真实图标。
//   · `LoadIconA(其它 hInstance, …)` → **hard error**（`shimAbort`）：本进程里没有这个模块。
//   · `LoadCursorA` → 一律 stock cursor（伪句柄）；hInstance 口径同上。
// ★ 实测（`.lst`，0x4BA99C / 0x4BA9B5）：
//     `mov ecx, <RCDLL>; call sub_48E620; push eax; call LoadIconA`
//   `sub_48E620` 是 `jmp [ecx+8]`，而 `[ecx+8]` 是 `GetProcAddress(AGERC, "_GetInstance@0")` 的返回值
//   ⇒ **引擎传给 LoadIconA 的 hInstance 是 AGERC `_GetInstance@0` 的返回值**（AGERC 的 HINSTANCE），
//   而 `"IDI_ICON1"` 这个名字确实在 **AGERC.DLL** 的 RT_GROUP_ICON 里（实测 AGE.EXE 的组图标是数字
//   138、没有这个名字）。⇒ 这条路径现在能用**真实装载的 AGERC 镜像**回答。
//   `LoadCursorA` 的调用点是 `LoadCursorA(NULL, 0x7F00)`（0x4BA9C9）⇒ 走 stock 分支。
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "runtime/image.h"
#include "runtime/pe_resource.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// 伪句柄：非 0、稳定、一眼能看出不是真 HANDLE（真机 HICON/HCURSOR 是内核对象表里的值）。
constexpr std::uint32_t kFakeIcon = 0x00020001u;
constexpr std::uint32_t kFakeCursor = 0x00020002u;

void requireMem(X86& cpu, const char* api) {
    if (!cpu.mem) shimAbort("user32", api, "cpu.mem 为空（Machine 没装配 guest 内存）");
}

bool asciiEqualNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return true;
}

// 资源 API 的 hInstance 口径（用户 2026-11-04）：接受 0（stock）、主模块句柄、以及
// **真实装载的 AGERC 模块基址**；其它一律 hard error。
void requireAgeModule(X86& cpu, std::uint32_t hinst, const char* api, std::uint32_t call_ea) {
    if (hinst == 0) return;
    if (hinst == cpu.shim.module_handle && cpu.image != nullptr) return;
    if (cpu.shim.agerc_base != 0 && hinst == cpu.shim.agerc_base && cpu.shim.agerc_image != nullptr) {
        return;
    }
    const char* hint = "";
    if (hinst == cpu.shim.module_handle) {
        shimAbort("user32", api, "hInstance=0x%08X 是主模块句柄，但 Machine 没有装配 PE 镜像（cpu.image 为空）",
                  hinst);
    } else if (hinst == cpu.shim.agerc_base) {
        hint = "（= AGERC 模块基址，但 Machine 没有装载 AGERC 镜像）";
    } else {
        hint = "（不是 AGE.EXE 主模块、也不是已装载的 AGERC 模块）";
    }
    shimAbort("user32", api,
              "hInstance=0x%08X 不是可识别的模块句柄（主模块 0x%08X / AGERC 0x%08X，调用点 EA=0x%08X）%s",
              hinst, cpu.shim.module_handle, cpu.shim.agerc_base, call_ea, hint);
}

// 取主镜像的资源视图（只有 hinst == 主模块时才会走到这里）
void openMainResources(X86& cpu, PeResources* res, const char* api) {
    std::string err;
    if (!res->init(*cpu.image, &err)) {
        shimAbort("user32", api, "AGE.EXE 的资源目录解析失败：%s", err.c_str());
    }
    if (!res->hasResources()) {
        shimLog("user32", api, "AGE.EXE 没有资源目录（DataDirectory[2] = 0）⇒ 没有可用的图标资源");
    }
}

// LoadIconA(0, …)：stock 图标
void loadStockIcon(X86& cpu, ShimCall& call) {
    (void)cpu;
    const std::uint32_t name = call.u32(1);
    shimLog("user32", "LoadIconA",
            "hInstance=NULL ⇒ stock 图标（lpIconName=0x%08X）⇒ 返回确定性伪 HICON 0x%08X"
            "（⛔ 不读任何模块的资源）",
            name, kFakeIcon);
    call.result = kFakeIcon;
}

// ---- 真实 AGERC 镜像的图标（hinst == cpu.shim.agerc_base）----
void loadAgercIcon(X86& cpu, ShimCall& call, std::uint32_t name_ea) {
    const char* api = "LoadIconA";
    const PeImage& img = *cpu.shim.agerc_image;
    const std::uint32_t base = cpu.shim.agerc_base;
    // "资源数据读不到"（原件 AGERC.DLL 的 `.rsrc` 虚拟尾部在文件里没有字节）⇒
    // **返回伪 HICON + 日志**（窗口图标不渲染；平台偏差，SCOPE §1）。若给了拆壳件 `--agerc`
    // 且资源可读，就按真实图标解析；不会 fake 资源本身。
    auto unreadable = [&](const std::string& why) {
        shimLog("user32", api,
                "真实 AGERC 镜像 0x%08X（%s）的图标读不到：%s\n"
                "  ⇒ 返回伪 HICON 0x%08X（窗口图标不渲染；平台偏差 SCOPE §1；"
                "用 --agerc 指向拆壳件可读真实图标）",
                base, img.path.c_str(), why.c_str(), kFakeIcon);
        call.result = kFakeIcon;
    };

    PeResources res;
    std::string err;
    if (!res.init(img, &err)) {
        unreadable("资源目录解析失败：" + err);
        return;
    }
    if (!res.hasResources()) {
        unreadable("镜像没有资源目录（DataDirectory[2] = 0）");
        return;
    }

    // 引擎传的是字符串名 "IDI_ICON1"；拿不到名字时也按这个名字找。
    const bool by_name = (name_ea >> 16) != 0;
    std::string want = by_name ? call.str(1) : std::string();
    if (want.empty()) want = "IDI_ICON1";

    std::vector<std::uint32_t> ids;
    std::string gerr;
    std::string how = "组 \"" + want + "\"";
    bool got = res.iconGroupByName(want, &ids, &gerr) && !ids.empty();
    if (!got) {
        // 区分"名字不存在"（⇒ 确定性回退第一组，与 AGE.EXE 同口径）与"名字在、数据读不到"（⇒ 伪 HICON + 日志）。
        std::vector<PeResourceName> names;
        std::string lerr;
        if (!res.listNames(kRtGroupIcon, &names, &lerr)) {
            // 类型不存在：镜像确实没有组图标 ⇒ 不是读失败（真机这里也拿不到图标）。
            shimLog("user32", api,
                    "真实 AGERC 镜像 0x%08X 里没有 RT_GROUP_ICON（%s）⇒ 返回确定性非空伪 HICON "
                    "0x%08X（窗口图标不渲染；平台偏差）",
                    base, lerr.c_str(), kFakeIcon);
            call.result = kFakeIcon;
            return;
        }
        bool want_present = false;
        for (const PeResourceName& n : names) {
            if (n.is_name && asciiEqualNoCase(n.name, want)) {
                want_present = true;
                break;
            }
        }
        if (want_present) {
            unreadable("RT_GROUP_ICON \"" + want + "\" 的数据读不到：" + gerr);
            return;
        }
        if (names.empty()) {
            shimLog("user32", api,
                    "真实 AGERC 镜像 0x%08X 的 RT_GROUP_ICON 目录是空的 ⇒ 伪 HICON 0x%08X", base,
                    kFakeIcon);
            call.result = kFakeIcon;
            return;
        }
        // 按名没找到 ⇒ 确定性回退到第一组（与 AGE.EXE 的分支同口径）。
        const PeResourceName& first = names.front();
        std::vector<std::uint32_t> first_ids;
        std::string ferr;
        const bool ok = first.is_name ? res.iconGroupByName(first.name, &first_ids, &ferr)
                                      : res.iconGroup(first.id, &first_ids, &ferr);
        if (!ok || first_ids.empty()) {
            unreadable("按名 \"" + want + "\" 没找到，第一组也读不到：" + ferr);
            return;
        }
        const std::string first_text =
            first.is_name ? ("\"" + first.name + "\"") : ("ID " + std::to_string(first.id));
        shimLog("user32", api,
                "真实 AGERC 镜像 0x%08X：按名 \"%s\" 没找到 RT_GROUP_ICON（%s）⇒ 确定性回退到第一组 %s"
                "（%zu 个 RT_ICON）",
                base, want.c_str(), gerr.c_str(), first_text.c_str(), first_ids.size());
        ids = std::move(first_ids);
        how = "第一组 " + first_text;
    }

    // 组内每个 ID 都应有可读的 RT_ICON（读不到 ⇒ 就是"资源数据 RVA 读不到"）。
    std::size_t resolved = 0;
    std::string bad;
    for (std::uint32_t id : ids) {
        PeResourceEntry entry;
        std::string ierr;
        std::vector<std::uint8_t> data;
        if (res.findEntry(kRtIcon, id, &entry, &ierr) && res.readData(entry, &data, &ierr) &&
            !data.empty()) {
            ++resolved;
        } else if (bad.empty()) {
            bad = "RT_ICON " + std::to_string(id) + "（" + ierr + "）";
        }
    }
    if (resolved != ids.size()) {
        unreadable("组内 " + std::to_string(ids.size() - resolved) + " 个 RT_ICON 读不到，首个：" + bad);
        return;
    }

    shimLog("user32", api,
            "真实 AGERC 镜像（base=0x%08X / %s）/ RT_GROUP_ICON（%s）/ %zu 个 RT_ICON 全部已读"
            " ⇒ 返回确定性伪 HICON 0x%08X（窗口图标不渲染；平台偏差，SCOPE §1）",
            base, img.path.c_str(), how.c_str(), ids.size(), kFakeIcon);
    call.result = kFakeIcon;
}

void shimLoadIconA(X86& cpu, ShimCall& call) {
    requireMem(cpu, "LoadIconA");
    const std::uint32_t hinst = call.u32(0);
    const std::uint32_t name_ea = call.u32(1);
    if (hinst == 0) {
        loadStockIcon(cpu, call);
        return;
    }
    // ★ 真实装载的 AGERC 模块：读它自己的 .rsrc（⛔ 不再 hard error / 不 fake 资源）。
    if (cpu.shim.agerc_image != nullptr && hinst == cpu.shim.agerc_base) {
        loadAgercIcon(cpu, call, name_ea);
        return;
    }
    requireAgeModule(cpu, hinst, "LoadIconA", call.ret_ea);

    PeResources res;
    openMainResources(cpu, &res, "LoadIconA");

    // lpIconName：HIWORD != 0 ⇒ 字符串名；HIWORD == 0 ⇒ 序号（MAKEINTRESOURCE）
    const bool by_name = (name_ea >> 16) != 0;
    const std::string want_name = by_name ? call.str(1) : std::string();
    const std::uint32_t want_id = by_name ? 0u : (name_ea & 0xFFFFu);

    std::vector<std::uint32_t> icon_ids;
    std::string err;
    bool found = by_name ? res.iconGroupByName(want_name, &icon_ids, &err)
                         : res.iconGroup(want_id, &icon_ids, &err);
    std::string how = by_name ? ("名字 \"" + want_name + "\"") : ("序号 " + std::to_string(want_id));
    if (!found) {
        // ★ 确定性回退：按名/序号没找到 ⇒ 取第一组 RT_GROUP_ICON。
        //   实测：AGE.EXE 的组图标是数字 ID 138（`.rsrc` 里没有 "IDI_ICON1" 这个名字），
        //   而引擎问的正是 "IDI_ICON1" ⇒ 不回退就等于"永远没有窗口图标"。
        std::vector<PeResourceName> names;
        std::string lerr;
        if (res.listNames(kRtGroupIcon, &names, &lerr) && !names.empty()) {
            const PeResourceName& first = names.front();
            std::vector<std::uint32_t> fallback_ids;
            const bool ok = first.is_name ? res.iconGroupByName(first.name, &fallback_ids, &lerr)
                                          : res.iconGroup(first.id, &fallback_ids, &lerr);
            const std::string first_text = first.is_name ? ("\"" + first.name + "\"")
                                                         : ("ID " + std::to_string(first.id));
            if (ok && !fallback_ids.empty()) {
                shimLog("user32", "LoadIconA",
                        "按 %s 没找到 RT_GROUP_ICON（%s）⇒ 确定性回退到第一组 %s（%zu 个 RT_ICON）",
                        how.c_str(), err.c_str(), first_text.c_str(), fallback_ids.size());
                icon_ids = std::move(fallback_ids);
                found = true;
                how = "第一组 " + first_text;
            } else {
                shimLog("user32", "LoadIconA", "按 %s 没找到 RT_GROUP_ICON（%s），第一组 %s 也解析失败（%s）",
                        how.c_str(), err.c_str(), first_text.c_str(), lerr.c_str());
            }
        } else {
            shimLog("user32", "LoadIconA", "按 %s 没找到 RT_GROUP_ICON（%s），镜像里也没有其它组（%s）",
                    how.c_str(), err.c_str(), lerr.c_str());
        }
    }

    if (!found || icon_ids.empty()) {
        shimLog("user32", "LoadIconA",
                "AGE.EXE 里没有可用的 RT_GROUP_ICON ⇒ 返回确定性非空伪 HICON 0x%08X"
                "（⛔ 窗口图标不会显示；这是可接受的平台偏差，plan D6）",
                kFakeIcon);
        call.result = kFakeIcon;
        return;
    }

    // 组内每个 ID 都应有对应的 RT_ICON（没有的话仍然给伪 HICON，但要看得见）
    std::size_t resolved = 0;
    for (std::uint32_t id : icon_ids) {
        PeResourceEntry entry;
        std::string ierr;
        if (res.findEntry(kRtIcon, id, &entry, &ierr) && entry.size > 0) ++resolved;
    }
    shimLog("user32", "LoadIconA",
            "AGE.EXE 的 RT_GROUP_ICON（%s）含 %zu 个 RT_ICON（首个 id=%u，%zu/%zu 个能在 RT_ICON 里找到）"
            " ⇒ 返回确定性伪 HICON 0x%08X（窗口图标不渲染；平台偏差，plan D6）",
            how.c_str(), icon_ids.size(), icon_ids.front(), resolved, icon_ids.size(), kFakeIcon);
    call.result = kFakeIcon;
}

void shimLoadCursorA(X86& cpu, ShimCall& call) {
    requireMem(cpu, "LoadCursorA");
    const std::uint32_t hinst = call.u32(0);
    const std::uint32_t name = call.u32(1);
    // ★ 同一个 hInstance 口径：资源 API 只认 0 / 主模块 / 已装载的 AGERC；
    //   ⚠ 本机的调用点是 `LoadCursorA(NULL, 0x7F00)`（IDC_ARROW，0x4BA9C9）⇒ stock 分支。
    requireAgeModule(cpu, hinst, "LoadCursorA", call.ret_ea);
    if (hinst != 0) {
        shimLog("user32", "LoadCursorA",
                "hInstance=0x%08X 是已装载的模块，且 lpCursorName=0x%08X：本线**只返回 stock cursor**"
                "（⛔ 不加载 RT_CURSOR；光标形状是平台呈现，plan D6/P3）",
                hinst, name);
    } else {
        shimLog("user32", "LoadCursorA",
                "hInstance=NULL（stock 光标，lpCursorName=0x%08X）⇒ 返回确定性伪 HCURSOR 0x%08X"
                "（本线不建真光标；P3 接 SDL 时再改）",
                name, kFakeCursor);
    }
    call.result = kFakeCursor;
}

}  // namespace

void registerUser32ResourceShims(ShimRegistry& s) {
    // ★ 实现（不是 stub）：图标读 AGE.EXE / 真实 AGERC 镜像的 .rsrc；资源数据读不到时伪 HICON + 日志。
    s.add("user32", "LoadIconA", Conv::Stdcall, 2, &shimLoadIconA);
    s.add("user32", "LoadCursorA", Conv::Stdcall, 2, &shimLoadCursorA);
}

}  // namespace lift
