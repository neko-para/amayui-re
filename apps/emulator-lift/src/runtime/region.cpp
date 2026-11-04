// emulator-lift · GDI region 数据结构与运算（M2 · P4 / B7）
//
// ★ 表示与不变量见 region.h。本文件只做**纯计算**（没有 guest 内存 / 句柄 / 日志）。
// ★ 算法：
//   · 布尔运算 = **y 带分解**：把两家的全部水平边排序成相邻带，每带内把矩形压成 x 区间表，
//     在区间表上做 AND/OR/DIFF/XOR ⇒ 结果天然是互不重叠的矩形，与输入顺序无关
//     （⇒ 同一组输入永远得到同一份矩形表，GetRegionData 的字节也稳定）。
//   · 椭圆 = 逐行（像素中心 y+0.5）解出 |X| ≤ ⌊√(rx²(ry²−Y²)/ry²)⌋，X = 2x+1−(l+r)；
//     整数精确（没有浮点）：`rx²(ry²−Y²) ≤ 2^60`（kMaxRegionCoord=16384 保证）。
//   · 多边形 = 偶数-奇数（ALTERNATE）扫描线，交点用**有理数**（num/den）保存，
//     排序用 int64 交叉相乘 ⇒ 边界像素不会因为浮点误差抖动。
#include "runtime/region.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace lift {
namespace {

// ---- 整数除法（C++ 的 / 向零截断，这里要的是**数学**上的 floor / ceil；b > 0）----
std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b) != 0 && a < 0) --q;
    return q;
}

std::int64_t ceilDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b) != 0 && a > 0) ++q;
    return q;
}

std::uint64_t isqrt64(std::uint64_t v) {
    if (v == 0) return 0;
    std::uint64_t r = static_cast<std::uint64_t>(std::sqrt(static_cast<double>(v)));
    while ((r + 1) * (r + 1) <= v && r < 0xFFFFFFFFu) ++r;
    while (r * r > v) --r;
    return r;
}

bool inCoordRange(std::int32_t v) {
    return v >= -kMaxRegionCoord && v <= kMaxRegionCoord;
}

// ---- 一个 y 带里的 x 区间（左闭右开；已排序、互不重叠）----
struct Interval {
    std::int32_t left = 0;
    std::int32_t right = 0;
};

std::vector<Interval> normalizeIntervals(std::vector<Interval> v) {
    std::sort(v.begin(), v.end(), [](const Interval& a, const Interval& b) {
        if (a.left != b.left) return a.left < b.left;
        return a.right < b.right;
    });
    std::vector<Interval> out;
    out.reserve(v.size());
    for (const Interval& iv : v) {
        if (iv.right <= iv.left) continue;
        if (!out.empty() && iv.left <= out.back().right) {
            // 重叠或相接 ⇒ 并成一段（相接的情况正是"两个相邻矩形其实是一个矩形"）
            if (iv.right > out.back().right) out.back().right = iv.right;
            continue;
        }
        out.push_back(iv);
    }
    return out;
}

std::vector<Interval> andIntervals(const std::vector<Interval>& a, const std::vector<Interval>& b) {
    std::vector<Interval> out;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        const std::int32_t l = a[i].left > b[j].left ? a[i].left : b[j].left;
        const std::int32_t r = a[i].right < b[j].right ? a[i].right : b[j].right;
        if (r > l) out.push_back({l, r});
        if (a[i].right < b[j].right) {
            ++i;
        } else {
            ++j;
        }
    }
    return out;  // 两端都已排序且互不重叠 ⇒ 结果同样
}

std::vector<Interval> diffIntervals(const std::vector<Interval>& a, const std::vector<Interval>& b) {
    std::vector<Interval> out;
    std::size_t j = 0;
    for (const Interval& ai : a) {
        while (j < b.size() && b[j].right <= ai.left) ++j;
        std::int32_t cur = ai.left;
        std::size_t k = j;
        while (k < b.size() && b[k].left < ai.right) {
            if (b[k].left > cur) out.push_back({cur, b[k].left < ai.right ? b[k].left : ai.right});
            if (b[k].right > cur) cur = b[k].right;
            if (cur >= ai.right) break;
            ++k;
        }
        if (cur < ai.right) out.push_back({cur, ai.right});
    }
    return out;
}

