// emulator-lift · Shape A / T1b：`emulator-lift-closure`（构建期 host tool）
//
// 职责：**census（实际执行过的块 EA 集合） + PE 字节 → `closure.txt`（分量根，入库件）**。
//
// 为什么单独一个 target、不并进 `lift-gen` 的 `--census ... --emit-roots ...` 子模式：
//   ① 生命周期不同：`closure.txt` 是**入库件**，只在"重新跑一次 census"时变；`lift-gen` 是
//      **每次构建**都跑（roots → `.ll`）。把两者塞进一个二进制，会让"构建期工具"同时承担
//      "改写入库件"的能力 —— 那正是最容易被误触的写路径。
//   ② 输入不同：本工具吃 **census**（动态观测），`lift-gen` 吃 **roots**（静态声明）。
//      合在一起时两个 `--out` 语义会互相纠缠（谁覆盖谁）。
//   ③ 本工具**不发射 IR**，所以它不需要 clang、也不需要 `.ll` 约定；`lift-gen` 反过来不需要 census。
//   ★ 两者共用同一份 `.text` 判据与 hex 文法（`lift_aot_text.h`），⛔ 不写第二份。
//
// ★ 本工具**不改解释器、不改 PE**、只读 census + `--exe`。
// ★ **确定性**：输入相同 ⇒ 输出**逐字节**相同。做法：所有容器按 EA 升序迭代、输出格式固定、
//   ⛔ 文件里不写任何路径 / 时间戳 / 计数（那些都会随机器与运行变）。closure.txt 要能 `git diff`。
//
// CLI：
//   --exe <PE>        字节真源（`--check` / `lift-gen` 用的同一份）
//   --census <file>   census：一行一个 hex **块 EA**（`#` 起注释；严格 `0x` + 8 位）
//                     ★ **可重复给多次**：多份 census 取**并集**（D9：含 LOGO ∪ `--skip-logo`）。
//                     并集进 `std::set` ⇒ 与文件给的顺序、文件内部顺序**无关**（确定性）。
//   --out <file>      `closure.txt` 落点（入库件：`apps/emulator-lift/aot/closure.txt`）
//   --verbose         逐类读数（census 越界块 / 根的分类计数 / 覆盖块数 / 每份输入的增量）
//   --help
//
// 退出码：0 成功 · 2 参数或输入错 · 4 解码失败 · 1 内部不一致（不收敛）
//
// =====================================================================================
// 推导规则（确定性、可复现；与父级 D5 口径逐条对应）：
//   ① 必有 PE `entry`（`PeImage::entry_ea`）⇒ `# why: pe-entry`。
//   ② **census 块里的 `call rel32` 目标** 各是一个根 ⇒ `# why: call-target`。
//      只读 census 块自身解出来的指令（⛔ 不把"静态可达但 census 没跑到的块里的 call"也算进来：
//      那会把闭包扩到整张调用图，与本轮"先给出一份真实可达闭包"的口径相反）。
//   ③ `jmp rel32` 目标 / `jcc` 落点：凡**不能被任一已定根静态解码覆盖**的，补成根
//      ⇒ `# why: indirect-entry`。实现上它是 ④ 的子集（静态展开本来就走这些边），
//      但判据仍单独算一次，免得将来改了展开规则就静默丢掉这一类。
//   ④ census 里出现、但静态展开到不了的块（间接跳 / 跳表 / `push`+`ret` 跳板 / `call reg`
//      的落点 / 256 条分片的后半段）⇒ 补成根 ⇒ `# why: indirect-entry`。
//      ★ 这是"用动态观测补静态枚举的洞"的那一步：不做的话 boot 一定在第一次间接跳转处断。
//      ★ **256 条分片**不算洞：它是解释器的块缓存把一条长直线切成两段，静态展开**已经**覆盖了
//      那段直线（判据 = 该 EA 是静态展开解出来的某条指令的 EA）⇒ 不补根（补了反而会让
//      `lift-gen` 的分区在同一块上冲突）。
//   ⑤ 重复 ①–④ 到不动点（补进来的根会覆盖更多 census 块）。
// ⛔ 不写派生值（字节范围 / 指令数 / 函数名 / 调用点 EA 都能从 PE 重算）。
// ⛔ 不"为了计数好看"裁掉任何 census 块：落在**别的 guest 模块**（如 AGERC.DLL）的块
//    **不是**被裁掉，而是**本 PE 里根本不存在这些字节** ⇒ 明确计数并报告，不静默。
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "aot/lift_aot_text.h"
#include "aot/lift_gen_emit.h"  // `genBranchTarget`：`jmp/jcc/call rel32` 目标只有这一份判据
#include "runtime/decode.h"
#include "runtime/exec.h"
#include "runtime/host_encoding.h"
#include "runtime/image.h"
#include "runtime/memory.h"

