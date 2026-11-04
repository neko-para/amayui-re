// emulator-lift · PE 资源目录（RT_ICON / RT_GROUP_ICON / VS_VERSIONINFO）
//
// ★ 判据（plan §4 D6 / §6 P0）：
//   ① **pure**：自造最小 PE + `.rsrc`（不碰游戏 / LFS）⇒ 目录树三层、按名 / 按 ID 查找、
//      GRPICONDIR 解析、VS_FIXEDFILEINFO 签名、以及**坏目录必须响亮失败**（⛔ 不静默跳过）；
//   ② **assets**：用 `LIFT_AGE_EXE`（或 `AMAYUI_AGE_EXE`）指向入库的拆壳件，断言
//      `versionInfo` 非空 + `VS_FIXEDFILEINFO` 签名存在，且**本镜像有的**每一组 RT_GROUP_ICON
//      都能解析出至少一个 RT_ICON（本镜像没有组图标时就断言"没有"并打印，⛔ 不硬编成功）。
//   ③ 缺资产 ⇒ `LIFT_SKIP`（退出码 77；CTest 的 SKIP_RETURN_CODE 认它）——⛔ 不"跑绿了"。
//
// ★ 这个测试**直接调 `PeResources`**（不经过 `register.cpp` / 一台 Machine）：资源解析是纯字节工作。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "runtime/image.h"
#include "runtime/pe_resource.h"

namespace {

constexpr std::uint32_t kPeOff = 0x40;
constexpr std::uint32_t kOptOff = kPeOff + 24;  // 0x58
constexpr std::uint32_t kSecOff = kOptOff + 224;
constexpr std::uint32_t kRsrcRva = 0x2000;      // 资源节 RVA
constexpr std::uint32_t kRsrcRaw = 0x400;       // 资源节在文件里的偏移（= SizeOfHeaders）

void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    std::memcpy(b.data() + off, &v, 2);
}
void put32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    std::memcpy(b.data() + off, &v, 4);
}

// 资源目录里的一个项：name_field / data_field 都是**原始**值（高位标志自己给）
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

void putBytes(std::vector<std::uint8_t>& b, std::uint32_t rel, const void* src, std::size_t n) {
    std::memcpy(b.data() + kRsrcRaw + rel, src, n);
}

// 目录树里用到的相对偏移（见 makePe 的注释）
constexpr std::uint32_t kRoot = 0x000;
constexpr std::uint32_t kIconDir = 0x040;
constexpr std::uint32_t kGroupDir = 0x070;
constexpr std::uint32_t kVersionDir = 0x090;
constexpr std::uint32_t kIcon1Lang = 0x0C0;
constexpr std::uint32_t kIcon2Lang = 0x0E0;
constexpr std::uint32_t kGroupLang = 0x100;
constexpr std::uint32_t kVersionLang = 0x120;
constexpr std::uint32_t kIcon1Data = 0x140;
constexpr std::uint32_t kIcon2Data = 0x150;
constexpr std::uint32_t kGroupData = 0x160;
constexpr std::uint32_t kVersionData = 0x170;
constexpr std::uint32_t kGroupName = 0x200;
constexpr std::uint32_t kIcon1Bytes = 0x220;
constexpr std::uint32_t kIcon2Bytes = 0x224;
constexpr std::uint32_t kGroupBytes = 0x230;
constexpr std::uint32_t kVersionBytes = 0x260;

constexpr std::uint32_t kVersionBlobSize = 92;  // 40（头 + key）+ 52（VS_FIXEDFILEINFO）
constexpr std::uint32_t kGroupBlobSize = 6 + 2 * 14;

