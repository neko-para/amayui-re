// emulator-lift · GDI32 对象表 / DC 状态（P2）
//
// ★ 口径（plan §6 P2 / §5 B3）：
//   · DC / 字体 / 位图都是 guest 句柄（`HandleTable` 的值），payload 是宿主对象
//     （`GdiDcObject` / `GdiFontObject`）；⛔ guest 从不看到宿主指针。
//   · `SelectObject` 返回**旧对象**，再选回去能完整恢复（引擎会存旧值再复原）。
//   · `DeleteObject` 对**仍被选中**的对象按 Win32 语义返回 FALSE（⛔ 不 free 成野指针）；
//     对 stock object / DC 句柄同样返回 FALSE（真机行为）。
//   · 无效句柄（不是任何已分配句柄）⇒ `HandleTable::invalidHandle` **响亮失败**（仓库口径）。
//   · `GetStockObject` 从 P0 的 gdi32_boot.cpp 迁到这里（P0 的确定性令牌值不变：0x00013000 + index）。
#include <cstdint>
#include <string>

#include "runtime/font.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

GdiDcObject* requireDc(X86& cpu, std::uint32_t hdc, const char* api) {
    if (cpu.handles == nullptr) shimAbort("gdi32", api, "cpu.handles 为空（Machine 没装配）");
    GdiDcObject* dc = gdiAsDc(*cpu.handles, hdc);
    if (dc != nullptr) return dc;
    if (cpu.handles->get(hdc) == nullptr) cpu.handles->invalidHandle(hdc, api);  // 打印 + 停
    shimAbort("gdi32", api, "句柄 0x%08X 不是 GDI DC（对象类型不匹配）", hdc);
}

// 这个对象句柄是不是被**任何一个 DC** 选中（DeleteObject 的判据）
bool selectedAnywhere(const HandleTable& table, std::uint32_t object) {
    const std::uint32_t end =
        HandleTable::kFirst + HandleTable::kStep * static_cast<std::uint32_t>(table.size());
    for (std::uint32_t value = HandleTable::kFirst; value < end; value += HandleTable::kStep) {
        const HandleEntry* entry = table.get(value);
        if (entry == nullptr || entry->kind != HandleKind::GdiDc || entry->payload == nullptr) continue;
        const auto* dc = static_cast<const GdiDcObject*>(entry->payload);
        if (dc->state.font_handle == object || dc->state.bitmap_handle == object) return true;
    }
    return false;
}

void shimGetStockObject(X86&, ShimCall& call) {
    const std::int32_t index = call.i32(0);
    const std::uint32_t handle = gdiStockObject(index);
    if (handle == 0) {
        shimLog("gdi32", "GetStockObject", "index=%d 不在 0..%d ⇒ NULL", index, kStockObjectMax - 1);
        call.result = 0;
        return;
    }
    call.result = handle;
    shimLog("gdi32", "GetStockObject", "index=%d ⇒ stock 0x%08X", index, handle);
}

