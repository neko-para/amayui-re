// emulator-lift · 载入器（assets 档：要 corpus 里的拆壳件与 `.lst`）
//
// ★ 判据（plan §6 S1）：
//   ① PE 的 sha256 == `.lst` 头部的 `; Input SHA256 :`（这就是"执行的字节 = 语料描述的字节"）；
//   ② 入口落在某个可执行节里；
//   ③ 导入表里每个 IAT 槽在 `prepare()` 之后都指向 **shim 桩页**（每个导入一个唯一地址）；
//   ④ 越界访问会 fault 而不是静默返回 0 —— 用"未提交页"探针断言（safe mode 下直接判）。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "corpus/lst_index.h"
#include "runtime/machine.h"
#include "runtime/sha256.h"

namespace {

std::uint32_t readLe32(const std::vector<std::uint8_t>& b, std::size_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

}  // namespace

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

        // ①b 导入描述符的身份（REQ-01M6KK2C6TKBA88M6EXG1H7E5B）：两张表保留的是**原始 RVA**，
        //   诊断要能打印 `descriptor #i (OFT=0x… FT=0x… module=…)`。
        //   ★ 取证（2026-11-05，同一份 `LIFT_AGE_EXE`）：18 个描述符的 **OFT 在文件里全是 0**、
        //   FT 全非 0 —— OFT=0 是合法形态（VC6 链接器的产物；解析器回退到 FT）。所以这份镜像上
        //   ⛔ 不能断言"OFT 非 0"（那会红）；能断言的是：FT 非 0、每对 OFT/FT 至少一个非 0、
        //   而且**逐条等于 PE 字节里的原值**（OFT=0 也必须如实是 0，不许拿 FT 顶替）。
        //   OFT 非 0 的形态由 ①d 的另一份真实镜像（同资产树的 AGERC.DLL）与 pure 夹具
        //   `pe_loader.cpp#two-descriptors-identity` 钉住。
        g_current_case = "import-descriptor-identity";
        std::size_t oft_nonzero = 0;
        std::size_t ft_nonzero = 0;
        bool have_prev = false;
        std::uint32_t prev_idx = 0;
        std::string prev_module;
        std::uint32_t printed_idx = 0xFFFFFFFFu;
        for (const lift::PeImportInfo& imp : img.imports) {
            if (imp.original_first_thunk != 0) ++oft_nonzero;
            if (imp.first_thunk != 0) ++ft_nonzero;
            LIFT_CHECK(!imp.module.empty());
            // 每对 (OFT, FT) 至少一个非 0 —— FirstThunk 为 0 已被解析器拒绝，这里把契约写死在用例里。
            LIFT_CHECK(imp.original_first_thunk != 0 || imp.first_thunk != 0);
            // IAT 槽 = FirstThunk + 4k：FT 必须是这张表的原始基 RVA（⛔ 不是文件偏移）
            LIFT_CHECK(imp.iat_rva >= imp.first_thunk);
            LIFT_CHECK_EQ_U32((imp.iat_rva - imp.first_thunk) % 4u, 0u);
            if (!have_prev) {
                LIFT_CHECK_EQ_U32(imp.descriptor_index, 0u);
            } else {
                LIFT_CHECK(imp.descriptor_index == prev_idx || imp.descriptor_index == prev_idx + 1u);
                if (imp.descriptor_index == prev_idx) LIFT_CHECK(imp.module == prev_module);
            }
            if (imp.descriptor_index != printed_idx) {
                printed_idx = imp.descriptor_index;
                std::printf("descriptor #%u (OFT=0x%08X FT=0x%08X module=%s)\n",
                            static_cast<unsigned>(imp.descriptor_index),
                            static_cast<unsigned>(imp.original_first_thunk),
                            static_cast<unsigned>(imp.first_thunk), imp.module.c_str());
            }
            prev_idx = imp.descriptor_index;
            prev_module = imp.module;
            have_prev = true;
        }
        LIFT_CHECK(!img.imports.empty());
        LIFT_CHECK(ft_nonzero > 0);
        std::printf("descriptor RVAs: OFT nonzero = %zu / %zu, FT nonzero = %zu / %zu\n", oft_nonzero,
                    img.imports.size(), ft_nonzero, img.imports.size());

