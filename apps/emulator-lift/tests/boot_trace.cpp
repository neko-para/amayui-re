// emulator-lift · boot trace（A4；assets 档）
//
// ★ 判据（plan §5 A4）：跑到 L2（`--until ea 0x4BA890`）成功；**同一条命令跑两次，trace 逐字节相同**。
//   这里不打印整条 trace（30 万条会淹掉输出），而是把"可复现"折成三个机械可比的量：
//     ① 停下来的原因与 eip；
//     ② 执行过的指令数；
//     ③ 最终寄存器 / 标志 / x87 元信息 + 已提交内存摘要的哈希。
//   三个量两次完全相同 ⇒ 虚拟时钟与"宿主侧不许引入随机源"（plan §8.3）这两条成立。
// ★ 如果没到 L2，本测试**红**并打印卡在哪个 EA —— 那是诚实的结果，不是"跳过"。
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "corpus/lst_index.h"
#include "runtime/machine.h"
#include "runtime/sha256.h"
#include "runtime/vfs.h"

namespace {

struct Outcome {
    lift::StopReason reason = lift::StopReason::None;
    std::uint32_t ea = 0;
    std::uint64_t insns = 0;
    std::uint64_t hash = 0;
    std::uint64_t inputs_hash = 0;  // A4 输入清单的指纹（与"执行结果"分开记）
    bool real_time = false;
    std::string detail;
    bool ok = false;
};

std::uint64_t fnv(std::uint64_t h, const void* p, std::size_t n) {    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

Outcome runOnce(const char* exe, const char* lst_path, std::uint32_t target, std::uint64_t max_insns,
                std::FILE* trace_out = nullptr) {    Outcome out;
    lift::Machine m;
    std::string err;
    if (!m.init(&err) || !m.loadImage(exe, &err) || !m.loadLst(lst_path, &err) || !m.prepare(0, &err)) {
        out.detail = "setup: " + err;
        return out;
    }
    lift::RunOptions o;
    o.until_ea = target;
    o.max_insns = max_insns;
    o.trace_shims = trace_out != nullptr;
    o.trace_out = trace_out;
    const lift::RunResult r = m.run(o);
    out.reason = r.reason;
    out.ea = r.ea;
    out.insns = r.insns;
    out.detail = r.detail;

    std::uint64_t h = 0xCBF29CE484222325ull;
    const lift::X86& cpu = m.cpu();
    h = fnv(h, cpu.r, sizeof(cpu.r));
    h = fnv(h, &cpu.eip, sizeof(cpu.eip));
    h = fnv(h, &cpu.eflags, sizeof(cpu.eflags));
    h = fnv(h, &cpu.fpu.top, sizeof(cpu.fpu.top));
    h = fnv(h, &cpu.fpu.control, sizeof(cpu.fpu.control));
    h = fnv(h, &cpu.fpu.status, sizeof(cpu.fpu.status));
    h = fnv(h, cpu.fpu.regs, sizeof(cpu.fpu.regs));
    const std::uint32_t heap_used = m.heap().bytesInUse();
    const std::uint32_t heap_peak = m.heap().peak();
    h = fnv(h, &heap_used, sizeof(heap_used));
    h = fnv(h, &heap_peak, sizeof(heap_peak));
    out.hash = h;
    // ★ A4 的输入清单（plan §5 A4）：exe 路径串 / guest 命令行 / 环境块 / 时钟起点。
    //   "同一条命令跑两次逐字节相同"只对**固定输入**成立 ⇒ 输入清单自己也要可比。
    out.inputs_hash = m.traceInputsHash();
    out.real_time = m.clock().realTime();
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------------------
// ★ 安装树红线（2026-11-05 缩减）：跑完整个 boot（含两遍 trace 运行）之后，`links/install`
//   根上那几个**已知会被写**的条目必须没变。
//   ★ 为什么不再做递归快照：安装树是 7.5 GB 量级（旧实现对 `links/install` 递归枚举，
//     实测 542 项，每项要尺寸 + mtime，再排序逐项比）—— 成本高、信息量低。而"引擎往安装树里
//     写"只可能落到下面这几个固定名字上（plan-m2-2d-text.md §3 F6 / §4 D5）：
//       `SYS4REG.INI`（退出时保存的玩家配置）· `SAVE`（存档目录）·
//       `$$AGE.TMP`（版本自检探针）· `_.exe`（自检 / `/FIX` 岔路的替换可执行件）。
//   ⇒ 每个名字只做一次 `fs::exists` + 一次 `last_write_time`（不存在 ⇒ false / 0），
//     boot 前后各取一遍逐项比：出现 / 消失 / mtime 变了都算"被写了"。
//   ★ boot_trace 直接跑 `Machine`，没有给 VFS 设 `--game-dir`（文件族会响亮失败）⇒ 这条断言
//     现在近乎恒真；它守的是"将来给 boot 接上 VFS 根之后不许往安装树里写"这条回归。
//   ⛔ 不要改回递归快照，也不要改成对安装树算 hash —— 那正是这次删掉的复杂度。
// ---------------------------------------------------------------------------------------
std::filesystem::path pathFromUtf8(const std::string& s) {
    const std::u8string u(reinterpret_cast<const char8_t*>(s.data()), s.size());
    return std::filesystem::path(u);
}

std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// `LIFT_AGE_EXE = <repo>/corpus/assets/age/AGE.EXE__dumped.sectfix.EXE` → `<repo>`
std::filesystem::path repoRootOf(const char* exe) {
    std::filesystem::path p = pathFromUtf8(exe);
    for (int i = 0; i < 4 && p.has_parent_path(); ++i) p = p.parent_path();
    return p;
}

// `--game-dir` 根上的已知写入点（相对路径；见上面的说明）
constexpr const char* kInstallWriteTargets[] = {"SYS4REG.INI", "$$AGE.TMP", "SAVE", "_.exe"};

struct PathState {
    std::string rel;
    bool exists = false;
    long long mtime = 0;  // `last_write_time` 的计数；不存在时 0（"从无到有"也会在这里变）
    bool operator==(const PathState&) const = default;
};

std::vector<PathState> probeInstallWriteTargets(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::vector<PathState> out;
    for (const char* rel : kInstallWriteTargets) {
        PathState st;
        st.rel = rel;
        std::error_code ec;
        const fs::path p = root / pathFromUtf8(rel);
        st.exists = fs::exists(p, ec) && !ec;
        if (st.exists) {
            ec.clear();
            const auto wt = fs::last_write_time(p, ec);
            if (!ec) st.mtime = static_cast<long long>(wt.time_since_epoch().count());
        }
        out.push_back(st);
    }
    return out;
}

}  // namespace

int main() {
    using namespace lift_test;

    const char* exe = std::getenv("LIFT_AGE_EXE");
    const char* lst_path = std::getenv("LIFT_AGE_LST");
    if (!exe || !*exe) LIFT_SKIP("没有 LIFT_AGE_EXE");
    if (!lst_path || !*lst_path) LIFT_SKIP("没有 LIFT_AGE_LST");

    const std::uint32_t kWinMain = 0x004BA890u;  // plan §6 S7 的 L2
    const std::uint64_t kMaxInsns = 200000000ull;

    // ★ 安装树红线：跑之前先给「已知写入点」拍一次照（见 probeInstallWriteTargets 的说明）
    const std::filesystem::path install_dir = repoRootOf(exe) / "links" / "install";
    const std::vector<PathState> install_before = probeInstallWriteTargets(install_dir);
    std::error_code install_ec;
    const bool install_is_dir = std::filesystem::is_directory(install_dir, install_ec) && !install_ec;
    std::printf("install-tree: %s（只查 %zu 个已知写入点%s）\n", pathToUtf8(install_dir).c_str(),
                install_before.size(),
                install_is_dir ? "" : "；目录不存在 ⇒ 全部按\"不存在\"比");

    g_current_case = "run1";
    const Outcome a = runOnce(exe, lst_path, kWinMain, kMaxInsns);
    if (!a.ok) {
        std::fprintf(stderr, "setup 失败：%s\n", a.detail.c_str());
        return 1;
    }
    std::printf("run1: reason=%s eip=%08X insns=%llu hash=%016llX inputs=%016llX\n", lift::stopReasonName(a.reason),
                a.ea, (unsigned long long)a.insns, (unsigned long long)a.hash, (unsigned long long)a.inputs_hash);
    if (!a.detail.empty()) std::printf("run1: detail=%s\n", a.detail.c_str());

    g_current_case = "run2";
    const Outcome b = runOnce(exe, lst_path, kWinMain, kMaxInsns);
    std::printf("run2: reason=%s eip=%08X insns=%llu hash=%016llX inputs=%016llX\n", lift::stopReasonName(b.reason),
                b.ea, (unsigned long long)b.insns, (unsigned long long)b.hash, (unsigned long long)b.inputs_hash);

    // ★ A4 输入清单：两次运行必须看到同一组输入（`--exe` 的路径串 / 命令行 / 环境块 / 时钟起点）
    g_current_case = "same-inputs";
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a.inputs_hash), static_cast<std::int64_t>(b.inputs_hash));
    LIFT_CHECK(!a.real_time);  // 默认必须是虚拟时钟（真时钟 ⇒ 逐字节断言自动跳过）

