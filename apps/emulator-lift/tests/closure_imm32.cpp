// emulator-lift · **D26 第二条扫描（未对齐指令 imm32）的闭包发现守卫**（顶层无条件注册 ⇒ pure 档）
//
// 判据（红了意味着什么）：
//   ① **候选面真的扫到了**语料里三个"只有未对齐 imm32 引用"的入口：
//        · `0x004F80E0`（`0x4F822A` 的 `push $0x4F80E0`；imm32 mod 4 = 3）
//        · `0x00513B90`（`0x412295` 的 `push offset SEH_412290`；imm32 mod 4 = 2）
//        · `0x0041389A`（`0x413562` 的 `mov eax, offset loc_41389A`；imm32 mod 4 = 3）
//      ⇒ 红了意味着扫描退化成"4 字节对齐"（`i += 4`）或 opcode 集被删掉一类。
//   ② **候选面是并集的两半**：`push imm32` 与 `mov reg, imm32` 各自 `in_exec > 0`，
//      且 `sites == push_sites + mov_sites` / `in_exec == push_in_exec + mov_in_exec`。
//   ③ **0x4F80E0 的旧手工根已删**：它不在 `aot/closure.txt` 里。⇒ 派发表里有它只可能来自扫描。
//   ④ **收进了生成物**：`out/aot/lift_table.inc` 有三行 `LIFT_AOT_ENTRY(0x…u, …)`，
//      且 `lift_aot*.ll` 里有对应的 `define i32 @lift_cc_<EA>(`。
//      ⇒ 红了意味着"候选扫到了但没被 `offerSoftRoot` 收下 / 没进派发表"。
//
// ★ 为什么断**存在性 / 包含关系**而不是数字：`sites` / `targets` 的绝对值随 PE 与扫描实现变，
//   写死就会变成"今天有 1234 个"那种假守卫（AGENTS.md §10）。本文件只断"这三个 EA 在/不在"。
// ★ 输入（PE / closure.txt / out/aot 生成物）假定在场；缺了走 `LIFT_SKIP`（77），
//    ⛔ 不许"什么都不做然后 return 0"（那与跑绿了长得一样）。
// ★ 本用例与生成器共用**同一份**扫描实现（`aot/closure_scan.h`）⇒ 它绿才说明生成器收得到；
//   两处各写一套的话，守卫绿只是"另一份扫描绿"。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "aot/closure_scan.h"
#include "aot/lift_aot_text.h"
#include "check.h"
#include "runtime/image.h"

namespace {

namespace fs = std::filesystem;

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// `aot/closure.txt` 里的 EA（严格文法与 `--check` 同一份：`0x` + 8 位十六进制）。
std::set<std::uint32_t> readClosureRoots(const fs::path& p) {
    std::set<std::uint32_t> out;
    std::ifstream in(p, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const std::string token = lift::aot::stripCommentAndTrim(line);
        std::uint32_t ea = 0;
        if (!token.empty() && lift::aot::strictParseEa(token, &ea)) out.insert(ea);
    }
    return out;
}

std::string hex8(std::uint32_t ea) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", ea);
    return buf;
}

// `lift_cc_<root>` 的函数名用**不补零**的大写 hex（与 `lift_gen_emit.cpp` 的 `hexEa` 同一份拼法）。
std::string hexFn(std::uint32_t ea) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%X", ea);
    return buf;
}

