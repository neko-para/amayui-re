// emulator-lift · 基座缺口（A5；assets 档）
//
// ★ 判据（plan §5 A5）：trace 里出现过的导入 ⊆ (实现 ∪ `stub` 带 reason)；
//   七族里的导入**不许**是 stub，除非写进 `SCOPE.md` 的偏差表。
// ★ 这里判的是"**执行到的**导入"（动态足迹），不是"导入表里有没有"（静态清单）——
//   静态缺口由 `--report` 打印，两者不能混。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "check.h"
#include "corpus/lst_index.h"
#include "runtime/machine.h"

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

    // 静态缺口（导入表里有、但没实现）
    const std::vector<const lift::ShimDef*> gaps = m.shims().importedButUnimplemented();
    std::printf("静态缺口（导入表里有但未实现）= %zu / 导入 %zu\n", gaps.size(), m.shims().importedCount());
    for (const lift::ShimDef* d : gaps) {
        std::printf("  %s!%s  —— %s\n", d->module.c_str(), d->name.c_str(), d->reason ? d->reason : "(无理由)");
    }

    // 动态缺口：执行到的未实现导入
    const std::vector<const lift::ShimDef*> called = m.shims().calledUnimplemented();
    std::printf("动态缺口（执行到了）= %zu\n", called.size());
    for (const lift::ShimDef* d : called) {
        std::printf("  CALLED %s!%s  —— %s\n", d->module.c_str(), d->name.c_str(),
                    d->reason ? d->reason : "(无理由)");
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
