// emulator-lift · 基座缺口（A5；assets 档）
//
// ★ 判据（plan §5 A5）：trace 里出现过的导入 ⊆ (实现 ∪ `stub` 带 reason)；
//   七族里的导入**不许**是 stub，除非写进 `SCOPE.md` 的偏差表。
// ★ 这里判的是"**执行到的**导入"（动态足迹），不是"导入表里有没有"（静态清单）——
//   静态缺口由 `--report` 打印，两者不能混。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "check.h"
#include "corpus/lst_index.h"
#include "runtime/machine.h"

namespace {

// 与 `ShimRegistry::normalizeModule` 同口径（小写 + 去尾部 `.dll`）：shim 注册名与 PE 导入名
// 在大小写 / 扩展名上可以不同，诊断回查不能因此漏掉。
std::string normalizedModule(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".dll") == 0) s.resize(s.size() - 4);
    return s;
}

bool sameAsciiNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return true;
}

}  // namespace

int main() {
    using namespace lift_test;

    const char* exe = std::getenv("LIFT_AGE_EXE");
    const char* lst_path = std::getenv("LIFT_AGE_LST");
    if (!exe || !*exe) LIFT_SKIP("没有 LIFT_AGE_EXE");
    if (!lst_path || !*lst_path) LIFT_SKIP("没有 LIFT_AGE_LST");

    g_current_case = "base-coverage";
    lift::Machine m;
    std::string err;
    if (!m.init(&err) || !m.loadImage(exe, &err) || !m.loadLst(lst_path, &err) || !m.prepare(0, &err)) {
        std::fprintf(stderr, "setup: %s\n", err.c_str());
        return 1;
    }

    lift::RunOptions o;
    o.until_ea = 0x004BA890u;
    o.max_insns = 200000000ull;
    const lift::RunResult r = m.run(o);
    m.dumpRunSummary(r, stdout);

    // ★ 先钉"真的跑到了 L2"：只判"动态缺口为空"在**一条导入都没执行**时恒真
    //   （跑空 ⇒ called.empty() ⇒ 假绿）。同 corpus-mnemonics / decode-crosscheck 的量级哨兵。
    //   指令数依赖 `--exe` 的路径写法（38,456 / 44,184 / 53,134 / 60,652），这里只取最保守的量级下界。
    LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
    LIFT_CHECK_EQ_U32(r.ea, 0x004BA890u);
    LIFT_CHECK(r.insns >= 30000ull);

    // ★ 导入描述符身份（REQ-01M6KK2C6TKBA88M6EXG1H7E5B）：A5 的"缺哪条导入"要能回查"坏在哪张表"。
    //   这里打印的是 PE 里的**原始 RVA**（⛔ 不是换算后的文件偏移），格式供人读 / 供脚本抓：
    //   `descriptor #i (OFT=0x… FT=0x… module=…)`。
    const std::vector<lift::PeImportInfo>& imports = m.image().imports;
    const auto find_import = [&imports](const lift::ShimDef& d) -> const lift::PeImportInfo* {
        const std::string want = normalizedModule(d.module);
        for (const lift::PeImportInfo& imp : imports) {
            if (normalizedModule(imp.module) != want) continue;
            if (imp.by_ordinal) {
                if (sameAsciiNoCase(d.name, "#" + std::to_string(imp.ordinal))) return &imp;
            } else if (sameAsciiNoCase(d.name, imp.name)) {
                return &imp;
            }
        }
        return nullptr;
    };
    const auto print_descriptor = [](const lift::PeImportInfo& imp) {
        std::printf("    descriptor #%u (OFT=0x%08X FT=0x%08X module=%s)\n",
                    static_cast<unsigned>(imp.descriptor_index), static_cast<unsigned>(imp.original_first_thunk),
                    static_cast<unsigned>(imp.first_thunk), imp.module.c_str());
    };
    std::size_t descriptor_count = 0;
    std::uint32_t shown_descriptor = 0xFFFFFFFFu;
    for (const lift::PeImportInfo& imp : imports) {
        if (imp.descriptor_index == shown_descriptor) continue;
        shown_descriptor = imp.descriptor_index;
        ++descriptor_count;
        std::printf("descriptor #%u (OFT=0x%08X FT=0x%08X module=%s)\n",
                    static_cast<unsigned>(imp.descriptor_index), static_cast<unsigned>(imp.original_first_thunk),
                    static_cast<unsigned>(imp.first_thunk), imp.module.c_str());
    }
    std::printf("导入描述符 = %zu 张表 / %zu 条导入\n", descriptor_count, imports.size());

    // 静态缺口（导入表里有、但没实现）
    const std::vector<const lift::ShimDef*> gaps = m.shims().importedButUnimplemented();
    std::printf("静态缺口（导入表里有但未实现）= %zu / 导入 %zu\n", gaps.size(), m.shims().importedCount());
    for (const lift::ShimDef* d : gaps) {
        std::printf("  %s!%s  —— %s\n", d->module.c_str(), d->name.c_str(), d->reason ? d->reason : "(无理由)");
        const lift::PeImportInfo* imp = find_import(*d);
        LIFT_CHECK(imp != nullptr);  // 缺口诊断必须能回查到具体的 PE 描述符
        if (imp) {
            print_descriptor(*imp);
        } else {
            std::printf("    descriptor #?（静态导入表里回查不到 %s!%s）\n", d->module.c_str(), d->name.c_str());
        }
    }

    // 动态缺口：执行到的未实现导入
    const std::vector<const lift::ShimDef*> called = m.shims().calledUnimplemented();
    std::printf("动态缺口（执行到了）= %zu\n", called.size());
    for (const lift::ShimDef* d : called) {
        std::printf("  CALLED %s!%s  —— %s\n", d->module.c_str(), d->name.c_str(),
                    d->reason ? d->reason : "(无理由)");
        const lift::PeImportInfo* imp = find_import(*d);
        LIFT_CHECK(imp != nullptr);  // 缺口诊断必须能回查到具体的 PE 描述符
        if (imp) {
            print_descriptor(*imp);
        } else {
            std::printf("    descriptor #?（静态导入表里回查不到 %s!%s）\n", d->module.c_str(), d->name.c_str());
        }
    }
    LIFT_CHECK(called.empty());

    // 统计：实现了多少个、被调用了多少个（不是判据，但报告里要能看见进度）
    std::size_t implemented = 0;
    std::size_t called_ok = 0;
    for (const lift::ShimDef& d : m.shims().all()) {
        if (d.fn) ++implemented;
        if (d.fn && d.called) ++called_ok;
    }
    std::printf("shims: 已实现 %zu，执行期被调用 %zu\n", implemented, called_ok);
    LIFT_CHECK(implemented > 0);

    return finish("base-coverage");
}
