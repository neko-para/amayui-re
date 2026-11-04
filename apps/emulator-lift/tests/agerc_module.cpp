// emulator-lift · AGERC.DLL 作为**真实 guest 模块**装载（用户 2026-11-04 改口径）
//
// ★ 判据（@env pure；自造最小 PE，不碰游戏 / LFS）：
//   ① `Machine::loadAgercModule` 真的把 PE 头 + 节映射进 guest：
//      `agerc_base != 0`、`MZ` / 节字节逐字节对、`cpu.shim.agerc_image` 指向同一份镜像、
//      `cpu.shim.agerc_base == agercBase()`，且 `cpu.shim.agerc_loaded == false`（还没 LoadLibrary）；
//   ② base 选择：空闲时优先 PE 的 ImageBase；与已占区域（这里用 placeCode 占住 0x400000）冲突时
//      改用 0x30000000；
//   ③ 句柄口径：`LoadLibraryA` / `GetModuleHandleA` 返回**同一个**真实 base；`GetProcAddress`
//      返回 stub thunk，`call thunk` ⇒ `_GetInstance@0` 返回同一个 base；
//      `LoadIconA(base, "IDI_ICON1")` 从**真实 AGERC 镜像**读 RT_GROUP_ICON / RT_ICON ⇒ 伪 HICON；
//   ④ 缺文件 / 没有镜像：`loadAgercModule` ⇒ false + err；`LoadLibraryA` ⇒ 0 + 126（⛔ 不假装成功）。
//
// ★ 为什么自造 PE：AGERC 的拆壳件不在仓库（在 win 设备上），而这一层是**纯字节工作** + guest 映射，
//   不需要真资产。⛔ 不用 `LIFT_AGE_EXE` 的 exe 冒充 AGERC（那会把 ImageBase / 资源语义混在一起）。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/pe_resource.h"
#include "shims/register.h"

namespace {

namespace fs = std::filesystem;

// ---- 最小 PE32：一节 `.rsrc`（RVA 0x2000 / raw 0x400 / 0x400 字节）----
// 目录树：type 3 → id 1 → lang 1041 → data（RT_ICON "ICON"）
//         type 14 → 名字 "IDI_ICON1" → lang → data（GRPICONDIR，组内 id=1）
//         type 16 → id 1 → lang → data（VS_VERSIONINFO；版本读取从**镜像**走）
constexpr std::uint32_t kPeOff = 0x40;
constexpr std::uint32_t kOptOff = kPeOff + 24;  // 0x58
constexpr std::uint32_t kSecOff = kOptOff + 224;
constexpr std::uint32_t kRsrcRva = 0x2000;
constexpr std::uint32_t kRsrcRaw = 0x400;
constexpr std::uint32_t kImageBase = 0x00400000;

constexpr std::uint32_t kRoot = 0x000;
constexpr std::uint32_t kIconDir = 0x030;
constexpr std::uint32_t kGroupDir = 0x050;
constexpr std::uint32_t kVersionDir = 0x070;
constexpr std::uint32_t kIconLang = 0x090;
constexpr std::uint32_t kGroupLang = 0x0B0;
constexpr std::uint32_t kVersionLang = 0x0D0;
constexpr std::uint32_t kIconData = 0x0F0;
constexpr std::uint32_t kGroupData = 0x100;
constexpr std::uint32_t kVersionData = 0x110;
constexpr std::uint32_t kIconBytes = 0x140;
constexpr std::uint32_t kGroupBytes = 0x150;
constexpr std::uint32_t kVersionBytes = 0x180;  // 0x180 + 92 = 0x1DC，不压到名字串（0x200）
constexpr std::uint32_t kGroupName = 0x200;
constexpr std::uint32_t kMarker = 0x300;  // 节里一个可逐字节核对的哨兵（"LFT!"）
constexpr std::uint32_t kMarkerValue = 0x2154464Cu;

constexpr std::uint32_t kGroupBlobSize = 6 + 14;  // GRPICONDIR + 1 个 GRPICONDIRENTRY
constexpr std::uint32_t kVersionBlobSize = 92;    // 40（头 + key）+ 52（VS_FIXEDFILEINFO）

void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    std::memcpy(b.data() + off, &v, 2);
}
void put32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    std::memcpy(b.data() + off, &v, 4);
}