std::vector<Interval> orIntervals(const std::vector<Interval>& a, const std::vector<Interval>& b) {
    std::vector<Interval> all;
    all.reserve(a.size() + b.size());
    all.insert(all.end(), a.begin(), a.end());
    all.insert(all.end(), b.begin(), b.end());
    return normalizeIntervals(std::move(all));
}

// 某个 y 带 [y0, y1) 里，区域覆盖到的 x 区间。
std::vector<Interval> intervalsAt(const GdiRegion& r, std::int32_t y0, std::int32_t y1) {
    std::vector<Interval> v;
    for (const RegionRect& rc : r.rects()) {
        if (rc.top <= y0 && rc.bottom >= y1) v.push_back({rc.left, rc.right});
    }
    return normalizeIntervals(std::move(v));
}

}  // namespace

void GdiRegion::setRect(std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom) {
    rects_.clear();
    // ★ 不归一化坐标（真机也不交换）：退化 / 反向 ⇒ 空区域，**对象仍然存在**。
    if (right <= left || bottom <= top) return;
    rects_.push_back({left, top, right, bottom});
}

bool GdiRegion::setEllipse(const RegionRect& box) {
    rects_.clear();
    if (!inCoordRange(box.left) || !inCoordRange(box.top) || !inCoordRange(box.right) ||
        !inCoordRange(box.bottom)) {
        return false;
    }
    if (box.empty()) return true;  // 退化盒 ⇒ 空区域（不是失败）
    const std::int64_t l = box.left;
    const std::int64_t t = box.top;
    const std::int64_t r = box.right;
    const std::int64_t b = box.bottom;
    const std::int64_t rx2 = r - l;  // 双倍 x 半径
    const std::int64_t ry2 = b - t;  // 双倍 y 半径
    const std::int64_t cx2 = l + r;  // 双倍中心
    const std::int64_t cy2 = t + b;
    const std::uint64_t ry_sq = static_cast<std::uint64_t>(ry2) * static_cast<std::uint64_t>(ry2);
    const std::uint64_t rx_sq = static_cast<std::uint64_t>(rx2) * static_cast<std::uint64_t>(rx2);
    for (std::int64_t y = t; y < b; ++y) {
        const std::int64_t Y = 2 * y + 1 - cy2;
        const std::int64_t rem = ry2 * ry2 - Y * Y;  // |Y| ≤ ry2-1 ⇒ ≥ 0
        if (rem < 0) continue;
        // |X| ≤ ⌊√(rx² · rem / ry²)⌋（等价于 X²·ry² ≤ rx²·rem 的整数判定）
        const std::uint64_t q = rx_sq * static_cast<std::uint64_t>(rem) / ry_sq;
        const std::int64_t xm = static_cast<std::int64_t>(isqrt64(q));
        std::int64_t xmin = ceilDiv(cx2 - xm - 1, 2);  // 2x+1 ≥ cx2-xm
        std::int64_t xmax = floorDiv(cx2 + xm - 1, 2);  // 2x+1 ≤ cx2+xm
        if (xmin < l) xmin = l;
        if (xmax > r - 1) xmax = r - 1;
        if (xmax < xmin) continue;
        rects_.push_back({static_cast<std::int32_t>(xmin), static_cast<std::int32_t>(y),
                          static_cast<std::int32_t>(xmax + 1), static_cast<std::int32_t>(y + 1)});
    }
    canonicalize();  // 相邻行（x 跨度相同）并成一条
    return true;
}