std::vector<std::uint8_t> makeVersionBlob() {
    std::vector<std::uint8_t> v(kVersionBlobSize, 0);
    put16(v, 0, static_cast<std::uint16_t>(kVersionBlobSize));  // wLength
    put16(v, 2, 52);                                            // wValueLength = sizeof(VS_FIXEDFILEINFO)
    put16(v, 4, 0);                                             // wType = 0（binary）
    // szKey = "VS_VERSION_INFO"（UTF-16 + NUL）⇒ 6 + 34 = 40（本来就 4 字节对齐）
    const char* key = "VS_VERSION_INFO";
    for (std::size_t i = 0; i < std::strlen(key); ++i) put16(v, 6 + i * 2, static_cast<std::uint16_t>(key[i]));
    const std::size_t ffi = 40;
    put32(v, ffi + 0, lift::kVsFixedFileInfoSignature);
    put32(v, ffi + 4, 0x00010000u);  // dwStrucVersion
    put32(v, ffi + 8, 0x00040001u);  // dwFileVersionMS = 4.1
    put32(v, ffi + 12, 0x00020003u); // dwFileVersionLS = 2.3
    put32(v, ffi + 16, 0x00040001u); // dwProductVersionMS
    put32(v, ffi + 20, 0x00020003u); // dwProductVersionLS
    put32(v, ffi + 24, 0x0000003Fu); // dwFileFlagsMask
    put32(v, ffi + 28, 0u);          // dwFileFlags
    put32(v, ffi + 32, 0x00040004u); // dwFileOS = VOS_NT_WINDOWS32
    put32(v, ffi + 36, 1u);          // dwFileType = VFT_APP
    put32(v, ffi + 40, 0u);          // dwFileSubtype
    put32(v, ffi + 44, 0u);          // dwFileDateMS
    put32(v, ffi + 48, 0u);          // dwFileDateLS
    return v;
}

std::vector<std::uint8_t> makeGroupBlob() {
    std::vector<std::uint8_t> g(kGroupBlobSize, 0);
    put16(g, 0, 0);  // reserved
    put16(g, 2, 1);  // type = 1（图标）
    put16(g, 4, 2);  // count
    // entry 0：32×32 / 32bpp / 4 字节 / id=1
    g[6 + 0] = 32;
    g[6 + 1] = 32;
    put16(g, 6 + 4, 1);   // wPlanes
    put16(g, 6 + 6, 32);  // wBitCount
    put32(g, 6 + 8, 4);   // dwBytes
    put16(g, 6 + 12, 1);  // nID
    // entry 1：16×16 / 32bpp / 4 字节 / id=2
    g[6 + 14 + 0] = 16;
    g[6 + 14 + 1] = 16;
    put16(g, 6 + 14 + 4, 1);
    put16(g, 6 + 14 + 6, 32);
    put32(g, 6 + 14 + 8, 4);
    put16(g, 6 + 14 + 12, 2);
    return g;
}

