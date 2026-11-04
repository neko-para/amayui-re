// emulator-lift · GDI32 region 族（M2 · P4 / §5 B7）
//
// 七条：CreateRectRgn · CreateEllipticRgnIndirect · CreatePolygonRgn · CombineRgn ·
//       GetRegionData · GetRgnBox · PtInRegion
//
// ★ 口径（plan §5 B7 / §6 P4）：
//   · 区域对象走现有 `HandleTable`（`HandleKind::GdiRegion`，payload = `runtime/region.h` 的 `GdiRegion`）
//     ⇒ `DeleteObject` 能删它（释放点在 `HandleTable::releasePayload` 的 destroy 回调里）。
//   · 空矩形是**合法区域**：`CreateRectRgn(x1==x2 …)` 在真机上返回**句柄**、区域为空
//     ⇒ 这里同样返回句柄（⛔ 不返回 NULL 表示"没创建"），并打一行日志说明。
//   · 未实现形态**响亮失败**：`CreatePolygonRgn(WINDING)` 只打日志 + NULL —— 本实现只做
//     `ALTERNATE`（奇偶），⛔ 不把 WINDING 当 ALTERNATE 糊过去。
//   · 无效 / 类型不对的句柄：Win32 这几条都有**文档化的失败值**（NULL / 0 / ERROR / FALSE），
//     且引擎会把它们当探测用 ⇒ 打一行日志后返回失败值（与 `DeleteObject` 的处理一致），
//     ⛔ 不 abort（那会把"探测"变成硬错），也 ⛔ 不静默。
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/region.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::int32_t kRgnAnd = 1;
constexpr std::int32_t kRgnOr = 2;
constexpr std::int32_t kRgnXor = 3;
constexpr std::int32_t kRgnDiff = 4;
constexpr std::int32_t kRgnCopy = 5;

const char* combineName(std::int32_t mode) {
    switch (mode) {
        case kRgnAnd: return "RGN_AND";
        case kRgnOr: return "RGN_OR";
        case kRgnXor: return "RGN_XOR";
        case kRgnDiff: return "RGN_DIFF";
        case kRgnCopy: return "RGN_COPY";
        default: return "?";
    }
}

void destroyRegion(void* payload) { delete static_cast<GdiRegion*>(payload); }

// 新建一个区域对象并返回 guest 句柄。⛔ guest 看不到宿主指针。
std::uint32_t createRegion(X86& cpu, const GdiRegion& region, const char* name) {
    if (cpu.handles == nullptr) shimAbort("gdi32", name, "cpu.handles 为空（Machine 没装配）");
    auto* payload = new GdiRegion(region);
    const std::uint32_t h = cpu.handles->add(HandleKind::GdiRegion, name, payload);
    if (HandleEntry* entry = cpu.handles->get(h)) entry->destroy = &destroyRegion;
    return h;
}

// 取区域对象。失败（不存在 / 类型不对）⇒ 打一行 + nullptr（调用方返回 Win32 失败值）。
GdiRegion* regionOf(X86& cpu, std::uint32_t h, const char* api) {
    if (cpu.handles == nullptr) shimAbort("gdi32", api, "cpu.handles 为空（Machine 没装配）");
    HandleEntry* entry = cpu.handles->get(h);
    if (entry == nullptr) {
        shimLog("gdi32", api, "无效区域句柄 0x%08X ⇒ Win32 失败值", h);
        return nullptr;
    }
    if (entry->kind != HandleKind::GdiRegion || entry->payload == nullptr) {
        shimLog("gdi32", api, "句柄 0x%08X（%s）不是 region ⇒ Win32 失败值", h, handleKindName(entry->kind));
        return nullptr;
    }
    return static_cast<GdiRegion*>(entry->payload);
}

