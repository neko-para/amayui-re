// emulator-lift · PE 载入 + guest 进程结构
//
// ★ 只认 PE 节表（plan §3 F3 的证据：`.lst` 只出现 6 段，且 seg002 有 11065 行 `db ?`）。
// ★ 固定 imageBase（0x400000）⇒ 不做重定位；换基址要先读 `.reloc`（M1 不做）。
// ★ sha256 是**启动前置条件**：载入器算一次，调用方拿 `.lst` 头部的值来比（A3 ⓪）。
//   ⛔ 不符就拒绝启动 —— 用错 exe 意味着"执行的字节"与"对照的语料"不是同一份。
#include "runtime/image.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <unistd.h>  // ftello（Windows 上走 _ftelli64，声明在 <cstdio>）
#endif

#include "runtime/sha256.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 路径）

namespace lift {
namespace {

template <typename T>
T rdAt(const std::vector<std::uint8_t>& b, std::size_t off, const char* what, std::string* err) {
    if (off + sizeof(T) > b.size()) {
        if (err) *err = std::string("PE 截断：读 ") + what + " 越过文件末尾";
        return T{};
    }
    T v{};
    std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

bool ranges(const std::vector<std::uint8_t>& b, std::size_t off, std::size_t n) { return off + n <= b.size(); }

// 错误信息里的数字一律**定宽十六进制**：`tests/pe_loader.cpp` 按这些字面量断言"原因真的写出来了"。
std::string hex32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(v));
    return buf;
}
std::string hex64(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%08llX", static_cast<unsigned long long>(v));
    return buf;
}

// RVA → 文件偏移（按节表换算；不落在任何节里 ⇒ false）
bool rvaToOffset(const PeImage& img, std::uint32_t rva, std::uint32_t* out) {
    for (const PeSectionInfo& s : img.sections) {
        if (rva >= s.va && rva < s.va + (s.vsize ? s.vsize : s.raw_size)) {
            const std::uint32_t delta = rva - s.va;
            if (delta >= s.raw_size) return false;  // 落在 .bss 部分（未初始化）
            *out = s.raw_offset + delta;
            return true;
        }
    }
    if (rva < img.size_of_headers) {
        // ★ 头部区域直接按 RVA 当文件偏移，但**必须还在文件里**（2026-11-04：只调 parsePeImage、不走
        //   mapPeImage 的消费方 —— 例如 `kernel32_version.cpp` 读版本资源 —— 否则会"截断头也解析成功"）。
        if (static_cast<std::size_t>(rva) >= img.bytes.size()) return false;
        *out = rva;
        return true;
    }
    return false;
}

// 读一个以 NUL 结尾的 ASCII 串（模块名 / 函数名）。
// ★ 返回 false = **读失败**，`why` 里是具体原因（⛔ 不许把"读不到"伪装成一个空串再让调用方猜）：
//   ① RVA 不落在任何节里（也不是 PE 头）；② 落在 .bss 部分（文件里没有这些字节）；
//   ③ 一路读到文件末尾都没有 NUL ⇒ 名字被截断 —— 旧实现会把截断的串**照用**（半读入）。
bool readAscii(const PeImage& img, std::uint32_t rva, std::string* out, std::string* why) {
    std::uint32_t off = 0;
    if (!rvaToOffset(img, rva, &off)) {
        std::string sec;
        if (const PeSectionInfo* s = img.sectionOfRva(rva)) sec = std::string("（落在节 ") + s->name + " 的 .bss 部分，文件里没有这些字节）";
        *why = "RVA = " + hex32(rva) + " 不落在任何节的 raw 数据里" + sec + "（文件长度 " + hex64(img.bytes.size()) + "）";
        return false;
    }
    std::string s;
    while (off < img.bytes.size() && img.bytes[off] != 0) s.push_back(static_cast<char>(img.bytes[off++]));
    if (off >= img.bytes.size()) {
        *why = "没有 NUL 终止符（RVA = " + hex32(rva) + "，读到文件末尾 " + hex64(img.bytes.size()) + "）";
        return false;
    }
    *out = s;
    return true;
}

}  // namespace