// 最小 PE32 + 一节 `.rsrc`（RVA 0x2000 / raw 0x400 / 0x400 字节）
// 目录树：type 3 → id 1,2 → lang 1041 → data；type 14 → 名字 "IDI_ICON1" → lang → data；
//         type 16 → id 1 → lang → data（VS_VERSIONINFO）
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
    put32(b, kOptOff + 16, 0x2000);      // AddressOfEntryPoint
    put32(b, kOptOff + 28, 0x00400000);  // ImageBase
    put32(b, kOptOff + 56, 0x3000);      // SizeOfImage
    put32(b, kOptOff + 60, 0x400);       // SizeOfHeaders
    put32(b, kOptOff + 72, 0x100000);    // SizeOfStackReserve
    // DataDirectory[2] = 资源目录（RVA 0x2000 / size 0x400）；导入表目录保持 0
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
           {{3, 0x80000000u | kIconDir}, {14, 0x80000000u | kGroupDir}, {16, 0x80000000u | kVersionDir}});
    setDir(b, kIconDir, 0, 2,
           {{1, 0x80000000u | kIcon1Lang}, {2, 0x80000000u | kIcon2Lang}});
    setDir(b, kGroupDir, 1, 0, {{0x80000000u | kGroupName, 0x80000000u | kGroupLang}});
    setDir(b, kVersionDir, 0, 1, {{1, 0x80000000u | kVersionLang}});
    setDir(b, kIcon1Lang, 0, 1, {{1041, kIcon1Data}});
    setDir(b, kIcon2Lang, 0, 1, {{1041, kIcon2Data}});
    setDir(b, kGroupLang, 0, 1, {{1041, kGroupData}});
    setDir(b, kVersionLang, 0, 1, {{1041, kVersionData}});

    // ---- 名字串：WORD 长度 + UTF-16（无结尾 0）----
    put16(b, kRsrcRaw + kGroupName, 9);
    const char* name = "IDI_ICON1";
    for (std::size_t i = 0; i < 9; ++i) {
        put16(b, kRsrcRaw + kGroupName + 2 + i * 2, static_cast<std::uint16_t>(name[i]));
    }

    // ---- 数据项（OffsetToData 是 RVA）----
    setDataEntry(b, kIcon1Data, kRsrcRva + kIcon1Bytes, 4, 1252);
    setDataEntry(b, kIcon2Data, kRsrcRva + kIcon2Bytes, 4, 1252);
    setDataEntry(b, kGroupData, kRsrcRva + kGroupBytes, kGroupBlobSize, 1252);
    setDataEntry(b, kVersionData, kRsrcRva + kVersionBytes, kVersionBlobSize, 1252);

    // ---- 数据 ----
    putBytes(b, kIcon1Bytes, "IC01", 4);
    putBytes(b, kIcon2Bytes, "IC02", 4);
    const std::vector<std::uint8_t> group = makeGroupBlob();
    putBytes(b, kGroupBytes, group.data(), group.size());
    const std::vector<std::uint8_t> version = makeVersionBlob();
    putBytes(b, kVersionBytes, version.data(), version.size());
    return b;
}

}  // namespace

