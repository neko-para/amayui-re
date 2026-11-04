// emulator-lift · PE 资源目录（`.rsrc`）解析（只读；口径见 pe_resource.h 文件头）
//
// ★ 为什么单独一个文件：资源解析是**纯函数式**的字节工作（`PeImage` 进、目录项出），
//   与 guest 内存 / shim 完全无关 ⇒ 可以直接用入库的拆壳件做判据（tests/pe_resource.cpp），
//   不需要一台 Machine、也不需要 `register.cpp`。
// ★ 这里的 `rvaToOffset` 与 `image.cpp` 的匿名命名空间版本**同一逻辑**（那份是文件内私有）。
//   ⚠ 两份必须同语义：改了一处就要改另一处（例如"RVA 落在 .bss 部分"的处理）。
#include "runtime/pe_resource.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace lift {
namespace {

constexpr std::uint32_t kMaxDirEntries = 4096;          // 单个目录的项数上限（防坏数据循环）
constexpr std::uint32_t kMaxResourceBytes = 64u << 20;  // 单个资源数据块上限（64 MiB）

// RVA → 文件偏移（与 image.cpp 的同名函数同语义：落在节的未初始化部分 ⇒ false）
bool rvaToOffset(const PeImage& img, std::uint32_t rva, std::uint32_t* out) {
    for (const PeSectionInfo& s : img.sections) {
        if (rva >= s.va && rva < s.va + (s.vsize ? s.vsize : s.raw_size)) {
            const std::uint32_t delta = rva - s.va;
            if (delta >= s.raw_size) return false;
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

template <typename T>
T rdAt(const std::uint8_t* p) {
    T v{};
    std::memcpy(&v, p, sizeof(T));
    return v;
}

std::string hex32(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return std::string(b);
}

std::string dec(std::uint64_t v) { return std::to_string(v); }

// 资源名的比较：Windows 的 loader 用**大小写不敏感**的比较（FindResource 的字符串名）
bool nameEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return true;
}

std::string typeText(std::uint16_t type) {
    switch (type) {
        case kRtIcon: return "RT_ICON(3)";
        case kRtGroupIcon: return "RT_GROUP_ICON(14)";
        case kRtVersion: return "RT_VERSION(16)";
        default: return "资源类型 " + std::to_string(type);
    }
}

}  // namespace

bool PeResources::init(const PeImage& img, std::string* err) {
    img_ = &img;
    ready_ = false;
    base_ = 0;
    res_size_ = 0;
    if (img.bytes.empty()) {
        if (err) *err = "空镜像：没有字节可解析资源";
        return false;
    }
    if (img.resource_rva == 0 || img.resource_size == 0) return true;  // 没有资源目录（不是错误）
    if (img.resource_size < 16) {
        if (err) *err = "资源目录声明的大小 " + dec(img.resource_size) + " 小于一个目录头（16 字节）";
        return false;
    }
    std::uint32_t off = 0;
    if (!rvaToOffset(img, img.resource_rva, &off)) {
        if (err) *err = "资源目录 RVA " + hex32(img.resource_rva) + " 不落在任何节里";
        return false;
    }
    if (static_cast<std::uint64_t>(off) + 16u > img.bytes.size()) {
        if (err) *err = "资源目录起点（文件偏移 " + hex32(off) + "）越过文件末尾";
        return false;
    }
    base_ = off;
    // ⚠ **不**要求"声明大小 <= 文件剩余"：未拆壳的镜像会声明一整片虚拟资源区（数据项指向别的节）。
    //   这里取两者的较小值当边界；真正越界的**条目**在查到它时报错（惰性校验）。
    const std::uint64_t avail = static_cast<std::uint64_t>(img.bytes.size()) - off;
    res_size_ = static_cast<std::uint32_t>(img.resource_size < avail ? img.resource_size : avail);
    ready_ = true;
    return true;
}

bool PeResources::inDir(std::uint64_t rel, std::uint64_t n) const {
    if (!ready_) return false;
    if (rel > res_size_ || n > static_cast<std::uint64_t>(res_size_) - rel) return false;
    return static_cast<std::uint64_t>(base_) + rel + n <= img_->bytes.size();
}

bool PeResources::readName(std::uint32_t rel, std::string* out, std::string* err) const {
    out->clear();
    if (!inDir(rel, 2)) {
        if (err) *err = "资源名字符串偏移 " + hex32(rel) + " 越过资源目录边界";
        return false;
    }
    const std::uint16_t chars = rdAt<std::uint16_t>(at(rel));
    if (!inDir(static_cast<std::uint64_t>(rel) + 2u, static_cast<std::uint64_t>(chars) * 2u)) {
        if (err) *err = "资源名字符串（" + dec(chars) + " 个 UTF-16 字符）越过资源目录边界";
        return false;
    }
    out->reserve(chars);
    for (std::uint32_t i = 0; i < chars; ++i) {
        const std::uint16_t c = rdAt<std::uint16_t>(at(static_cast<std::uint64_t>(rel) + 2u + i * 2u));
        // 资源名在本作的镜像里全是 ASCII（"IDI_ICON1"）；非 ASCII 只做占位（匹配用不到）。
        out->push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return true;
}

bool PeResources::readDir(std::uint32_t rel, std::vector<DirEntry>* out, std::string* err) const {
    out->clear();
    if (!ready_) {
        if (err) *err = "这个镜像没有资源目录（PeResources 没有就绪）";
        return false;
    }
    if (!inDir(rel, 16)) {
        if (err) *err = "资源目录头偏移 " + hex32(rel) + " 越过资源目录边界";
        return false;
    }
    const std::uint8_t* hdr = at(rel);
    const std::uint16_t named = rdAt<std::uint16_t>(hdr + 12);
    const std::uint16_t ids = rdAt<std::uint16_t>(hdr + 14);
    const std::uint32_t count = static_cast<std::uint32_t>(named) + static_cast<std::uint32_t>(ids);
    if (count > kMaxDirEntries) {
        if (err) *err = "资源目录项数 " + dec(count) + " 过多（上限 " + dec(kMaxDirEntries) + "）";
        return false;
    }
    if (!inDir(static_cast<std::uint64_t>(rel) + 16u, static_cast<std::uint64_t>(count) * 8u)) {
        if (err) *err = "资源目录项表（" + dec(count) + " 项）越过资源目录边界";
        return false;
    }
    out->reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* e = at(static_cast<std::uint64_t>(rel) + 16u + i * 8u);
        const std::uint32_t name_field = rdAt<std::uint32_t>(e);
        const std::uint32_t data_field = rdAt<std::uint32_t>(e + 4);
        DirEntry d;
        d.is_dir = (data_field & 0x80000000u) != 0;
        d.offset = data_field & 0x7FFFFFFFu;
        if ((name_field & 0x80000000u) != 0) {
            d.is_name = true;
            if (!readName(name_field & 0x7FFFFFFFu, &d.name, err)) return false;
        } else {
            d.id = name_field;
        }
        out->push_back(std::move(d));
    }
    return true;
}

bool PeResources::readDataEntry(std::uint32_t rel, PeResourceEntry* out, std::string* err) const {
    if (!inDir(rel, 16)) {
        if (err) *err = "IMAGE_RESOURCE_DATA_ENTRY 偏移 " + hex32(rel) + " 越过资源目录边界";
        return false;
    }
    const std::uint8_t* p = at(rel);
    const std::uint32_t rva = rdAt<std::uint32_t>(p);
    const std::uint32_t size = rdAt<std::uint32_t>(p + 4);
    const std::uint32_t codepage = rdAt<std::uint32_t>(p + 8);
    if (rva == 0 || size == 0) {
        if (err) *err = "资源数据项为空（RVA=" + hex32(rva) + " size=" + dec(size) + "）";
        return false;
    }
    if (size > kMaxResourceBytes) {
        if (err) *err = "资源数据项过大（" + dec(size) + " 字节 > 上限 " + dec(kMaxResourceBytes) + "）";
        return false;
    }
    std::uint32_t off = 0;
    if (!rvaToOffset(*img_, rva, &off)) {
        // ⚠ 注意：这是"数据 RVA 指到了节外 / .bss 部分"——未拆壳镜像的典型症状
        if (err) *err = "资源数据 RVA " + hex32(rva) + " 不落在任何已初始化节里（镜像被壳改过？）";
        return false;
    }
    if (static_cast<std::uint64_t>(off) + size > img_->bytes.size()) {
        if (err) *err = "资源数据（RVA " + hex32(rva) + "，" + dec(size) + " 字节）越过文件末尾";
        return false;
    }
    out->rva = rva;
    out->size = size;
    out->codepage = codepage;
    return true;
}

bool PeResources::locate(std::uint16_t type, bool by_name, std::uint32_t id, const std::string& name,
                         PeResourceEntry* out, std::string* err) const {
    if (!ready_) {
        if (err) *err = "这个镜像没有资源目录";
        return false;
    }
    std::vector<DirEntry> root;
    if (!readDir(0, &root, err)) return false;
    const DirEntry* type_entry = nullptr;
    for (const DirEntry& d : root) {
        if (!d.is_name && d.id == type) {
            type_entry = &d;
            break;
        }
    }
    if (!type_entry) {
        if (err) *err = "镜像里没有 " + typeText(type) + " 资源";
        return false;
    }
    if (!type_entry->is_dir) {
        if (err) *err = typeText(type) + " 的项不是目录（资源目录树少了一层）";
        return false;
    }
    std::vector<DirEntry> level2;
    if (!readDir(type_entry->offset, &level2, err)) return false;
    const DirEntry* name_entry = nullptr;
    for (const DirEntry& d : level2) {
        if (by_name) {
            if (d.is_name && nameEqual(d.name, name)) {
                name_entry = &d;
                break;
            }
        } else if (!d.is_name && d.id == id) {
            name_entry = &d;
            break;
        }
    }
    if (!name_entry) {
        if (err)
            *err = by_name ? (typeText(type) + " 下没有名字 \"" + name + "\"")
                           : (typeText(type) + " 下没有 ID " + dec(id));
        return false;
    }
    if (!name_entry->is_dir) {
        if (err) *err = typeText(type) + " 的名字层项不是目录（资源目录树少了一层）";
        return false;
    }
    std::vector<DirEntry> level3;
    if (!readDir(name_entry->offset, &level3, err)) return false;
    if (level3.empty()) {
        if (err) *err = typeText(type) + " 的名字层下没有语言项（资源目录是空的）";
        return false;
    }
    // ★ 语言层**不做过滤**：按目录顺序取第一条（确定性）。Windows 会按线程 locale 选最佳匹配；
    //   本作的镜像每个名字下只有 1041 一条，差异不可观测（若将来遇到多语言镜像，从这里扩展）。
    const DirEntry& lang = level3.front();
    if (lang.is_dir) {
        if (err) *err = typeText(type) + " 的语言层项是目录（资源目录树多了一层）";
        return false;
    }
    PeResourceEntry e;
    if (!readDataEntry(lang.offset, &e, err)) return false;
    e.has_name = name_entry->is_name;
    e.name = name_entry->name;
    e.id = name_entry->id;
    e.has_language_name = lang.is_name;
    e.language_name = lang.name;
    e.language = lang.id;
    *out = std::move(e);
    return true;
}

bool PeResources::findEntry(std::uint16_t type, std::uint32_t id, PeResourceEntry* out,
                            std::string* err) const {
    return locate(type, false, id, std::string(), out, err);
}

bool PeResources::findEntryByName(std::uint16_t type, const std::string& name, PeResourceEntry* out,
                                  std::string* err) const {
    if (name.empty()) {
        if (err) *err = "资源名是空串（数字 ID 要走 findEntry）";
        return false;
    }
    return locate(type, true, 0, name, out, err);
}

bool PeResources::listNames(std::uint16_t type, std::vector<PeResourceName>* out,
                            std::string* err) const {
    out->clear();
    if (!ready_) {
        if (err) *err = "这个镜像没有资源目录";
        return false;
    }
    std::vector<DirEntry> root;
    if (!readDir(0, &root, err)) return false;
    const DirEntry* type_entry = nullptr;
    for (const DirEntry& d : root) {
        if (!d.is_name && d.id == type) {
            type_entry = &d;
            break;
        }
    }
    if (!type_entry) {
        if (err) *err = "镜像里没有 " + typeText(type) + " 资源";
        return false;
    }
    if (!type_entry->is_dir) {
        if (err) *err = typeText(type) + " 的项不是目录（资源目录树少了一层）";
        return false;
    }
    std::vector<DirEntry> level2;
    if (!readDir(type_entry->offset, &level2, err)) return false;
    out->reserve(level2.size());
    for (const DirEntry& d : level2) {
        PeResourceName n;
        n.is_name = d.is_name;
        n.id = d.id;
        n.name = d.name;
        out->push_back(std::move(n));
    }
    return true;
}

bool PeResources::readData(const PeResourceEntry& entry, std::vector<std::uint8_t>* out,
                           std::string* err) const {
    out->clear();
    if (!ready_) {
        if (err) *err = "这个镜像没有资源目录";
        return false;
    }
    std::uint32_t off = 0;
    if (!rvaToOffset(*img_, entry.rva, &off)) {
        if (err) *err = "资源数据 RVA " + hex32(entry.rva) + " 不落在任何已初始化节里";
        return false;
    }
    if (entry.size == 0) {
        if (err) *err = "资源数据项的大小是 0";
        return false;
    }
    if (static_cast<std::uint64_t>(off) + entry.size > img_->bytes.size()) {
        if (err) *err = "资源数据越过文件末尾（RVA " + hex32(entry.rva) + "，" + dec(entry.size) + " 字节）";
        return false;
    }
    out->assign(img_->bytes.begin() + off, img_->bytes.begin() + off + entry.size);
    return true;
}

bool PeResources::parseIconGroup(const PeResourceEntry& entry, std::vector<std::uint32_t>* iconIds,
                                 std::string* err) const {
    iconIds->clear();
    std::vector<std::uint8_t> data;
    if (!readData(entry, &data, err)) return false;
    // GRPICONDIR：reserved(2) / type(2) / count(2) 后接 count × GRPICONDIRENTRY(14)
    if (data.size() < 6) {
        if (err) *err = "GRPICONDIR 只有 " + dec(data.size()) + " 字节（至少 6）";
        return false;
    }
    const std::uint16_t reserved = rdAt<std::uint16_t>(data.data());
    const std::uint16_t type = rdAt<std::uint16_t>(data.data() + 2);
    const std::uint16_t count = rdAt<std::uint16_t>(data.data() + 4);
    if (reserved != 0) {
        if (err) *err = "GRPICONDIR.reserved = " + dec(reserved) + " ≠ 0";
        return false;
    }
    if (type != 1) {
        if (err) *err = "GRPICONDIR.wType = " + dec(type) + " ≠ 1（不是图标组）";
        return false;
    }
    if (count == 0) {
        if (err) *err = "GRPICONDIR 的 wCount = 0（空图标组）";
        return false;
    }
    if (6u + static_cast<std::uint64_t>(count) * 14u > data.size()) {
        if (err)
            *err = "GRPICONDIR 声明 " + dec(count) + " 项，但数据只有 " + dec(data.size()) + " 字节";
        return false;
    }
    iconIds->reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t o = 6u + static_cast<std::size_t>(i) * 14u + 12u;  // nID
        iconIds->push_back(rdAt<std::uint16_t>(data.data() + o));
    }
    return true;
}

bool PeResources::iconGroup(std::uint32_t id, std::vector<std::uint32_t>* iconIds,
                            std::string* err) const {
    PeResourceEntry entry;
    if (!findEntry(kRtGroupIcon, id, &entry, err)) return false;
    return parseIconGroup(entry, iconIds, err);
}

bool PeResources::iconGroupByName(const std::string& name, std::vector<std::uint32_t>* iconIds,
                                  std::string* err) const {
    PeResourceEntry entry;
    if (!findEntryByName(kRtGroupIcon, name, &entry, err)) return false;
    return parseIconGroup(entry, iconIds, err);
}

bool PeResources::versionInfo(std::vector<std::uint8_t>* out, std::string* err) const {
    out->clear();
    if (!ready_) {
        if (err) *err = "这个镜像没有资源目录";
        return false;
    }
    PeResourceEntry entry;
    std::string why;
    if (!findEntry(kRtVersion, 1, &entry, &why)) {
        // 资源脚本的惯例是 ID 1；不是 1 就取该类型下第一个名字（确定性）。
        std::vector<PeResourceName> names;
        if (!listNames(kRtVersion, &names, err)) return false;
        if (names.empty()) {
            if (err) *err = "镜像里没有 RT_VERSION(16) 资源";
            return false;
        }
        const PeResourceName& first = names.front();
        const bool ok = first.is_name ? findEntryByName(kRtVersion, first.name, &entry, err)
                                      : findEntry(kRtVersion, first.id, &entry, err);
        if (!ok) return false;
    }
    return readData(entry, out, err);
}

bool PeResources::findFixedFileInfo(const std::vector<std::uint8_t>& block, std::size_t* off,
                                    std::string* err) {
    // 按 4 字节对齐扫签名：VS_VERSIONINFO 的所有节点都以 DWORD 对齐（wLength/wValueLength/wType
    // 之后补到 4 的倍数），VS_FIXEDFILEINFO 本身是 DWORD 结构 ⇒ 对齐扫描不会漏。
    for (std::size_t o = 0; o + kVsFixedFileInfoSize <= block.size(); o += 4) {
        if (rdAt<std::uint32_t>(block.data() + o) == kVsFixedFileInfoSignature) {
            if (off) *off = o;
            return true;
        }
    }
    if (err) {
        *err = "版本块（" + dec(block.size()) + " 字节）里没有 VS_FIXEDFILEINFO 签名 " +
               hex32(kVsFixedFileInfoSignature);
    }
    return false;
}

}  // namespace lift