const PeSectionInfo* PeImage::sectionOfRva(std::uint32_t rva) const {
    for (const PeSectionInfo& s : sections) {
        if (rva >= s.va && rva < s.va + (s.vsize ? s.vsize : s.raw_size)) return &s;
    }
    return nullptr;
}

bool loadPeFile(const std::string& path, PeImage* out, std::string* err) {
    std::FILE* f = openHostFileUtf8(path, "rb");
    if (!f) {
        if (err) *err = "打不开文件：" + path;
        return false;
    }
    // ★ 64 位取文件长度，⛔ 不用 `std::ftell`（2026-11-04 实测：Windows 的 `long` 是 32 位 ⇒
    //   >2 GiB 的文件会拿到截断值/负值，而 `version!GetFileVersionInfo*` 会对游戏目录里的**任意** DLL 走这条路）。
    //   写法与 `kernel32_file.cpp` 的 fileSizeOf 对齐：Windows `_ftelli64` / POSIX `ftello`。
    std::fseek(f, 0, SEEK_END);
#ifdef _WIN32
    const std::int64_t size64 = static_cast<std::int64_t>(::_ftelli64(f));
#else
    const std::int64_t size64 = static_cast<std::int64_t>(::ftello(f));
#endif
    std::fseek(f, 0, SEEK_SET);
    if (size64 <= 0) {
        std::fclose(f);
        if (err) *err = "文件为空：" + path;
        return false;
    }
    if (static_cast<std::uint64_t>(size64) > 0xFFFFFFFFull) {
        // guest 是 32 位地址空间：>4 GiB 的镜像根本装不进去 ⇒ 明确拒绝，⛔ 不许截断后当成功。
        std::fclose(f);
        if (err) *err = "文件过大（>4 GiB）：" + path;
        return false;
    }
    out->path = path;
    out->bytes.resize(static_cast<std::size_t>(size64));
    const std::size_t got = std::fread(out->bytes.data(), 1, out->bytes.size(), f);
    std::fclose(f);
    if (got != out->bytes.size()) {
        if (err) *err = "读文件不完整：" + path;
        return false;
    }
    out->sha256 = toHex(sha256(out->bytes));
    return parsePeImage(*out, err);
}

