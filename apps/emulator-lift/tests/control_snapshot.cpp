// emulator-lift · 控制层**快照**（S3）的 pure 守卫（顶层无条件注册 ⇒ pure 档）
//
// 判据（红了意味着什么）：
//   ① **读不到时是枚举**（判据 3d）：boot 前 / 身份未发布 ⇒ `sample.script.available:false` 且
//      `why` ∈ `engine_marker` 里**已存在**的 8 个字面量（⛔ 不新造 `identity_unpublished` /
//      `no_sha` / `header_unreadable` 这类设计稿名字）。
//   ② **位置只走唯一公式**（判据 3a 的机制面）：`pos = kScriptHeaderLen + (pc − base)`、
//      `header_len == 0x3C`（编译期常量，⛔ 不是从头里读的）、`pos % 4 == 0`。
//   ③ **`script_sha` 覆盖脚本体**（判据 3c 的机制面）：宿主文件多 4 字节 ⇒ sha 必须变；
//      解析不到 ⇒ `sha:null` + `sha_why:"unresolved"`（⛔ 不静默回一个空 sha 当"没有内容"）。
//   ④ **快照 JSON 逐位稳定**（判据 3b 的机制面）：同一份值序列化两次逐字节相同；且**没有**
//      墙钟 / "距上次…" / 自增类字段 —— 那些会让"同一暂停点连采两次"不等。
// ★ 不碰 GPU / 不起进程 / 不读 LFS：`GuestMemory` + 手摆的假 Engine + 临时目录里的 TITLE.BIN。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "check.h"
#include "runtime/control_snapshot.h"
#include "runtime/engine_marker.h"
#include "runtime/memory.h"
#include "runtime/present.h"  // `dumpLastFrame` + `classifyStaleEvent`（判据 5 / 13 的机制面）
#include "runtime/sha256.h"
#include "runtime/surface.h"
#include "runtime/vfs.h"

