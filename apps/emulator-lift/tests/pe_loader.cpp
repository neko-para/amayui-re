// emulator-lift · PE 载入的坏输入（pure：自造最小 PE 字节，不碰游戏 / LFS）
//
// ★ 判据（缺陷单 REQ-01M6JFZFCQ956CBFMKKQ37PCYK）：坏节 / 坏导入必须**带原因拒绝启动**，
//   ⛔ 不许"静默跳过、半载入仍返回 true"。旧实现：`mapPeImage` 越界就不拷贝；导入表解析
//   对坏 RVA 只 `continue/break` ⇒ A5 的"缺口 = 0"会假绿（缺口被解析器吃掉，IAT 保留 PE 原值）。
// ★ 本单的动机是 M2 要按 PE 资源目录读图标 / 版本资源：半载入会让资源解析指到错的地方，
//   所以"拒绝"必须和 sha256 不符**同档**，并且拒绝时 guest 里**一个字节都不许写**。
// ★ 两条硬约束（⛔ 别走过头把正常形态判成坏）：
//   ① `SizeOfRawData < VirtualSize` **合法**（BSS 尾巴 / 资源虚拟尾部，文件里本来就没有这些字节）
//      ⇒ `virtual-tail-ok` 必须绿；
//   ② `SizeOfRawData > VirtualSize` 也合法（FileAlignment 的填充）⇒ 只拷 min(raw, vsize) 字节。
// ★ 错误信息按**定宽十六进制**断言（`0x000003F0` 这类字面量），所以"原因写出来了"是可机械校验的，
//   而且不依赖中文措辞。
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/image.h"
#include "runtime/memory.h"

namespace {

constexpr std::uint32_t kPeOff = 0x40;
constexpr std::uint32_t kOptOff = kPeOff + 24;  // 0x58
constexpr std::uint32_t kSecOff = kOptOff + 224;

constexpr std::uint32_t kImageBase = 0x00400000u;

void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    std::memcpy(b.data() + off, &v, 2);
}
void put32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    std::memcpy(b.data() + off, &v, 4);
}

// 最小 PE32：1 个 .rdata 节（RVA 0x1000 / raw 0x200），导入 x.dll!Func。
std::vector<std::uint8_t> makePe() {
    std::vector<std::uint8_t> b(0x400, 0);
    b[0] = 'M';
    b[1] = 'Z';
    put32(b, 0x3C, kPeOff);
    b[kPeOff] = 'P';
    b[kPeOff + 1] = 'E';
    put16(b, kPeOff + 6, 1);              // NumberOfSections
    put16(b, kPeOff + 20, 224);           // SizeOfOptionalHeader
    put16(b, kOptOff, 0x010B);            // PE32
    put32(b, kOptOff + 16, 0x1000);       // AddressOfEntryPoint
    put32(b, kOptOff + 28, kImageBase);   // ImageBase
    put32(b, kOptOff + 56, 0x3000);       // SizeOfImage
    put32(b, kOptOff + 60, 0x200);        // SizeOfHeaders
    put32(b, kOptOff + 72, 0x100000);     // SizeOfStackReserve
    // DataDirectory[1] = import table（RVA 0x1000 / size 0x100）
    put32(b, kOptOff + 96 + 8, 0x1000);
    put32(b, kOptOff + 96 + 12, 0x100);
    // 节表：.rdata VA 0x1000 / vsize 0x200 / raw 0x200 @ 0x200
    std::memcpy(b.data() + kSecOff, ".rdata", 6);
    put32(b, kSecOff + 8, 0x200);
    put32(b, kSecOff + 12, 0x1000);
    put32(b, kSecOff + 16, 0x200);
    put32(b, kSecOff + 20, 0x200);
    put32(b, kSecOff + 36, 0x40000040);  // INITIALIZED_DATA | READ
    // 导入描述符 @ file 0x200（RVA 0x1000）
    put32(b, 0x200, 0x1028);   // OriginalFirstThunk
    put32(b, 0x20C, 0x1040);   // Name
    put32(b, 0x210, 0x1060);   // FirstThunk
    // 终止描述符（0x214 起 20 字节保持 0）
    // thunk 数组 @ 0x228（RVA 0x1028）：一个 Hint/Name RVA + 0（⛔ 不能与终止描述符 0x214..0x227 重叠）
    put32(b, 0x228, 0x1050);
    // DLL 名 @ 0x240（RVA 0x1040）
    std::memcpy(b.data() + 0x240, "x.dll", 6);
    // Hint/Name @ 0x250（RVA 0x1050）：hint=0, "Func"
    std::memcpy(b.data() + 0x252, "Func", 5);
    return b;
}