void setDir(std::vector<std::uint8_t>& b, std::uint32_t rel, std::uint16_t named, std::uint16_t ids,
            const std::vector<std::pair<std::uint32_t, std::uint32_t>>& entries) {
    const std::size_t o = kRsrcRaw + rel;
    put16(b, o + 12, named);
    put16(b, o + 14, ids);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        put32(b, o + 16 + i * 8, entries[i].first);
        put32(b, o + 20 + i * 8, entries[i].second);
    }
}

void setDataEntry(std::vector<std::uint8_t>& b, std::uint32_t rel, std::uint32_t rva,
                  std::uint32_t size, std::uint32_t codepage) {
    const std::size_t o = kRsrcRaw + rel;
    put32(b, o, rva);
    put32(b, o + 4, size);
    put32(b, o + 8, codepage);
    put32(b, o + 12, 0);
}

// VS_VERSIONINFO 最小块：头 + szKey("VS_VERSION_INFO") + VS_FIXEDFILEINFO（签名 0xFEEF04BD）
std::vector<std::uint8_t> makeVersionBlob() {
    std::vector<std::uint8_t> v(kVersionBlobSize, 0);
    put16(v, 0, static_cast<std::uint16_t>(kVersionBlobSize));  // wLength
    put16(v, 2, 52);                                            // wValueLength = sizeof(VS_FIXEDFILEINFO)
    put16(v, 4, 0);                                             // wType = 0（binary）
    const char* key = "VS_VERSION_INFO";
    for (std::size_t i = 0; i < std::strlen(key); ++i) {
        put16(v, 6 + i * 2, static_cast<std::uint16_t>(key[i]));
    }
    const std::size_t ffi = 40;
    put32(v, ffi + 0, lift::kVsFixedFileInfoSignature);
    put32(v, ffi + 4, 0x00010000u);   // dwStrucVersion
    put32(v, ffi + 8, 0x00040001u);   // dwFileVersionMS = 4.1
    put32(v, ffi + 12, 0x00020003u);  // dwFileVersionLS = 2.3
    put32(v, ffi + 16, 0x00040001u);  // dwProductVersionMS
    put32(v, ffi + 20, 0x00020003u);  // dwProductVersionLS
    put32(v, ffi + 32, 0x00040004u);  // dwFileOS = VOS_NT_WINDOWS32
    put32(v, ffi + 36, 2u);           // dwFileType = VFT_DLL
    return v;
}