bool parsePeImage(PeImage& img, std::string* err) {
    const auto& b = img.bytes;
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') {
        if (err) *err = "不是 PE：缺 MZ 头";
        return false;
    }
    const std::uint32_t pe_off = rdAt<std::uint32_t>(b, 0x3C, "e_lfanew", err);
    if (!ranges(b, pe_off, 24) || b[pe_off] != 'P' || b[pe_off + 1] != 'E' || b[pe_off + 2] != 0 ||
        b[pe_off + 3] != 0) {
        if (err) *err = "不是 PE：缺 PE\\0\\0 签名";
        return false;
    }
    const std::uint16_t num_sections = rdAt<std::uint16_t>(b, pe_off + 6, "NumberOfSections", err);
    const std::uint16_t opt_size = rdAt<std::uint16_t>(b, pe_off + 20, "SizeOfOptionalHeader", err);
    const std::size_t opt = static_cast<std::size_t>(pe_off) + 24;
    if (!ranges(b, opt, opt_size)) {
        if (err) *err = "PE 截断：optional header 越界";
        return false;
    }
    const std::uint16_t magic = rdAt<std::uint16_t>(b, opt, "Magic", err);
    if (magic != 0x010Bu) {
        if (err) *err = "本线只支持 32 位 PE（PE32 / 0x10B）；实际 magic=0x" + toHex(&b[opt], 2);
        return false;
    }
    img.entry_rva = rdAt<std::uint32_t>(b, opt + 16, "AddressOfEntryPoint", err);
    img.image_base = rdAt<std::uint32_t>(b, opt + 28, "ImageBase", err);
    img.size_of_image = rdAt<std::uint32_t>(b, opt + 56, "SizeOfImage", err);
    img.size_of_headers = rdAt<std::uint32_t>(b, opt + 60, "SizeOfHeaders", err);
    img.size_of_stack_reserve = rdAt<std::uint32_t>(b, opt + 72, "SizeOfStackReserve", err);
    img.entry_ea = img.image_base + img.entry_rva;

    const std::size_t sec_off = opt + opt_size;
    if (!ranges(b, sec_off, static_cast<std::size_t>(num_sections) * 40)) {
        if (err) *err = "PE 截断：节表越界";
        return false;
    }
    img.sections.clear();
    for (std::uint16_t i = 0; i < num_sections; ++i) {
        const std::size_t o = sec_off + static_cast<std::size_t>(i) * 40;
        PeSectionInfo s;
        std::memcpy(s.name, b.data() + o, 8);
        s.name[8] = 0;
        s.vsize = rdAt<std::uint32_t>(b, o + 8, "VirtualSize", err);
        s.va = rdAt<std::uint32_t>(b, o + 12, "VirtualAddress", err);
        s.raw_size = rdAt<std::uint32_t>(b, o + 16, "SizeOfRawData", err);
        s.raw_offset = rdAt<std::uint32_t>(b, o + 20, "PointerToRawData", err);
        s.characteristics = rdAt<std::uint32_t>(b, o + 36, "Characteristics", err);
        img.sections.push_back(s);
    }

    // ---- 导入表（DataDirectory[1]）----
    img.imports.clear();
    img.resource_rva = 0;
    img.resource_size = 0;
    const std::size_t dd = opt + 96;  // PE32 的 DataDirectory 起点
    if (!ranges(b, dd, 8 * 2)) return true;  // 没有导入表也算成功（无导入的镜像）
    // ★ 资源目录（DataDirectory[2]）：**只记录**，解析在 `runtime/pe_resource.h`。
    //   旧版 optional header 可能连 3 个目录项都放不下 ⇒ 缺这一项按"没有资源"处理。
    if (ranges(b, dd + 8 * 2, 8)) {
        img.resource_rva = rdAt<std::uint32_t>(b, dd + 8 * 2, "ResourceTable.RVA", err);
        img.resource_size = rdAt<std::uint32_t>(b, dd + 8 * 2 + 4, "ResourceTable.Size", err);
    }
    const std::uint32_t imp_rva = rdAt<std::uint32_t>(b, dd + 8 * 1, "ImportTable.RVA", err);
    if (imp_rva == 0) return true;
    std::uint32_t desc_off = 0;
    if (!rvaToOffset(img, imp_rva, &desc_off)) {
        if (err) *err = "导入表 DataDirectory[1].RVA 不落在任何节的 raw 数据里：RVA = " + hex32(imp_rva) +
                         "（文件长度 " + hex64(b.size()) + "）";
        return false;
    }
    // ★ 这一段里的每一条 `return false` 都是**拒绝启动**（与 sha256 不符同档）：
    //   旧实现对坏 RVA 只 `continue` / `break`，被吃掉的导入不会进 `imports` ⇒
    //   `importedButUnimplemented()` 少一条 ⇒ A5 的"静态缺口 = 0"假绿（本单 REQ-01M6JFZFCQ956CBFMKKQ37PCYK）。
    // ★ 两个循环都**不设条数上限**：终止条件只有"0 描述符 / 0 thunk"，越界/读不到一律报错。
    //   （旧实现的 `i < 4096` / `k < 4096` 会在第 4096 条静默返回 true —— 同一种"少读一点但看起来成功"。）
    for (std::size_t i = 0;; ++i) {
        const std::size_t o = desc_off + i * 20;
        // ★ 没有 0 终止项说明表被截断 ⇒ 拒绝启动（旧实现 break，把后面的导入静默吞掉）。
        if (!ranges(b, o, 20)) {
            if (err) *err = "导入描述符表没有 0 终止项：第 #" + std::to_string(i) +
                             " 条描述符越过文件末尾（文件长度 " + hex64(b.size()) + "）";
            return false;
        }
        const std::uint32_t orig_thunk = rdAt<std::uint32_t>(b, o + 0, "OriginalFirstThunk", err);
        const std::uint32_t name_rva = rdAt<std::uint32_t>(b, o + 12, "Name", err);
        const std::uint32_t first_thunk = rdAt<std::uint32_t>(b, o + 16, "FirstThunk", err);
        if (orig_thunk == 0 && name_rva == 0 && first_thunk == 0) break;
        const std::string where = "导入描述符 #" + std::to_string(i);
        std::string module;
        std::string why;
        if (!readAscii(img, name_rva, &module, &why)) {
            if (err) *err = where + " 的模块名读不到：" + why;
            return false;
        }
        if (module.empty()) {
            if (err) *err = where + " 的模块名为空（Name RVA = " + hex32(name_rva) + "）";
            return false;
        }
        if (first_thunk == 0) {
            if (err) *err = where + "（模块 " + module + "）的 FirstThunk 为 0（IAT 没有落点）";
            return false;
        }
        const std::uint32_t thunk_rva = orig_thunk ? orig_thunk : first_thunk;
        std::uint32_t thunk_off = 0;
        if (!rvaToOffset(img, thunk_rva, &thunk_off)) {
            if (err) *err = where + "（模块 " + module +
                             "）的导入 thunk 表 RVA 不落在任何节的 raw 数据里：RVA = " + hex32(thunk_rva);
            return false;
        }
        for (std::size_t k = 0;; ++k) {
            const std::size_t to = thunk_off + k * 4;
            if (!ranges(b, to, 4)) {
                if (err) *err = where + "（模块 " + module + "）的导入 thunk 表没有 0 终止项：第 #" +
                                 std::to_string(k) + " 条越过文件末尾（文件长度 " + hex64(b.size()) + "）";
                return false;
            }
            const std::uint32_t entry = rdAt<std::uint32_t>(b, to, "thunk", err);
            if (entry == 0) break;
            // IAT 槽位：`first_thunk + k*4` 必须用 **64 位**算 —— uint32 在这里会回绕成一个小 RVA，
            //   于是 IAT 绑到了 PE 头上还"看起来成功"。
            const std::uint64_t iat64 = static_cast<std::uint64_t>(first_thunk) + static_cast<std::uint64_t>(k) * 4u;
            if (iat64 > 0xFFFFFFFFull) {
                if (err) *err = where + "（模块 " + module + "）的 IAT 槽位 RVA 越过 4 GiB：" + hex64(iat64);
                return false;
            }
            const std::uint32_t iat_rva = static_cast<std::uint32_t>(iat64);
            // IAT 槽位必须落在这个镜像的**虚拟**范围里（绑定 IAT 要往里写）。
            // ⛔ 不要求它也在 raw 里：未初始化形态的 IAT 段是合法的（绑定前它是 0）。
            if (!img.sectionOfRva(iat_rva)) {
                if (err) *err = where + "（模块 " + module + "）的 IAT 槽位 RVA 不落在任何节里：" + hex32(iat_rva);
                return false;
            }
            PeImportInfo imp;
            imp.module = module;
            imp.iat_rva = iat_rva;
            // ★ 原始 RVA 原样保留（⛔ 不换算成文件偏移）：A5 的诊断要能打印 descriptor 身份。
            imp.original_first_thunk = orig_thunk;
            imp.first_thunk = first_thunk;
            imp.descriptor_index = static_cast<std::uint32_t>(i);
            if (entry & 0x80000000u) {
                imp.by_ordinal = true;
                imp.ordinal = static_cast<std::uint16_t>(entry & 0xFFFFu);
            } else {
                std::string fn_why;
                if (!readAscii(img, entry + 2, &imp.name, &fn_why)) {  // 跳过 2 字节 hint
                    if (err) *err = where + "（模块 " + module + "）第 #" + std::to_string(k) +
                                     " 个导入的函数名读不到（thunk = " + hex32(entry) + "）：" + fn_why;
                    return false;
                }
                if (imp.name.empty()) {
                    if (err) *err = where + "（模块 " + module + "）第 #" + std::to_string(k) +
                                     " 个导入的函数名为空（hint-name RVA = " + hex32(entry + 2) + "）";
                    return false;
                }
            }
            img.imports.push_back(imp);
        }
    }
    return true;
}

