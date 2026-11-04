// emulator-lift · 解码对照（A3；assets 档：要 corpus 里的 `.lst` + 拆壳件）
//
// ★ 判据（plan §5 A3）：
//   ⓪ PE 的 sha256 == `.lst` 头部 `; Input SHA256 :`；
//   ① 对 `.lst` 里每一条**非 collapsed 指令行**，在同一 EA 解码真实字节 ⇒ 长度 == 下一行 EA 差；
//   ② 助记符 == 规范名。
//   不一致 = 0。不一致时打印 EA + 两侧文本（前 20 条）。
// ★ 这一条是整份计划里最强的判据：它把我们的解码器钉在 30 万条**独立产出**的指令边界上。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "check.h"
#include "corpus/lst_index.h"
#include "runtime/decode.h"
#include "runtime/machine.h"
#include "runtime/sha256.h"

namespace {

// `.lst` 一侧的规范名：`rep movsd` / `lock xadd` 要拼起来
std::string expectedCanonical(const lift::LstEntry& e) {
    std::string_view text(e.text);
    std::string_view t0;
    std::string_view t1;
    // 切头两个 token
    std::size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    std::size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\t') ++j;
    t0 = text.substr(i, j - i);
    i = j;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\t') ++j;
    t1 = text.substr(i, j - i);

    if (lift::isLstInstructionPrefix(t0)) {
        const char* p = lift::canonicalFromIda(t0);
        const char* m = lift::canonicalFromIda(t1);
        std::string out = p ? p : std::string(t0);
        out += " ";
        out += m ? m : std::string(t1);
        return out;
    }
    const char* m = lift::canonicalFromIda(t0);
    return m ? m : std::string(t0);
}

}  // namespace

int main() {
    using namespace lift_test;

    const char* exe = std::getenv("LIFT_AGE_EXE");
    const char* lst_path = std::getenv("LIFT_AGE_LST");
    if (!exe || !*exe) LIFT_SKIP("没有 LIFT_AGE_EXE");
    if (!lst_path || !*lst_path) LIFT_SKIP("没有 LIFT_AGE_LST");

    lift::Machine m;
    std::string err;
    if (!m.init(&err) || !m.loadImage(exe, &err) || !m.loadLst(lst_path, &err)) {
        std::fprintf(stderr, "setup: %s\n", err.c_str());
        return 1;
    }

    // ⓪ sha256 对齐
    {
        g_current_case = "sha256-alignment";
        std::string lst_sha;
        if (!lift::readLstHeaderSha256(lst_path, &lst_sha, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        const std::string pe_sha = lift::toLower(m.image().sha256);
        if (pe_sha != lst_sha) {
            std::fprintf(stderr,
                         "A3 ⓪ 红：执行的字节与语料不是同一份\n  PE   = %s (%s)\n  .lst = %s (%s)\n",
                         pe_sha.c_str(), exe, lst_sha.c_str(), lst_path);
            return 1;
        }
        std::printf("A3 ⓪ ok: sha256 = %s\n", pe_sha.c_str());
    }

    g_current_case = "decode-vs-lst";
    std::size_t checked = 0;
    std::size_t skipped_not_in_image = 0;
    std::size_t length_mismatch = 0;
    std::size_t mnemonic_mismatch = 0;
    std::size_t decode_failed = 0;
    std::size_t len_checked = 0;  // 真正比过"长度 == 下一行 EA 差"的行
    const std::size_t kMaxPrint = 25;
    std::size_t printed = 0;

    for (const lift::LstEntry& e : m.lst().entries()) {
        if (e.ea < m.image().image_base) {
            ++skipped_not_in_image;
            continue;
        }
        const std::uint32_t rva = e.ea - m.image().image_base;
        const lift::PeSectionInfo* sec = m.image().sectionOfRva(rva);
        if (!sec) {
            ++skipped_not_in_image;
            continue;
        }
        if (!m.mem().committed(e.ea, 1)) {
            ++skipped_not_in_image;
            continue;
        }
        ++checked;
        lift::Insn in;
        std::string why;
        if (!m.decoder().decode(m.mem(), e.ea, &in, &why)) {
            ++decode_failed;
            if (printed < kMaxPrint) {
                std::printf("  DECODE-FAIL %08X  lst=%-40s  %s\n", e.ea, e.text.c_str(), why.c_str());
                ++printed;
            }
            continue;
        }
        // ① 长度 == 下一行 EA 差
        if (e.next_ea != 0 && e.next_ea > e.ea) {
            ++len_checked;
            const std::uint32_t expect = e.next_ea - e.ea;
            if (expect != in.length) {
                ++length_mismatch;
                if (printed < kMaxPrint) {
                    std::printf("  LEN  %08X  lst=%-40s  ours=%-20s len=%u expect=%u\n", e.ea, e.text.c_str(),
                                in.mnemonic, static_cast<unsigned>(in.length), expect);
                    ++printed;
                }
                continue;
            }
        }
        // ② 助记符 == 规范名
        const std::string expect_mn = expectedCanonical(e);
        if (expect_mn != in.mnemonic) {
            ++mnemonic_mismatch;
            if (printed < kMaxPrint) {
                std::printf("  MNEM %08X  lst=%-30s (canon=%s)  ours=%s\n", e.ea, e.text.c_str(),
                            expect_mn.c_str(), in.mnemonic);
                ++printed;
            }
        }
    }

    std::printf("A3: entries=%zu checked=%zu len_checked=%zu skipped(不在镜像里)=%zu decode_fail=%zu "
                "len_mismatch=%zu mnem_mismatch=%zu\n",
                m.lst().entries().size(), checked, len_checked, skipped_not_in_image, decode_failed,
                length_mismatch, mnemonic_mismatch);
    std::printf("A3: collapsed 行 = %zu（不参与对照，见 plan §3 F2）\n", m.lst().collapsedCount());
    // ★ plan §3 F5 的 299,249 是 2026-11-03 那次导出的经验值；当前 `.lst` 的严格分类是 298,299
    //   （差 950 未解释）。本测试只认**当前 `.lst`** 的分类结果，不拿旧数字当断言。
    std::printf("A3: plan §3 F5 的 299,249 与当前 entries 差 %lld（旧导出经验值；不参与断言）\n",
                static_cast<long long>(299249) - static_cast<long long>(m.lst().entries().size()));

    LIFT_CHECK_EQ_I64(checked > 200000 ? 1 : 0, 1);  // 语料量级哨兵：跑空了的"全绿"没有意义
    LIFT_CHECK_EQ_I64(len_checked, checked);         // 每条 entry 都真的比过长度（不许静默跳过）
    LIFT_CHECK_EQ_I64(decode_failed, 0);
    LIFT_CHECK_EQ_I64(length_mismatch, 0);
    LIFT_CHECK_EQ_I64(mnemonic_mismatch, 0);
    return finish("decode-crosscheck");
}
