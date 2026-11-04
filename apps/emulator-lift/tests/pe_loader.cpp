// emulator-lift · PE 载入的坏输入（pure：自造最小 PE 字节，不碰游戏 / LFS）
//
// ★ 判据（审计 T5）：坏节 / 坏导入必须**拒绝启动**，⛔ 不许"静默跳过、半载入仍返回 true"。
//   旧实现：`mapPeImage` 越界就不拷贝；导入表解析对坏 RVA 只 `continue/break` ⇒
//   A5 的"缺口 = 0"会假绿（缺口被解析器吃掉）。
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
    put32(b, kOptOff + 28, 0x00400000);   // ImageBase
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

}  // namespace

int main() {
    using namespace lift_test;

    // ① 好 PE：解析出一个导入
    {
        g_current_case = "good-pe";
        lift::PeImage img;
        img.bytes = makePe();
        std::string err;
        const bool ok = lift::parsePeImage(img, &err);
        LIFT_CHECK(ok);
        LIFT_CHECK(img.imports.size() == 1);
        if (img.imports.size() == 1) {
            LIFT_CHECK(img.imports[0].module == "x.dll");
            LIFT_CHECK(img.imports[0].name == "Func");
            LIFT_CHECK_EQ_U32(img.imports[0].iat_rva, 0x1060u);
        }
    }

    // ② 坏 thunk RVA（不在任何节里）⇒ 拒绝启动，⛔ 不是"跳过这条导入"
    {
        g_current_case = "bad-thunk-rva";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, 0x200, 0x3000);  // OriginalFirstThunk 越界
        std::string err;
        LIFT_CHECK(!lift::parsePeImage(img, &err));
        LIFT_CHECK(!err.empty());
    }

    // ③ 坏模块名 RVA ⇒ 拒绝启动
    {
        g_current_case = "bad-module-name";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, 0x20C, 0x3000);
        std::string err;
        LIFT_CHECK(!lift::parsePeImage(img, &err));
        LIFT_CHECK(!err.empty());
    }

    // ④ 节 raw 范围越过文件 ⇒ mapPeImage 拒绝（旧实现静默不拷贝、返回 true）
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
        img.image_base = 0x00400000;
        img.size_of_headers = 0x200;
        lift::GuestMemory mem;
        LIFT_CHECK(mem.reserved());
        std::string err;
        LIFT_CHECK(!lift::mapPeImage(mem, img, &err));
        LIFT_CHECK(!err.empty());
    }

    return finish("pe-loader");
}
