// emulator-lift · 语料助记符覆盖（判据 A2；assets 档，因为 `.lst` 不在仓库里）
//
// ★ 判据（plan §5 A2）：语料里出现的**每个助记符**都能映射到规范名，或被显式判为
//   "数据 / 伪指令"：**未映射 = 0**。
// ★ 它判的是"我们读懂了语料"（映射），⛔ 不是"我们实现了"（那是 A1 / A5）。
//   `emms` / `movq` 这类 MMX 助记符在表里、但执行是 `Op::Other`（M1b）—— 两者必须分开看。
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

#include "check.h"
#include "corpus/lst_index.h"
#include "runtime/decode.h"

int main() {
    using namespace lift_test;

    const char* lst_path = std::getenv("LIFT_AGE_LST");
    if (!lst_path || !*lst_path) LIFT_SKIP("没有 LIFT_AGE_LST");

    g_current_case = "A2-coverage";
    lift::LstIndex lst;
    std::string err;
    if (!lst.load(lst_path, &err)) {
        std::fprintf(stderr, "loadLst: %s\n", err.c_str());
        return 1;
    }
    std::printf("A2: 指令行 = %zu，collapsed = %zu，段 = %zu\n", lst.entries().size(), lst.collapsedCount(),
                lst.segments().size());

    // ① 未分类的行（既不是指令、也判不成数据/伪指令）—— A2 的红点
    const auto& unknown = lst.unknownLines();
    std::printf("A2: 未分类行 = %zu\n", unknown.size());
    for (std::size_t i = 0; i < unknown.size() && i < 30; ++i) {
        std::printf("  UNKNOWN %08X  %s\n", unknown[i].ea, unknown[i].text.c_str());
    }
    LIFT_CHECK(unknown.empty());

    // ② 每个指令行的第一个 token 必须映射得到（`rep` / `lock` 前缀看第二个 token）
    std::size_t unmapped = 0;
    std::set<std::string> raw_mnemonics;
    std::set<std::string> canon_set;
    for (const lift::LstEntry& e : lst.entries()) {
        const char* c = lift::canonicalFromIda(e.mnemonic);
        if (!c && lift::isLstInstructionPrefix(e.mnemonic)) {
            // 前缀形态：第二个 token 才是助记符
            std::string_view t = e.text;
            const std::size_t sp = t.find(' ');
            if (sp != std::string_view::npos) {
                std::string_view rest = t.substr(sp + 1);
                while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
                const std::size_t e2 = rest.find(' ');
                c = lift::canonicalFromIda(rest.substr(0, e2));
            }
        }
        if (!c) {
            ++unmapped;
            if (unmapped <= 20) std::printf("  UNMAPPED %08X  %s\n", e.ea, e.text.c_str());
            continue;
        }
        raw_mnemonics.insert(e.mnemonic);
        canon_set.insert(c);
    }
    std::printf("A2: 助记符形态（IDA 原样）= %zu，规范名 = %zu，未映射 = %zu\n", raw_mnemonics.size(),
                canon_set.size(), unmapped);
    LIFT_CHECK_EQ_I64(unmapped, 0);

    // ③ 量级哨兵：跑空了不算绿（plan §3 F5 的 143 个形态）
    LIFT_CHECK(raw_mnemonics.size() >= 100);

    return finish("corpus-mnemonics");
}