void shimSelectObject(X86& cpu, ShimCall& call) {
    const std::uint32_t hdc = call.u32(0);
    const std::uint32_t object = call.u32(1);
    GdiDcObject* dc = requireDc(cpu, hdc, "SelectObject");

    const int stock = gdiStockIndex(object);
    if (stock >= 0) {
        if (gdiIsStockFont(stock)) {
            const std::uint32_t old = dc->state.font_handle;
            dc->state.font = gdiDefaultFontDesc();
            dc->state.font_handle = object;
            shimLog("gdi32", "SelectObject", "hdc=0x%08X ← stock 字体 %d；返回旧对象 0x%08X", hdc, stock,
                    old);
            call.result = old;
            return;
        }
        std::uint32_t* slot = nullptr;
        if (stock <= 5) {
            slot = &dc->state.brush_handle;
        } else if (stock <= 8) {
            slot = &dc->state.pen_handle;
        } else if (stock == 15) {
            slot = &dc->state.palette_handle;
        } else if (stock == 18) {
            slot = &dc->state.brush_handle;
        } else if (stock == 19) {
            slot = &dc->state.pen_handle;
        }
        if (slot != nullptr) {
            const std::uint32_t old = *slot;
            *slot = object;
            shimLog("gdi32", "SelectObject",
                    "hdc=0x%08X ← stock %d（画笔 / 刷子 / 调色板：只记句柄，本模拟器不画它们）；"
                    "返回旧对象 0x%08X",
                    hdc, stock, old);
            call.result = old;
            return;
        }
        shimLog("gdi32", "SelectObject", "stock index=%d 未建模 ⇒ 0（Win32 失败值）", stock);
        call.result = 0;
        return;
    }

    HandleEntry* entry = cpu.handles->get(object);
    if (entry == nullptr) {
        if (object == 0) {
            shimLog("gdi32", "SelectObject",
                    "hdc=0x%08X ← NULL ⇒ 0（Win32 失败值；DC 当前对象不变）", hdc);
        } else {
            // ★ 已观测到的 guest 形态（vertical-font cache，0x0045A0F1..0x0045A104）：
            //   SelectObject(hdc, stock) → DeleteObject(cached) → SelectObject(hdc, **cached**)
            //   ⇒ 第二次 select 的是刚删掉的字形（vertical `@ＭＳ ゴシック`）。Win32 同样返回 NULL；
            //   ⛔ 这里不复活已删对象（那会掩盖 guest 的 use-after-delete，并把句柄生命周期变成假的）。
            shimLog("gdi32", "SelectObject",
                    "hdc=0x%08X ← 未知 / 已删除对象 0x%08X ⇒ 0（Win32 失败值；DC 当前对象不变）",
                    hdc, object);
        }
        call.result = 0;
        return;
    }
    if (entry->kind == HandleKind::GdiFont && entry->payload != nullptr) {
        const auto* font = static_cast<const GdiFontObject*>(entry->payload);
        const std::uint32_t old = dc->state.font_handle;
        dc->state.font = font->desc;
        dc->state.font_handle = object;
        shimLog("gdi32", "SelectObject", "hdc=0x%08X ← 字体 0x%08X（lfHeight=%d weight=%d）；旧对象 0x%08X",
                hdc, object, font->desc.height, font->desc.weight, old);
        call.result = old;
        return;
    }
    if (entry->kind == HandleKind::GdiBitmap && entry->payload != nullptr) {
        const std::uint32_t old = dc->state.bitmap_handle;
        dc->state.bitmap_handle = object;
        shimLog("gdi32", "SelectObject", "hdc=0x%08X ← 位图 0x%08X；旧对象 0x%08X", hdc, object, old);
        call.result = old;
        return;
    }
    shimLog("gdi32", "SelectObject", "句柄 0x%08X（%s）不是可选中对象 ⇒ 0", object,
            handleKindName(entry->kind));
    call.result = 0;
}

void shimDeleteObject(X86& cpu, ShimCall& call) {
    const std::uint32_t object = call.u32(0);
    if (gdiStockIndex(object) >= 0) {
        // Win32：stock object 不是调用方拥有的 ⇒ DeleteObject 返回 FALSE（GetLastError 不变）。
        shimLog("gdi32", "DeleteObject", "stock object 0x%08X ⇒ FALSE（Win32 语义）", object);
        call.result = 0;
        return;
    }
    HandleEntry* entry = cpu.handles->get(object);
    if (entry == nullptr) {
        // 真机同样返回 FALSE；这里留一行日志（⛔ 不静默，但也不把引擎的探测性删除变成硬错）。
        shimLog("gdi32", "DeleteObject", "无效对象句柄 0x%08X ⇒ FALSE（Win32 语义）", object);
        call.result = 0;
        return;
    }
    if (entry->kind == HandleKind::GdiDc) {
        shimLog("gdi32", "DeleteObject", "0x%08X 是 DC ⇒ FALSE（DeleteDC 才删 DC）", object);
        call.result = 0;
        return;
    }
    // ★ P4：region（`Create*Rgn`）也是可删除的 GDI 对象 —— 释放走 `releasePayload` 的 destroy 回调
    //   （`gdi32_region.cpp` 注册的 `delete GdiRegion`），本函数只放宽种类判定。
    if (entry->kind != HandleKind::GdiFont && entry->kind != HandleKind::GdiBitmap &&
        entry->kind != HandleKind::GdiRegion) {
        shimLog("gdi32", "DeleteObject", "0x%08X（%s）不是可删除的 GDI 对象 ⇒ FALSE", object,
                handleKindName(entry->kind));
        call.result = 0;
        return;
    }
    if (selectedAnywhere(*cpu.handles, object)) {
        // ★ 明确处置：仍被 DC 选中 ⇒ FALSE；对象**不释放**（⛔ 不许让 DC 里留野指针）。
        //   引擎若要用"先选回旧对象再删"的 Win32 习惯，这条就按真机走。
        shimLog("gdi32", "DeleteObject", "对象 0x%08X 仍被某个 DC 选中 ⇒ FALSE（不释放）", object);
        call.result = 0;
        return;
    }
    releasePayload(*cpu.handles, object);
    cpu.handles->close(object);
    shimLog("gdi32", "DeleteObject", "删除对象 0x%08X（%s）", object, handleKindName(entry->kind));
    call.result = 1;
}

void shimDeleteDc(X86& cpu, ShimCall& call) {
    const std::uint32_t hdc = call.u32(0);
    GdiDcObject* dc = requireDc(cpu, hdc, "DeleteDC");
    (void)dc;
    releasePayload(*cpu.handles, hdc);
    cpu.handles->close(hdc);
    shimLog("gdi32", "DeleteDC", "删除 DC 0x%08X", hdc);
    call.result = 1;
}