bool mapPeImage(GuestMemory& mem, const PeImage& img, std::string* err) {
    return mapPeImageAt(mem, img, img.image_base, err);
}

bool mapPeImageAt(GuestMemory& mem, const PeImage& img, std::uint32_t base, std::string* err) {
    // ★ `base + RVA + size` 必须在 guest 的 4 GiB 空间里（旧行为用 image_base 不会触发；
    //   AGERC 的换基址路径必须在这里挡住 uint32 回绕，⛔ 不静默截断）。
    auto inSpace = [](std::uint64_t begin, std::uint64_t size) {
        return begin + size <= GuestMemory::kSpace;
    };
    // 头也映射进去（有些代码会读 PE 头；且 GetModuleHandle 的基址要能读到 MZ）
    const std::uint32_t hdr = img.size_of_headers ? img.size_of_headers : 0x400u;

    // =========================================================================================
    // 第一遍：**只校验、一个字节都不写**。
    // ★ 为什么分两遍：本单的判据是"⛔ 不许半载入"。旧形态在下述任一失败之前已经把前面的节
    //   提交进去了 —— 调用方即使忽略返回值，guest 里也会留下一个"看起来像镜像"的东西
    //   （M2 的资源目录 / `LoadIconA` 会照着它解析，然后指到错的地方）。
    //   先全量校验再落盘 ⇒ 失败时 guest 里**零字节**（`tests/pe_loader.cpp` 的
    //   `section-raw-out-of-range` / `headers-out-of-range` 直接断言这一点）。
    // =========================================================================================
    if (!inSpace(base, img.size_of_image ? img.size_of_image : 0x1000u)) {
        if (err) *err = "映射 PE 失败：base(" + hex32(base) + ") + SizeOfImage(" + hex32(img.size_of_image) +
                         ") 越过 guest 的 4 GiB 空间";
        return false;
    }
    // ★ 声明的头长度超出文件 ⇒ 文件连自己的头都不完整（旧实现 `copy = min(hdr, 文件长)` 静默截断）。
    if (img.size_of_headers && img.size_of_headers > img.bytes.size()) {
        if (err) *err = "PE 头越界：SizeOfHeaders = " + hex32(img.size_of_headers) + " > 文件长度 " +
                         hex64(img.bytes.size());
        return false;
    }
    for (const PeSectionInfo& s : img.sections) {
        const std::uint32_t vsize = s.vsize ? s.vsize : s.raw_size;
        // 空节（VirtualSize 与 SizeOfRawData 都是 0）是合法的对齐产物：不映射、不算坏。
        if (vsize == 0) continue;
        if (!inSpace(static_cast<std::uint64_t>(base) + s.va, vsize)) {
            if (err) *err = std::string("映射节失败（base + RVA + vsize 越过 4 GiB）：") + s.name + "（RVA " +
                             hex32(s.va) + "，vsize " + hex32(vsize) + "）";
            return false;
        }
        // ★ **要拷的字节 = min(SizeOfRawData, VirtualSize)**，⛔ 不是 SizeOfRawData。
        //   `SizeOfRawData < VirtualSize` 是**合法的正常形态**（BSS 尾巴 / 资源虚拟尾部 ——
        //   尾部的字节在文件里根本不存在，载入后是 0）。把它当坏节会把正常镜像拒掉。
        //   `SizeOfRawData > VirtualSize`（文件内对齐填充）也合法：只拷 min，填充不进 guest。
        //   `raw_offset + size` 在 uint32 里会回绕，所以这里一律用 64 位算。
        const std::uint32_t copy = s.raw_size < vsize ? s.raw_size : vsize;
        const std::uint64_t copy_end = static_cast<std::uint64_t>(s.raw_offset) + copy;
        if (copy && copy_end > img.bytes.size()) {
            if (err) *err = std::string("节 raw 数据越界：") + s.name + "（RVA " + hex32(s.va) + "，PointerToRawData " +
                             hex32(s.raw_offset) + " + 要拷的 " + hex32(copy) + " = " + hex64(copy_end) +
                             " > 文件长度 " + hex64(img.bytes.size()) + "）";
            return false;
        }
        // ★ 2026-11-05 加严（REQ-01M6KK2C6TKBA88M6EXG1H7E5B）：**整个 raw 声明区间**必须落在文件里。
        //   旧判据只查 `min(SizeOfRawData, VirtualSize)` ⇒ `SizeOfRawData` 声称到文件外、但越界部分
        //   全在 VirtualSize 之后的"纯填充"会被静默接受（那些字节反正不会被拷）。
        //   为什么不误杀：只读扫过 `links/install` 的 12 份 PE32 / 67 个节（2026-11-05，
        //   脚本读 PE 头、不改安装树）——`PointerToRawData + SizeOfRawData > 文件长度` 的节 **0 个**；
        //   其中 26 节 raw_size > vsize（文件内对齐填充）、38 节 vsize > raw_size（虚拟尾部）、
        //   10 节 raw 恰好止于文件尾（其中 7 节是 raw_size > vsize 的文件内填充 ⇒ 边界必须是 `>` 不是 `>=`）。
        //   ⇒ 按"节头声称的区间必须整个在文件里"加严，对这份安装树零误杀；
        //   `tests/pe_loader.cpp` 的 `raw-range-at-eof-ok` 钉住"虚拟尾部 / 文件内填充 + raw 恰好止于 EOF"
        //   仍然接受。⛔ 不要退回"只看要拷的字节"：那是本单要消掉的静默接受。
        const std::uint64_t declared_end = static_cast<std::uint64_t>(s.raw_offset) + s.raw_size;
        if (s.raw_size && declared_end > img.bytes.size()) {
            const std::uint64_t overrun = declared_end - img.bytes.size();
            if (err) *err = std::string("节 raw 声明区间越过文件末尾（纯填充越界也拒绝）：") + s.name + "（RVA " +
                             hex32(s.va) + "，PointerToRawData " + hex32(s.raw_offset) + " + SizeOfRawData " +
                             hex32(s.raw_size) + " = " + hex64(declared_end) + " > 文件长度 " +
                             hex64(img.bytes.size()) + "，越界 " + hex64(overrun) + " 字节；要拷的 " +
                             hex32(copy) + " 字节未越界，多出的部分在 VirtualSize " + hex32(vsize) +
                             " 之后、文件里不存在）";
            return false;
        }
    }

    // =========================================================================================
    // 第二遍：提交 + 拷贝（到这里所有输入都已验证过；下面的失败只可能是宿主侧提交失败）。
    // =========================================================================================
    if (hdr && !mem.commitZeroed(base, hdr, RegionKind::Image, "pe-headers")) {
        if (err) *err = "映射 PE 头失败（" + hex32(base) + " + " + hex32(hdr) + "）";
        return false;
    }
    // ★ 2026-11-04：头的**字节**也要拷进 guest（旧实现只 commitZeroed ⇒ 基址读到的不是 MZ）。
    //   `GetModuleHandle` 的句柄是真 image_base / AGERC base，guest 可能按 PE 头反查模块。
    {
        const std::uint32_t copy = hdr < img.bytes.size() ? hdr : static_cast<std::uint32_t>(img.bytes.size());
        if (copy) mem.writeBytes(base, img.bytes.data(), copy);
    }
    for (const PeSectionInfo& s : img.sections) {
        const std::uint32_t vsize = s.vsize ? s.vsize : s.raw_size;
        if (vsize == 0) continue;
        char nm[64];
        std::snprintf(nm, sizeof(nm), "pe%s", s.name);
        if (!mem.commitZeroed(base + s.va, vsize, RegionKind::Image, nm)) {
            if (err) *err = std::string("映射节失败：") + s.name + "（RVA " + hex32(s.va) + "，vsize " +
                             hex32(vsize) + "）";
            return false;
        }
        const std::uint32_t copy = s.raw_size < vsize ? s.raw_size : vsize;
        if (copy) mem.writeBytes(base + s.va, img.bytes.data() + s.raw_offset, copy);
    }
    return true;
}

