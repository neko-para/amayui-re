// emulator-lift · 脚本身份 marker 事件源（`TriggerKind::Marker`）—— pure 守卫
//
// ★ 判据（红了意味着什么）：`at marker <名字>` 必须由**引擎脚本身份的边沿**驱动 ——
//   采样点 = VM 循环头 `0x4123F0`（+ 消息泵回来的块入口 `0x4123E5`），载荷 = id→名字
//   （读不到 ⇒ 降级成 `id:0x%08X` 仍触发 + 告警），同一 `(cur,id)` 重复出现不重复发，
//   首个样本只播种；等到指定名字而超时 ⇒ **非 0 结局**（`InteractionFailed` + `marker-timeout:` detail）。
// ★ 用例（kebab-case；需求单拿 `<文件>#<用例名>` 当 verify 锚）：
//   ① `marker-value-change-one-event`    值变 ⇒ 一次事件（边沿）
//   ② `marker-no-change-zero-events`     值不变 ⇒ 零事件
//   ③ `marker-a-b-a-two-events`          来回变 ⇒ 两次事件（"重复出现不重复发"只约束**连续**同值）
//   ④ `marker-id-resolves-name`          id → 名字
//   ⑤ `marker-id-out-of-range-degrades`  越界 id ⇒ 降级 + 一次警告（⛔ 不崩、⛔ 不静默）
//   ⑥ `marker-negative-id-degrades`      负数 id ⇒ 不崩、按裁决降级（⛔ 不复刻引擎的表外读）
//   ⑦ `marker-name-base-zero-degrades`   名字表基址 0 ⇒ 降级仍触发 + 警告
//   ⑧ `marker-timeout-loud-failure`      等不到名字 ⇒ `InteractionFailed` + detail 带名字 / 等待时长
//   ⑨ `marker-first-sample-seeds-only`   首采样只播种（"启动时已经在 X 里" ⇒ 不发事件）
//   ⑩ `marker-syntax-rejects-relative`   `after marker` / `wait marker` / `at marker` 缺名字 / `stall` ⇒ 拒绝
//   ⑪ `marker-loop-head-hook`            真 `Machine::run`：自造字节放在 `0x4123F0` ⇒ 钩子在循环头采样
//   ⑫ `marker-pump-return-entry`         真 `Machine::run`：块入口是 `0x4123E5`（线性跨过循环头）也采样
//   ⑬ `marker-cp932-name`                TOC 字节 CP932 → UTF-8（非 ASCII 名整条链）
//   ⑭ `marker-name-80-bytes-no-nul`      80 字节无 NUL ⇒ 截断到 80 接受（⛔ 不越界、⛔ 不误判降级）
//   ⑮ `marker-append-continues`          `appendText` 追加 marker 动作（跳过同单位单调；边沿账继续）
//   ⑯ `marker-name-table-load-refresh`   名字表从"未装载"→"已装载"且身份不变 ⇒ 名字缓存必须刷新
// ★ 夹具形状：裸 `Machine` + `commitZeroed(0x00480000, 1 MiB)` 的假 Engine（`wr32(0x55E1BC, base)`）；
//   ⛔ 不依赖游戏安装 / LFS / 真实窗口（`WindowManager` 只用来走 `quit` 的应用路径）。
//   ★ 每个 4 字节写前先 `committed`：safe 模式缺省关，漏提交就是宿主 SIGSEGV（⛔ 不是"降级"）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "runtime/engine_marker.h"
#include "runtime/input_script.h"
#include "runtime/machine.h"
#include "runtime/memory.h"

