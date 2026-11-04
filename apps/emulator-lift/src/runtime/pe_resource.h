// emulator-lift · PE 资源目录（`.rsrc`）的只读视图：RT_ICON / RT_GROUP_ICON / VS_VERSIONINFO
//
// ★ 口径（plan §4 D6 + 用户 2026-11-04 拍板）：
//   · 本文件不认识"哪个镜像"，只解析传进来的 `PeImage`：AGE.EXE 与 **真实装载的 AGERC 镜像**
//     都可以传进来；由调用方决定"资源读不到时 hard error 还是回退"（D6：⛔ 不编造资源）。
//   · 只读：不碰 guest 内存、不登记 shim、不改 PE 字节（`PeImage::bytes` 是 const 语义）。
//   · **解析失败一律响亮**：目录越界 / 名字层不是目录 / 数据 RVA 不落在任何节里 ⇒ 返回 false + `err`，
//     ⛔ 不"跳过这一项当作没有资源"（那会让 A5 的"缺口 = 0"假绿，见 plan §2.6 的载入器债务）。
//
// ★ 资源目录的结构（PE32 / IMAGE_RESOURCE_DIRECTORY）：
//     目录项 = 16 字节头 + N×8 字节项；每一项的 `Name` 高位 = 1 ⇒ 指向 UTF-16 串（偏移相对资源目录起点），
//     否则是数字 ID；`OffsetToData` 高位 = 1 ⇒ 子目录（偏移相对资源目录起点），否则是
//     IMAGE_RESOURCE_DATA_ENTRY（16 字节：OffsetToData=**RVA** / Size / CodePage / Reserved）。
//     Windows 固定三层：type → name/id → language → data entry。
//   ⇒ 三层里**语言层不做过滤**：按目录顺序取第一条（确定性）。本作的两个镜像每个名字下都只有 1041。
//
// ★ 资源数据块的 `OffsetToData` 是 **RVA**（不是文件偏移），所以它可能落在 `.rsrc` 之外
//   （实测：未拆壳的 AGERC.DLL 的 rsrc 数据项就指向 `.data`）；这里按节表映射，允许落在任何节里。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/image.h"

namespace lift {

// 本线用到的三个资源类型（其余类型照旧用数字查）
constexpr std::uint16_t kRtIcon = 3;        // RT_ICON：单个图标的位图数据
constexpr std::uint16_t kRtGroupIcon = 14;  // RT_GROUP_ICON：GRPICONDIR（组内图标 ID 列表）
constexpr std::uint16_t kRtVersion = 16;    // RT_VERSION：VS_VERSIONINFO

// VS_FIXEDFILEINFO 的签名（dwSignature = 0xFEEF04BD，结构 52 字节）
constexpr std::uint32_t kVsFixedFileInfoSignature = 0xFEEF04BDu;
constexpr std::uint32_t kVsFixedFileInfoSize = 52u;

// 一条命中的资源数据项（`IMAGE_RESOURCE_DATA_ENTRY` + 命中路径上的名字/语言，日志用）
struct PeResourceEntry {
    std::uint32_t rva = 0;        // 数据块在镜像里的 RVA（⚠ 不是文件偏移）
    std::uint32_t size = 0;       // 数据块字节数
    std::uint32_t codepage = 0;   // IMAGE_RESOURCE_DATA_ENTRY.CodePage（资源自己声明的，不是线程代码页）
    bool has_name = false;        // 第二层命中项是不是字符串名
    std::string name;             // 字符串名（`has_name == true` 时非空）
    std::uint32_t id = 0;         // 数字 ID（`has_name == false` 时有效）
    bool has_language_name = false;
    std::string language_name;    // 语言层的字符串名（罕见；一般是数字 LCID）
    std::uint32_t language = 0;   // 语言层数字 LCID（`has_language_name == false` 时有效）
};

// 某一类型下的一层名字（数字 ID 或字符串名）—— 给"按名没找到 ⇒ 退到第一组"这类确定性回退用
struct PeResourceName {
    bool is_name = false;  // true = 字符串名；false = 数字 ID
    std::uint32_t id = 0;
    std::string name;
};

class PeResources {
public:
    // 解析 `img.resource_rva` / `img.resource_size` 指向的资源目录。
    //   · 成功且 `hasResources() == false` ⇒ 镜像没有资源目录（不是错误）；
    //   · 返回 false ⇒ 资源目录**声明**存在但坏了（越界 / 头部读不出），`err` 说明原因。
    //   ⚠ 目录内部各项**惰性校验**（查哪一项就校验到哪一项）：资源声明的大小可以大于文件
    //     （未拆壳的 AGE.EXE / AGERC.DLL 就是这样），只有真正越界的项才算坏。
    bool init(const PeImage& img, std::string* err);

