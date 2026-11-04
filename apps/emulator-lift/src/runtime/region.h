// emulator-lift · GDI region（矩形表 + 椭圆 / 多边形扫描线 + 布尔运算）（M2 · P4 / B7）
//
// ★ 口径（plan §5 B7 / §6 P4）：
//   · 表示 = **规范化的矩形表**：矩形互不重叠、右 / 下**不含**（Win32 RECT 语义）、
//     能合并的相邻矩形已经合并 ⇒ `type()` 才能按"恰好 1 个矩形 = SIMPLEREGION"回答。
//   · `PtInRegion` / `GetRgnBox` / `GetRegionData` **全部从这一份表示派生** ⇒ 三者永远自洽。
//   · 空区域是**合法对象**：`CreateRectRgn(x1==x2 …)` 在真机上返回**句柄**、区域为空
//     ⇒ ⛔ 不把"空"当成"没创建"（NULL 只表示**失败**）。
//   · 填充规则：多边形**只做 `ALTERNATE`（奇偶）**；`WINDING` 明确拒绝（`setPolygon` 返回 false，
//     由 shim 打一行日志并返回 Win32 失败值）—— ⛔ 不假装做了。
//   · 光栅化（椭圆 / 多边形）在**像素中心**（y + 0.5）上求交：结果确定性、可复现，
//     且用**整数**精确算（没有浮点）⇒ 换平台不会差一个像素。
//   · 坐标上限（只限**光栅化**的两个创建函数）：|坐标| ≤ `kMaxRegionCoord`。
//     理由：整数精确扫描线要求 `rx²·(ry²−Y²) ≤ 2^63`（见 region.cpp 的推导），
//     16384 给到 2^60 的余量。矩形区域不受限（只比较与复制，没有乘法）。
// ★ 本文件不碰 guest 内存 / 句柄：`shims/gdi32_region.cpp` 负责句柄与 Win32 返回值。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lift {

// 光栅化坐标上限（含端点）。屏幕是 1280×720 ⇒ 余量很大；超出一律**响亮拒绝**。
inline constexpr std::int32_t kMaxRegionCoord = 16384;

// GetRgnBox / CombineRgn / GetUpdateRgn 的返回值 —— Win32 的**整数**取值（⛔ 不用 enum class：
// guest 拿到的是 EAX，要能直接比较）。
inline constexpr std::int32_t kRgnError = 0;    // ERROR
inline constexpr std::int32_t kRgnNull = 1;     // NULLREGION（空）
inline constexpr std::int32_t kRgnSimple = 2;   // SIMPLEREGION（恰好一个矩形）
inline constexpr std::int32_t kRgnComplex = 3;  // COMPLEXREGION（多个矩形）

// CreatePolygonRgn 的填充规则（Win32 取值）。
inline constexpr std::int32_t kPolygonAlternate = 1;  // 奇偶（本实现支持）
inline constexpr std::int32_t kPolygonWinding = 2;    // 非零环绕（**未实现** ⇒ 明确失败）
inline constexpr std::int32_t kPolygonMaxCoord = kMaxRegionCoord;

// CombineRgn 的合并模式（Win32 取值；`Copy` 忽略第二个源）。
enum class RegionCombine : std::uint8_t { And, Or, Xor, Diff, Copy };

// 一个矩形（Win32 RECT 语义：右 / 下**不含**）。`empty()` 用 >=（退化矩形 = 空）。
struct RegionRect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    bool empty() const { return right <= left || bottom <= top; }
    // 两个矩形的并能否拼成**一个**矩形（相邻），用于规范化。
    friend bool operator==(const RegionRect& a, const RegionRect& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
};

struct RegionPoint {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

// 一个 GDI 区域对象。⛔ 不含句柄：句柄在 `HandleTable`（payload = 本对象）。
class GdiRegion {
public:
    // ---- 构造 ----
    void setEmpty() { rects_.clear(); }
    // `CreateRectRgn` 的语义：**不归一化**坐标 —— `x1 >= x2` 或 `y1 >= y2` ⇒ 空区域
    // （真机也不交换两端，只是"空"）。
    void setRect(std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom);
    // 包围盒内切椭圆（`CreateEllipticRgnIndirect`）；越界 / 退化 ⇒ 空。
    // 返回 false = 坐标超出 `kMaxRegionCoord`（shim 转成 Win32 失败值）。
    bool setEllipse(const RegionRect& box);
    // 多边形扫描线（像素中心、奇偶填充）。返回 false = 填充规则不支持 / 坐标越界。
    bool setPolygon(const RegionPoint* pts, std::size_t count, std::int32_t fill_mode);
    // 任意矩形列表的**并集**（`parseRegionData` 用；容忍重叠 / 未排序的输入）。
    void setFromRects(const std::vector<RegionRect>& rects);

    // ---- 布尔运算（`CombineRgn`）----
    // `Copy` ⇒ 本对象 = a（b 忽略）；`Xor` = 对称差（在两家之一里、不同时在）。
    void combine(const GdiRegion& a, const GdiRegion& b, RegionCombine op);

    // ---- 查询 ----
    bool empty() const { return rects_.empty(); }
    std::size_t size() const { return rects_.size(); }
    const std::vector<RegionRect>& rects() const { return rects_; }
    // 命中测试：右 / 下边界**不含**（Win32：`PtInRegion(r, right, y) == FALSE`）。
    bool contains(std::int32_t x, std::int32_t y) const;
    // 空区域 ⇒ 全 0 的矩形（Win32 `GetRgnBox` 对空区域写 (0,0,0,0)）。
    RegionRect bounds() const;
    std::int32_t type() const;

    // ---- `RGNDATA`（GetRegionData 的字节布局；x86 / Win64 都是 32 字节头 + 16 字节/矩形）----
    // dwSize(4) + iType(4) + rcBound(16) + RECT[n]
    static constexpr std::size_t kRegionDataHeader = 32;
    static constexpr std::size_t kRegionDataRect = 16;
    std::size_t regionDataBytes() const { return kRegionDataHeader + kRegionDataRect * rects_.size(); }
    void writeRegionData(std::vector<std::uint8_t>* out) const;
    // 反向：解析 `GetRegionData` 写出的缓冲（往返一致性的唯一判据）。
    // 返回 false = 头不合规（长度 / dwSize / iType / 矩形数）。
    static bool parseRegionData(const std::uint8_t* data, std::size_t n, GdiRegion* out);

    friend bool operator==(const GdiRegion& a, const GdiRegion& b) { return a.rects_ == b.rects_; }

private:
    // 规范化：丢掉空矩形 → 纵向合并同 x 跨度的相邻矩形 → 横向合并 → 再纵向一次（各一趟，
    // 足以覆盖扫描线 / 布尔运算产生的形态；见 region.cpp）。
    void canonicalize();
    std::vector<RegionRect> rects_;
};

}  // namespace lift