// ★ 合法但有"虚拟尾部"的合成 PE —— 专门防"把正常形态判成坏节"：
//   .rdata：vsize 0x600 > raw 0x200 ⇒ 尾部 0x400 字节在文件里不存在（载入后必须是 0）
//   .pad  ：raw 0x100 > vsize 0x40 ⇒ 只拷 0x40 字节（FileAlignment 填充不能当"要拷的字节"）
//   .empty：vsize == raw == 0 ⇒ 空节（合法的对齐产物，跳过、不算坏）
std::vector<std::uint8_t> makePeWithVirtualTail() {
    std::vector<std::uint8_t> b = makePe();
    b.resize(0x600, 0);                  // 给 .pad 的 raw 数据留位置
    put16(b, kPeOff + 6, 3);             // NumberOfSections = 3
    put32(b, kOptOff + 56, 0x4000);      // SizeOfImage
    // .rdata：vsize 0x600（> raw 0x200）—— 虚拟尾部
    put32(b, kSecOff + 8, 0x600);
    // .pad：VA 0x2000 / vsize 0x40 / raw 0x100 @ 0x400
    const std::size_t s2 = kSecOff + 40;
    std::memcpy(b.data() + s2, ".pad", 4);
    put32(b, s2 + 8, 0x40);
    put32(b, s2 + 12, 0x2000);
    put32(b, s2 + 16, 0x100);
    put32(b, s2 + 20, 0x400);
    put32(b, s2 + 36, 0x40000040);
    // .empty：全 0 的节头（vsize / raw / VA 都是 0）
    for (std::uint32_t i = 0; i < 0x100; ++i) b[0x400 + i] = static_cast<std::uint8_t>(0x40u + (i & 0x3Fu));
    return b;
}

// 错误信息里必须有某个字面量（失败时把整条错误打出来 —— 否则"断言写错了"看不出来）
#define LIFT_CHECK_ERR_HAS(errstr, needle)                                                              \
    do {                                                                                                \
        ++::lift_test::g_checks;                                                                        \
        const std::string& e_ = (errstr);                                                               \
        if (e_.find(needle) == std::string::npos)                                                       \
            ::lift_test::failf(__FILE__, __LINE__, "错误信息里没有 \"%s\"：%s", (needle), e_.c_str());   \
    } while (0)

bool memEquals(const lift::GuestMemory& mem, std::uint32_t ea, const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (mem.rd8(ea + static_cast<std::uint32_t>(i)) != p[i]) return false;
    }
    return true;
}

bool memZero(const lift::GuestMemory& mem, std::uint32_t ea, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (mem.rd8(ea + static_cast<std::uint32_t>(i)) != 0) return false;
    }
    return true;
}

}  // namespace