// 在 `out/aot` 的 `.ll` 生成物里找 `define i32 @lift_cc_<EA>(`（单份 `lift_aot.ll` 或分片）。
bool emittedDefineFor(const fs::path& dir, std::uint32_t ea) {
    const std::string needle = "define i32 @lift_cc_" + hexFn(ea) + "(";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.compare(0, 8, "lift_aot") != 0) continue;
        if (name.size() < 3 || name.compare(name.size() - 3, 3, ".ll") != 0) continue;
        // 流式找：这些 .ll 单份可达十几 MB，⛔ 不整份读进内存再 find。
        std::ifstream in(entry.path(), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (line.compare(0, needle.size(), needle) == 0) return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

    if (argc < 5) {
        LIFT_SKIP("需要 CTest 传 <AGE.EXE__dumped.sectfix.EXE> <aot/closure.txt> "
                  "<out/aot/lift_table.inc> <out/aot>（见 CMakeLists）");
    }
    const fs::path pe_path = argv[1];
    const fs::path closure_path = argv[2];
    const fs::path table_path = argv[3];
    const fs::path aot_dir = argv[4];

    std::error_code ec;
    if (!fs::exists(pe_path, ec)) LIFT_SKIP("执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）不在场");
    if (!fs::exists(closure_path, ec)) LIFT_SKIP("aot/closure.txt 不在场");
    if (!fs::exists(table_path, ec)) LIFT_SKIP("out/aot/lift_table.inc 不在场（构建期没生成？）");

    lift::PeImage image;
    {
        std::string err;
        if (!lift::loadPeFile(pe_path.string(), &image, &err)) {
            std::fprintf(stderr, "载入 PE 失败：%s\n", err.c_str());
            return 1;
        }
    }
    const std::vector<lift::aot::ExecRange> ranges = lift::aot::execRanges(image);
    const lift::aot::Imm32Scan scan = lift::aot::scanInstructionImmediates(image, ranges);

    // ---- ② 输入面自洽（不是数字断言的替代品，只是"两半都还在"的结构判据）----
    g_current_case = "closure-imm32/shape";
    LIFT_CHECK(scan.sites == scan.push_sites + scan.mov_sites);
    LIFT_CHECK(scan.in_exec == scan.push_in_exec + scan.mov_in_exec);
    LIFT_CHECK(scan.push_in_exec > 0);  // `push imm32` 这一半真的扫到了
    LIFT_CHECK(scan.mov_in_exec > 0);   // `mov reg, imm32` 这一半真的扫到了
    for (const std::uint32_t ea : scan.targets) {
        LIFT_CHECK(lift::aot::execRangeOf(ranges, ea) != nullptr);  // 候选定义 = 落在可执行节
    }

    // ---- ① 三个"只有未对齐 imm32 引用"的入口都在候选集里 ----
    g_current_case = "closure-imm32/candidates";
    const std::uint32_t kLanguageEnumProc = 0x004F80E0u;
    const std::uint32_t kSeh412290 = 0x00513B90u;
    const std::uint32_t kCatchExitContinuation = 0x0041389Au;
    LIFT_CHECK(scan.targets.count(kLanguageEnumProc) != 0);
    LIFT_CHECK(scan.targets.count(kSeh412290) != 0);
    LIFT_CHECK(scan.targets.count(kCatchExitContinuation) != 0);

    // ---- ③ 旧手工根已删：0x4F80E0 不在 closure.txt ----
    g_current_case = "closure-imm32/manual-root-removed";
    const std::set<std::uint32_t> roots = readClosureRoots(closure_path);
    LIFT_CHECK(roots.count(kLanguageEnumProc) == 0);

    // ---- ④ 收进了生成物：派发表 + `.ll` 的 define ----
    g_current_case = "closure-imm32/dispatch-table";
    const std::string table = readAll(table_path);
    LIFT_CHECK(!table.empty());
    for (const std::uint32_t ea : {kLanguageEnumProc, kSeh412290, kCatchExitContinuation}) {
        const std::string row = "LIFT_AOT_ENTRY(0x" + hex8(ea) + "u, ";
        if (!contains(table, row)) {
            failf(__FILE__, __LINE__, "派发表里没有 0x%s：`%s`", hex8(ea).c_str(), row.c_str());
        } else {
            ++g_checks;
        }
    }
    g_current_case = "closure-imm32/emitted-blocks";
    LIFT_CHECK(emittedDefineFor(aot_dir, kLanguageEnumProc));
    LIFT_CHECK(emittedDefineFor(aot_dir, kSeh412290));
    LIFT_CHECK(emittedDefineFor(aot_dir, kCatchExitContinuation));

    return finish("lift-closure-imm32");
}