bool GdiRegion::setPolygon(const RegionPoint* pts, std::size_t count, std::int32_t fill_mode) {
    rects_.clear();
    // ★ 只做 ALTERNATE（奇偶）。WINDING 明确拒绝：调用方要**响亮失败**，⛔ 不假装做了。
    if (fill_mode != kPolygonAlternate) return false;
    // 少于三个点 / 空数组 ⇒ 空区域（对象仍然存在；真机也不因此失败）
    if (count < 3 || pts == nullptr) return true;
    std::int32_t min_x = pts[0].x;
    std::int32_t max_x = pts[0].x;
    std::int32_t min_y = pts[0].y;
    std::int32_t max_y = pts[0].y;
    for (std::size_t i = 0; i < count; ++i) {
        if (!inCoordRange(pts[i].x) || !inCoordRange(pts[i].y)) return false;
        min_x = std::min(min_x, pts[i].x);
        max_x = std::max(max_x, pts[i].x);
        min_y = std::min(min_y, pts[i].y);
        max_y = std::max(max_y, pts[i].y);
    }
    struct Cross {
        std::int64_t num = 0;  // X = num / den（双倍坐标）
        std::int64_t den = 1;  // > 0
    };
    for (std::int64_t y = min_y; y < max_y; ++y) {
        const std::int64_t Y = 2 * y + 1;  // 像素中心的双倍 y
        std::vector<Cross> xs;
        for (std::size_t i = 0; i < count; ++i) {
            const RegionPoint& p = pts[i];
            const RegionPoint& q = pts[(i + 1) % count];
            if (p.y == q.y) continue;  // 水平边不产生交点
            // 半开规则 [ylo, yhi)：顶点只算一次（奇偶填充的标准做法）
            std::int64_t px = p.x;
            std::int64_t py = p.y;
            std::int64_t qx = q.x;
            std::int64_t qy = q.y;
            if (py > qy) {
                std::swap(px, qx);
                std::swap(py, qy);
            }
            if (y < py || y >= qy) continue;
            const std::int64_t dy = qy - py;  // > 0
            // X = 2px + (Y − 2py)·(qx−px)/dy  ⇒  通分后 num/den
            xs.push_back({2 * px * dy + (Y - 2 * py) * (qx - px), dy});
        }
        if (xs.size() < 2) continue;
        std::sort(xs.begin(), xs.end(), [](const Cross& a, const Cross& b) {
            return a.num * b.den < b.num * a.den;  // den > 0；|num·den| ≤ 2^46（见 region.h 的上限）
        });
        for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
            // 像素中心严格落在 (Xa, Xb) 内 ⇒ 2x+1 ≥ ⌊Xa⌋+1 且 2x+1 ≤ ⌈Xb⌉−1
            const std::int64_t fa = floorDiv(xs[k].num, xs[k].den);
            const std::int64_t cb = ceilDiv(xs[k + 1].num, xs[k + 1].den);
            std::int64_t xmin = ceilDiv(fa, 2);
            std::int64_t xmax = floorDiv(cb - 2, 2);
            if (xmin < min_x) xmin = min_x;
            if (xmax > max_x - 1) xmax = static_cast<std::int64_t>(max_x) - 1;
            if (xmax < xmin) continue;
            rects_.push_back({static_cast<std::int32_t>(xmin), static_cast<std::int32_t>(y),
                              static_cast<std::int32_t>(xmax + 1), static_cast<std::int32_t>(y + 1)});
        }
    }
    canonicalize();
    return true;
}

void GdiRegion::setFromRects(const std::vector<RegionRect>& rects) {
    GdiRegion acc;
    for (const RegionRect& r : rects) {
        if (r.empty()) continue;
        GdiRegion one;
        one.setRect(r.left, r.top, r.right, r.bottom);
        GdiRegion next;
        next.combine(acc, one, RegionCombine::Or);
        acc = next;
    }
    rects_ = acc.rects_;
}