namespace {

// 假 Engine 落在 [0x00480000, 0x00580000)：覆盖
//   base+0x5D880（cur）/ base+0x5D894+0x78*39（记录）/ base+0xA609C+0x3028+0x400（FileDB + 卷表）
//   以及 Engine 单例全局 0x0055E1BC（同一块里 ⇒ 不用第二次 commit）。
constexpr std::uint32_t kBase = 0x00480000u;
constexpr std::uint32_t kBaseSize = 0x00100000u;
constexpr std::uint32_t kEngineGlobal = 0x0055E1BCu;
constexpr std::uint32_t kNameBase = 0x004A0000u;
constexpr std::uint32_t kSubBase = 0x004B0000u;
constexpr std::uint32_t kCodeBase = 0x004C0000u;  // 记录 +0x00 指向的"代码区基址"（必须已提交）
constexpr std::uint32_t kLoopHeadEa = lift::kMarkerLoopHeadEa;
constexpr std::uint32_t kPumpReturnEa = lift::kMarkerPumpReturnEa;
constexpr std::uint32_t kFrameBase = kBase + lift::kEngineFrameBase;  // 0x004DD894
constexpr std::uint32_t kCurEa = kBase + lift::kEngineCurOffset;      // 0x004DD880
constexpr std::uint32_t kFileDbEa = kBase + lift::kFileIndexOffset;   // 0x0052609C

void put32(lift::GuestMemory& mem, std::uint32_t ea, std::uint32_t v) {
    LIFT_CHECK(mem.committed(ea, 4u));
    mem.wr32(ea, v);
}

bool boot(lift::Machine& m) {
    std::string err;
    if (!m.initBare(&err)) {
        std::fprintf(stderr, "initBare 失败：%s\n", err.c_str());
        return false;
    }
    if (!m.mem().commitZeroed(kBase, kBaseSize, lift::RegionKind::Heap, "fake-engine")) {
        std::fprintf(stderr, "commit 失败：%s\n", m.mem().lastError());
        return false;
    }
    put32(m.mem(), kEngineGlobal, kBase);  // 发布单例（0x4BAA3A 的同形）
    return true;
}

// 摆一条帧记录：`+0x00` = 代码区基址（形状有效），`+0x3C` = 统一文件 id。
//   ★ 只写记录，**不动 cur**（集成用例要先把 0 / 1 两条记录都摆好，再让 guest 自己推 cur）。
void setRecord(lift::Machine& m, std::uint32_t cur, std::uint32_t id) {
    const std::uint32_t rec = kFrameBase + lift::kFrameStride * cur;
    put32(m.mem(), rec, kCodeBase);
    put32(m.mem(), rec + lift::kFrameIdentityOffset, id);
}

void setIdentity(lift::Machine& m, std::uint32_t cur, std::uint32_t id) {
    put32(m.mem(), kCurEa, cur);
    setRecord(m, cur, id);
}

void installRootTable(lift::Machine& m, std::uint32_t count, std::uint32_t name_base = kNameBase) {
    put32(m.mem(), kFileDbEa + lift::kFileRootCountOffset, count);
    put32(m.mem(), kFileDbEa + lift::kFileRootNameBaseOffset, name_base);
}

// 名字条目：名字在 80 字节**开头**、NUL 结尾（是否保证 NUL 未取证；两种读法都接受）。
void putName(lift::Machine& m, std::uint32_t name_base, std::uint32_t id, const char* name,
             bool nul_terminated = true) {
    const std::uint32_t entry = name_base + lift::kFileNameStride * id;
    const std::size_t n = std::strlen(name);
    for (std::size_t i = 0; i < n; ++i) {
        LIFT_CHECK(m.mem().committed(entry + static_cast<std::uint32_t>(i), 1u));
        m.mem().wr8(entry + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(name[i]));
    }
    if (nul_terminated) {
        LIFT_CHECK(m.mem().committed(entry + static_cast<std::uint32_t>(n), 1u));
        m.mem().wr8(entry + static_cast<std::uint32_t>(n), 0);
    }
}

// 原始字节版（CP932 名字 / 80 字节无 NUL 的条目）：⛔ 不经过 `strlen`。
void putNameBytes(lift::Machine& m, std::uint32_t name_base, std::uint32_t id,
                  const std::vector<std::uint8_t>& bytes, bool nul_terminated) {
    const std::uint32_t entry = name_base + lift::kFileNameStride * id;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        LIFT_CHECK(m.mem().committed(entry + static_cast<std::uint32_t>(i), 1u));
        m.mem().wr8(entry + static_cast<std::uint32_t>(i), bytes[i]);
    }
    if (nul_terminated) {
        LIFT_CHECK(m.mem().committed(entry + static_cast<std::uint32_t>(bytes.size()), 1u));
        m.mem().wr8(entry + static_cast<std::uint32_t>(bytes.size()), 0);
    }
}

bool loadScript(lift::Machine& m, const char* text) {
    std::string err;
    const bool ok = m.loadInputScriptText(text, "inline:marker", &err);
    if (!ok) std::fprintf(stderr, "脚本解析失败：%s\n", err.c_str());
    return ok;
}

