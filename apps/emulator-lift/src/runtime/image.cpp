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

#include "runtime/sha256.h"

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
        *out = rva;
        return true;
    }
    return false;
}

std::string readAscii(const PeImage& img, std::uint32_t rva) {
    std::uint32_t off = 0;
    if (!rvaToOffset(img, rva, &off)) return {};
    std::string s;
    while (off < img.bytes.size() && img.bytes[off] != 0) s.push_back(static_cast<char>(img.bytes[off++]));
    return s;
}

}  // namespace

const PeSectionInfo* PeImage::sectionOfRva(std::uint32_t rva) const {
    for (const PeSectionInfo& s : sections) {
        if (rva >= s.va && rva < s.va + (s.vsize ? s.vsize : s.raw_size)) return &s;
    }
    return nullptr;
}

bool loadPeFile(const std::string& path, PeImage* out, std::string* err) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "打不开文件：" + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        if (err) *err = "文件为空：" + path;
        return false;
    }
    out->path = path;
    out->bytes.resize(static_cast<std::size_t>(size));
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
    const std::size_t dd = opt + 96;  // PE32 的 DataDirectory 起点
    if (!ranges(b, dd, 8 * 2)) return true;  // 没有导入表也算成功（无导入的镜像）
    const std::uint32_t imp_rva = rdAt<std::uint32_t>(b, dd + 8 * 1, "ImportTable.RVA", err);
    if (imp_rva == 0) return true;
    std::uint32_t desc_off = 0;
    if (!rvaToOffset(img, imp_rva, &desc_off)) {
        if (err) *err = "导入表 RVA 不落在任何节里";
        return false;
    }
    for (std::size_t i = 0; i < 4096; ++i) {
        const std::size_t o = desc_off + i * 20;
        if (!ranges(b, o, 20)) break;
        const std::uint32_t orig_thunk = rdAt<std::uint32_t>(b, o + 0, "OriginalFirstThunk", err);
        const std::uint32_t name_rva = rdAt<std::uint32_t>(b, o + 12, "Name", err);
        const std::uint32_t first_thunk = rdAt<std::uint32_t>(b, o + 16, "FirstThunk", err);
        if (orig_thunk == 0 && name_rva == 0 && first_thunk == 0) break;
        const std::string module = readAscii(img, name_rva);
        const std::uint32_t thunk_rva = orig_thunk ? orig_thunk : first_thunk;
        std::uint32_t thunk_off = 0;
        if (!rvaToOffset(img, thunk_rva, &thunk_off)) continue;
        for (std::size_t k = 0; k < 4096; ++k) {
            const std::size_t to = thunk_off + k * 4;
            if (!ranges(b, to, 4)) break;
            const std::uint32_t entry = rdAt<std::uint32_t>(b, to, "thunk", err);
            if (entry == 0) break;
            PeImportInfo imp;
            imp.module = module;
            imp.iat_rva = first_thunk + static_cast<std::uint32_t>(k) * 4;
            if (entry & 0x80000000u) {
                imp.by_ordinal = true;
                imp.ordinal = static_cast<std::uint16_t>(entry & 0xFFFFu);
            } else {
                imp.name = readAscii(img, entry + 2);  // 跳过 2 字节 hint
            }
            img.imports.push_back(imp);
        }
    }
    return true;
}

bool mapPeImage(GuestMemory& mem, const PeImage& img, std::string* err) {
    // 头也映射进去（有些代码会读 PE 头；且 GetModuleHandle 的基址要能读到 MZ）
    const std::uint32_t hdr = img.size_of_headers ? img.size_of_headers : 0x400u;
    if (hdr && !mem.commitZeroed(img.image_base, hdr, RegionKind::Image, "pe-headers")) {
        if (err) *err = "映射 PE 头失败";
        return false;
    }
    for (const PeSectionInfo& s : img.sections) {
        const std::uint32_t vsize = s.vsize ? s.vsize : s.raw_size;
        if (vsize == 0) continue;
        char nm[64];
        std::snprintf(nm, sizeof(nm), "pe%s", s.name);
        if (!mem.commitZeroed(img.image_base + s.va, vsize, RegionKind::Image, nm)) {
            if (err) *err = std::string("映射节失败：") + s.name;
            return false;
        }
        const std::uint32_t copy = s.raw_size < vsize ? s.raw_size : vsize;
        if (copy && s.raw_offset + copy <= img.bytes.size()) {
            mem.writeBytes(img.image_base + s.va, img.bytes.data() + s.raw_offset, copy);
        }
    }
    return true;
}

bool setupProcessStructures(GuestMemory& mem, const PeImage& img, const GuestLayout& layout, ProcessSetup* out,
                            std::string* err) {
    (void)img;
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
    if (!mem.commitZeroed(layout.argv_block, 0x1000, RegionKind::Other, "cmdline")) {
        if (err) *err = "提交命令行块失败";
        return false;
    }
    const std::string exe_path = img.path;
    const std::string cmdline = "\"" + exe_path + "\"";
    mem.writeBytes(layout.argv_block, exe_path.c_str(), exe_path.size() + 1);
    out->exe_path_ea = layout.argv_block;
    out->cmdline_ea = layout.argv_block + 0x400;
    mem.writeBytes(out->cmdline_ea, cmdline.c_str(), cmdline.size() + 1);

    // ---- 环境块（`_setenvp` / GetEnvironmentStringsW 读；也是 A4 输入清单的一项）----
    // ★ 只放**确定性的最小集合**：宿主环境变量不进 guest（否则 trace 会随机器变）。
    static const char kEnvAnsi[] = "PATH=.\0TEMP=E:\\TMP\0\0";
    out->environment_ansi = "PATH=.;TEMP=E:\\TMP";
    out->env_ea = layout.argv_block + 0x800;
    std::uint32_t env_off = 0;
    for (std::size_t i = 0; i < sizeof(kEnvAnsi); ++i) {
        mem.wr16(out->env_ea + env_off, static_cast<std::uint16_t>(static_cast<unsigned char>(kEnvAnsi[i])));
        env_off += 2;
    }
    out->exe_path = exe_path;
    out->cmdline = cmdline;
    return true;
}

}  // namespace lift