void logCreated(const char* api, std::uint32_t h, const GdiRegion& r) {
    const RegionRect b = r.bounds();
    shimLog("gdi32", api, "⇒ 0x%08X（%s；%zu 个矩形；包围盒 (%d,%d)-(%d,%d)）", h,
            r.empty() ? "空区域（合法：真机同样返回句柄）" : "非空", r.size(), b.left, b.top, b.right, b.bottom);
}

RegionRect readRectAt(X86& cpu, std::uint32_t ea, bool* ok) {
    RegionRect out;
    *ok = ea != 0 && cpu.mem->committed(ea, 16);
    if (!*ok) return out;
    out.left = static_cast<std::int32_t>(cpu.mem->rd32(ea));
    out.top = static_cast<std::int32_t>(cpu.mem->rd32(ea + 4));
    out.right = static_cast<std::int32_t>(cpu.mem->rd32(ea + 8));
    out.bottom = static_cast<std::int32_t>(cpu.mem->rd32(ea + 12));
    return out;
}

// ---- CreateRectRgn(x1, y1, x2, y2) ----
void shimCreateRectRgn(X86& cpu, ShimCall& call) {
    GdiRegion r;
    r.setRect(call.i32(0), call.i32(1), call.i32(2), call.i32(3));
    const std::uint32_t h = createRegion(cpu, r, "rgn-rect");
    logCreated("CreateRectRgn", h, r);
    call.result = h;
}

// ---- CreateEllipticRgnIndirect(const RECT*) ----
void shimCreateEllipticRgnIndirect(X86& cpu, ShimCall& call) {
    bool ok = false;
    const RegionRect box = readRectAt(cpu, call.u32(0), &ok);
    if (!ok) {
        shimLog("gdi32", "CreateEllipticRgnIndirect", "RECT 指针 0x%08X 不可读 ⇒ NULL（Win32 失败值）",
                call.u32(0));
        call.result = 0;
        return;
    }
    GdiRegion r;
    if (!r.setEllipse(box)) {
        // ★ 光栅化用整数精确扫描线 ⇒ 坐标有上限（见 runtime/region.h）。超出就**如实拒绝**。
        shimLog("gdi32", "CreateEllipticRgnIndirect",
                "RECT (%d,%d)-(%d,%d) 超出 ±%d ⇒ NULL（整数精确扫描线的坐标上限）", box.left, box.top,
                box.right, box.bottom, kMaxRegionCoord);
        call.result = 0;
        return;
    }
    const std::uint32_t h = createRegion(cpu, r, "rgn-ellipse");
    logCreated("CreateEllipticRgnIndirect", h, r);
    call.result = h;
}

// ---- CreatePolygonRgn(const POINT*, int count, int fillMode) ----
void shimCreatePolygonRgn(X86& cpu, ShimCall& call) {
    const std::uint32_t pts_ea = call.u32(0);
    const std::int32_t count = call.i32(1);
    const std::int32_t mode = call.i32(2);
    if (mode == kPolygonWinding) {
        // ★ 未实现形态必须响亮：只做 ALTERNATE（奇偶）。自交多边形在 WINDING 下结果不同 ⇒
        //   ⛔ 不拿 ALTERNATE 冒充（那会静默给出错误的命中测试）。
        shimLog("gdi32", "CreatePolygonRgn",
                "填充规则 WINDING 未实现（自交多边形只支持 ALTERNATE 奇偶）⇒ NULL");
        call.result = 0;
        return;
    }
    if (mode != kPolygonAlternate) {
        shimLog("gdi32", "CreatePolygonRgn", "填充规则 %d 非法（只认 ALTERNATE=1 / WINDING=2）⇒ NULL", mode);
        call.result = 0;
        return;
    }
    if (count < 0) {
        shimLog("gdi32", "CreatePolygonRgn", "count=%d 非法 ⇒ NULL", count);
        call.result = 0;
        return;
    }
    std::vector<RegionPoint> pts;
    if (count > 0) {
        const std::size_t bytes = static_cast<std::size_t>(count) * 8u;  // POINT = 2 × LONG
        if (pts_ea == 0 || !cpu.mem->committed(pts_ea, bytes)) {
            shimLog("gdi32", "CreatePolygonRgn", "POINT 数组 0x%08X × %d 不可读 ⇒ NULL", pts_ea, count);
            call.result = 0;
            return;
        }
        pts.resize(static_cast<std::size_t>(count));
        for (std::int32_t i = 0; i < count; ++i) {
            const std::uint32_t at = pts_ea + static_cast<std::uint32_t>(i) * 8u;
            pts[static_cast<std::size_t>(i)].x = static_cast<std::int32_t>(cpu.mem->rd32(at));
            pts[static_cast<std::size_t>(i)].y = static_cast<std::int32_t>(cpu.mem->rd32(at + 4));
        }
    }
    GdiRegion r;
    if (!r.setPolygon(pts.empty() ? nullptr : pts.data(), pts.size(), mode)) {
        shimLog("gdi32", "CreatePolygonRgn", "多边形超出 ±%d 的坐标上限 ⇒ NULL", kMaxRegionCoord);
        call.result = 0;
        return;
    }
    const std::uint32_t h = createRegion(cpu, r, "rgn-polygon");
    logCreated("CreatePolygonRgn", h, r);
    call.result = h;
}