int main() {
    using namespace lift_test;

    // =====================================================================================
    // pure：自造最小 PE + .rsrc
    // =====================================================================================
    {
        g_current_case = "pure-parse";
        lift::PeImage img;
        img.bytes = makePe();
        std::string err;
        LIFT_CHECK(lift::parsePeImage(img, &err));
        LIFT_CHECK_EQ_U32(img.resource_rva, kRsrcRva);
        LIFT_CHECK_EQ_U32(img.resource_size, 0x400u);

        lift::PeResources res;
        LIFT_CHECK(res.init(img, &err));
        LIFT_CHECK(res.hasResources());
        if (!err.empty()) std::printf("pure-parse init err = %s\n", err.c_str());
    }

    {
        g_current_case = "pure-find-by-id";
        lift::PeImage img;
        img.bytes = makePe();
        std::string err;
        LIFT_CHECK(lift::parsePeImage(img, &err));
        lift::PeResources res;
        LIFT_CHECK(res.init(img, &err));

        lift::PeResourceEntry e;
        LIFT_CHECK(res.findEntry(lift::kRtIcon, 2, &e, &err));
        LIFT_CHECK_EQ_U32(e.rva, kRsrcRva + kIcon2Bytes);
        LIFT_CHECK_EQ_U32(e.size, 4u);
        LIFT_CHECK_EQ_U32(e.codepage, 1252u);
        LIFT_CHECK(!e.has_name);
        LIFT_CHECK_EQ_U32(e.id, 2u);
        LIFT_CHECK_EQ_U32(e.language, 1041u);
        std::vector<std::uint8_t> data;
        LIFT_CHECK(res.readData(e, &data, &err));
        LIFT_CHECK(data.size() == 4 && data[0] == 'I' && data[3] == '2');

        // 数字 ID 不存在的项 ⇒ false（不是"返回 0 当成功"）
        LIFT_CHECK(!res.findEntry(lift::kRtIcon, 99, &e, &err));
        LIFT_CHECK(!err.empty());
        // 类型不存在 ⇒ false
        LIFT_CHECK(!res.findEntry(10, 1, &e, &err));
        LIFT_CHECK(!err.empty());
    }

    {
        g_current_case = "pure-find-by-name";
        lift::PeImage img;
        img.bytes = makePe();
        std::string err;
        LIFT_CHECK(lift::parsePeImage(img, &err));
        lift::PeResources res;
        LIFT_CHECK(res.init(img, &err));

        lift::PeResourceEntry e;
        LIFT_CHECK(res.findEntryByName(lift::kRtGroupIcon, "IDI_ICON1", &e, &err));
        LIFT_CHECK(e.has_name);
        LIFT_CHECK(e.name == "IDI_ICON1");
        LIFT_CHECK_EQ_U32(e.size, kGroupBlobSize);
        // 资源名比较是大小写不敏感的（Windows 的 loader 口径）
        LIFT_CHECK(res.findEntryByName(lift::kRtGroupIcon, "idi_icon1", &e, &err));
        // 空名字 / 不存在的名字 ⇒ false + err
        LIFT_CHECK(!res.findEntryByName(lift::kRtGroupIcon, "", &e, &err));
        LIFT_CHECK(!err.empty());
        LIFT_CHECK(!res.findEntryByName(lift::kRtGroupIcon, "IDI_ICON2", &e, &err));
        LIFT_CHECK(!err.empty());

        // listNames：顺序 = 目录项顺序
        std::vector<lift::PeResourceName> names;
        LIFT_CHECK(res.listNames(lift::kRtGroupIcon, &names, &err));
        LIFT_CHECK(names.size() == 1);
        if (names.size() == 1) {
            LIFT_CHECK(names[0].is_name);
            LIFT_CHECK(names[0].name == "IDI_ICON1");
        }
        std::vector<lift::PeResourceName> icons;
        LIFT_CHECK(res.listNames(lift::kRtIcon, &icons, &err));
        LIFT_CHECK(icons.size() == 2);
        if (icons.size() == 2) {
            LIFT_CHECK(!icons[0].is_name && icons[0].id == 1);
            LIFT_CHECK(!icons[1].is_name && icons[1].id == 2);
        }
    }

    {
        g_current_case = "pure-icon-group";
        lift::PeImage img;
        img.bytes = makePe();
        std::string err;
        LIFT_CHECK(lift::parsePeImage(img, &err));
        lift::PeResources res;
        LIFT_CHECK(res.init(img, &err));

        std::vector<std::uint32_t> ids;
        LIFT_CHECK(res.iconGroupByName("IDI_ICON1", &ids, &err));
        LIFT_CHECK(ids.size() == 2);
        if (ids.size() == 2) {
            LIFT_CHECK_EQ_U32(ids[0], 1u);
            LIFT_CHECK_EQ_U32(ids[1], 2u);
        }
        // 按 ID 找组：本夹具的组是**名字**，所以 id 查不到
        LIFT_CHECK(!res.iconGroup(1, &ids, &err));
        LIFT_CHECK(!err.empty());
    }

    {
        g_current_case = "pure-version";
        lift::PeImage img;
        img.bytes = makePe();
        std::string err;
        LIFT_CHECK(lift::parsePeImage(img, &err));
        lift::PeResources res;
        LIFT_CHECK(res.init(img, &err));

        std::vector<std::uint8_t> blob;
        LIFT_CHECK(res.versionInfo(&blob, &err));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(blob.size()), kVersionBlobSize);
        std::size_t off = 0;
        LIFT_CHECK(lift::PeResources::findFixedFileInfo(blob, &off, &err));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(off), 40u);
        if (off + 52 <= blob.size()) {
            std::uint32_t sig = 0;
            std::uint32_t fv_ms = 0;
            std::memcpy(&sig, blob.data() + off, 4);
            std::memcpy(&fv_ms, blob.data() + off + 8, 4);
            LIFT_CHECK_EQ_U32(sig, lift::kVsFixedFileInfoSignature);
            LIFT_CHECK_EQ_U32(fv_ms, 0x00040001u);
        }
        // 空块 ⇒ false（不能"找到 0 字节的固定信息"）
        std::vector<std::uint8_t> empty;
        LIFT_CHECK(!lift::PeResources::findFixedFileInfo(empty, &off, &err));
        LIFT_CHECK(!err.empty());
    }

    {
        // 没有资源目录的镜像：init 成功、hasResources false、查找一律 false（不崩）
        g_current_case = "pure-no-resources";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, kOptOff + 96 + 8 * 2, 0);
        put32(img.bytes, kOptOff + 96 + 8 * 2 + 4, 0);
        std::string err;
        LIFT_CHECK(lift::parsePeImage(img, &err));
        LIFT_CHECK_EQ_U32(img.resource_rva, 0u);
        lift::PeResources res;
        LIFT_CHECK(res.init(img, &err));
        LIFT_CHECK(!res.hasResources());
        lift::PeResourceEntry e;
        LIFT_CHECK(!res.findEntry(lift::kRtVersion, 1, &e, &err));
        LIFT_CHECK(!err.empty());
        std::vector<std::uint8_t> blob;
        LIFT_CHECK(!res.versionInfo(&blob, &err));
        LIFT_CHECK(!err.empty());
    }

    {
        // 坏目录：声明大小 < 一个目录头 ⇒ init 拒绝（⛔ 不是"当作没有资源"）
        g_current_case = "pure-bad-dir-size";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, kOptOff + 96 + 8 * 2 + 4, 8);  // resource_size = 8
        std::string err;
        LIFT_CHECK(lift::parsePeImage(img, &err));
        lift::PeResources res;
        LIFT_CHECK(!res.init(img, &err));
        LIFT_CHECK(!err.empty());
    }

    {
        // 坏目录项数：根目录声明 5000 项 ⇒ 查找时必须响亮失败（⛔ 不静默读越界）
        g_current_case = "pure-bad-dir-entries";
        lift::PeImage img;
        img.bytes = makePe();
        put16(img.bytes, kRsrcRaw + kRoot + 14, 5000);  // NumberOfIdEntries
        std::string err;
        LIFT_CHECK(lift::parsePeImage(img, &err));
        lift::PeResources res;
        LIFT_CHECK(res.init(img, &err));
        lift::PeResourceEntry e;
        LIFT_CHECK(!res.findEntry(lift::kRtIcon, 1, &e, &err));
        LIFT_CHECK(!err.empty());
    }

    {
        // 坏数据项：资源数据的 RVA 指到节外 ⇒ 查找必须失败（壳改过的镜像的典型症状）
        g_current_case = "pure-bad-data-rva";
        lift::PeImage img;
        img.bytes = makePe();
        put32(img.bytes, kRsrcRaw + kIcon1Data, 0x00777777);
        std::string err;
        LIFT_CHECK(lift::parsePeImage(img, &err));
        lift::PeResources res;
        LIFT_CHECK(res.init(img, &err));
        lift::PeResourceEntry e;
        LIFT_CHECK(!res.findEntry(lift::kRtIcon, 1, &e, &err));
        LIFT_CHECK(!err.empty());
    }

    // =====================================================================================
    // assets：入库的拆壳件（没有就 SKIP；⛔ 不假绿）
    // =====================================================================================
    const char* exe = std::getenv("LIFT_AGE_EXE");
    if (!exe || !*exe) exe = std::getenv("AMAYUI_AGE_EXE");
    if (!exe || !*exe) LIFT_SKIP("没有 LIFT_AGE_EXE / AMAYUI_AGE_EXE（要 corpus 里的拆壳件）");

    g_current_case = "age-load";
    lift::PeImage img;
    std::string err;
    if (!lift::loadPeFile(exe, &img, &err)) {
        std::fprintf(stderr, "loadPeFile(%s): %s\n", exe, err.c_str());
        return 1;
    }
    std::printf("AGE 镜像：%s（%zu 字节，sha256=%s）\n", exe, img.bytes.size(), img.sha256.c_str());
    LIFT_CHECK(img.resource_rva != 0);
    LIFT_CHECK(img.resource_size != 0);

    lift::PeResources res;
    LIFT_CHECK(res.init(img, &err));
    LIFT_CHECK(res.hasResources());
    if (!res.hasResources()) {
        std::fprintf(stderr, "资源目录解析失败：%s\n", err.c_str());
        return finish("pe-resource");
    }

    // ---- ① RT_VERSION / VS_FIXEDFILEINFO ----
    {
        g_current_case = "age-version";
        std::vector<std::uint8_t> blob;
        LIFT_CHECK(res.versionInfo(&blob, &err));
        if (blob.empty()) std::fprintf(stderr, "versionInfo: %s\n", err.c_str());
        LIFT_CHECK(!blob.empty());
        std::printf("AGE.EXE 的 RT_VERSION 数据块 = %zu 字节\n", blob.size());
        std::size_t off = 0;
        LIFT_CHECK(lift::PeResources::findFixedFileInfo(blob, &off, &err));
        if (off + 52 <= blob.size()) {
            std::uint32_t sig = 0;
            std::uint32_t fv_ms = 0;
            std::uint32_t fv_ls = 0;
            std::memcpy(&sig, blob.data() + off, 4);
            std::memcpy(&fv_ms, blob.data() + off + 8, 4);
            std::memcpy(&fv_ls, blob.data() + off + 12, 4);
            LIFT_CHECK_EQ_U32(sig, lift::kVsFixedFileInfoSignature);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(off) % 4, 0u);
            std::printf("VS_FIXEDFILEINFO @ +0x%zX（52 字节），文件版本 %u.%u.%u.%u\n", off,
                        (fv_ms >> 16) & 0xFFFFu, fv_ms & 0xFFFFu, (fv_ls >> 16) & 0xFFFFu, fv_ls & 0xFFFFu);
        }
    }

    // ---- ② RT_GROUP_ICON → RT_ICON ----
    {
        g_current_case = "age-icon-group";
        std::vector<lift::PeResourceName> groups;
        const bool listed = res.listNames(lift::kRtGroupIcon, &groups, &err);
        LIFT_CHECK(listed);
        if (!listed) {
            std::printf("本镜像没有 RT_GROUP_ICON 类型（%s）；只断言版本资源 —— ⛔ 不硬编成功\n",
                        err.c_str());
        } else if (groups.empty()) {
            // ★ 不硬编："本镜像没有组图标"本身也是一个可断言的事实
            std::printf("本镜像没有 RT_GROUP_ICON 条目（目录是空的）⇒ 版本资源仍须存在\n");
            LIFT_CHECK(groups.empty());
        } else {
            std::printf("AGE.EXE 有 %zu 组 RT_GROUP_ICON\n", groups.size());
            for (const lift::PeResourceName& g : groups) {
                std::vector<std::uint32_t> ids;
                std::string gerr;
                const bool ok = g.is_name ? res.iconGroupByName(g.name, &ids, &gerr)
                                          : res.iconGroup(g.id, &ids, &gerr);
                if (g.is_name) {
                    std::printf("  组 \"%s\" ⇒ %zu 个图标", g.name.c_str(), ids.size());
                } else {
                    std::printf("  组 ID %u ⇒ %zu 个图标", g.id, ids.size());
                }
                if (!ok) std::printf("（解析失败：%s）", gerr.c_str());
                std::printf("\n");
                LIFT_CHECK(ok);
                LIFT_CHECK(!ids.empty());
                // 组声明的每个 ID 都要能在 RT_ICON 里找到，且大小 > 0
                std::size_t resolved = 0;
                for (std::uint32_t id : ids) {
                    lift::PeResourceEntry ie;
                    std::string ierr;
                    if (res.findEntry(lift::kRtIcon, id, &ie, &ierr) && ie.size > 0) ++resolved;
                }
                LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(resolved),
                                  static_cast<std::int64_t>(ids.size()));
            }
        }
    }

    return finish("pe-resource");
}