int main() {
    using namespace lift_test;

    // ① 好 PE：解析出一个导入（基线：⛔ 别把正常形态判成坏）
    {
        g_current_case = "good-pe";
        lift::PeImage img;
        img.bytes = makePe();
        std::string err;
        const bool ok = lift::parsePeImage(img, &err);
        LIFT_CHECK(ok);
        LIFT_CHECK(err.empty());
        LIFT_CHECK(img.imports.size() == 1);
        if (img.imports.size() == 1) {
            LIFT_CHECK(img.imports[0].module == "x.dll");
            LIFT_CHECK(img.imports[0].name == "Func");
            LIFT_CHECK(!img.imports[0].by_ordinal);
            LIFT_CHECK_EQ_U32(img.imports[0].iat_rva, 0x1060u);
        }
    }

    // ② 节 raw 范围越过文件 ⇒ mapPeImage 拒绝，且**具体原因**（节名 / raw 起点 / 文件长度）
    //   ★ 这是本单的 repro：旧实现"越界就不拷贝、仍返回 true" ⇒ 这一条会红。
    {
        g_current_case = "section-raw-out-of-range";
        lift::PeImage img;
        img.bytes = makePe();
        lift::PeSectionInfo s;
        std::strcpy(s.name, ".text");
        s.va = 0x1000;
        s.vsize = 0x200;
        s.raw_size = 0x200;
        s.raw_offset = 0x3F0;  // 0x3F0 + 0x200 > 0x400
        img.sections = {s};
        img.image_base = kImageBase;
        img.size_of_headers = 0x200;
        lift::GuestMemory mem;
        LIFT_CHECK(mem.reserved());
        std::string err;
        LIFT_CHECK(!lift::mapPeImage(mem, img, &err));
        LIFT_CHECK_ERR_HAS(err, ".text");
        LIFT_CHECK_ERR_HAS(err, "0x000003F0");  // PointerToRawData
        LIFT_CHECK_ERR_HAS(err, "0x00000400");  // 文件长度
        // ★ "不许半载入"的机械判据：拒绝时 guest 里**零字节**（连 PE 头都没提交）
        LIFT_CHECK(!mem.committed(kImageBase, 1));
        LIFT_CHECK(!mem.committed(kImageBase + 0x1000, 1));
    }

    // ③ 同上，但走"解析成功 ⇒ 映射拒绝"的整链（坏节表本身不越界 ⇒ parsePeImage 会过）
    {
        g_current_case = "section-raw-out-of-range-end2end";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, kOptOff + 96 + 8, 0);  // 去掉导入表（否则解析先在这里失败，测不到 map）
        put32(img.bytes, kSecOff + 20, 0x3F0);  // PointerToRawData 越界
        std::string err;
        LIFT_CHECK(lift::parsePeImage(img, &err));
        lift::GuestMemory mem;
        LIFT_CHECK(mem.reserved());
        LIFT_CHECK(!lift::mapPeImage(mem, img, &err));
        LIFT_CHECK_ERR_HAS(err, "0x000003F0");
        LIFT_CHECK(!mem.committed(kImageBase, 1));
    }

    // ④ 声明的 PE 头长度超过文件 ⇒ 拒绝（旧实现按文件长度静默截断）
    {
        g_current_case = "headers-out-of-range";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, kOptOff + 60, 0x600);  // SizeOfHeaders = 0x600 > 0x400
        std::string err;
        LIFT_CHECK(lift::parsePeImage(img, &err));
        lift::GuestMemory mem;
        LIFT_CHECK(mem.reserved());
        LIFT_CHECK(!lift::mapPeImage(mem, img, &err));
        LIFT_CHECK_ERR_HAS(err, "SizeOfHeaders");
        LIFT_CHECK_ERR_HAS(err, "0x00000600");
        LIFT_CHECK(!mem.committed(kImageBase, 1));
    }

    // ⑤ 合法但有虚拟尾部的 PE **必须载入成功**（⛔ 别把 SizeOfRawData < VirtualSize 当坏节）
    {
        g_current_case = "virtual-tail-ok";
        lift::PeImage img;
        img.bytes = makePeWithVirtualTail();
        std::string err;
        LIFT_CHECK(lift::parsePeImage(img, &err));
        LIFT_CHECK(err.empty());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(img.sections.size()), 3);
        lift::GuestMemory mem;
        LIFT_CHECK(mem.reserved());
        const bool mapped = lift::mapPeImage(mem, img, &err);
        if (!mapped) std::fprintf(stderr, "[virtual-tail-ok] mapPeImage 拒绝：%s\n", err.c_str());
        LIFT_CHECK(mapped);
        LIFT_CHECK(err.empty());
        // ★ 只有映射成功才去读 guest（否则下面每一条都会变成"读未提交页"的 guest fault，
        //   红是红了，但报错从"哪条断言"退化成"崩了"）
        if (!mapped) return finish("pe-loader");
        // 头：拷进去的是真字节（MZ）
        LIFT_CHECK_EQ_U32(mem.rd8(kImageBase), static_cast<std::uint32_t>('M'));
        // .rdata：raw 0x200 原样拷入，虚拟尾部（0x200..0x600）**必须为 0**
        LIFT_CHECK(memEquals(mem, kImageBase + 0x1000, img.bytes.data() + 0x200, 0x200));
        LIFT_CHECK(memZero(mem, kImageBase + 0x1200, 0x400));
        LIFT_CHECK(mem.committed(kImageBase + 0x15FF, 1));
        // .pad：raw 0x100 > vsize 0x40 ⇒ 只拷 0x40 字节；vsize 之外的 raw（0x40..0x100）**不许**被拷进来
        LIFT_CHECK(memEquals(mem, kImageBase + 0x2000, img.bytes.data() + 0x400, 0x40));
        LIFT_CHECK(memZero(mem, kImageBase + 0x2040, 0xC0));
        // 空节（vsize == raw == 0）跳过即可，⛔ 不提交、也不算坏
        LIFT_CHECK(!mem.committed(kImageBase + 0x3000, 1));
    }

    // ⑥ 坏 thunk 表 RVA（不在任何节里）⇒ 拒绝启动，⛔ 不是"跳过这条导入"
    {
        g_current_case = "bad-thunk-rva";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, 0x200, 0x3000);  // OriginalFirstThunk 越界
        std::string err;
        LIFT_CHECK(!lift::parsePeImage(img, &err));
        LIFT_CHECK_ERR_HAS(err, "0x00003000");
        LIFT_CHECK_ERR_HAS(err, "x.dll");  // 哪条导入：模块名要写出来
    }

    // ⑦ thunk 表一路非 0 到文件末尾（没有 0 终止项）⇒ 拒绝，⛔ 不是"读到 EOF 就收工"
    //   ★ 名字串放在 PE 头区的空隙（RVA 0x180 / 0x1A0）⇒ 不被 thunk 数组覆盖
    {
        g_current_case = "unterminated-thunk-table";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, 0x20C, 0x180);  // Name → 头部空隙
        put32(img.bytes, 0x210, 0x1000);  // FirstThunk → 节首（IAT 槽位始终在节的虚拟范围里）
        std::memcpy(img.bytes.data() + 0x180, "x.dll", 6);
        std::memcpy(img.bytes.data() + 0x1A2, "Func", 5);  // Hint/Name → RVA 0x1A0
        for (std::size_t off = 0x228; off + 4 <= 0x400; off += 4) put32(img.bytes, off, 0x1A0);
        std::string err;
        LIFT_CHECK(!lift::parsePeImage(img, &err));
        LIFT_CHECK_ERR_HAS(err, "0 终止项");
        LIFT_CHECK_ERR_HAS(err, "thunk");
    }

    // ⑧ 坏模块名 RVA ⇒ 拒绝启动（原因里要有那个 RVA）
    {
        g_current_case = "bad-module-name";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, 0x20C, 0x3000);
        std::string err;
        LIFT_CHECK(!lift::parsePeImage(img, &err));
        LIFT_CHECK_ERR_HAS(err, "0x00003000");
        LIFT_CHECK_ERR_HAS(err, "模块名");
    }

    // ⑨ 坏模块名：名字一路到文件末尾都没有 NUL（旧实现把**截断的串**当名字用）
    {
        g_current_case = "unterminated-module-name";
        lift::PeImage img;
        img.bytes = makePe();
        std::memset(img.bytes.data() + 0x240, 'A', 0x400 - 0x240);  // 0x240..0x3FF 全 'A'
        std::string err;
        LIFT_CHECK(!lift::parsePeImage(img, &err));
        LIFT_CHECK_ERR_HAS(err, "NUL");
        LIFT_CHECK_ERR_HAS(err, "0x00001040");
    }

    // ⑩ 坏函数名 RVA（hint/name 指到节外）⇒ 拒绝启动
    {
        g_current_case = "bad-import-name-rva";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, 0x228, 0x3000);  // thunk = Hint/Name RVA
        std::string err;
        LIFT_CHECK(!lift::parsePeImage(img, &err));
        LIFT_CHECK_ERR_HAS(err, "0x00003002");  // 跳过 2 字节 hint 后的名字 RVA
        LIFT_CHECK_ERR_HAS(err, "x.dll");
    }

    // ⑪ 坏函数名：hint 之后到文件末尾都没有 NUL ⇒ 拒绝
    {
        g_current_case = "unterminated-import-name";
        lift::PeImage img;
        img.bytes = makePe();
        std::memset(img.bytes.data() + 0x252, 'A', 0x400 - 0x252);
        std::string err;
        LIFT_CHECK(!lift::parsePeImage(img, &err));
        LIFT_CHECK_ERR_HAS(err, "NUL");
        LIFT_CHECK_ERR_HAS(err, "0x00001052");  // 跳过 2 字节 hint 后的名字 RVA
    }

    // ⑫ 导入表 RVA 不在任何节里 ⇒ 拒绝启动
    {
        g_current_case = "import-table-rva-out-of-range";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, kOptOff + 96 + 8, 0x5000);
        std::string err;
        LIFT_CHECK(!lift::parsePeImage(img, &err));
        LIFT_CHECK_ERR_HAS(err, "0x00005000");
        LIFT_CHECK_ERR_HAS(err, "导入表");
    }

    // ⑬ 坏 IAT 槽位（FirstThunk 指到节外）⇒ 拒绝启动
    //   ★ 旧实现不查 FirstThunk，于是绑定 IAT 时会写到镜像外（或回绕到 PE 头上）
    {
        g_current_case = "bad-iat-rva";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, 0x210, 0x3000);
        std::string err;
        LIFT_CHECK(!lift::parsePeImage(img, &err));
        LIFT_CHECK_ERR_HAS(err, "0x00003000");
        LIFT_CHECK_ERR_HAS(err, "IAT");
    }

    // ⑭ FirstThunk == 0（非终止描述符）⇒ 拒绝启动
    {
        g_current_case = "first-thunk-zero";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, 0x210, 0);
        std::string err;
        LIFT_CHECK(!lift::parsePeImage(img, &err));
        LIFT_CHECK_ERR_HAS(err, "FirstThunk");
    }

    return finish("pe-loader");
}