bool setupProcessStructures(GuestMemory& mem, const PeImage& img, const GuestLayout& layout,
                            const Vfs& vfs, ProcessSetup* out, std::string* err) {
    out->stack_top = layout.stack_top;
    out->stack_bottom = layout.stack_top - layout.stack_size;
    out->teb = layout.teb_ea;
    out->peb = layout.peb_ea;

    // ---- 栈：1 MiB 已提交 + 下面一页 guard（**不提交** ⇒ 一碰就 fault）----
    if (!mem.commitZeroed(out->stack_bottom, layout.stack_size, RegionKind::Stack, "guest-stack")) {
        if (err) *err = "提交 guest 栈失败";
        return false;
    }
    mem.annotate(out->stack_bottom - layout.stack_guard, layout.stack_guard, RegionKind::StackGuard, "stack-guard");

    // ---- TEB / PEB / TLS 数组 ----
    if (!mem.commitZeroed(layout.teb_ea, 0x1000, RegionKind::Teb, "teb")) {
        if (err) *err = "提交 TEB 失败";
        return false;
    }
    if (!mem.commitZeroed(layout.peb_ea, 0x1000, RegionKind::Peb, "peb")) {
        if (err) *err = "提交 PEB 失败";
        return false;
    }
    if (!mem.commitZeroed(layout.tls_array_ea, 0x1000, RegionKind::Teb, "tls-array")) {
        if (err) *err = "提交 TLS 数组失败";
        return false;
    }
    // TIB
    mem.wr32(layout.teb_ea + 0x00, 0xFFFFFFFFu);           // SEH 链末端
    mem.wr32(layout.teb_ea + 0x04, layout.stack_top);      // StackBase
    mem.wr32(layout.teb_ea + 0x08, out->stack_bottom);     // StackLimit
    mem.wr32(layout.teb_ea + 0x18, layout.teb_ea);         // Self
    mem.wr32(layout.teb_ea + 0x20, 0x00001000u);           // UniqueProcess
    mem.wr32(layout.teb_ea + 0x24, 0x00001004u);           // UniqueThread
    mem.wr32(layout.teb_ea + 0x2C, layout.tls_array_ea);   // ThreadLocalStoragePointer
    mem.wr32(layout.teb_ea + 0x30, layout.peb_ea);         // ProcessEnvironmentBlock
    // PEB
    mem.wr8(layout.peb_ea + 0x00, 0);                      // InheritedAddressSpace
    mem.wr8(layout.peb_ea + 0x02, 0);                      // BeingDebugged
    mem.wr32(layout.peb_ea + 0x08, img.image_base);        // ImageBaseAddress
    mem.wr32(layout.peb_ea + 0x18, 0x00010000u);           // ProcessHeap（与 shim 的 heap_handle 一致）

    // ---- 命令行 / 模块路径（CRT 启动会读）----
    // ★ 2026-11-05 用户口径：guest 眼里的 exe = **游戏目录里的 `AGE.EXE`**。
    //   ⛔ 宿主 `--exe` 字符串不能再出现在 `GetModuleFileNameA` / `GetCommandLineA` 里：
    //   引擎会从 exe 路径推导"安装目录"，多出来的宿主路径段会让它写错地方。
    //   guest_exe_path 的真源是 Vfs（guest 根 + `AGE.EXE`），不是 `img.path`。
    if (!mem.commitZeroed(layout.argv_block, 0x1000, RegionKind::Other, "cmdline")) {
        if (err) *err = "提交命令行块失败";
        return false;
    }
    const std::string guest_exe_path = vfs.guestExePath();
    const std::string cmdline = "\"" + guest_exe_path + "\"";
    mem.writeBytes(layout.argv_block, guest_exe_path.c_str(), guest_exe_path.size() + 1);
    out->exe_path_ea = layout.argv_block;
    out->cmdline_ea = layout.argv_block + 0x400;
    mem.writeBytes(out->cmdline_ea, cmdline.c_str(), cmdline.size() + 1);

    // ---- 环境块（`_setenvp` / GetEnvironmentStringsW 读；也是 A4 输入清单的一项）----
    // ★ **唯一真源** = `Vfs::guestEnvironment()`（与 GetEnvironmentVariable* / 特殊目录同源）。
    //   不含任何宿主环境变量（否则 trace 会随机器变）；没有 --appdata 时 AppData 派生项不提供。
    const std::vector<std::string> env = vfs.guestEnvironment();
    out->environment_ansi = vfs.guestEnvironmentText();
    out->env_ea = layout.argv_block + 0x800;
    std::uint32_t env_off = 0;
    for (const std::string& item : env) {
        for (const char c : item) {
            mem.wr16(out->env_ea + env_off, static_cast<std::uint16_t>(static_cast<unsigned char>(c)));
            env_off += 2;
        }
        mem.wr16(out->env_ea + env_off, 0);  // 每项结尾的 0
        env_off += 2;
    }
    mem.wr16(out->env_ea + env_off, 0);  // 整块的额外结尾 0（Windows 环境块以双 0 结束）
    out->exe_path = img.path;
    out->guest_exe_path = guest_exe_path;
    out->cmdline = cmdline;
    return true;
}

}  // namespace lift
