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
#include "runtime/vfs.h"

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
    // ★ 2026-11-05（REQ-01M6KK2C6TKBA88M6EXG1H7E5B）：导入描述符的身份 = PE 里的**原始 RVA**，
    //   ⛔ 不是换算后的文件偏移。`original_first_thunk == 0` 是合法形态（部分链接器只填 FirstThunk），
    //   这时要**如实保留 0**，不能拿 FirstThunk 冒名顶替（A5 的诊断要能说清"坏在哪张表"）。
    std::uint32_t original_first_thunk = 0;  // 描述符 +0x00 OriginalFirstThunk
    std::uint32_t first_thunk = 0;           // 描述符 +0x10 FirstThunk
    // 描述符在导入表里的序号（0 起）。同一描述符的多个函数共用同一个序号 ⇒ 诊断能回查"是哪一张表"。
    std::uint32_t descriptor_index = 0;
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
    // ★ 资源目录（DataDirectory[2]）的**原始值**：只记录，不在载入阶段解析。
    //   解析归 `runtime/pe_resource.h` 的 `PeResources`（plan §4 D6 / §6 P0）。
    //   两者都为 0 ⇒ 这个镜像没有资源目录（不是错误：无资源的镜像照样能跑）。
    std::uint32_t resource_rva = 0;
    std::uint32_t resource_size = 0;
    std::vector<PeSectionInfo> sections;
    std::vector<PeImportInfo> imports;

    const PeSectionInfo* sectionOfRva(std::uint32_t rva) const;
};

bool loadPeFile(const std::string& path, PeImage* out, std::string* err);
// 解析已经读进 `bytes` 的镜像（`loadPeFile` 内部调它；测试可以直接构造字节后用）
// ★ 坏输入一律**带原因返回 false**（⛔ 不许静默跳过：少读一条导入 ⇒ A5 的"静态缺口 = 0"假绿，
//   见 REQ-01M6JFZFCQ956CBFMKKQ37PCYK）。拒绝的形态：导入表 / thunk 表 RVA 解不出文件偏移、
//   thunk 表或描述符表没有 0 终止项、模块名 / 函数名读不到或没有 NUL 终止、FirstThunk 为 0、
//   IAT 槽位不落在任何节的虚拟范围里。
bool parsePeImage(PeImage& img, std::string* err);
// 把节按 `image_base + RVA` 映射进 guest（提交 + 清 .bss 部分）
// ★ "半载入"是不允许的：先**全量校验**（4 GiB 空间 / 头长度 / 每节的
//   `PointerToRawData + SizeOfRawData` 是否越出文件），再提交与拷贝 ⇒ 返回 false 时 guest 里**一个字节都没写**。
// ★ 要拷的字节 = `min(SizeOfRawData, VirtualSize)`（调用方自己填的结构也一样）：
//   `SizeOfRawData < VirtualSize` 是**合法的正常形态**（BSS 尾巴 / 资源虚拟尾部），⛔ 不是坏节。
// ★ 2026-11-05 加严：节头声明的**整个 raw 区间**必须落在文件里 —— 即使 `SizeOfRawData > VirtualSize`
//   的那段只是对齐填充、根本不会被拷进 guest，`PointerToRawData + SizeOfRawData > 文件长度` 也要拒绝。
//   取证与"为什么不会误杀真实 DLL"见 `image.cpp` 的 `mapPeImageAt` 第一遍注释。
bool mapPeImage(GuestMemory& mem, const PeImage& img, std::string* err);
// 同上，但映射到**指定的 guest 基址** `base`（不是 PE 头的 ImageBase）。
// ★ 2026-11-04 新口径：AGERC.DLL 作为**真实 guest 模块**装载 ⇒ 它的 ImageBase 可能与主镜像 /
//   堆 / 栈 / TEB / 桩页冲突，`Machine::loadAgercModule` 会先挑一个不冲突的 base 再调这里。
//   `mapPeImage` 等价于 `mapPeImageAt(mem, img, img.image_base, err)`（旧行为不变）。
bool mapPeImageAt(GuestMemory& mem, const PeImage& img, std::uint32_t base, std::string* err);

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
    std::uint32_t shim_slots = 0x2000u;        // 0x2000 = 8192 个槽（4 字节步进 = 32 KiB）
    std::uint32_t argv_block = 0x7FFD4000u;    // 命令行 / 环境块（CRT 启动读它）
};

struct ProcessSetup {
    std::uint32_t stack_top = 0;      // 初始 esp 取这个值（8 字节对齐）
    std::uint32_t stack_bottom = 0;
    std::uint32_t teb = 0;
    std::uint32_t peb = 0;
    std::uint32_t cmdline_ea = 0;     // guest 里的命令行字符串
    std::uint32_t exe_path_ea = 0;
    std::uint32_t env_ea = 0;         // guest 里的环境块（UTF-16，NAME=VALUE\0…\0\0）
    // ★ A4 的**输入清单**（plan §5 A4）：这几项一变，执行路径就可能变 ⇒ trace 必须记下来。
    std::string exe_path;             // `--exe` 传进来的**宿主**原始路径串（只进 trace 输入清单）
    std::string guest_exe_path;       // guest 读到的 exe 全路径（`<guest 根>\AGE.EXE`）
    std::string cmdline;              // guest 读到的命令行
    std::string environment_ansi;     // 环境块的 ANSI 视图（人读 / 哈希用；分隔符 ';'）
};

// 建栈（1 MiB + guard 页）+ TEB/PEB + 命令行块 + 环境块。
// ★ fs 段基址指向 `teb`（由 CPU 的 `fsBase` 表示；⛔ 不是宿主的 fs）。
// ★ 2026-11-05 两个口径（都由 `vfs` 提供真源，⛔ 不在本函数里另写字符串）：
//   ① guest 的主模块路径 = `vfs.guestExePath()`（guest 根之下的 `AGE.EXE`）——引擎从 exe 路径
//      推导"安装目录"时必须得到游戏目录本身；宿主 `--exe` 字符串**不进 guest**。
//   ② 环境块 = `vfs.guestEnvironment()`（与 GetEnvironmentVariable* / 特殊目录同一份）。
bool setupProcessStructures(GuestMemory& mem, const PeImage& img, const GuestLayout& layout,
                            const Vfs& vfs, ProcessSetup* out, std::string* err);

}  // namespace lift