void GdiRegion::combine(const GdiRegion& a, const GdiRegion& b, RegionCombine op) {
    std::vector<std::int32_t> ys;
    ys.reserve(2 * (a.rects_.size() + b.rects_.size()));
    for (const RegionRect& r : a.rects_) {
        ys.push_back(r.top);
        ys.push_back(r.bottom);
    }
    for (const RegionRect& r : b.rects_) {
        ys.push_back(r.top);
        ys.push_back(r.bottom);
    }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

    std::vector<RegionRect> out;
    for (std::size_t i = 0; i + 1 < ys.size(); ++i) {
        const std::int32_t y0 = ys[i];
        const std::int32_t y1 = ys[i + 1];
        if (y1 <= y0) continue;
        const std::vector<Interval> ia = intervalsAt(a, y0, y1);
        const std::vector<Interval> ib = intervalsAt(b, y0, y1);
        std::vector<Interval> res;
        switch (op) {
            case RegionCombine::And:
                res = andIntervals(ia, ib);
                break;
            case RegionCombine::Or:
                res = orIntervals(ia, ib);
                break;
            case RegionCombine::Xor:
                // ★ 语义（Win32 `RGN_XOR`）：**对称差** —— 在 src1 或 src2 里，但不同时在。
                res = orIntervals(diffIntervals(ia, ib), diffIntervals(ib, ia));
                break;
            case RegionCombine::Diff:
                // ★ `RGN_DIFF`：src1 − src2（**不是**对称差，也不是 AND）。
                res = diffIntervals(ia, ib);
                break;
            case RegionCombine::Copy:
                res = ia;  // src2 被忽略（Win32 语义）
                break;
        }
        for (const Interval& iv : res) out.push_back({iv.left, y0, iv.right, y1});
    }
    rects_ = std::move(out);
    canonicalize();
}

bool GdiRegion::contains(std::int32_t x, std::int32_t y) const {
    // ★ 右 / 下**不含**：`x < right && y < bottom`（Win32 的 PtInRegion 就是半开区间）。
    for (const RegionRect& r : rects_) {
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return true;
    }
    return false;
}

RegionRect GdiRegion::bounds() const {
    if (rects_.empty()) return RegionRect{};  // 空区域 ⇒ (0,0,0,0)（Win32 GetRgnBox 真值）
    RegionRect b = rects_[0];
    for (const RegionRect& r : rects_) {
        if (r.left < b.left) b.left = r.left;
        if (r.top < b.top) b.top = r.top;
        if (r.right > b.right) b.right = r.right;
        if (r.bottom > b.bottom) b.bottom = r.bottom;
    }
    return b;
}

std::int32_t GdiRegion::type() const {
    if (rects_.empty()) return kRgnNull;
    return rects_.size() == 1 ? kRgnSimple : kRgnComplex;
}

void GdiRegion::writeRegionData(std::vector<std::uint8_t>* out) const {
    if (out == nullptr) return;
    const std::size_t bytes = regionDataBytes();
    out->assign(bytes, 0);
    auto put32 = [out](std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) (*out)[off + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
    };
    // RGNDATA：dwSize / iType(RDH_RECTANGLES=1) / rcBound / RECT[]
    put32(0, static_cast<std::uint32_t>(bytes));
    put32(4, 1);
    const RegionRect b = bounds();
    put32(8, static_cast<std::uint32_t>(b.left));
    put32(12, static_cast<std::uint32_t>(b.top));
    put32(16, static_cast<std::uint32_t>(b.right));
    put32(20, static_cast<std::uint32_t>(b.bottom));
    for (std::size_t i = 0; i < rects_.size(); ++i) {
        const std::size_t off = kRegionDataHeader + kRegionDataRect * i;
        put32(off, static_cast<std::uint32_t>(rects_[i].left));
        put32(off + 4, static_cast<std::uint32_t>(rects_[i].top));
        put32(off + 8, static_cast<std::uint32_t>(rects_[i].right));
        put32(off + 12, static_cast<std::uint32_t>(rects_[i].bottom));
    }
}