    // ① 可复现（这一条与环境无关，永远该绿）
    g_current_case = "determinism";
    LIFT_CHECK(a.reason == b.reason);
    LIFT_CHECK_EQ_U32(a.ea, b.ea);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a.insns), static_cast<std::int64_t>(b.insns));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a.hash), static_cast<std::int64_t>(b.hash));

    // ② 到达 L2
    g_current_case = "reaches-L2";
    if (a.reason != lift::StopReason::UntilEa || a.ea != kWinMain) {
        std::fprintf(stderr,
                     "A4 红：没到 L2（_WinMain@16 = 0x%08X）。停在 %s @ eip=%08X，已执行 %llu 条。\n"
                     "  detail: %s\n",
                     kWinMain, lift::stopReasonName(a.reason), a.ea, (unsigned long long)a.insns, a.detail.c_str());
    }
    LIFT_CHECK(a.reason == lift::StopReason::UntilEa);
    LIFT_CHECK_EQ_U32(a.ea, kWinMain);

    // ③ A4 的**逐字节**判据：同一次初始化的 shim trace 写两份文件后逐字节比较。
    //   旧实现只哈希寄存器 / FPU 元信息 / heap 计数 —— trace 本身一条都没比过（M1a 审计 T2）。
    g_current_case = "trace-bytes-identical";
    // ★ 文件名必须带进程号（2026-11-04 实测）：固定名 `lift-boot-trace-a.log` 在两份 ctest
    //   （并行构建目录 / 两个代理同时跑）**互相撞名** ⇒ 一个 `fopen` 失败 ⇒ 假红。
    //   `file_io.cpp` 早就是这么做的（`-<pid>`），这里跟上。
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    const std::string suffix = "-" + std::to_string(pid) + ".log";
    const std::filesystem::path ta =
        std::filesystem::temp_directory_path() / ("lift-boot-trace-a" + suffix);
    const std::filesystem::path tb =
        std::filesystem::temp_directory_path() / ("lift-boot-trace-b" + suffix);
    auto utf8 = [](const std::filesystem::path& p) {
        const std::u8string s = p.u8string();
        return std::string(reinterpret_cast<const char*>(s.data()), s.size());
    };
    std::FILE* fa = lift::openHostFileUtf8(utf8(ta), "wb");
    std::FILE* fb = lift::openHostFileUtf8(utf8(tb), "wb");
    LIFT_CHECK(fa != nullptr);
    LIFT_CHECK(fb != nullptr);
    Outcome t1;
    Outcome t2;
    if (fa && fb) {
        t1 = runOnce(exe, lst_path, kWinMain, kMaxInsns, fa);
        std::fclose(fa);
        t2 = runOnce(exe, lst_path, kWinMain, kMaxInsns, fb);
        std::fclose(fb);
        LIFT_CHECK(t1.ok && t1.reason == lift::StopReason::UntilEa && t1.ea == kWinMain);
        LIFT_CHECK(t2.ok && t2.reason == lift::StopReason::UntilEa && t2.ea == kWinMain);
        std::ifstream in_a(ta, std::ios::binary);
        std::ifstream in_b(tb, std::ios::binary);
        const std::string sa((std::istreambuf_iterator<char>(in_a)), std::istreambuf_iterator<char>());
        const std::string sb((std::istreambuf_iterator<char>(in_b)), std::istreambuf_iterator<char>());
        std::printf("trace: run1=%zu bytes run2=%zu bytes\n", sa.size(), sb.size());
        LIFT_CHECK(!sa.empty());  // 空 trace 不能因为"两边都空"而假绿
        if (sa != sb) {
            std::size_t i = 0;
            while (i < sa.size() && i < sb.size() && sa[i] == sb[i]) ++i;
            std::fprintf(stderr, "A4 红：trace 第 %zu 字节不同（a=%zu 字节，b=%zu 字节）\n", i, sa.size(),
                         sb.size());
        }
        LIFT_CHECK(sa == sb);
    }
    std::remove(utf8(ta).c_str());
    std::remove(utf8(tb).c_str());

    // ④ 安装树红线：整个 boot（4 次运行）之后，`links/install` 根上那几个已知写入点必须没变。
    g_current_case = "install-tree-untouched";
    const std::vector<PathState> install_after = probeInstallWriteTargets(install_dir);
    LIFT_CHECK(install_after == install_before);
    if (install_after != install_before) {
        for (std::size_t i = 0; i < install_before.size() && i < install_after.size(); ++i) {
            if (install_before[i] == install_after[i]) continue;
            std::fprintf(stderr,
                         "红线：`links/install/%s` 被写了 —— exists: %s → %s，mtime: %lld → %lld\n",
                         install_before[i].rel.c_str(), install_before[i].exists ? "yes" : "no",
                         install_after[i].exists ? "yes" : "no", install_before[i].mtime,
                         install_after[i].mtime);
        }
    } else {
        std::printf("install-tree: %zu 个已知写入点，跑完 boot 后未变\n", install_after.size());
    }

    return finish("boot-trace");
}