        // ★ 原始字段回查：descriptor #i 的 OFT/FT 必须逐字节等于 PE 里的原值（这才是"原始 RVA"的判据）。
        {
            const std::size_t pe = readLe32(img.bytes, 0x3C);
            const std::size_t opt = pe + 24;
            const std::uint32_t imp_rva = readLe32(img.bytes, opt + 96 + 8);
            std::size_t desc_off = 0;
            bool desc_found = false;
            for (const lift::PeSectionInfo& s : img.sections) {
                const std::uint64_t vsize = s.vsize ? s.vsize : s.raw_size;
                if (imp_rva >= s.va && static_cast<std::uint64_t>(imp_rva) < static_cast<std::uint64_t>(s.va) + vsize) {
                    const std::uint32_t delta = imp_rva - s.va;
                    if (delta < s.raw_size) {
                        desc_off = static_cast<std::size_t>(s.raw_offset) + delta;
                        desc_found = true;
                    }
                    break;
                }
            }
            LIFT_CHECK(desc_found);
            if (desc_found) {
                for (const lift::PeImportInfo& imp : img.imports) {
                    const std::size_t o = desc_off + static_cast<std::size_t>(imp.descriptor_index) * 20u;
                    LIFT_CHECK(o + 20 <= img.bytes.size());
                    if (o + 20 > img.bytes.size()) break;
                    LIFT_CHECK_EQ_U32(imp.original_first_thunk, readLe32(img.bytes, o));
                    LIFT_CHECK_EQ_U32(imp.first_thunk, readLe32(img.bytes, o + 16));
                }
            }
        }

        // ② 入口落在可执行节里
        bool entry_in_text = false;
        for (const lift::PeSectionInfo& s : img.sections) {
            const std::uint32_t vsize = s.vsize ? s.vsize : s.raw_size;
            if (img.entry_rva >= s.va && img.entry_rva < s.va + vsize && s.executable()) entry_in_text = true;
        }
        LIFT_CHECK(entry_in_text);
    }

    // ①d 第二份**真实镜像**（同一 corpus 资产树里的 AGERC.DLL）：这份是 **OFT 非 0** 的形态。
    //   ★ 为什么需要它：`LIFT_AGE_EXE`（AGE.EXE）是 VC6 链接的 OFT=0 形态，单靠它无法断言"OFT 非 0"；
    //   而"字段是原始 RVA、不是文件偏移"必须同时钉住 0 与非 0 两种真实形态。找不到这份资产就只打印，
    //   ⛔ 不让 loader 的 assets 档因为一个未在 CMake 里声明的可选文件而红。
    {
        const std::string exe_path = exe;
        const std::size_t last = exe_path.find_last_of("\\/");
        const std::size_t prev =
            (last == std::string::npos || last == 0) ? std::string::npos : exe_path.find_last_of("\\/", last - 1);
        if (prev != std::string::npos) {
            const std::string agerc_path = exe_path.substr(0, prev) + "/agerc/AGERC.DLL";
            std::FILE* probe = lift::openHostFileUtf8(agerc_path, "rb");
            if (!probe) {
                std::printf("SKIP: 没有 %s（可选的真实 OFT 非 0 镜像）\n", agerc_path.c_str());
            } else {
                std::fclose(probe);
                g_current_case = "import-descriptor-identity-agerc";
                lift::PeImage agerc;
                const bool ok = lift::loadPeFile(agerc_path, &agerc, &err);
                if (!ok) std::fprintf(stderr, "loadPeFile(%s): %s\n", agerc_path.c_str(), err.c_str());
                LIFT_CHECK(ok);
                if (ok) {
                    bool any_oft = false;
                    bool any_ft = false;
                    std::uint32_t shown = 0xFFFFFFFFu;
                    for (const lift::PeImportInfo& imp : agerc.imports) {
                        if (imp.original_first_thunk != 0) any_oft = true;
                        if (imp.first_thunk != 0) any_ft = true;
                        LIFT_CHECK(!imp.module.empty());
                        if (imp.descriptor_index != shown) {
                            shown = imp.descriptor_index;
                            std::printf("AGERC descriptor #%u (OFT=0x%08X FT=0x%08X module=%s)\n",
                                        static_cast<unsigned>(imp.descriptor_index),
                                        static_cast<unsigned>(imp.original_first_thunk),
                                        static_cast<unsigned>(imp.first_thunk), imp.module.c_str());
                        }
                    }
                    LIFT_CHECK(!agerc.imports.empty());
                    LIFT_CHECK(any_oft);  // 真镜像上 OFT 非 0（AGE.EXE 只覆盖了 OFT=0 的合法形态）
                    LIFT_CHECK(any_ft);
                }
            }
        }
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