namespace {

struct Options {
    std::string exe;
    std::vector<std::string> census_files;  // ★ 可多份（D9：并集）；顺序不影响输出
    std::string out;
    bool verbose = false;
};

void usage() {
    std::printf(
        "emulator-lift-closure —— Shape A / T1b：census + PE → `closure.txt`（分量根；入库件）\n"
        "  --exe <PE>        字节真源\n"
        "  --census <file>   census：一行一个 hex 块 EA（`#` 起注释；严格 0x + 8 位）\n"
        "                    ★ 可重复给多次：多份 census 取并集（D9：含 LOGO ∪ skip-logo）\n"
        "  --out <file>      closure.txt 落点（aot/closure.txt 是入库件）\n"
        "  --verbose         逐类读数\n"
        "  --help\n"
        "退出码：0 成功 · 2 参数/输入错 · 4 解码失败 · 1 内部不一致\n");
}

enum class RootWhy { PeEntry = 0, CallTarget = 1, IndirectEntry = 2 };

const char* whyName(RootWhy why) {
    switch (why) {
        case RootWhy::PeEntry: return "pe-entry";
        case RootWhy::CallTarget: return "call-target";
        case RootWhy::IndirectEntry: return "indirect-entry";
    }
    return "?";
}

// =====================================================================================
// ① 静态展开：从根出发"按 lift-gen 完全相同的规则"收集**已覆盖的指令 EA**
// ★ 为什么在这里重写一遍而不是复用 `lift_gen.cpp` 里的 `Explorer`：那份是匿名 namespace 的
//   TU 内部类，跨 TU 复用要把它提成库（本轮只许**插入** target，不做结构重构）。
//   ⇒ 代价是"展开规则有两份"；判据落在 T1b 报告里：closure.txt 的每个根都过
//   `lift-gen --check`，而 `--check` 的覆盖块数用的就是 `lift_gen.cpp` 的那一份
//   ⇒ 两边一旦漂，`--check` 的 blocks / 越界 / 不可解码计数就对不上（下一轮能立刻看见）。
// =====================================================================================
class Expand {
public:
    Expand(const lift::Decoder& decoder, const lift::GuestMemory& mem)
        : decoder_(decoder), mem_(mem) {}

    bool run(const std::set<std::uint32_t>& roots, std::string* err, std::uint32_t* err_ea) {
        leaders_ = roots;
        std::set<std::uint32_t> pending(roots.begin(), roots.end());
        std::set<std::uint32_t> scanned;
        while (!pending.empty()) {
            const std::uint32_t leader = *pending.begin();
            pending.erase(pending.begin());
            if (!scanned.insert(leader).second) continue;
            std::uint32_t cur = leader;
            bool reached_end = false;
            for (int guard = 0; guard < 1000000; ++guard) {
                const lift::Insn* insn = get(cur, err, err_ea);
                if (insn == nullptr) return false;
                const lift::Insn in = *insn;
                if (lift::opIsControl(in.op)) {
                    switch (in.op) {
                        case lift::Op::Call: {
                            const std::uint32_t ret = in.ea + in.length;
                            leaders_.insert(ret);
                            pending.insert(ret);
                            break;
                        }
                        case lift::Op::Jmp:
                            if (in.op0().kind == lift::OpKind::Imm) {
                                const std::uint32_t target = lift::aot::genBranchTarget(in);
                                leaders_.insert(target);
                                pending.insert(target);
                            }
                            break;  // `jmp reg/mem`：目标运行期才知道
                        case lift::Op::Jcc: {
                            const std::uint32_t target = lift::aot::genBranchTarget(in);
                            const std::uint32_t fall = in.ea + in.length;
                            leaders_.insert(target);
                            pending.insert(target);
                            leaders_.insert(fall);
                            pending.insert(fall);
                            break;
                        }
                        default:
                            break;  // ret / hlt / int3 / int：没有后继
                    }
                    reached_end = true;
                    break;
                }
                cur = in.ea + in.length;
                if (insns_.find(cur) != insns_.end()) {
                    reached_end = true;
                    break;
                }
            }
            if (!reached_end) {
                if (err != nullptr) *err = "直线扫描超过 1e6 条上限（可能落在数据里）";
                if (err_ea != nullptr) *err_ea = leader;
                return false;
            }
        }
        return true;
    }