// ---- CombineRgn(HRGN dst, HRGN src1, HRGN src2, int mode) ----
// 返回 NULLREGION / SIMPLEREGION / COMPLEXREGION / ERROR（dst 被**就地**改写）。
void shimCombineRgn(X86& cpu, ShimCall& call) {
    GdiRegion* dst = regionOf(cpu, call.u32(0), "CombineRgn");
    if (dst == nullptr) {
        call.result = static_cast<std::uint32_t>(kRgnError);
        return;
    }
    const std::int32_t mode = call.i32(3);
    RegionCombine op = RegionCombine::Copy;
    switch (mode) {
        case kRgnAnd: op = RegionCombine::And; break;
        case kRgnOr: op = RegionCombine::Or; break;
        case kRgnXor: op = RegionCombine::Xor; break;
        case kRgnDiff: op = RegionCombine::Diff; break;
        case kRgnCopy: op = RegionCombine::Copy; break;
        default:
            shimLog("gdi32", "CombineRgn", "合并模式 %d 非法 ⇒ ERROR", mode);
            call.result = static_cast<std::uint32_t>(kRgnError);
            return;
    }
    GdiRegion* src1 = regionOf(cpu, call.u32(1), "CombineRgn");
    if (src1 == nullptr) {
        call.result = static_cast<std::uint32_t>(kRgnError);
        return;
    }
    GdiRegion empty;
    GdiRegion* src2 = &empty;  // RGN_COPY 忽略 src2（Win32 语义）
    if (op != RegionCombine::Copy) {
        src2 = regionOf(cpu, call.u32(2), "CombineRgn");
        if (src2 == nullptr) {
            call.result = static_cast<std::uint32_t>(kRgnError);
            return;
        }
    }
    GdiRegion result;
    result.combine(*src1, *src2, op);
    *dst = result;
    shimLog("gdi32", "CombineRgn", "dst=0x%08X src1=0x%08X src2=0x%08X %s ⇒ %d（%zu 个矩形）", call.u32(0),
            call.u32(1), call.u32(2), combineName(mode), dst->type(), dst->size());
    call.result = static_cast<std::uint32_t>(dst->type());
}