    bool hasResources() const { return ready_; }
    const PeImage* image() const { return img_; }

    // 按 数字ID / 字符串名 找三层下的数据项。找不到 ⇒ false + `err`（"没有这一项"也是 false）。
    bool findEntry(std::uint16_t type, std::uint32_t id, PeResourceEntry* out, std::string* err) const;
    bool findEntryByName(std::uint16_t type, const std::string& name, PeResourceEntry* out,
                         std::string* err) const;

    // 列出某类型下的全部名字（顺序 = 目录项顺序）。类型不存在 ⇒ false + err。
    bool listNames(std::uint16_t type, std::vector<PeResourceName>* out, std::string* err) const;

    // 把数据项的内容拷出来（`size` 字节；⛔ 不越界读文件）。
    bool readData(const PeResourceEntry& entry, std::vector<std::uint8_t>* out, std::string* err) const;

    // RT_GROUP_ICON：解析 GRPICONDIR，返回组内的 RT_ICON ID 列表（按组内顺序）。
    //   ⚠ 返回的是**组声明的 ID**，不保证每个 ID 在 RT_ICON 里都存在 —— 调用方可自行校验
    //     （`findEntry(kRtIcon, id)`）。
    bool iconGroup(std::uint32_t id, std::vector<std::uint32_t>* iconIds, std::string* err) const;
    bool iconGroupByName(const std::string& name, std::vector<std::uint32_t>* iconIds,
                         std::string* err) const;

    // RT_VERSION：优先 id = 1（Windows 资源脚本的惯例），没有就取该类型下第一个名字。
    // 返回的是**整块 VS_VERSIONINFO 字节**（= GetFileVersionInfoSize 的返回值）。
    bool versionInfo(std::vector<std::uint8_t>* out, std::string* err) const;

    // 在版本块里找 VS_FIXEDFILEINFO（按 4 字节对齐扫签名 0xFEEF04BD；找到后还要有 52 字节）。
    // ⇒ `off` 是块内偏移，`VerQueryValueA("\")` 的返回值就是 `块指针 + off`、长度 52。
    static bool findFixedFileInfo(const std::vector<std::uint8_t>& block, std::size_t* off,
                                  std::string* err);

private:
    // 目录项（内部形态；`offset` 相对资源目录起点）
    struct DirEntry {
        bool is_dir = false;
        bool is_name = false;
        std::uint32_t id = 0;
        std::string name;
        std::uint32_t offset = 0;
    };

    bool readDir(std::uint32_t rel, std::vector<DirEntry>* out, std::string* err) const;
    bool readName(std::uint32_t rel, std::string* out, std::string* err) const;
    bool readDataEntry(std::uint32_t rel, PeResourceEntry* out, std::string* err) const;
    // 在 `type` 下按 (id | name) 走到第三层，取第一条语言项
    bool locate(std::uint16_t type, bool by_name, std::uint32_t id, const std::string& name,
                PeResourceEntry* out, std::string* err) const;
    bool parseIconGroup(const PeResourceEntry& entry, std::vector<std::uint32_t>* iconIds,
                        std::string* err) const;
    // 边界：`rel` 起 `n` 字节必须同时在「资源目录声明的大小」与「文件」里
    bool inDir(std::uint64_t rel, std::uint64_t n) const;
    const std::uint8_t* at(std::uint64_t rel) const { return img_->bytes.data() + base_ + rel; }

    const PeImage* img_ = nullptr;
    bool ready_ = false;
    std::uint32_t base_ = 0;      // 资源目录在**文件**里的偏移
    std::uint32_t res_size_ = 0;  // 可用的资源目录字节数（= min(声明大小, 文件剩余)）
};

}  // namespace lift