    const std::map<std::uint32_t, lift::Insn>& insns() const { return insns_; }
    const std::set<std::uint32_t>& leaders() const { return leaders_; }

private:
    const lift::Insn* get(std::uint32_t ea, std::string* err, std::uint32_t* err_ea) {
        const auto it = insns_.find(ea);
        if (it != insns_.end()) return &it->second;
        lift::Insn in;
        std::string why;
        if (!decoder_.decode(mem_, ea, &in, &why)) {
            if (err != nullptr) *err = why;
            if (err_ea != nullptr) *err_ea = ea;
            return nullptr;
        }
        return &insns_.emplace(ea, in).first->second;
    }

    const lift::Decoder& decoder_;
    const lift::GuestMemory& mem_;
    std::map<std::uint32_t, lift::Insn> insns_;
    std::set<std::uint32_t> leaders_;
};

// census 文件：一行一个块 EA（严格 `0x` + 8 位十六进制）。
bool readCensus(const std::string& path, std::set<std::uint32_t>* eas, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *err = "打不开 `" + path + "`";
        return false;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const std::string token = lift::aot::stripCommentAndTrim(line);
        if (token.empty()) continue;
        std::uint32_t ea = 0;
        if (!lift::aot::strictParseEa(token, &ea)) {
            *err = "第 " + std::to_string(lineno) + " 行不是 `0x` + 8 位十六进制：`" + token + "`";
            return false;
        }
        eas->insert(ea);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    lift::suppressCrashDialogsOnce();
    lift::initConsoleUtf8();
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);