// ---- stdout + stderr 一起捕获（"降级必须有一条警告"要断言"响亮"）----
#ifdef _WIN32
using FdT = int;
int dupFd(int fd) { return _dup(fd); }
int dup2Fd(int src, int dst) { return _dup2(src, dst); }
int filenoOf(std::FILE* f) { return _fileno(f); }
void closeFd(int fd) { _close(fd); }
#else
using FdT = int;
int dupFd(int fd) { return ::dup(fd); }
int dup2Fd(int src, int dst) { return ::dup2(src, dst); }
int filenoOf(std::FILE* f) { return ::fileno(f); }
void closeFd(int fd) { ::close(fd); }
#endif

struct Capture {
    std::FILE* tmp = nullptr;
    FdT saved_out = -1;
    FdT saved_err = -1;
    bool active = false;

    bool begin() {
        tmp = std::tmpfile();
        if (tmp == nullptr) return false;
        std::fflush(stdout);
        std::fflush(stderr);
        saved_out = static_cast<FdT>(dupFd(1));
        saved_err = static_cast<FdT>(dupFd(2));
        if (saved_out < 0 || saved_err < 0) return false;
        if (dup2Fd(filenoOf(tmp), 1) < 0) return false;
        if (dup2Fd(filenoOf(tmp), 2) < 0) return false;
        active = true;
        return true;
    }

    std::string finish() {
        if (!active || tmp == nullptr) return {};
        std::fflush(stdout);
        std::fflush(stderr);
        dup2Fd(saved_out, 1);
        dup2Fd(saved_err, 2);
        closeFd(saved_out);
        closeFd(saved_err);
        saved_out = saved_err = -1;
        active = false;
        std::string out;
        std::fseek(tmp, 0, SEEK_END);
        const long size = std::ftell(tmp);
        if (size > 0) {
            out.resize(static_cast<std::size_t>(size));
            std::fseek(tmp, 0, SEEK_SET);
            const std::size_t got = std::fread(out.data(), 1, out.size(), tmp);
            out.resize(got);
        }
        std::fclose(tmp);
        tmp = nullptr;
        return out;
    }
};

bool contains(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

// `inc dword ptr [kCurEa]` + 短跳回块首（`0x4123F0`）。
std::vector<std::uint8_t> loopAtLoopHead() {
    return {0xFF, 0x05, 0x80, 0xD8, 0x4D, 0x00,  // inc dword ptr [0x004DD880]
            0xEB, 0xF8};                          // jmp 0x4123F0（下一句 0x4123F8 − 8）
}

// 原地空转（`jmp 0x4123F0`）：身份永不变化 ⇒ 只用来测超时（⛔ 不推进 cur）。
std::vector<std::uint8_t> spinAtLoopHead() { return {0xEB, 0xFE}; }

// 同一段 inc 放在 `0x4123E5`：5 个 NOP 补齐到 `0x4123F0`，再 `jmp 0x4123E5`。
// ★ 这样**块入口永远是 `0x4123E5`**（`0x4123F0` 只是块内的一条指令）⇒ 只挂 `eip==0x4123F0`
//   的钩子在这里一次都采不到。
std::vector<std::uint8_t> loopAtPumpReturn() {
    return {0xFF, 0x05, 0x80, 0xD8, 0x4D, 0x00,  // inc dword ptr [0x004DD880]
            0x90, 0x90, 0x90, 0x90, 0x90,        // 5×nop → 0x4123F0
            0xE9, 0xF0, 0xFF, 0xFF, 0xFF};       // jmp 0x4123E5
}

}  // namespace