void shimGetCurrentObject(X86& cpu, ShimCall& call) {
    const std::uint32_t hdc = call.u32(0);
    const std::uint32_t type = call.u32(1);
    GdiDcObject* dc = requireDc(cpu, hdc, "GetCurrentObject");
    std::uint32_t result = 0;
    switch (type) {
        case kObjFont:
            // 真机上 DC 一定有一个字体；我们的缺省是 stock DEFAULT_GUI_FONT。
            result = dc->state.font_handle != 0 ? dc->state.font_handle
                                                : gdiStockObject(static_cast<int>(kDefaultGuiFont));
            break;
        case kObjBitmap: result = dc->state.bitmap_handle; break;
        case kObjPen: result = dc->state.pen_handle; break;
        case kObjBrush: result = dc->state.brush_handle; break;
        case kObjPal: result = dc->state.palette_handle; break;
        default:
            shimLog("gdi32", "GetCurrentObject", "未知对象类型 %u ⇒ 0", type);
            call.result = 0;
            return;
    }
    shimLog("gdi32", "GetCurrentObject", "hdc=0x%08X type=%u ⇒ 0x%08X", hdc, type, result);
    call.result = result;
}

void shimCreateICA(X86& cpu, ShimCall& call) {
    const std::string driver = call.str(0);
    const std::string device = call.str(1);
    if (cpu.handles == nullptr) shimAbort("gdi32", "CreateICA", "cpu.handles 为空");
    // Win32：pszDriver == NULL / "DISPLAY" 都是显示 DC；本模拟器**没有**打印机等其它设备。
    const bool display = driver.empty() || driver == "DISPLAY" || driver == "display";
    if (!display) {
        shimLog("gdi32", "CreateICA", "driver=\"%s\" 不是 DISPLAY ⇒ 仍建屏幕 DC（平台偏差，登记在 SCOPE）",
                driver.c_str());
    }
    const std::uint32_t hdc =
        gdiCreateDc(*cpu.handles, display, device.empty() ? std::string("DISPLAY") : device);
    if (hdc == 0) {
        call.result = 0;
        shimAbort("gdi32", "CreateICA", "建 DC 失败（句柄表分配不出来）");
    }
    shimLog("gdi32", "CreateICA", "driver=\"%s\" device=\"%s\" ⇒ hdc=0x%08X（屏幕 DC；像素不落宿主）",
            driver.c_str(), device.c_str(), hdc);
    call.result = hdc;
}

void shimSetBkMode(X86& cpu, ShimCall& call) {
    const std::uint32_t hdc = call.u32(0);
    const std::int32_t mode = call.i32(1);
    GdiDcObject* dc = requireDc(cpu, hdc, "SetBkMode");
    const std::int32_t old = dc->state.bk_mode;
    if (mode != kTransparent && mode != kOpaque) {
        shimLog("gdi32", "SetBkMode", "mode=%d 非法（只认 TRANSPARENT=1 / OPAQUE=2）⇒ 0", mode);
        call.result = 0;
        return;
    }
    dc->state.bk_mode = mode;
    shimLog("gdi32", "SetBkMode", "hdc=0x%08X mode=%d；旧值 %d", hdc, mode, old);
    call.result = static_cast<std::uint32_t>(old);
}

void shimSetTextColor(X86& cpu, ShimCall& call) {
    const std::uint32_t hdc = call.u32(0);
    const std::uint32_t color = call.u32(1);
    GdiDcObject* dc = requireDc(cpu, hdc, "SetTextColor");
    const std::uint32_t old = dc->state.text_color;
    dc->state.text_color = color;
    shimLog("gdi32", "SetTextColor", "hdc=0x%08X color=%06X；旧值 %06X", hdc, color, old);
    call.result = old;
}

}  // namespace

void registerGdiObjectShims(ShimRegistry& s) {
    s.add("gdi32", "CreateICA", Conv::Stdcall, 4, &shimCreateICA);
    s.add("gdi32", "GetStockObject", Conv::Stdcall, 1, &shimGetStockObject);
    s.add("gdi32", "SelectObject", Conv::Stdcall, 2, &shimSelectObject);
    s.add("gdi32", "DeleteObject", Conv::Stdcall, 1, &shimDeleteObject);
    s.add("gdi32", "DeleteDC", Conv::Stdcall, 1, &shimDeleteDc);
    s.add("gdi32", "GetCurrentObject", Conv::Stdcall, 2, &shimGetCurrentObject);
    s.add("gdi32", "SetBkMode", Conv::Stdcall, 2, &shimSetBkMode);
    s.add("gdi32", "SetTextColor", Conv::Stdcall, 2, &shimSetTextColor);
}

}  // namespace lift