    Options opt;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto needValue = [&](const char* flag, std::string* dst) -> bool {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "emulator-lift-closure: %s 需要一个值\n", flag);
                return false;
            }
            *dst = args[++i];
            return true;
        };
        if (a == "--exe") {
            if (!needValue("--exe", &opt.exe)) return 2;
        } else if (a == "--census") {
            std::string file;
            if (!needValue("--census", &file)) return 2;
            opt.census_files.push_back(file);
        } else if (a == "--out") {
            if (!needValue("--out", &opt.out)) return 2;
        } else if (a == "--verbose") {
            opt.verbose = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "emulator-lift-closure: 未知参数：%s\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (opt.exe.empty() || opt.census_files.empty() || opt.out.empty()) {
        std::fprintf(stderr, "emulator-lift-closure: --exe / --census / --out 都是必需的\n");
        usage();
        return 2;
    }

    // ---- 输入 ①：census（**多份取并集**；D9）----
    // ★ 并集落在 `std::set` ⇒ 输出与"文件给的顺序 / 文件内部顺序"无关 ⇒ 确定性。
    std::set<std::uint32_t> census;
    std::vector<std::pair<std::string, std::size_t>> census_notes;  // (文件名, 该份的 EA 数)
    std::vector<std::pair<std::string, std::size_t>> census_added;  // (文件名, 该份新增的 EA 数)
    for (const std::string& path : opt.census_files) {
        std::set<std::uint32_t> one;
        std::string err;
        if (!readCensus(path, &one, &err)) {
            std::fprintf(stderr, "emulator-lift-closure: --census 读不了：%s\n", err.c_str());
            return 2;
        }
        if (one.empty()) {
            std::fprintf(stderr, "emulator-lift-closure: --census 里一个 EA 都没有：%s\n", path.c_str());
            return 2;
        }
        std::size_t added = 0;
        for (const std::uint32_t ea : one) {
            if (census.insert(ea).second) ++added;
        }
        census_notes.emplace_back(path, one.size());
        census_added.emplace_back(path, added);
    }

    // ---- 输入 ②：PE 字节（与 lift-gen 同一份解码器）----
    lift::GuestMemory mem;
    if (!mem.reserve()) {
        std::fprintf(stderr, "emulator-lift-closure: 预留 guest 地址空间失败：%s\n", mem.lastError());
        return 2;
    }
    lift::PeImage image;
    {
        std::string err;
        if (!lift::loadPeFile(opt.exe, &image, &err)) {
            std::fprintf(stderr, "emulator-lift-closure: 载入 PE 失败（%s）：%s\n", opt.exe.c_str(), err.c_str());
            return 2;
        }
        if (!lift::mapPeImage(mem, image, &err)) {
            std::fprintf(stderr, "emulator-lift-closure: 映射 PE 失败（%s）：%s\n", opt.exe.c_str(), err.c_str());
            return 2;
        }
    }
    lift::Decoder decoder;
    if (!decoder.ok()) {
        std::fprintf(stderr, "emulator-lift-closure: 解码器初始化失败：%s\n", decoder.lastError());
        return 4;
    }

    const std::vector<lift::aot::ExecRange> ranges = lift::aot::execRanges(image);
    if (ranges.empty()) {
        std::fprintf(stderr, "emulator-lift-closure: PE 里没有一个可执行节（%s）\n", opt.exe.c_str());
        return 2;
    }

    // ---- ② 把 census 分成"本 PE 的 .text 内"与"别的 guest 模块"----
    std::set<std::uint32_t> in_text;
    std::size_t outside = 0;
    std::uint32_t outside_min = 0xFFFFFFFFu;
    std::uint32_t outside_max = 0;
    for (const std::uint32_t ea : census) {
        if (lift::aot::execRangeOf(ranges, ea) != nullptr) {
            in_text.insert(ea);
        } else {
            ++outside;
            outside_min = std::min(outside_min, ea);
            outside_max = std::max(outside_max, ea);
        }
    }

    // ---- ③ 根：entry + census 块里的 `call rel32` 目标 ----
    std::map<std::uint32_t, RootWhy> why;
    why[image.entry_ea] = RootWhy::PeEntry;

    std::size_t call_targets = 0;
    std::size_t call_outside = 0;
    std::size_t census_undecodable = 0;
    for (const std::uint32_t ea : in_text) {
        lift::Block block;
        std::string err;
        if (!decoder.decodeBlock(mem, ea, &block, &err)) {
            // census 的块 EA 是**解释器真的装过块**的地址；这里解不出来 ⇒ 解码器与 census 不一致 ⇒ 响亮失败。
            std::fprintf(stderr, "emulator-lift-closure: census 块 %08X 解码失败：%s\n", ea, err.c_str());
            ++census_undecodable;
            continue;
        }
        for (const lift::Insn& in : block.insns) {
            if (in.op != lift::Op::Call) continue;
            if (in.op0().kind != lift::OpKind::Imm) continue;
            const std::uint32_t target = lift::aot::genBranchTarget(in);
            if (lift::aot::execRangeOf(ranges, target) == nullptr) {
                // `call rel32` 的目标不在本 PE 的可执行节里：⛔ 不能当根（lift-gen 解不了那字节），
                // 但**必须报出来**，不静默。
                ++call_outside;
                if (opt.verbose) {
                    std::fprintf(stderr, "  [call-outside] %08X -> %08X（不在本 PE 的可执行节）\n", in.ea,
                                 target);
                }
                continue;
            }
            if (why.find(target) == why.end()) {
                why[target] = RootWhy::CallTarget;
                ++call_targets;
            }
        }
    }
    if (census_undecodable != 0) {
        std::fprintf(stderr, "emulator-lift-closure: census 里有 %zu 个块解不出来 ⇒ 不能派生闭包\n",
                     census_undecodable);
        return 4;
    }

    std::set<std::uint32_t> roots;
    for (const auto& kv : why) roots.insert(kv.first);

    // ---- ④ 不动点：静态展开 ⇒ 没被覆盖的 census 块补成根 ⇒ 重展开 ----
    std::size_t promotions = 0;
    std::size_t covered_blocks = 0;
    std::size_t covered_insns = 0;
    for (;;) {
        Expand expand(decoder, mem);
        std::string err;
        std::uint32_t err_ea = 0;
        if (!expand.run(roots, &err, &err_ea)) {
            std::fprintf(stderr, "emulator-lift-closure: 静态展开解码失败 @ %08X：%s\n", err_ea, err.c_str());
            return 4;
        }
        covered_blocks = expand.leaders().size();
        covered_insns = expand.insns().size();

        std::vector<std::uint32_t> uncovered;
        for (const std::uint32_t ea : in_text) {
            if (expand.insns().find(ea) == expand.insns().end()) uncovered.push_back(ea);
        }
        if (uncovered.empty()) break;
        for (const std::uint32_t ea : uncovered) {
            why[ea] = RootWhy::IndirectEntry;
            roots.insert(ea);
            ++promotions;
        }
        if (promotions > census.size()) {
            std::fprintf(stderr, "emulator-lift-closure: 根数超过 census 块数 ⇒ 推导不收敛\n");
            return 1;
        }
    }

    // ---- ⑤ 落盘（确定性：按 EA 升序 + 固定文法；⛔ 不写路径/计数/时间戳）----
    std::string text;
    text += "# closure.txt —— Shape A 静态 lift 的**分量根**（一行一个 hex 入口 EA，升序）。\n";
    text += "#\n";
    text += "# ★ 本文件是**入库件**（可 review、可 diff、可复现），不是构建产物；生成器 =\n";
    text += "#   `apps/emulator-lift/src/aot/closure_build.cpp`（target `emulator-lift-closure`）：\n";
    text += "#   census（实际执行过的块 EA）+ PE 字节 ⇒ 本文件。\n";
    text += "# ★ 一行一个根 EA；`# why:` = 它为什么是根（pe-entry / call-target / indirect-entry）。\n";
    text += "#   ⛔ 不写派生值（字节范围 / 指令数 / 函数名 / 调用点 EA 都能从 PE 重算）。\n";
    text += "# ★ 校验：`emulator-lift-gen --check aot/closure.txt`（格式 / 升序 / 可解码 / 在 .text / entry 必在）。\n";
    text += "# ★ 闭包靠『跑起来报缺 EA ⇒ 补一行 / 重跑 census』迭代增长（父级 D5）。\n";
    char line[64];
    for (const auto& kv : why) {
        std::snprintf(line, sizeof(line), "0x%08X  # why: %s\n", kv.first, whyName(kv.second));
        text += line;
    }
    {
        const std::filesystem::path out_path(opt.out);
        const std::filesystem::path out_dir =
            out_path.has_parent_path() ? out_path.parent_path() : std::filesystem::path(".");
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        std::ofstream file(out_path, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::fprintf(stderr, "emulator-lift-closure: 写不了 `%s`\n", out_path.string().c_str());
            return 2;
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();
        if (!file) {
            std::fprintf(stderr, "emulator-lift-closure: 写 `%s` 失败\n", out_path.string().c_str());
            return 2;
        }
    }

    // ---- ⑥ 读数 ----
    std::size_t indirect = 0;
    for (const auto& kv : why) {
        if (kv.second == RootWhy::IndirectEntry) ++indirect;
    }
    std::printf(
        "closure: census=%zu in-text=%zu outside=%zu call-target=%zu call-outside=%zu indirect=%zu "
        "roots=%zu covered-blocks=%zu covered-insns=%zu promotions=%zu\n",
        census.size(), in_text.size(), outside, call_targets, call_outside, indirect, why.size(),
        covered_blocks, covered_insns, promotions);
    if (opt.verbose) {
        std::printf("  census-inputs=%zu（并集 %zu 个块 EA；★ 并集 ⇒ 输出与给定顺序无关）\n",
                    census_added.size(), census.size());
        for (std::size_t i = 0; i < census_added.size(); ++i) {
            std::printf("    [%zu] %s：%zu 个 EA（其中新增 %zu）\n", i, census_added[i].first.c_str(),
                        census_notes[i].second, census_added[i].second);
        }
    }
    if (opt.verbose) {
        std::printf("  entry=%08X sections=", image.entry_ea);
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            std::printf("%s%s[%08X,%08X)", i == 0 ? "" : ",", ranges[i].name.c_str(), ranges[i].begin,
                        ranges[i].end);
        }
        std::printf("\n");
        if (outside != 0) {
            std::printf("  outside-range=[%08X,%08X]（别的 guest 模块；本 PE 里没有这些字节 ⇒ 不进闭包）\n",
                        outside_min, outside_max);
        }
    }
    return 0;
}