std::vector<std::uint8_t> makePe() {
    std::vector<std::uint8_t> b(0x800, 0);
    b[0] = 'M';
    b[1] = 'Z';
    put32(b, 0x3C, kPeOff);
    b[kPeOff] = 'P';
    b[kPeOff + 1] = 'E';
    put16(b, kPeOff + 6, 1);             // NumberOfSections
    put16(b, kPeOff + 20, 224);          // SizeOfOptionalHeader
    put16(b, kOptOff, 0x010B);           // PE32
    put32(b, kOptOff + 16, kRsrcRva);    // AddressOfEntryPoint（本测试不执行它）
    put32(b, kOptOff + 28, kImageBase);  // ImageBase
    put32(b, kOptOff + 56, 0x3000);      // SizeOfImage
    put32(b, kOptOff + 60, 0x400);       // SizeOfHeaders
    put32(b, kOptOff + 72, 0x100000);    // SizeOfStackReserve
    // DataDirectory[2] = 资源目录（RVA 0x2000 / size 0x400）
    put32(b, kOptOff + 96 + 8 * 2, kRsrcRva);
    put32(b, kOptOff + 96 + 8 * 2 + 4, 0x400);
    // 节表：.rsrc VA 0x2000 / vsize 0x400 / raw 0x400 @ 0x400
    std::memcpy(b.data() + kSecOff, ".rsrc", 5);
    put32(b, kSecOff + 8, 0x400);
    put32(b, kSecOff + 12, kRsrcRva);
    put32(b, kSecOff + 16, 0x400);
    put32(b, kSecOff + 20, kRsrcRaw);
    put32(b, kSecOff + 36, 0x40000040);  // INITIALIZED_DATA | READ

    // ---- 目录树 ----
    setDir(b, kRoot, 0, 3,
           {{3, 0x80000000u | kIconDir},
            {14, 0x80000000u | kGroupDir},
            {16, 0x80000000u | kVersionDir}});
    setDir(b, kIconDir, 0, 1, {{1, 0x80000000u | kIconLang}});
    setDir(b, kGroupDir, 1, 0, {{0x80000000u | kGroupName, 0x80000000u | kGroupLang}});
    setDir(b, kVersionDir, 0, 1, {{1, 0x80000000u | kVersionLang}});
    setDir(b, kIconLang, 0, 1, {{1041, kIconData}});
    setDir(b, kGroupLang, 0, 1, {{1041, kGroupData}});
    setDir(b, kVersionLang, 0, 1, {{1041, kVersionData}});

    // ---- 名字串：WORD 长度 + UTF-16（无结尾 0）----
    const char* name = "IDI_ICON1";
    put16(b, kRsrcRaw + kGroupName, 9);
    for (std::size_t i = 0; i < 9; ++i) {
        put16(b, kRsrcRaw + kGroupName + 2 + i * 2, static_cast<std::uint16_t>(name[i]));
    }

    // ---- 数据项（OffsetToData 是 RVA）----
    setDataEntry(b, kIconData, kRsrcRva + kIconBytes, 4, 1252);
    setDataEntry(b, kGroupData, kRsrcRva + kGroupBytes, kGroupBlobSize, 1252);
    setDataEntry(b, kVersionData, kRsrcRva + kVersionBytes, kVersionBlobSize, 1252);

    // ---- RT_ICON 的 4 字节 + GRPICONDIR ----
    std::memcpy(b.data() + kRsrcRaw + kIconBytes, "ICON", 4);
    std::vector<std::uint8_t> group(kGroupBlobSize, 0);
    put16(group, 0, 0);  // reserved
    put16(group, 2, 1);  // type = 1（图标）
    put16(group, 4, 1);  // count
    group[6 + 0] = 32;   // bWidth
    group[6 + 1] = 32;   // bHeight
    put16(group, 6 + 4, 1);   // wPlanes
    put16(group, 6 + 6, 32);  // wBitCount
    put32(group, 6 + 8, 4);   // dwBytes
    put16(group, 6 + 12, 1);  // nID ⇒ RT_ICON 1
    std::memcpy(b.data() + kRsrcRaw + kGroupBytes, group.data(), group.size());

    // ---- RT_VERSION：VS_VERSIONINFO（版本读取必须从**这份镜像**走，⛔ 不重读宿主文件）----
    const std::vector<std::uint8_t> version = makeVersionBlob();
    std::memcpy(b.data() + kRsrcRaw + kVersionBytes, version.data(), version.size());

    // ---- 哨兵：节里一块可逐字节核对的字节 ----
    std::memcpy(b.data() + kRsrcRaw + kMarker, "LFT!", 4);
    return b;
}

// 临时 PE 文件（RAII；析构删掉）
class TempPeFile {
public:
    TempPeFile() {
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec);
        if (ec) return;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = (dir / ("lift-agerc-" + std::to_string(stamp) + ".dll")).string();
        const std::vector<std::uint8_t> bytes = makePe();
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        ok_ = out.good();
    }
    ~TempPeFile() {
        if (path_.empty()) return;
        std::error_code ec;
        fs::remove(path_, ec);
    }
    TempPeFile(const TempPeFile&) = delete;
    TempPeFile& operator=(const TempPeFile&) = delete;

    bool ok() const { return ok_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    bool ok_ = false;
};

// 一个**不存在**的临时路径（用于"缺文件 ⇒ false + err"）
std::string missingPath() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return (dir / ("lift-agerc-missing-" + std::to_string(stamp) + ".dll")).string();
}

}  // namespace