// ---- GetRegionData(HRGN, DWORD nCount, LPRGNDATA) ----
// 缓冲够 ⇒ 写 RGNDATA 并返回 1；不够 / 传 NULL ⇒ 返回**所需字节数**（真机的探测用法）。
void shimGetRegionData(X86& cpu, ShimCall& call) {
    GdiRegion* r = regionOf(cpu, call.u32(0), "GetRegionData");
    if (r == nullptr) {
        call.result = 0;
        return;
    }
    std::vector<std::uint8_t> bytes;
    r->writeRegionData(&bytes);
    const std::uint32_t need = static_cast<std::uint32_t>(bytes.size());
    const std::uint32_t ncount = call.u32(1);
    const std::uint32_t dst = call.u32(2);
    if (dst == 0 || ncount < need) {
        shimLog("gdi32", "GetRegionData", "0x%08X：需要 %u 字节（nCount=%u，lpRgnData=0x%08X）⇒ 返回所需字节数",
                call.u32(0), need, ncount, dst);
        call.result = need;
        return;
    }
    if (!cpu.mem->committed(dst, need)) {
        shimLog("gdi32", "GetRegionData", "缓冲 0x%08X × %u 不可写 ⇒ 0（Win32 失败值）", dst, need);
        call.result = 0;
        return;
    }
    cpu.mem->writeBytes(dst, bytes.data(), bytes.size());
    shimLog("gdi32", "GetRegionData", "0x%08X ⇒ %u 字节（%zu 个 RECT）", call.u32(0), need, r->size());
    call.result = 1;  // 真机：缓冲足够时返回**非零**（1）
}

// ---- GetRgnBox(HRGN, LPRECT) ----
void shimGetRgnBox(X86& cpu, ShimCall& call) {
    GdiRegion* r = regionOf(cpu, call.u32(0), "GetRgnBox");
    if (r == nullptr) {
        call.result = static_cast<std::uint32_t>(kRgnError);
        return;
    }
    const std::uint32_t ea = call.u32(1);
    if (ea == 0 || !cpu.mem->committed(ea, 16)) {
        shimLog("gdi32", "GetRgnBox", "LPRECT 0x%08X 不可写 ⇒ ERROR", ea);
        call.result = static_cast<std::uint32_t>(kRgnError);
        return;
    }
    const RegionRect b = r->bounds();  // 空区域 ⇒ (0,0,0,0)（真机真值）
    cpu.mem->wr32(ea, static_cast<std::uint32_t>(b.left));
    cpu.mem->wr32(ea + 4, static_cast<std::uint32_t>(b.top));
    cpu.mem->wr32(ea + 8, static_cast<std::uint32_t>(b.right));
    cpu.mem->wr32(ea + 12, static_cast<std::uint32_t>(b.bottom));
    call.result = static_cast<std::uint32_t>(r->type());
}

// ---- PtInRegion(HRGN, int x, int y) ----
void shimPtInRegion(X86& cpu, ShimCall& call) {
    GdiRegion* r = regionOf(cpu, call.u32(0), "PtInRegion");
    if (r == nullptr) {
        call.result = 0;
        return;
    }
    // ★ 右 / 下边界**不含**（Win32：x < right、y < bottom）。
    call.result = r->contains(call.i32(1), call.i32(2)) ? 1u : 0u;
}

}  // namespace

void registerGdiRegionShims(ShimRegistry& shims) {
    // 顺序 = `ShimDef::id` = trace 顺序（改动它 = 改 trace ⇒ 是判据的一部分）。
    shims.add("gdi32", "CreateRectRgn", Conv::Stdcall, 4, &shimCreateRectRgn);
    shims.add("gdi32", "CreateEllipticRgnIndirect", Conv::Stdcall, 1, &shimCreateEllipticRgnIndirect);
    shims.add("gdi32", "CreatePolygonRgn", Conv::Stdcall, 3, &shimCreatePolygonRgn);
    shims.add("gdi32", "CombineRgn", Conv::Stdcall, 4, &shimCombineRgn);
    shims.add("gdi32", "GetRegionData", Conv::Stdcall, 3, &shimGetRegionData);
    shims.add("gdi32", "GetRgnBox", Conv::Stdcall, 2, &shimGetRgnBox);
    shims.add("gdi32", "PtInRegion", Conv::Stdcall, 3, &shimPtInRegion);
}

}  // namespace lift
