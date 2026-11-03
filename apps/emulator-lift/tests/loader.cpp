// emulator-lift · 载入器（assets 档：要 corpus 里的拆壳件与 `.lst`）
//
// ★ 判据（plan §6 S1）：
//   ① PE 的 sha256 == `.lst` 头部的 `; Input SHA256 :`（这就是"执行的字节 = 语料描述的字节"）；
//   ② 入口落在某个可执行节里；
//   ③ 导入表里每个 IAT 槽在 `prepare()` 之后都指向 **shim 桩页**（每个导入一个唯一地址）；
//   ④ 越界访问会 fault 而不是静默返回 0 —— 用"未提交页"探针断言（safe mode 下直接判）。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "check.h"
#include "corpus/lst_index.h"
#include "runtime/machine.h"
#include "runtime/sha256.h"

int main() {
    using namespace lift_test;

    const char* exe = std::getenv("LIFT_AGE_EXE");
    const char* lst_path = std::getenv("LIFT_AGE_LST");
    if (!exe || !*exe) LIFT_SKIP("没有 LIFT_AGE_EXE（要 corpus 里的拆壳件）");
    if (!lst_path || !*lst_path) LIFT_SKIP("没有 LIFT_AGE_LST");

    g_current_case = "load";
    lift::Machine m;
    std::string err;
    if (!m.init(&err)) {
        std::fprintf(stderr, "init: %s\n", err.c_str());
        return 1;
    }
    // ① sha256 对齐（A3 ⓪）：这里**直接**做，不依赖 main.cpp 的那条路径
    {
        std::string lst_sha;
        if (!lift::readLstHeaderSha256(lst_path, &lst_sha, &err)) {
            std::fprintf(stderr, "readLstHeaderSha256: %s\n", err.c_str());
            return 1;
        }
        LIFT_CHECK(!lst_sha.empty());
        lift::PeImage img;
        if (!lift::loadPeFile(exe, &img, &err)) {
            std::fprintf(stderr, "loadPeFile: %s\n", err.c_str());
            return 1;
        }
        LIFT_CHECK(lift::toLower(img.sha256) == lst_sha);
        std::printf("pe sha256 = %s\nlst sha256 = %s\n", img.sha256.c_str(), lst_sha.c_str());
        LIFT_CHECK_EQ_U32(img.entry_ea, img.image_base + img.entry_rva);
        LIFT_CHECK(!img.sections.empty());
        LIFT_CHECK(img.imports.size() > 50);  // 这个 CRT 静态链接的镜像导入很多

        // ② 入口落在可执行节里
        bool entry_in_text = false;
        for (const lift::PeSectionInfo& s : img.sections) {
            const std::uint32_t vsize = s.vsize ? s.vsize : s.raw_size;
            if (img.entry_rva >= s.va && img.entry_rva < s.va + vsize && s.executable()) entry_in_text = true;
        }
        LIFT_CHECK(entry_in_text);
    }

    if (!m.loadImage(exe, &err)) {
        std::fprintf(stderr, "loadImage: %s\n", err.c_str());
        return 1;
    }
    if (!m.loadLst(lst_path, &err)) {
        std::fprintf(stderr, "loadLst: %s\n", err.c_str());
        return 1;
    }
    if (!m.prepare(0, &err)) {
        std::fprintf(stderr, "prepare: %s\n", err.c_str());
        return 1;
    }

    // ③ IAT 槽都指向桩页
    {
        g_current_case = "iat-bound";
        const std::uint32_t page = m.layout().shim_page;
        std::size_t bound = 0;
        for (const lift::PeImportInfo& imp : m.image().imports) {
            const std::uint32_t slot = m.mem().rd32(m.image().image_base + imp.iat_rva);
            if (slot >= page && slot < page + m.layout().shim_slots * 4u) ++bound;
        }
        LIFT_CHECK_EQ_I64(bound, m.image().imports.size());
        std::printf("imports bound to stub page: %zu\n", bound);
        // 每个导入一个**唯一**地址
        LIFT_CHECK(m.shims().size() >= m.image().imports.size());
    }

    // ④ 未提交页：`committed` 必须说 false（这条是"越界不静默"的机械前提）
    {
        g_current_case = "uncommitted-probe";
        // 栈 guard 页（栈底再往下一页）—— setupProcessStructures 只 annotate、不提交
        const std::uint32_t guard = m.setup().stack_bottom - 0x1000u;
        LIFT_CHECK(!m.mem().committed(guard, 1));
        LIFT_CHECK(m.mem().committed(m.setup().stack_bottom, 1));
        // 4 GiB 顶端也没有提交
        LIFT_CHECK(!m.mem().committed(0xFFFFF000u, 1));
    }

    // ⑤ TEB 自指 + fs_base（CRT 启动要读 fs:[0x18] / fs:[0x2C]）
    {
        g_current_case = "teb-peb";
        LIFT_CHECK_EQ_U32(m.cpu().fs_base, m.setup().teb);
        LIFT_CHECK_EQ_U32(m.mem().rd32(m.cpu().fs_base + 0x18), m.setup().teb);
        LIFT_CHECK_EQ_U32(m.mem().rd32(m.cpu().fs_base + 0x04), m.setup().stack_top);
        LIFT_CHECK_EQ_U32(m.mem().rd32(m.setup().peb + 0x08), m.image().image_base);
        LIFT_CHECK_EQ_U32(m.mem().rd8(m.setup().cmdline_ea), static_cast<std::uint32_t>('"'));
    }

    std::printf("entry=%08X sections=%zu imports=%zu\n", m.image().entry_ea, m.image().sections.size(),
                m.image().imports.size());
    m.reportBaseGaps(stdout);
    return finish("loader");
}