int main() {
    using namespace lift_test;
    using lift_fixture::Fixture;

    TempPeFile pe;
    if (!pe.ok()) LIFT_SKIP("临时目录不可写，造不出自造 PE");

    // =====================================================================================
    // ① 真实装载：节映射 + shim 状态
    // =====================================================================================
    Fixture fx;
    LIFT_CHECK(fx.ok);
    if (!fx.ok) {
        std::fprintf(stderr, "fixture init: %s\n", fx.err.c_str());
        return finish("agerc-module");
    }
    {
        g_current_case = "load-maps-image";
        std::string err;
        LIFT_CHECK(fx.m.loadAgercModule(pe.path(), &err));
        if (!err.empty()) std::printf("loadAgercModule err = %s\n", err.c_str());
        const std::uint32_t base = fx.m.agercBase();
        LIFT_CHECK_NE_U32(base, 0u);
        // bare 机器上 0x400000 空闲 ⇒ 优先用 PE 的 ImageBase（不换基址）
        LIFT_CHECK_EQ_U32(base, kImageBase);
        LIFT_CHECK(fx.m.agercModuleLoaded());
        LIFT_CHECK(fx.m.cpu().shim.agerc_image == &fx.m.agercImage());
        LIFT_CHECK_EQ_U32(fx.m.cpu().shim.agerc_base, base);
        // loadAgercModule 只"映射"；只有 guest 调 LoadLibrary 才置 loaded
        LIFT_CHECK(!fx.m.cpu().shim.agerc_loaded);

        // guest 里能读到 MZ + 整个 .rsrc 节（逐字节 = 文件里的 raw 数据）
        LIFT_CHECK_EQ_U32(fx.m.mem().rd8(base), 'M');
        LIFT_CHECK_EQ_U32(fx.m.mem().rd8(base + 1), 'Z');
        LIFT_CHECK(fx.m.mem().committed(base, 0x400));
        LIFT_CHECK(fx.m.mem().committed(base + kRsrcRva, 0x400));
        LIFT_CHECK_EQ_U32(fx.m.mem().rd32(base + kRsrcRva + kMarker), kMarkerValue);
        const lift::PeImage& ai = fx.m.agercImage();
        LIFT_CHECK(ai.sections.size() == 1);
        LIFT_CHECK(std::strcmp(ai.sections.front().name, ".rsrc") == 0);
        LIFT_CHECK(std::memcmp(fx.m.mem().host(base + kRsrcRva), ai.bytes.data() + kRsrcRaw, 0x400) ==
                   0);
    }

    // =====================================================================================
    // ② 句柄口径：LoadLibrary / GetModuleHandle / GetProcAddress / _GetInstance / LoadIconA
    // =====================================================================================
    {
        g_current_case = "handles-are-the-real-base";
        lift::registerProcessShims(fx.m.shims());
        lift::registerAgercShims(fx.m.shims());
        lift::registerUser32ResourceShims(fx.m.shims());
        lift::registerVersionShims(fx.m.shims());

        const std::uint32_t base = fx.m.agercBase();
        const std::uint32_t lib_ea = fx.use("LoadLibraryA");
        const std::uint32_t gmh_ea = fx.use("GetModuleHandleA");
        const std::uint32_t gpa_ea = fx.use("GetProcAddress");
        const std::uint32_t getinst_ea = fx.use("agerc", "_GetInstance@0");
        const std::uint32_t icon_ea = fx.use("user32", "LoadIconA");
        LIFT_CHECK(lib_ea != 0 && gmh_ea != 0 && gpa_ea != 0 && getinst_ea != 0 && icon_ea != 0);

        const std::uint32_t mod_ea = fx.cstr("AGERC.DLL");
        const std::uint32_t proc_ea = fx.cstr("_GetInstance@0");
        const std::uint32_t icon_name_ea = fx.cstr("IDI_ICON1");

        // LoadLibraryA("AGERC.DLL") ⇒ 真实 base（并置 agerc_loaded）
        LIFT_CHECK_EQ_U32(fx.call(lib_ea, {mod_ea}), base);
        LIFT_CHECK(fx.m.cpu().shim.agerc_loaded);

        // GetModuleHandleA 用**同一个**句柄回答（全名 / 去 .dll 两种写法）
        LIFT_CHECK_EQ_U32(fx.call(gmh_ea, {mod_ea}), base);
        LIFT_CHECK_EQ_U32(fx.call(gmh_ea, {fx.cstr("AGERC")}), base);

        // GetProcAddress(base, "_GetInstance@0") ⇒ stub thunk；call 它 ⇒ 同一个 base
        const std::uint32_t thunk = fx.call(gpa_ea, {base, proc_ea});
        LIFT_CHECK_NE_U32(thunk, 0u);
        const lift::ShimDef* def = fx.m.shims().find("agerc", "_GetInstance@0");
        LIFT_CHECK(def != nullptr);
        if (def) LIFT_CHECK_EQ_U32(thunk, def->stub_ea);
        LIFT_CHECK_EQ_U32(fx.call(thunk, {}), base);

        // LoadIconA(AGERC 的 HINSTANCE, "IDI_ICON1") ⇒ 从真实镜像读资源，返回伪 HICON
        LIFT_CHECK_EQ_U32(fx.call(icon_ea, {base, icon_name_ea}), 0x00020001u);

        // 版本：GetFileVersionInfoSizeA("AGERC.DLL") 读的是**已装载的镜像**。
        // bare 夹具没有装配 VFS ⇒ 一旦走"按宿主文件读"的分支就会 shimAbort（这本身就是判据）。
        const std::uint32_t vsize_ea = fx.use("version", "GetFileVersionInfoSizeA");
        LIFT_CHECK(vsize_ea != 0);
        LIFT_CHECK_EQ_U32(fx.call(vsize_ea, {fx.cstr("AGERC.DLL"), 0u}), kVersionBlobSize);
    }

    // =====================================================================================
    // ③ base 选择：与已占区域冲突 ⇒ 0x30000000
    // =====================================================================================
    {
        g_current_case = "base-conflict-falls-back";
        Fixture fx2;
        LIFT_CHECK(fx2.ok);
        std::string err;
        const std::uint8_t nop = 0x90;
        LIFT_CHECK(fx2.m.placeCode(kImageBase, &nop, 1, &err));  // 占住 PE 的 ImageBase
        LIFT_CHECK(fx2.m.loadAgercModule(pe.path(), &err));
        if (!err.empty()) std::printf("loadAgercModule(conflict) err = %s\n", err.c_str());
        LIFT_CHECK_EQ_U32(fx2.m.agercBase(), 0x30000000u);
        LIFT_CHECK_EQ_U32(fx2.m.cpu().shim.agerc_base, 0x30000000u);
        LIFT_CHECK_EQ_U32(fx2.m.mem().rd8(0x30000000u), 'M');
    }

    // =====================================================================================
    // ④ 没有镜像 / 缺文件：⛔ 不假装成功
    // =====================================================================================
    {
        g_current_case = "missing-file";
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(!m.loadAgercModule(missingPath(), &err));
        LIFT_CHECK(!err.empty());
        LIFT_CHECK_EQ_U32(m.agercBase(), 0u);
        LIFT_CHECK(m.cpu().shim.agerc_image == nullptr);
        LIFT_CHECK_EQ_U32(m.cpu().shim.agerc_base, 0u);
    }
    {
        g_current_case = "loadlibrary-without-image";
        Fixture fx3;
        LIFT_CHECK(fx3.ok);
        lift::registerProcessShims(fx3.m.shims());
        lift::registerAgercShims(fx3.m.shims());
        const std::uint32_t lib_ea = fx3.use("LoadLibraryA");
        LIFT_CHECK(lib_ea != 0);
        LIFT_CHECK_EQ_U32(fx3.call(lib_ea, {fx3.cstr("AGERC.DLL")}), 0u);
        LIFT_CHECK_EQ_U32(fx3.m.cpu().shim.last_error, 126u);  // ERROR_MOD_NOT_FOUND
        LIFT_CHECK(!fx3.m.cpu().shim.agerc_loaded);
    }

    return finish("agerc-module");
}