namespace {

// 假 Engine 的落点（与 tests/input_marker.cpp 同一套布局常量；⛔ 不另编一套偏移）。
constexpr std::uint32_t kBase = 0x00480000u;
constexpr std::uint32_t kBaseSize = 0x00100000u;
constexpr std::uint32_t kEngineGlobal = lift::kEngineSingletonGlobal;
constexpr std::uint32_t kNameBase = 0x004A0000u;
constexpr std::uint32_t kCodeBase = 0x004C0000u;  // 脚本体缓冲基址（必须已提交）
constexpr std::uint32_t kFrameBase = kBase + lift::kEngineFrameBase;
constexpr std::uint32_t kCurEa = kBase + lift::kEngineCurOffset;
constexpr std::uint32_t kFileDbEa = kBase + lift::kFileIndexOffset;

bool put32(lift::GuestMemory& mem, std::uint32_t ea, std::uint32_t v) {
    if (!mem.committed(ea, 4u)) return false;
    mem.wr32(ea, v);
    return true;
}

bool putName(lift::GuestMemory& mem, std::uint32_t id, const char* name) {
    const std::uint32_t entry = kNameBase + lift::kFileNameStride * id;
    const std::size_t n = std::strlen(name);
    for (std::size_t i = 0; i <= n; ++i) {
        if (!mem.committed(entry + static_cast<std::uint32_t>(i), 1u)) return false;
        mem.wr8(entry + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(i == n ? 0 : name[i]));
    }
    return true;
}

// 摆一份"已发布、已进入脚本"的假 Engine：`cur = 0`、记录 +0x00 = 代码区基址、+0x04 = pc、
// +0x3C = id 0；名字表第 0 条 = `name`。
bool makePublishedEngine(lift::GuestMemory& mem, const char* name, std::uint32_t pc_delta) {
    if (!mem.reserve()) return false;
    if (!mem.commitZeroed(kBase, kBaseSize, lift::RegionKind::Heap, "fake-engine")) return false;
    if (!put32(mem, kEngineGlobal, kBase)) return false;
    if (!put32(mem, kCurEa, 0u)) return false;
    const std::uint32_t rec = kFrameBase;
    if (!put32(mem, rec + lift::kFrameBodyBaseOffset, kCodeBase)) return false;
    if (!put32(mem, rec + lift::kFramePcOffset, kCodeBase + pc_delta)) return false;
    if (!put32(mem, rec + lift::kFrameIdentityOffset, 0u)) return false;
    // 名字表：根条目数 1、名字数组基址 kNameBase（kNameBase 在同一块已提交内存里）。
    if (!put32(mem, kFileDbEa + lift::kFileRootCountOffset, 1u)) return false;
    if (!put32(mem, kFileDbEa + lift::kFileRootNameBaseOffset, kNameBase)) return false;
    return putName(mem, 0u, name);
}

std::string readWholeFile2(const std::string& path) {
    std::string out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

std::string hexOf(const std::vector<std::uint8_t>& bytes) {
    return lift::toHex(lift::sha256(bytes.data(), bytes.size()));
}

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool hasAny(const std::string& text, const std::vector<std::string>& needles) {
    for (const std::string& n : needles) {
        if (text.find(n) != std::string::npos) return true;
    }
    return false;
}

// =====================================================================================
// ① 读不到时是枚举（判据 3d）
// =====================================================================================
void caseUnavailableIsEnum() {
    ::lift_test::g_current_case = "script-unavailable-enum";
    lift::GuestMemory mem;  // 不 reserve：任何读都失败（= boot 之前）
    lift::Vfs vfs;          // 没有 --game-dir
    lift::MarkerSampler sampler;
    lift::ControlScriptSample out;
    lift::fillControlScriptSample(mem, sampler, vfs, &out);

    // `engine_marker.cpp` 里 `readScriptPositionSample` 的 8 个静态字面量（既有集合，⛔ 新造名字不算）。
    const std::vector<std::string> kExisting = {
        "identity-invalid",       "engine-unpublished",  "frame-record-uncommitted",
        "body-base-unpublished",  "pc-outside-body",     "pc-outside-region",
        "pc-offset-over-max",     "pos-overflow",
    };
    bool known = false;
    for (const std::string& w : kExisting) {
        if (out.why != nullptr && w == out.why) known = true;
    }
    std::printf("[3d] empty memory: available=%d why=%s sha_why=%s known=%d\n", out.available ? 1 : 0,
                out.why != nullptr ? out.why : "(null)", out.sha_why != nullptr ? out.sha_why : "(null)",
                known ? 1 : 0);
    LIFT_CHECK(!out.available);
    LIFT_CHECK(out.why != nullptr);
    LIFT_CHECK(known);  // ★ 只能是既有 8 个之一
    LIFT_CHECK(std::string(out.sha_why) == "no_identity");
    LIFT_CHECK(out.sha.empty());
    LIFT_CHECK(out.header_len == lift::kScriptHeaderLen);  // 常量照实回（0x3C）
}

// =====================================================================================
// ② 位置公式 + ③ script_sha（判据 3a / 3c 的机制面）
// =====================================================================================
void casePositionAndSha() {
    ::lift_test::g_current_case = "script-position-and-sha";
    namespace fs = std::filesystem;
    const fs::path dir = fs::path("control-snapshot-work") / "install";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    LIFT_CHECK(fs::is_directory(dir, ec));

    std::vector<std::uint8_t> body(256, 0x11u);  // 假脚本体（内容随便；sha 只看字节）
    const fs::path script = dir / "TITLE.BIN";
    writeBytes(script, body);

    lift::GuestMemory mem;
    LIFT_CHECK(makePublishedEngine(mem, "TITLE.BIN", 0x24u));
    lift::Vfs vfs;
    vfs.setGameDir(dir.string());
    lift::MarkerSampler sampler;
    lift::ControlScriptSample out;
    lift::fillControlScriptSample(mem, sampler, vfs, &out);

    const std::uint32_t expected_pos = lift::kScriptHeaderLen + 0x24u;
    std::printf(
        "[3a] published engine: available=%d script=%s pos=0x%08X (expect 0x%08X) header_len=%u "
        "sha_why=%s sha=%s\n",
        out.available ? 1 : 0, out.name.c_str(), out.pos, expected_pos, out.header_len,
        out.sha_why != nullptr ? out.sha_why : "(null)", out.sha.c_str());
    LIFT_CHECK(out.available);
    LIFT_CHECK(out.why == nullptr);
    LIFT_CHECK(out.name == "TITLE.BIN");
    LIFT_CHECK(out.pos == expected_pos);
    LIFT_CHECK(out.pos % 4u == 0u);
    LIFT_CHECK(out.pos_dwords == (expected_pos - lift::kScriptHeaderLen) / 4u);
    LIFT_CHECK(out.header_len == 0x3Cu);  // ★ `engine_marker.h:53` 的编译期常量
    LIFT_CHECK(std::string(out.sha_why) == "none");
    LIFT_CHECK(out.sha == hexOf(body));

    // ★ 判据 3c 的机制：宿主文件多 4 字节 ⇒ sha 必须变（"sha 覆盖脚本体"）。
    body.push_back(0x22u);
    body.push_back(0x33u);
    body.push_back(0x44u);
    body.push_back(0x55u);
    writeBytes(script, body);
    lift::ControlScriptSample out2;
    lift::fillControlScriptSample(mem, sampler, vfs, &out2);
    std::printf("[3c] after +4 bytes: sha=%s changed=%d\n", out2.sha.c_str(),
                out2.sha != out.sha ? 1 : 0);
    LIFT_CHECK(out2.available);
    LIFT_CHECK(out2.sha == hexOf(body));
    LIFT_CHECK(out2.sha != out.sha);

    // ★ 解析不到 ⇒ sha 缺省 + 枚举理由（⛔ 静默空串）。
    writeBytes(script, body);
    fs::remove(script, ec);
    lift::ControlScriptSample out3;
    lift::fillControlScriptSample(mem, sampler, vfs, &out3);
    std::printf("[3c] script file removed: sha_why=%s sha=%s available=%d\n",
                out3.sha_why != nullptr ? out3.sha_why : "(null)", out3.sha.c_str(),
                out3.available ? 1 : 0);
    LIFT_CHECK(out3.available);  // 位置仍可读（那条路不依赖宿主文件）
    LIFT_CHECK(out3.sha.empty());
    LIFT_CHECK(std::string(out3.sha_why) == "unresolved");
    fs::remove_all(fs::path("control-snapshot-work"), ec);
}

// =====================================================================================
// ④ 快照 JSON 逐位稳定 + 没有墙钟 / 自增字段（判据 3b 的机制面）
// =====================================================================================
void caseJsonStable() {
    ::lift_test::g_current_case = "snapshot-json-stable";
    lift::ControlSnapshot s;
    s.paused = true;
    s.paused_at = "block";
    s.state_seq = 7;
    s.insns = 123456789ull;
    s.frame = 42;
    s.last_hash = 0xD6ED710894A39FA5ull;
    s.present = "hidden";
    s.guest_ms = 998877;
    s.paused_total_ms = 2000;
    s.script.available = true;
    s.script.name = "TITLE.BIN";
    s.script.script_id = 0;
    s.script.cur = 0;
    s.script.sha = std::string(64, 'a');
    s.script.sha_why = "none";
    s.script.pos = 0x39Cu;
    s.script.pos_dwords = 0xD8u;
    s.script.header_len = lift::kScriptHeaderLen;

    const std::string a = lift::controlSnapshotJson(s);
    const std::string b = lift::controlSnapshotJson(s);
    std::printf("[3b] serialize twice: len=%zu identical=%d\n", a.size(), a == b ? 1 : 0);
    std::printf("[3b] sample json: %s\n", a.c_str());
    LIFT_CHECK(a == b);  // ★ 同输入 ⇒ 逐字节同输出

    // ★ 判据 3b 的机制面：这些字段名**一个都不许出现**（它们会让"同一暂停点连采两次"不等）。
    const std::vector<std::string> forbidden = {
        "\"wall", "\"now", "\"elapsed", "\"since", "_ms_since", "\"timestamp", "\"seq_auto",
    };
    LIFT_CHECK(!hasAny(a, forbidden));

    // 关键字段必须在（否则"稳定"可能只是"空对象"）。
    for (const char* key : {"\"paused\"", "\"paused_at\"", "\"state_seq\"", "\"insns\"", "\"screen\"",
                            "\"frame\"", "\"last_hash\"", "\"clock\"", "\"guest_ms\"",
                            "\"paused_total_ms\"", "\"present\"", "\"script\"", "\"available\"",
                            "\"script_sha\"", "\"script_sha_why\"", "\"script_sha_source\"",
                            "\"pos\"", "\"pos_hex\"", "\"pos_dwords\"", "\"header_len\""}) {
        LIFT_CHECK(a.find(key) != std::string::npos);
    }
    LIFT_CHECK(a.find("\"pos_hex\":\"0x0000039C\"") != std::string::npos);

    // 只有 state_seq 变 ⇒ 文本必须不同（防"恒真"：上一条相同不是因为序列化器忽略输入）。
    lift::ControlSnapshot s2 = s;
    s2.state_seq = 8;
    LIFT_CHECK(lift::controlSnapshotJson(s2) != a);
}

// =====================================================================================
// ⑤ 截图是**同一帧**（判据 5 的机制面）：同一暂停点连拍两张**字节相同**；
//    `hash_still_matches` 的语义 = "现场重算 == 记录的 last_surface_hash_"，⛔ 与暂停无关。
// =====================================================================================
void caseDumpLastFrame() {
    ::lift_test::g_current_case = "screenshot-same-frame";
    namespace fs = std::filesystem;
    const fs::path dir = fs::path("control-snapshot-work") / "shots";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    LIFT_CHECK(fs::is_directory(dir, ec));

    lift::GuestMemory mem;
    LIFT_CHECK(mem.reserve());
    lift::SurfaceArena arena;
    LIFT_CHECK(arena.init(&mem, 0x10000000u, 0x00100000u));
    lift::SurfaceFormat fmt;
    fmt.bpp = 32;
    fmt.rmask = 0x00FF0000u;
    fmt.gmask = 0x0000FF00u;
    fmt.bmask = 0x000000FFu;
    fmt.amask = 0xFF000000u;
    const std::uint32_t ea = arena.create(8, 6, fmt);
    lift::GuestSurface* surface = arena.find(ea);
    LIFT_CHECK(ea != 0 && surface != nullptr);
    if (surface == nullptr) return;
    auto* px = arena.pixels(*surface);
    LIFT_CHECK(px != nullptr);
    if (px == nullptr) return;
    for (int i = 0; i < 8 * 6; ++i) px[i] = 0xFF204060u;  // ARGB 小端 = B,G,R,A

    lift::HeadlessPresenter presenter;
    std::string err;
    LIFT_CHECK(presenter.present(arena, *surface, std::string(), &err));
    LIFT_CHECK(presenter.frameCount() == 1);
    LIFT_CHECK(presenter.hasLastFrame());

    const fs::path a_path = dir / "a.bmp";
    const fs::path b_path = dir / "b.bmp";
    bool hash_a = false;
    bool hash_b = false;
    LIFT_CHECK(presenter.dumpLastFrame(a_path.string(), &hash_a, &err));
    LIFT_CHECK(presenter.dumpLastFrame(b_path.string(), &hash_b, &err));
    const std::string a = readWholeFile2(a_path.string());
    const std::string b = readWholeFile2(b_path.string());
    std::printf("[judge5] dump twice: bytes a=%zu b=%zu identical=%d hash_still_matches a=%d b=%d source=%s\n",
                a.size(), b.size(), a == b ? 1 : 0, hash_a ? 1 : 0, hash_b ? 1 : 0,
                "guest-backbuffer");
    LIFT_CHECK(!a.empty());
    LIFT_CHECK(a == b);        // ★ 同一暂停点连拍两张字节相同
    LIFT_CHECK(hash_a);        // ★ 像素仍是被采样到的那一帧
    LIFT_CHECK(hash_b);

    // ★ 语义对照：guest 在**同一块后缓冲**上画了下一帧 ⇒ 现场重算 != 记录的 hash ⇒ false。
    px[0] = 0xFF00FF00u;
    bool hash_c = true;
    LIFT_CHECK(presenter.dumpLastFrame((dir / "c.bmp").string(), &hash_c, &err));
    std::printf("[judge5] after guest drew next frame: hash_still_matches=%d\n", hash_c ? 1 : 0);
    LIFT_CHECK(!hash_c);  // ⛔ 不是"恒 true"的装饰字段
    fs::remove_all(fs::path("control-snapshot-work"), ec);
}

// =====================================================================================
// ⑬ 控制层 resume 的陈旧事件闸门（判据 13 的**纯**机制面；真 SDL 队列在 tests/present_window.cpp）
// =====================================================================================
void caseClassifyStale() {
    ::lift_test::g_current_case = "stale-event-classify";
    using lift::HostEventClass;
    using lift::StaleEventAction;
    constexpr std::uint64_t kResume = 10'000'000'000ull;  // 10 s（ns）

    // 恢复时刻之后的事件 ⇒ 一律照旧（不能把"新"输入误丢）。
    for (HostEventClass c : {HostEventClass::Other, HostEventClass::OsInput, HostEventClass::CloseRequest,
                             HostEventClass::EscapeKey, HostEventClass::AppQuit}) {
        LIFT_CHECK(lift::classifyStaleEvent(c, kResume, kResume) == StaleEventAction::Keep);
        LIFT_CHECK(lift::classifyStaleEvent(c, kResume + 1, kResume) == StaleEventAction::Keep);
    }
    // 暂停期间的 **OS 输入** ⇒ 丢弃（⛔ 压进 guest 队列）。
    LIFT_CHECK(lift::classifyStaleEvent(HostEventClass::OsInput, kResume - 1, kResume) ==
               StaleEventAction::Drop);
    // 暂停期间的 **关窗 / ESC / QUIT** ⇒ 宿主 stop（⛔ 投 WM_CLOSE 进 guest）。
    LIFT_CHECK(lift::classifyStaleEvent(HostEventClass::CloseRequest, kResume - 1, kResume) ==
               StaleEventAction::HostStop);
    LIFT_CHECK(lift::classifyStaleEvent(HostEventClass::EscapeKey, kResume - 1, kResume) ==
               StaleEventAction::HostStop);
    LIFT_CHECK(lift::classifyStaleEvent(HostEventClass::AppQuit, kResume - 1, kResume) ==
               StaleEventAction::HostStop);
    // 非输入类（resize/expose…）不受影响；没启用 / 没时间戳 ⇒ 不猜。
    LIFT_CHECK(lift::classifyStaleEvent(HostEventClass::Other, kResume - 1, kResume) ==
               StaleEventAction::Keep);
    LIFT_CHECK(lift::classifyStaleEvent(HostEventClass::OsInput, kResume - 1, 0) == StaleEventAction::Keep);
    LIFT_CHECK(lift::classifyStaleEvent(HostEventClass::OsInput, 0, kResume) == StaleEventAction::Keep);
    std::printf("[judge13-pure] stale classify: drop-os-input / host-stop-close+esc+quit / keep-new\n");
}

}  // namespace

int main() {
    caseUnavailableIsEnum();
    casePositionAndSha();
    caseJsonStable();
    caseDumpLastFrame();
    caseClassifyStale();
    return lift_test::finish("lift-control-snapshot");
}
