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

// ★ AGERC 的真实导出表问答（实现与签名真源 = `src/shims/kernel32_agerc.cpp`）。
//   声明**故意**写在这里而不是某个头文件：头文件不在本缺陷单
//   （REQ-01M75SDKZGY87B8SK7GBP92PXD）的输入边界内。⚠ 改签名 ⇒ 三处一起改
//   （kernel32_agerc.cpp / kernel32_process.cpp / 这里）。
namespace lift {
std::size_t agercExportCount(const PeImage& img);
std::string agercExportName(const PeImage& img, std::size_t index);
}  // namespace lift

namespace {

std::uint32_t readLe32(const std::vector<std::uint8_t>& b, std::size_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

// `LIFT_AGE_EXE` 的**上一级**目录下那份真实 AGERC.DLL（同一 corpus 资产树）。
// 返回空串 = 没有这份可选资产（调用方按"不验"处理，⛔ 不让 assets 档因为可选文件而红）。
std::string siblingAgercPath(const char* exe) {
    if (!exe || !*exe) return {};
    const std::string exe_path = exe;
    const std::size_t last = exe_path.find_last_of("\\/");
    if (last == std::string::npos || last == 0) return {};
    const std::size_t prev = exe_path.find_last_of("\\/", last - 1);
    if (prev == std::string::npos) return {};
    const std::string agerc_path = exe_path.substr(0, prev) + "/agerc/AGERC.DLL";
    std::FILE* probe = lift::openHostFileUtf8(agerc_path, "rb");
    if (!probe) return {};
    std::fclose(probe);
    return agerc_path;
}

// 直接调一条**已注册**的 shim，不铺 guest 机器码：把 `[esp]` 放哨兵返回地址、`[esp+4…]` 放参数。
// ★ loader 这条测试不执行 guest 代码 ⇒ 借用栈顶下面一段空白当临时帧是安全的。
// ★ 走 `ShimRegistry::invoke` 而不是手搓参数：调用约定 / 弹参 / 返回值那条路径与真跑时**同一份**代码。
std::uint32_t callShim(lift::Machine& m, const char* module, const char* name,
                       const std::vector<std::uint32_t>& args) {
    lift::ShimDef* def = m.shims().findMutable(module, name);
    if (def == nullptr || def->stub_ea == 0) {
        std::printf("SKIP: shim 没注册（%s!%s）⇒ 这个用例没验到东西\n", module, name);
        return 0;
    }
    const std::uint32_t sp = m.setup().stack_top - 0x80u;
    std::uint32_t cursor = sp;
    m.mem().wr32(cursor, 0xFFFFFFFFu);  // 返回地址哨兵
    cursor += 4u;
    for (const std::uint32_t a : args) {
        m.mem().wr32(cursor, a);
        cursor += 4u;
    }
    m.cpu().esp() = sp;
    m.shims().invoke(m.cpu(), *def);
    return m.cpu().eax();
}

// 把一个 C 串写进 guest 的临时区（用同一块地址反复覆盖 ⇒ 不需要分配器）
std::uint32_t guestScratchString(lift::Machine& m, std::uint32_t ea, const std::string& s) {
    std::memcpy(m.mem().host(ea), s.c_str(), s.size() + 1u);
    return ea;
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
    const std::string agerc_asset = siblingAgercPath(exe);  // ③b 也用它（同一份）⇒ 只剩一处路径口径
    {
        if (agerc_asset.empty()) {
            std::printf("SKIP: 没有 <exe 上级>/agerc/AGERC.DLL（可选的真实 OFT 非 0 镜像）\n");
        } else {
            g_current_case = "import-descriptor-identity-agerc";
            lift::PeImage agerc;
            const bool ok = lift::loadPeFile(agerc_asset, &agerc, &err);
            if (!ok) std::fprintf(stderr, "loadPeFile(%s): %s\n", agerc_asset.c_str(), err.c_str());
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

    // ⑥ `--native-overrides` 的**名字解析 + 隔离臂**（`all` 装得进 / `all,-名字` 排得掉）
    //
    // 判据（红了意味着什么）：`configureNativeOverrides("all")` 之后表里**必须**有 `__ftol2_sse`
    // 那一项（`0x4E82C0`）；`"all,-__ftol2_sse"` 之后必须没有，且其余项仍在。
    // ★ 为什么这条必须有：`all` 走的是 `addName(name, /*required=*/false)`，而"`.lst` 里找不到
    //   函数入口"那一条分支是**静默跳过**（不报错）⇒ 名字挂错（例如挂 `__ftol2` —— 它只是
    //   `0x4E82E3 jz` 的**函数内**目标，没有 `proc near` / `COLLAPSED` 行）的表现是
    //   "`all` 里没有这一项、hits 恒 0，而所有差分类测试都绿"（那些测试自己 `add()` 的是 EA）。
    //   ⇒ 这条把"名字 → `.lst` 函数入口 → 表"整段钉住；`-名字` 那一半就是**隔离臂**。
    // ★ 需要 `.lst`（`loadLst` 已在上面的 `prepare` 之前跑过）⇒ 只能落在这个 assets 档，
    //   `tests/native_override.cpp`（pure、bare 机器）够不着。
    {
        g_current_case = "native-override-name-resolution";
        const lift::LstFunction* ftol_fn = m.lst().findFunction("__ftol2_sse");
        LIFT_CHECK(ftol_fn != nullptr);
        if (ftol_fn != nullptr) LIFT_CHECK_EQ_U32(ftol_fn->ea, 0x004E82C0u);
        // ★ Huffman 两片叶子（REQ-01M75WED0VFTEYXX43KGHNPPP0）：名字必须解析到 `.lst` 的
        //   `proc near` 行（`0x4E1570` / `0x4E1950`）—— 名字挂错的表现是"`all` 里没有这一项、
        //   hits 恒 0，而所有差分类测试都绿"（那些测试自己 `add()` 的是 EA）。
        const lift::LstFunction* huff_dec = m.lst().findFunction("sub_4E1570");
        const lift::LstFunction* huff_vec = m.lst().findFunction("sub_4E1950");
        LIFT_CHECK(huff_dec != nullptr);
        LIFT_CHECK(huff_vec != nullptr);
        if (huff_dec != nullptr) LIFT_CHECK_EQ_U32(huff_dec->ea, 0x004E1570u);
        if (huff_vec != nullptr) LIFT_CHECK_EQ_U32(huff_vec->ea, 0x004E1950u);
        std::string cerr;
        m.overrides().clear();
        m.blockOverrides().clear();
        LIFT_CHECK(m.configureNativeOverrides("all", &cerr));
        LIFT_CHECK(m.overrides().find(0x004E82C0u) != nullptr);
        LIFT_CHECK(m.overrides().find(0x004E1570u) != nullptr);
        LIFT_CHECK(m.overrides().find(0x004E1950u) != nullptr);
        // ★ 段级（`REQ-01M75W6BB3PY3MF42ZTS23KTBS` 的**第 7 处落点**判据）：`all` 必须把 PCM 段
        //   （entry `0x48D767`）**真的装进主表**。锚校验（`verifyKnownBlockAnchor`）一旦失败，
        //   `addBlock` 的 `required=false` 分支是**静默跳过**（只打一行 stderr）⇒ 下面这两条是唯一
        //   会红的判据。★ `feeder_ok` 也在这里读：它是 feeder 私有副本的唯一 opt-in 真源。
        LIFT_CHECK(m.blockOverrides().find(0x0048D767u) != nullptr);
        if (const lift::NativeBlockOverride* pcm_blk = m.blockOverrides().find(0x0048D767u)) {
            LIFT_CHECK_EQ_U32(pcm_blk->resume_ea, 0x0048D89Au);
            LIFT_CHECK(pcm_blk->feeder_ok);
        }
        // 旧段级项仍在，且 **feeder_ok=false**（opt-in 是一对一的：不是"段级一律 copy"）
        LIFT_CHECK(m.blockOverrides().find(0x00415384u) != nullptr);
        if (const lift::NativeBlockOverride* pool_blk = m.blockOverrides().find(0x00415384u))
            LIFT_CHECK(!pool_blk->feeder_ok);
        // 隔离臂：段级项也能被 `-名字` 单独排掉，且函数级不受影响
        m.blockOverrides().clear();
        LIFT_CHECK(m.configureNativeOverrides("all,-sub_48D610", &cerr));
        LIFT_CHECK(m.blockOverrides().find(0x0048D767u) == nullptr);
        LIFT_CHECK(m.overrides().find(0x004E82C0u) != nullptr);
        m.blockOverrides().clear();
        // 隔离臂：两项各自能单独排掉（`all,-名字`），而且**另一项仍在**
        m.overrides().clear();
        LIFT_CHECK(m.configureNativeOverrides("all,-sub_4E1570", &cerr));
        LIFT_CHECK(m.overrides().find(0x004E1570u) == nullptr);
        LIFT_CHECK(m.overrides().find(0x004E1950u) != nullptr);
        m.overrides().clear();
        LIFT_CHECK(m.configureNativeOverrides("all,-sub_4E1950", &cerr));
        LIFT_CHECK(m.overrides().find(0x004E1950u) == nullptr);
        LIFT_CHECK(m.overrides().find(0x004E1570u) != nullptr);
        m.overrides().clear();
        LIFT_CHECK(m.configureNativeOverrides("all,-__ftol2_sse", &cerr));
        LIFT_CHECK(m.overrides().find(0x004E82C0u) == nullptr);  // 隔离臂：排除项优先
        LIFT_CHECK(m.overrides().find(0x004E1570u) != nullptr);  // 别的项不受影响
        LIFT_CHECK(!m.overrides().empty());                      // 不是"整张表都没了"的假绿
        m.overrides().clear();
    }

    // ⑦ 真实 AGERC 的**导出表**（REQ-01M75SDKZGY87B8SK7GBP92PXD）
    //
    // 红了意味着什么：`GetProcAddress(AGERC, <真实导出名>)` 又变成 0 ⇒ 引擎按名取 AGERC 导出
    // 会拿到 127 ⇒ 它弹「致命的错误」框（引擎自己的 `%sのアドレス取得に失敗しました`，
    // AGE.EXE `0x42FFD2` / `0x422B0D`）⇒ 被 `MessageBoxA` 的恒 IDOK 压成"中止" ⇒
    // 点一下 LoadData 就干净退出。★ 本单的实测就是 `_SetNameLenMax@20`（SAVE.BIN COMMAND=332）。
    //
    // 两条机械判据：
    //   ① **白名单 ⊇ 真实 PE 导出表**：镜像 export directory 里的**每一条**按名导出都必须解析成非 0
    //      （前提是 `agercExportCount > 0` —— 读不出导出表时这个循环会是**真空绿**，所以先断言它）；
    //   ② 引擎真正取的那一条（`_SetNameLenMax@20`）必须解析到**已登记实现**的 stub thunk
    //      （`fn != nullptr`），而不是"有地址但没实现"的运行期 thunk，且它的返回值 = 真实实现的 0。
    {
        g_current_case = "agerc-exports";
        if (agerc_asset.empty()) {
            std::printf("SKIP: 没有 <exe 上级>/agerc/AGERC.DLL ⇒ 不验真实 AGERC 导出表\n");
        } else {
            std::string aerr;
            const bool mapped = m.loadAgercModule(agerc_asset, &aerr);
            if (!mapped) std::fprintf(stderr, "loadAgercModule(%s): %s\n", agerc_asset.c_str(), aerr.c_str());
            LIFT_CHECK(mapped);
            const std::uint32_t base = m.agercBase();
            LIFT_CHECK_NE_U32(base, 0u);
            // guest 调 LoadLibraryA("AGERC.DLL") ⇒ 真实模块基址（GetProcAddress 的前提）
            const std::uint32_t str_ea = m.setup().stack_top - 0x200u;  // 临时字符串区（不跑 guest 代码）
            const std::uint32_t lib_ea =
                callShim(m, "kernel32", "LoadLibraryA", {guestScratchString(m, str_ea, "AGERC.DLL")});
            LIFT_CHECK_EQ_U32(lib_ea, base);

            // ---- ① 白名单 ⊇ 真实导出表 ----
            const std::size_t export_count = lift::agercExportCount(m.agercImage());
            std::printf("AGERC real exports: %zu (base=%08X imageBase=%08X)\n", export_count,
                        static_cast<unsigned>(base),
                        static_cast<unsigned>(m.agercImage().image_base));
            LIFT_CHECK(export_count > 0);  // 读不出 ⇒ 下面的循环没有意义（真空绿）
            std::size_t resolved = 0;
            for (std::size_t i = 0; i < export_count; ++i) {
                const std::string proc = lift::agercExportName(m.agercImage(), i);
                LIFT_CHECK(!proc.empty());
                if (proc.empty()) continue;
                const std::uint32_t name_ea = guestScratchString(m, str_ea + 0x40u, proc);
                const std::uint32_t ea = callShim(m, "kernel32", "GetProcAddress", {base, name_ea});
                // ★ 差集必须为空：真实导出一条都不许解析成 0
                if (ea == 0) std::fprintf(stderr, "AGERC!%s 解析成 0（差集非空）\n", proc.c_str());
                LIFT_CHECK_NE_U32(ea, 0u);
                if (ea != 0) ++resolved;
            }
            LIFT_CHECK_EQ_I64(resolved, export_count);

            // ---- ② 引擎实际取的那一条：**已登记的实现** + 真实返回值 0 ----
            const std::string want = "_SetNameLenMax@20";
            const std::uint32_t want_ea =
                callShim(m, "kernel32", "GetProcAddress", {base, guestScratchString(m, str_ea + 0x40u, want)});
            LIFT_CHECK_NE_U32(want_ea, 0u);
            const lift::ShimDef* def = m.shims().find("agerc", want.c_str());
            LIFT_CHECK(def != nullptr);
            if (def != nullptr) {
                // ⛔ 不是"有这个导出但还没实现"（那种 thunk 的 fn 是 nullptr，调用即响亮失败）
                LIFT_CHECK(def->fn != nullptr);
                LIFT_CHECK_EQ_U32(want_ea, def->stub_ea);
            }
            // 真实实现（AGERC export 20 → 体 0x100367D0）= 存 `*arg1` 进 g_nameLenMax、**返回 0**
            const std::uint32_t value_ea = str_ea + 0x80u;
            m.mem().wr32(value_ea, 24u);
            LIFT_CHECK_EQ_U32(callShim(m, "agerc", want.c_str(), {0u, value_ea, 0u, 0u, 0u}), 0u);
        }
    }

    std::printf("entry=%08X sections=%zu imports=%zu\n", m.image().entry_ea, m.image().sections.size(),
                m.image().imports.size());
    m.reportBaseGaps(stdout);
    return finish("loader");
}