bool GdiRegion::parseRegionData(const std::uint8_t* data, std::size_t n, GdiRegion* out) {
    if (data == nullptr || out == nullptr) return false;
    if (n < kRegionDataHeader) return false;
    auto rd32 = [data](std::size_t off) {
        return static_cast<std::uint32_t>(data[off]) |
               (static_cast<std::uint32_t>(data[off + 1]) << 8) |
               (static_cast<std::uint32_t>(data[off + 2]) << 16) |
               (static_cast<std::uint32_t>(data[off + 3]) << 24);
    };
    const std::uint32_t size = rd32(0);
    if (rd32(4) != 1) return false;  // 只认 RDH_RECTANGLES
    if (size < kRegionDataHeader) return false;
    if (size > n) return false;
    if ((size - kRegionDataHeader) % kRegionDataRect != 0) return false;
    const std::size_t count = (size - kRegionDataHeader) / kRegionDataRect;
    std::vector<RegionRect> rects;
    rects.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t off = kRegionDataHeader + kRegionDataRect * i;
        RegionRect r;
        r.left = static_cast<std::int32_t>(rd32(off));
        r.top = static_cast<std::int32_t>(rd32(off + 4));
        r.right = static_cast<std::int32_t>(rd32(off + 8));
        r.bottom = static_cast<std::int32_t>(rd32(off + 12));
        rects.push_back(r);
    }
    out->setFromRects(rects);
    return true;
}

void GdiRegion::canonicalize() {
    std::vector<RegionRect> out;
    out.reserve(rects_.size());
    for (const RegionRect& r : rects_) {
        if (!r.empty()) out.push_back(r);
    }
    auto sortByTopLeft = [](std::vector<RegionRect>& v) {
        std::sort(v.begin(), v.end(), [](const RegionRect& a, const RegionRect& b) {
            if (a.top != b.top) return a.top < b.top;
            if (a.left != b.left) return a.left < b.left;
            if (a.bottom != b.bottom) return a.bottom < b.bottom;
            return a.right < b.right;
        });
    };
    // 纵向合并：同一 (left,right) 且上一条正好以当前 top 结束 ⇒ 延长。
    // 按 top 升序扫一遍，用 map 记住"每种 x 跨度最近的一条"（更早的只会更早结束 ⇒ 不可能接上）。
    auto mergeVertical = [](std::vector<RegionRect>& v) {
        std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> open;
        std::vector<RegionRect> merged;
        merged.reserve(v.size());
        for (const RegionRect& r : v) {
            const std::pair<std::int32_t, std::int32_t> key{r.left, r.right};
            const auto it = open.find(key);
            if (it != open.end() && merged[it->second].bottom == r.top) {
                merged[it->second].bottom = r.bottom;
                continue;
            }
            open[key] = merged.size();
            merged.push_back(r);
        }
        v.swap(merged);
    };
    // 横向合并：同一 (top,bottom) 且左条右端 == 右条左端 ⇒ 拼成一条。
    auto mergeHorizontal = [](std::vector<RegionRect>& v) {
        std::sort(v.begin(), v.end(), [](const RegionRect& a, const RegionRect& b) {
            if (a.top != b.top) return a.top < b.top;
            if (a.bottom != b.bottom) return a.bottom < b.bottom;
            return a.left < b.left;
        });
        std::vector<RegionRect> merged;
        merged.reserve(v.size());
        for (const RegionRect& r : v) {
            if (!merged.empty()) {
                RegionRect& back = merged.back();
                if (back.top == r.top && back.bottom == r.bottom && back.right == r.left) {
                    back.right = r.right;
                    continue;
                }
            }
            merged.push_back(r);
        }
        v.swap(merged);
    };
    sortByTopLeft(out);
    mergeVertical(out);
    mergeHorizontal(out);
    mergeVertical(out);  // 横向合并可能让上下两条的 x 跨度变得相同 ⇒ 再纵向一趟
    sortByTopLeft(out);
    rects_.swap(out);
}

}  // namespace lift