int main() {
    using namespace lift_test;

    // =================================================================================
    // ① 值变 ⇒ 一次事件
    // =================================================================================
    {
        g_current_case = "marker-value-change-one-event";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, 2);
        putName(m, kNameBase, 0, "INIT.BIN");
        putName(m, kNameBase, 1, "LOGO.BIN");
        setIdentity(m, 0, 0);
        LIFT_CHECK(loadScript(m, "at marker LOGO.BIN\nquit\n"));
        LIFT_CHECK(m.inputScript().hasMarkerActions());
        LIFT_CHECK(!m.sampleMarker());  // 首采样只播种
        LIFT_CHECK(m.inputScript().markerSamples() == 1);
        LIFT_CHECK(m.inputScript().markerEdges() == 0);
        LIFT_CHECK(m.inputScript().appliedCount() == 0);
        setIdentity(m, 1, 1);
        LIFT_CHECK(m.sampleMarker());  // 值变 ⇒ 一次事件
        LIFT_CHECK(m.inputScript().markerEdges() == 1);
        LIFT_CHECK(m.inputScript().markerSamples() == 2);
        LIFT_CHECK(m.inputScript().appliedCount() == 1);  // `quit` 被应用
        LIFT_CHECK(m.inputScript().quitRequested());
        LIFT_CHECK(m.lastMarkerSample().name == "LOGO.BIN");
        LIFT_CHECK(!m.lastMarkerSample().name_degraded);
    }

    // =================================================================================
    // ② 值不变 ⇒ 零事件（同一 (cur,id) 采 100 次）
    // =================================================================================
    {
        g_current_case = "marker-no-change-zero-events";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, 2);
        putName(m, kNameBase, 0, "INIT.BIN");
        putName(m, kNameBase, 1, "LOGO.BIN");
        setIdentity(m, 0, 0);
        LIFT_CHECK(loadScript(m, "at marker LOGO.BIN\nquit\n"));
        LIFT_CHECK(!m.sampleMarker());
        for (int i = 0; i < 100; ++i) LIFT_CHECK(!m.sampleMarker());
        LIFT_CHECK(m.inputScript().markerEdges() == 0);
        LIFT_CHECK(m.inputScript().markerSamples() == 101);
        LIFT_CHECK(!m.inputScript().quitRequested());
    }

    // =================================================================================
    // ③ 来回变 ⇒ 两次事件（A→B→A；"重复出现不重复发"只约束**连续**同值）
    // =================================================================================
    {
        g_current_case = "marker-a-b-a-two-events";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, 2);
        putName(m, kNameBase, 0, "INIT.BIN");
        putName(m, kNameBase, 1, "LOGO.BIN");
        setIdentity(m, 0, 0);
        LIFT_CHECK(loadScript(m, "at marker LOGO.BIN\nquit\n"));
        LIFT_CHECK(!m.sampleMarker());  // A（播种）
        setIdentity(m, 1, 1);
        LIFT_CHECK(m.sampleMarker());  // A→B（事件 1）
        setIdentity(m, 0, 0);
        LIFT_CHECK(m.sampleMarker());  // B→A（事件 2）
        LIFT_CHECK(m.inputScript().markerEdges() == 2);
        // 同名可以再发一次：事件 3 必须还是由"变"产生，而不是重复同值。
        setIdentity(m, 1, 1);
        LIFT_CHECK(m.sampleMarker());
        LIFT_CHECK(m.inputScript().markerEdges() == 3);
        LIFT_CHECK(m.inputScript().appliedCount() == 1);  // 动作只应用一次（B 的 quit）
    }

    // =================================================================================
    // ④ id → 名字
    // =================================================================================
    {
        g_current_case = "marker-id-resolves-name";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, 2);
        putName(m, kNameBase, 1, "SCENE_01.BIN");
        setIdentity(m, 0, 1);
        LIFT_CHECK(loadScript(m, "at marker SCENE_01.BIN\nquit\n"));
        LIFT_CHECK(!m.sampleMarker());  // 播种
        LIFT_CHECK(m.lastMarkerSample().valid);
        LIFT_CHECK(m.lastMarkerSample().id == 1);
        LIFT_CHECK(m.lastMarkerSample().name == "SCENE_01.BIN");
        LIFT_CHECK(!m.lastMarkerSample().name_degraded);
        LIFT_CHECK(m.markerSampler().published() == 1);
    }

    // =================================================================================
    // ⑤ 越界 id ⇒ 降级 + 一次警告（⛔ 不崩、⛔ 不静默；降级仍按 id 触发）
    // =================================================================================
    {
        g_current_case = "marker-id-out-of-range-degrades";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, /*count=*/2);  // id 2 越界
        putName(m, kNameBase, 0, "INIT.BIN");
        setIdentity(m, 0, 0);
        LIFT_CHECK(loadScript(m, "at marker id:0x00000002\nquit\n"));
        LIFT_CHECK(!m.sampleMarker());  // 播种
        setIdentity(m, 1, 2);
        std::string log;
        {
            Capture cap;
            LIFT_CHECK(cap.begin());
            LIFT_CHECK(m.sampleMarker());
            log = cap.finish();
        }
        LIFT_CHECK(m.lastMarkerSample().valid);           // 身份可读 ⇒ 事件仍发
        LIFT_CHECK(m.lastMarkerSample().name_degraded);   // 名字读不到 ⇒ 降级
        LIFT_CHECK(m.lastMarkerSample().name == "id:0x00000002");
        LIFT_CHECK(contains(log, "[marker] ⚠ 名字表不可读"));
        LIFT_CHECK(contains(log, "id:0x00000002"));
        LIFT_CHECK(m.inputScript().quitRequested());  // 降级后仍按 id 形触发
    }

    // =================================================================================
    // ⑥ 负数 id ⇒ 不崩、按裁决降级（钉住"负数不是无条件 ERROR"这个真实语义：
    //    引擎 `sar 24` 得 −1 ⇒ 读卷表**之前** 4 字节（表外读）；宿主必须自己判 id<0）
    // =================================================================================
    {
        g_current_case = "marker-negative-id-degrades";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, 2);
        putName(m, kNameBase, 0, "INIT.BIN");
        // 故意把 `[index+0x3024]`（负 id 在引擎里会读到的那个表外槽）指向一个**完全合法**的子索引：
        // 如果实现照抄引擎，就会返回 "TRAP.BIN"；正确实现必须在 `id<0` 处直接降级。
        put32(m.mem(), kFileDbEa + lift::kFileVolumeTableOffset - 4u, kSubBase);
        put32(m.mem(), kSubBase + lift::kFileSubIndexCountOffset, 1);
        put32(m.mem(), kSubBase + lift::kFileSubIndexNameBaseOffset, kNameBase);
        putName(m, kNameBase, 0, "TRAP.BIN");
        setIdentity(m, 0, 0);
        LIFT_CHECK(loadScript(m, "at marker id:0xFFFFFFFF\nquit\n"));
        LIFT_CHECK(!m.sampleMarker());
        setIdentity(m, 1, 0xFFFFFFFFu);
        std::string log;
        {
            Capture cap;
            LIFT_CHECK(cap.begin());
            LIFT_CHECK(m.sampleMarker());
            log = cap.finish();
        }
        LIFT_CHECK(m.lastMarkerSample().valid);
        LIFT_CHECK(m.lastMarkerSample().name_degraded);
        LIFT_CHECK(m.lastMarkerSample().name == "id:0xFFFFFFFF");  // ⛔ 不是 "TRAP.BIN"
        LIFT_CHECK(contains(log, "id<0"));
        LIFT_CHECK(m.inputScript().quitRequested());
    }

    // =================================================================================
    // ⑦ 名字表基址 = 0 ⇒ 降级仍触发 + 警告
    // =================================================================================
    {
        g_current_case = "marker-name-base-zero-degrades";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, /*count=*/2, /*name_base=*/0);
        setIdentity(m, 0, 0);
        LIFT_CHECK(loadScript(m, "at marker id:0x00000001\nquit\n"));
        // 播种那次就已经降级（名字表基址 0）⇒ 警告必须在那一次打出来（每类一次）。
        std::string log;
        {
            Capture cap;
            LIFT_CHECK(cap.begin());
            LIFT_CHECK(!m.sampleMarker());  // 播种（降级样本仍算有效）
            setIdentity(m, 1, 1);
            LIFT_CHECK(m.sampleMarker());   // 值变、降级后仍按 id 形触发
            log = cap.finish();
        }
        LIFT_CHECK(m.lastMarkerSample().valid);
        LIFT_CHECK(m.lastMarkerSample().name_degraded);
        LIFT_CHECK(contains(log, "名字表不可读"));
        LIFT_CHECK(m.inputScript().quitRequested());  // 降级**没有**让事件静默丢失
        LIFT_CHECK(m.inputScript().markerEdges() == 1);
    }

    // =================================================================================
    // ⑧ 超时 ⇒ 响亮失败（InteractionFailed + detail 带名字与等待时长）
    // =================================================================================
    {
        g_current_case = "marker-timeout-loud-failure";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, 1);
        putName(m, kNameBase, 0, "INIT.BIN");
        setIdentity(m, 0, 0);
        LIFT_CHECK(loadScript(m, "at marker NEVER.BIN\nquit\n"));
        m.setInputMarkerTimeoutMs(100);
        // 纯 tick 路径：基线在第一次看见 pending 时记（wall=1000），100 ms 后到点。
        lift::WindowManager wm;
        lift::InputScript& s = m.inputScript();
        LIFT_CHECK(!s.tick(wm, 0, 0, 0, 1000));  // 基线
        LIFT_CHECK(!s.markerTimedOut());
        LIFT_CHECK(!s.tick(wm, 0, 0, 0, 1099));
        LIFT_CHECK(!s.markerTimedOut());
        LIFT_CHECK(!s.tick(wm, 0, 0, 0, 1100));
        LIFT_CHECK(s.markerTimedOut());
        LIFT_CHECK(s.markerTimeoutDetail().find("marker-timeout: waited=NEVER.BIN") == 0);
        LIFT_CHECK(contains(s.markerTimeoutDetail(), "last=none"));
        LIFT_CHECK(contains(s.markerTimeoutDetail(), "waited_ms=100"));
    }
    // 真 `Machine::run`：程序在循环头空转、身份永不变化 ⇒ 50 ms 后必须判死（⛔ 不挂死、exit 5）。
    {
        g_current_case = "marker-timeout-loud-failure";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, 1);
        putName(m, kNameBase, 0, "INIT.BIN");
        setIdentity(m, 0, 0);
        std::string err;
        const std::vector<std::uint8_t> code = spinAtLoopHead();  // 身份不变：只测超时
        LIFT_CHECK(m.placeCode(kLoopHeadEa, code.data(), code.size(), &err));
        LIFT_CHECK(loadScript(m, "at marker NEVER.BIN\nquit\n"));
        m.setInputMarkerTimeoutMs(50);
        lift::RunOptions o;
        o.max_insns = 20000000ull;  // 兜底：超时坏了也不会挂死（只是慢）
        o.time_limit_sec = 10.0;
        const lift::RunResult r = m.run(o);
        LIFT_CHECK(r.reason == lift::StopReason::InteractionFailed);
        LIFT_CHECK(r.marker_timeout);
        LIFT_CHECK(!r.ok());
        LIFT_CHECK(r.detail.find("marker-timeout: waited=NEVER.BIN") == 0);
        LIFT_CHECK(contains(r.detail, "last=INIT.BIN"));  // 实际看到的是什么（名字形态）
        LIFT_CHECK(contains(r.detail, "waited_ms="));
        LIFT_CHECK(m.inputScript().markerSamples() >= 1);  // 采样点确实在工作
        // ★ 超时必须**唤醒**可能阻塞在 GetMessageA 的 guest（窗口模式下 run 的检查点不在跑）：
        //   这条断言钉住"一次性 postQuit(0)"真的发生了（同 `quit` 的处置）。
        LIFT_CHECK(m.cpu().windows != nullptr);
        LIFT_CHECK(m.cpu().windows->quitPosted());
    }

    // =================================================================================
    // ⑨ 首采样只播种："启动时已经在 X 里" ⇒ 不发事件（消费者单的口径，本单只提供边沿）
    // =================================================================================
    {
        g_current_case = "marker-first-sample-seeds-only";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, 2);
        putName(m, kNameBase, 1, "LOGO.BIN");
        setIdentity(m, 0, 1);  // 一开始就在 LOGO
        LIFT_CHECK(loadScript(m, "at marker LOGO.BIN\nquit\n"));
        LIFT_CHECK(!m.sampleMarker());  // 播种
        LIFT_CHECK(!m.sampleMarker());  // 同值 ⇒ 仍然零事件
        LIFT_CHECK(m.inputScript().markerEdges() == 0);
        LIFT_CHECK(m.inputScript().appliedCount() == 0);
        LIFT_CHECK(!m.inputScript().quitRequested());
    }

    // =================================================================================
    // ⑩ 语法：`at marker <名字>` 是唯一形态；`after` / `wait` 没有意义 ⇒ 解析失败
    // =================================================================================
    {
        g_current_case = "marker-syntax-rejects-relative";
        lift::InputScript s;
        std::string err;
        LIFT_CHECK(!s.loadText("at marker A\nwait marker B\n", "inline:syntax", &err));
        LIFT_CHECK(contains(err, "只能写成 `at marker <名字>`"));
        err.clear();
        LIFT_CHECK(!s.loadText("at marker A\nafter 1 marker\n", "inline:syntax", &err));
        LIFT_CHECK(contains(err, "只能写成 `at marker <名字>`"));
        err.clear();
        LIFT_CHECK(!s.loadText("at marker\n", "inline:syntax", &err));  // 缺名字（2 个词）
        LIFT_CHECK(contains(err, "需要 3 个词"));
        err.clear();
        LIFT_CHECK(!s.loadText("at marker A stall 100\nquit\n", "inline:syntax", &err));
        LIFT_CHECK(contains(err, "不接受 `stall`"));
        err.clear();
        LIFT_CHECK(s.loadText("at marker A\nquit\n", "inline:syntax", &err));
        LIFT_CHECK(s.hasMarkerActions());
        // `at marker` 之后的名字不许漏进后面的数值触发（同一个 `Trigger` 被复用）。
        err.clear();
        LIFT_CHECK(s.loadText("at marker A\nquit\nat frame 1\nquit\n", "inline:syntax", &err));
        LIFT_CHECK(s.actions().size() == 2);
        LIFT_CHECK(s.actions()[1].trigger.kind == lift::TriggerKind::Frame);
        LIFT_CHECK(s.actions()[1].trigger.name.empty());
    }

    // =================================================================================
    // ⑪ 真 Machine：钩子在 VM 循环头 `0x4123F0` 采样（自造字节，⛔ 不依赖游戏）
    // =================================================================================
    {
        g_current_case = "marker-loop-head-hook";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, 2);
        putName(m, kNameBase, 0, "INIT.BIN");
        putName(m, kNameBase, 1, "LOGO.BIN");
        setRecord(m, 0, 0);  // cur=0：INIT（首个样本只播种）
        setRecord(m, 1, 1);  // cur=1：LOGO（第二次采样是边沿）
        put32(m.mem(), kCurEa, 0);
        std::string err;
        const std::vector<std::uint8_t> code = loopAtLoopHead();
        LIFT_CHECK(m.placeCode(kLoopHeadEa, code.data(), code.size(), &err));
        LIFT_CHECK(loadScript(m, "at marker LOGO.BIN\nquit\n"));
        lift::RunOptions o;
        o.max_insns = 1000;
        o.time_limit_sec = 10.0;
        const lift::RunResult r = m.run(o);
        LIFT_CHECK(r.reason == lift::StopReason::Stopped);
        LIFT_CHECK(r.detail == "input script: quit");
        LIFT_CHECK(m.inputScript().markerSamples() >= 2);
        LIFT_CHECK(m.inputScript().markerEdges() == 1);
        LIFT_CHECK(m.markerSampler().published() >= 2);
    }

    // =================================================================================
    // ⑫ 真 Machine：块入口是 `0x4123E5`（线性跨过 `0x4123F0`）时也必须采样
    //     ★ 只挂 `eip==0x4123F0` ⇒ 这里一次采样都没有 ⇒ 永远等不到 ⇒ MaxInsns（用例当场红）
    // =================================================================================
    {
        g_current_case = "marker-pump-return-entry";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, 2);
        putName(m, kNameBase, 0, "INIT.BIN");
        putName(m, kNameBase, 1, "LOGO.BIN");
        setRecord(m, 0, 0);
        setRecord(m, 1, 1);
        put32(m.mem(), kCurEa, 0);
        std::string err;
        const std::vector<std::uint8_t> code = loopAtPumpReturn();
        LIFT_CHECK(m.placeCode(kPumpReturnEa, code.data(), code.size(), &err));
        m.cpu().eip = kPumpReturnEa;  // 块入口 = 0x4123E5（0x4123F0 只是块内一条 jmp）
        LIFT_CHECK(loadScript(m, "at marker LOGO.BIN\nquit\n"));
        lift::RunOptions o;
        o.max_insns = 1000;
        o.time_limit_sec = 10.0;
        const lift::RunResult r = m.run(o);
        LIFT_CHECK(r.reason == lift::StopReason::Stopped);
        LIFT_CHECK(r.detail == "input script: quit");
        LIFT_CHECK(m.inputScript().markerSamples() >= 1);
        LIFT_CHECK(m.inputScript().markerEdges() == 1);
    }

    // =================================================================================
    // ⑬ CP932 非 ASCII 名：TOC 字节 → CP932 → UTF-8 整条链（脚本里写 UTF-8 名字）
    // =================================================================================
    {
        g_current_case = "marker-cp932-name";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, 2);
        // "ロゴ" 的 CP932 字节 = 83 8D 83 53（TOC 是字节表，未取证是否为 CP932，按最常见口径）。
        putNameBytes(m, kNameBase, 1, {0x83, 0x8D, 0x83, 0x53}, /*nul_terminated=*/true);
        setIdentity(m, 0, 1);
        LIFT_CHECK(loadScript(m, "at marker ロゴ\nquit\n"));
        LIFT_CHECK(!m.sampleMarker());  // 播种
        LIFT_CHECK(m.lastMarkerSample().valid);
        LIFT_CHECK(!m.lastMarkerSample().name_degraded);
        LIFT_CHECK(m.lastMarkerSample().name == "ロゴ");  // UTF-8
    }

    // =================================================================================
    // ⑭ 名字条目 80 字节**没有 NUL** ⇒ 截断到 80 字节接受（⛔ 不越界读、⛔ 不误判降级）
    // =================================================================================
    {
        g_current_case = "marker-name-80-bytes-no-nul";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        installRootTable(m, 2);
        const std::string long_name(80, 'A');  // 80 字节，刚好占满条目、没有 NUL
        putNameBytes(m, kNameBase, 1, std::vector<std::uint8_t>(long_name.begin(), long_name.end()),
                     /*nul_terminated=*/false);
        setIdentity(m, 0, 1);
        LIFT_CHECK(loadScript(m, "at marker A\nquit\n"));  // 只为 hasMarkerActions（名字不参与本断言）
        LIFT_CHECK(!m.sampleMarker());  // 播种
        LIFT_CHECK(m.lastMarkerSample().valid);
        LIFT_CHECK(!m.lastMarkerSample().name_degraded);
        LIFT_CHECK(m.lastMarkerSample().name.size() == 80);
        LIFT_CHECK(m.lastMarkerSample().name == long_name);
    }

    // =================================================================================
    // ⑮ appendText 追加 marker 动作：跳过"同单位单调"、边沿状态与 applied 账继续有效
    // =================================================================================
    {
        g_current_case = "marker-append-continues";
        lift::InputScript s;
        lift::WindowManager wm;
        std::string err;
        LIFT_CHECK(s.loadText("at marker LATE.BIN\nprofile_begin p\n", "inline:append", &err));
        lift::MarkerSample seed;
        seed.valid = true;
        seed.cur = 0;
        seed.id = 0;
        seed.name = "INIT.BIN";
        LIFT_CHECK(!s.noteMarker(wm, seed, 0, 0));  // 播种
        lift::MarkerSample edge;
        edge.valid = true;
        edge.cur = 1;
        edge.id = 1;
        edge.name = "LATE.BIN";
        LIFT_CHECK(s.noteMarker(wm, edge, 0, 0));  // 边沿 ⇒ 应用 profile_begin
        LIFT_CHECK(s.appliedCount() == 1);
        LIFT_CHECK(s.appendText("at marker EARLY.BIN\nquit\n", "inline:append", &err));
        LIFT_CHECK(s.actionCount() == 2);
        lift::MarkerSample edge2;
        edge2.valid = true;
        edge2.cur = 2;
        edge2.id = 2;
        edge2.name = "EARLY.BIN";
        LIFT_CHECK(s.noteMarker(wm, edge2, 0, 0));  // 追加的动作也能被边沿触发
        LIFT_CHECK(s.quitRequested());
        LIFT_CHECK(s.appliedCount() == 2);
        LIFT_CHECK(s.markerEdges() == 2);
    }

    // =================================================================================
    // ⑯ 名字表从"未装载"变成"已装载"：**同一身份**也必须重新解析名字（缓存不能把降级焊死）
    // =================================================================================
    {
        g_current_case = "marker-name-table-load-refresh";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        setIdentity(m, 0, 7);  // id 7，名字表还没装载（count/base 都是 0）
        LIFT_CHECK(loadScript(m, "at marker FRESH.BIN\nquit\n"));
        LIFT_CHECK(!m.sampleMarker());  // 播种 + 降级
        LIFT_CHECK(m.lastMarkerSample().valid);
        LIFT_CHECK(m.lastMarkerSample().name_degraded);
        LIFT_CHECK(m.lastMarkerSample().name == "id:0x00000007");
        // 装载 TOC（身份**不变**）：名字表状态指纹变了 ⇒ 必须重解析。
        installRootTable(m, 8);
        putName(m, kNameBase, 7, "FRESH.BIN");
        LIFT_CHECK(!m.sampleMarker());  // 仍是同一 (cur,id) ⇒ 零事件
        LIFT_CHECK(!m.lastMarkerSample().name_degraded);
        LIFT_CHECK(m.lastMarkerSample().name == "FRESH.BIN");
        LIFT_CHECK(m.inputScript().lastMarkerName() == "FRESH.BIN");  // timeout 的 `last=` 也跟着刷新
        LIFT_CHECK(m.inputScript().markerEdges() == 0);
    }

    return finish("input-marker");
}
