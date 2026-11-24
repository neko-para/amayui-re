// emulator-lift · S7/IAT：`槽 EA → 清理字节` 表的**唯一**实现（口径见 `aot/lift_iat.h` 的头注）
//
// ★ 数据来源是 `ShimRegistry`（= 每个 `shims/*.cpp` 的 `add(...)` 实参），不是文档、不是源码解析。
// ★ 本文件**不碰** guest 内存、不碰 `.text`、不 bind —— 它只读 `PeImage::imports`（PE 头里的数据）。
#include "aot/lift_iat.h"

#include <cctype>
#include <string>
#include <string_view>

#include "runtime/image.h"

namespace lift {
namespace aot {
namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 与 `shims.cpp` 的 `normalizeModule` 同一条规则（小写 + 去掉尾部 `.dll`）：
// PE 的导入表写 `KERNEL32.dll`，注册时写 `kernel32`。
std::string normalizeModule(std::string_view s) {
    std::string o = lowerAscii(std::string(s));
    if (o.size() > 4 && o.compare(o.size() - 4, 4, ".dll") == 0) o.resize(o.size() - 4);
    return o;
}

bool equalsIgnoreCase(const std::string& a, const char* b) {
    const std::string lb = lowerAscii(b != nullptr ? b : "");
    return lowerAscii(a) == lb;
}

// ★ `<待核>`（design §1.4-3 / §8.2-2）：`ShimDef` 今天**没有**"这个 shim 会 take_control"的
//   字段 ⇒ 这里用一张**已知名单**兜住 `rg 'take_control = true' src/shims` 的全部站点
//   （2026-11-24 扫描：`ddraw.cpp` / `gdi32_font.cpp` / `kernel32_bootstrap.cpp` /
//   `kernel32_string.cpp` / `user32_window.cpp` / `quartz_video.cpp`）。
//   · 名单里**只列静态导入能命中的那些**：COM vtable 槽（ddraw 的 `EnumDisplayModes`、
//     quartz 的 Graph 方法）不是 IAT 条目 ⇒ 本来就进不了这张表。
//   · `user32_window.cpp` 的 `finishShim()` 是"启动 guest 回调就交控制权"的那条路
//     ⇒ 会走到它的 6 个入口逐个列出（CreateWindowExA / DestroyWindow / ShowWindow /
//     UpdateWindow / DispatchMessageA / SendMessageA）。
//   ⛔ 名单漏一条 ⇒ 那个导入会被当成"已建模"（= design §8.2-2 的**静默**风险）。
//   ⇒ 落地 `ShimDef::callback_capable` 时**删掉这张表**（本单的边界：不新增 `ShimDef` 字段）。
struct TakeControlImport {
    const char* module;  // 注册表里的模块名（大小写不敏感；不带 `.dll`）
    const char* name;    // 注册表里的函数名（大小写不敏感）
};

constexpr TakeControlImport kTakeControlImports[] = {
    {"kernel32", "EnumSystemLocalesA"},   // kernel32_string.cpp（guest 回调 + 续延桩）
    {"kernel32", "RaiseException"},       // kernel32_bootstrap.cpp（SEH 派发，自己安排 eip/esp）
    {"kernel32", "RtlUnwind"},            // kernel32_bootstrap.cpp（同上）
    {"gdi32", "EnumFontFamiliesExA"},     // gdi32_font.cpp（guest 回调）
    {"user32", "CreateWindowExA"},        // user32_window.cpp（WM_CREATE 回调）
    {"user32", "DestroyWindow"},          // user32_window.cpp（WM_DESTROY 回调）
    {"user32", "ShowWindow"},             // user32_window.cpp（WndProc 回调）
    {"user32", "UpdateWindow"},           // user32_window.cpp（WM_PAINT 回调）
    {"user32", "DispatchMessageA"},       // user32_window.cpp（WndProc 回调）
    {"user32", "SendMessageA"},           // user32_window.cpp（WndProc 回调）
};

bool mayTakeControl(const std::string& module, const std::string& name) {
    const std::string m = normalizeModule(module);
    for (const TakeControlImport& e : kTakeControlImports) {
        if (m == normalizeModule(e.module) && equalsIgnoreCase(name, e.name)) return true;
    }
    return false;
}

}  // namespace

IatModel buildIatModel(const ShimRegistry& shims, const PeImage& img) {
    IatModel model;
    for (const PeImportInfo& imp : img.imports) {
        // ★ 与 `ShimRegistry::bind()` 的 :430-431 **逐字相同**的名字归一口径：
        //   按序号导入的条目在注册表里的名字是 `#<ordinal>`。
        const std::string name = imp.by_ordinal ? ("#" + std::to_string(imp.ordinal)) : imp.name;
        IatRow row;
        row.slot_ea = img.image_base + imp.iat_rva;
        // `find` 与 `bind` 的 `findMutable` 是同一套归一化 + 大小写不敏感的线性扫描 ⇒
        // 这里命中的条目就是运行期 `bind` 会给这个槽分桩的那一条（同一个 id）。
        const ShimDef* def = shims.find(imp.module.c_str(), name.c_str());
        if (def != nullptr) {
            row.conv = def->conv;
            row.argc = def->argc;
            row.cleanup =
                (def->conv == Conv::Stdcall) ? static_cast<std::uint32_t>(def->argc) * 4u : 0u;
            // ⛔ 未实现（fn == nullptr ⇒ 执行即 trap）与会 take_control 的 shim **都不是**
            //    "会照常返回、按约定弹参的 callee" ⇒ 不许建模（否则是"假设 0 清理"的假证明）。
            row.modelled = (def->fn != nullptr) && !mayTakeControl(imp.module, name);
        }
        // 查不到 ⇒ `modelled == false`（`bind()` 会现场补一条裸缺口；它同样执行即 trap）。
        model[row.slot_ea] = row;
    }
    return model;
}

}  // namespace aot
}  // namespace lift
