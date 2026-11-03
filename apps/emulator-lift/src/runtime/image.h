// emulator-lift · PE 载入（节表 / 导入表 / 入口 / sha256）+ guest 进程结构
//
// ★ 镜像只能认 **PE 节表**（README §2.6；证据见 plan §3 F3：`.lst` 只出现 6 段，seg002 有 11065 行 `db ?`）
//   ⇒ ⛔ 不要从 `.lst` 复原因像，也不要把 `.c` 里的 `dword_XXXX` 当成 C 变量。
// ★ 执行源的字节由 sha256 自证：必须等于 `.lst` 头部的 `; Input SHA256 :`（plan §4 D1）。
//   不符 ⇒ **拒绝启动**并把两侧都打印出来（⛔ 不许自动改用另一份 exe）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/memory.h"

namespace lift {

struct PeSectionInfo {
    char name[9] = {};
    std::uint32_t va = 0;          // RVA
    std::uint32_t vsize = 0;
    std::uint32_t raw_offset = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t characteristics = 0;
    bool executable() const { return (characteristics & 0x20000000u) != 0; }
    bool written() const { return (characteristics & 0x80000000u) != 0; }
};

struct PeImportInfo {
    std::string module;    // 大写还是原样？保持 PE 里的原样（对照 census 用原样）
    std::string name;      // 按名字导入时非空
    std::uint32_t iat_rva = 0;
    std::uint16_t ordinal = 0;
    bool by_ordinal = false;
};

struct PeImage {
    std::string path;
    std::vector<std::uint8_t> bytes;
    std::string sha256;            // 小写十六进制
    std::uint32_t image_base = 0;
    std::uint32_t entry_rva = 0;
    std::uint32_t entry_ea = 0;
    std::uint32_t size_of_image = 0;
    std::uint32_t size_of_headers = 0;
    std::uint32_t size_of_stack_reserve = 0;
    std::vector<PeSectionInfo> sections;
    std::vector<PeImportInfo> imports;

    const PeSectionInfo* sectionOfRva(std::uint32_t rva) const;
};

bool loadPeFile(const std::string& path, PeImage* out, std::string* err);
// 解析已经读进 `bytes` 的镜像（`loadPeFile` 内部调它；测试可以直接构造字节后用）
bool parsePeImage(PeImage& img, std::string* err);
// 把节按 `image_base + RVA` 映射进 guest（提交 + 清 .bss 部分）
bool mapPeImage(GuestMemory& mem, const PeImage& img, std::string* err);

// guest 的地址布局（README §2.6 的建议值，集中在结构里而不是散落成常量）
struct GuestLayout {
    std::uint32_t image_base = 0x00400000u;   // 实际从 PE 头取；这里只作缺省
    std::uint32_t stack_top = 0x70000000u;    // 栈顶（向下增长）
    std::uint32_t stack_size = 0x00100000u;   // 1 MiB 已提交
    std::uint32_t stack_guard = 0x00001000u;  // 栈底再往下 1 页 guard（**不提交**）
    std::uint32_t heap_base = 0x10000000u;
    std::uint32_t heap_size = 0x08000000u;
    std::uint32_t teb_ea = 0x7FFD0000u;
    std::uint32_t peb_ea = 0x7FFD1000u;
    std::uint32_t tls_array_ea = 0x7FFD2000u;  // TEB+0x2C 指向它（64 个槽）
    std::uint32_t shim_page = 0x7FFE0000u;     // 导入桩页（每个导入一个唯一地址）
    std::uint32_t shim_slots = 0x2000u;        // 最多 2048 个导入（4 字节步进 = 8 KiB）
    std::uint32_t argv_block = 0x7FFD4000u;    // 命令行 / 环境块（CRT 启动读它）
};

struct ProcessSetup {
    std::uint32_t stack_top = 0;      // 初始 esp 取这个值（8 字节对齐）
    std::uint32_t stack_bottom = 0;
    std::uint32_t teb = 0;
    std::uint32_t peb = 0;
    std::uint32_t cmdline_ea = 0;     // guest 里的命令行字符串
    std::uint32_t exe_path_ea = 0;
};

// 建栈（1 MiB + guard 页）+ TEB/PEB + 命令行块。
// ★ fs 段基址指向 `teb`（由 CPU 的 `fsBase` 表示；⛔ 不是宿主的 fs）。
bool setupProcessStructures(GuestMemory& mem, const PeImage& img, const GuestLayout& layout,
                            ProcessSetup* out, std::string* err);

}  // namespace lift
