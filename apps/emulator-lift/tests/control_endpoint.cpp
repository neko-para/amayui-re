// emulator-lift · **控制层端点**（S0 传输 + S1 闸门/时钟）的 pure 守卫（顶层无条件注册）
//
// 判据（红了意味着什么）：
//   ① **监听失败要响亮**：同一个端口 bind 两次 ⇒ 第二次 `start()` 返回 false，且 err **点名端口**
//      （没有静默换端口 / 没有降级成"没有控制层"）。
//   ② **只绑 loopback**：`localEndpoint()` 的地址是 `127.0.0.1`（⛔ 不用 lsof / netstat —— 那是
//      平台工具，不是这个进程的事实）。
//   ③ **未授权被拒**：错 `token` ⇒ 握手失败（拿不到 101）；带 `Origin` 头的握手 ⇒ 被拒。
//   ④ **身份只有一个来源**：`hello` 的 `schema` / `pid` / `nonce` / `port` 与
//      `<root>/ctl.<pid>.<nonce8>.json` 里的逐字段一致（两个来源各写一份 ⇒ 红）。
//   ⑤ **协议形状**：未知 `op` ⇒ `unknown_op`（⛔ 不静默）；**每条 `res` 的 `nonce` == `hello.nonce`**；
//      `id` 原样回显。`capabilities` 只列真的实现了的（S2/S3 之后含 `sample` / `screenshot`）。
//   ⑧ **`hello.present` 在顶层**且 ∈ {visible,hidden}（判据 2 的父级裁决；⛔ 不在 `control` 里）。
//   ⑨ **`pause` 幂等**（判据：已暂停态再 `pause` ⇒ `ok:true`，`state_seq` 不动）。
//   ⑩ **`sample` 读不到时是枚举**（判据 3d）：`script.available:false` + `why` ∈ 既有 8 个字面量，
//      且 `res.ok:true`（数据缺失不是错误）；同一暂停点连采两次**逐字节相同**（判据 3b 的机制面）。
//   ⑪ **`resume --for frames`**：谓词成立 ⇒ 重停（`state_seq` +2：离开 + 进入）；等不到 ⇒
//      `error.code:"timeout"` **并取消谓词**（之后的 `resume` 是 `not_paused`，且不会"自己停住"）。
//   ⑥ **文件命名与 gc 范围**：端点文件名 = `ctl.<pid>.<nonce8>.json`；正常退出**只删自己那一个**
//      （同目录手放的 `keep.txt` 与别的实例的文件都不许被碰 —— 判据 9b 的机制面）。
//   ⑦ **暂停对 guest 不可见（判据 6）**：`pause` → 真等 2.1 s → `resume` ⇒ `guest_ms` 差 < 5 ms、
//      `paused_total_ms` 增 ≈ 2000。
//      ★ 2026-11-27（用户裁决 `REQ-01M875NDRETPPNN68VCE3VMCT1`）：旧 ⑦ 的第二半（"配方的每步守卫钟
//        同冻"）与 ⑫ 的"配方 click vs 立即队列 click 逐位相同"都是 `InputPlan` 面 ⇒ 随 `--input-plan`
//        退役**已删**（子用例 `click-enqueue-at-read-point` / `click-coordinate-map` 整条删，
//        `clock-freeze+plan-guard` 只留 guest 时钟那一半 = `clock-freeze`）。控制层立即队列本身
//        住在 `InputPlan` 里，产品侧本批**不动** ⇒ 那三条判据等下一批把它挪到 `IntentQueue` /
//        `WindowManager` 时再重建。
//   ⑫ **M1 动态投递 `click`**：未暂停 ⇒ `not_paused`（且**不入队**）；坐标非法 ⇒ `bad_request`；
//      队列**有界**（第 `kImmediateClickCapacity+1` 条 `bad_request`，前 N 条不丢）；
//      **判据 2**：入队 + 到达下一个读点 ⇒ click **生效**（fired +1 / `WM_LBUTTONDOWN` / 光标）
//      但配方的 `next_` / `pending` / 当前门 **一位不动**；
//      **判据 1**：配方 click 与立即队列 click 投出的消息与坐标**逐位相同**（同一段 apply）；
//      **判据 4**：缩放客户区下只有一个映射公式（1280×720 恒等 / 1920×1080 按同一公式；回推参考系同一点）。
//   ⑬ **`resume` 真的通知宿主**（`REQ-01M7JFDZK1SZW72SBAW6BC3DRT`）：paused → running 的**每一次**转移各 arm 一次
//      "恢复时刻"（`controlOnResume`）；`pause` / `sample` / 运行态的 `resume`（`not_paused`）都不 arm。
//      ⇒ 生产路径上陈旧事件闸门不是死代码。⚠ `present-window` 的陈旧事件用例直接调 sink 的
//      `noteControlResume()`，它证明的是**纯映射 + sink 一侧**，证明不了这条接线存在 —— 两层都要有。
//
// ★ 传输侧的 WS 客户端是**本文件自带的裸实现**（upgrade 请求 + `Sec-WebSocket-Accept` 校验 +
//   掩码帧），不引任何新依赖；⛔ **不 spawn 子进程**（`AGENTS.md` §5.1：捕获子进程输出 = EPERM）。
// ★ 不需要 GPU / 不需要真游戏 / 不碰 LFS：端点 + 注入时钟 + 一个空 `WindowManager` 就够。
// ★ 跨平台：socket 的 POSIX / Winsock 两支都**实现**（⛔ 不用 `#if defined(_WIN32)` 跳过用例）。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "check.h"
#include "runtime/clock.h"
#include "runtime/control.h"
#include "runtime/control_snapshot.h"
#include "runtime/engine_marker.h"
#include "runtime/host_window.h"
#include "runtime/memory.h"
#include "runtime/vfs.h"

namespace {

// =====================================================================================
// 平台薄层：socket 的两支都实现（⛔ 不跳过任何一端）
// =====================================================================================
#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
struct WinsockInit {
    WinsockInit() {
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WinsockInit() { WSACleanup(); }
};
const WinsockInit g_winsock;
int sockSend(socket_t s, const char* d, std::size_t n) {
    return ::send(s, d, static_cast<int>(n), 0);
}
int sockRecv(socket_t s, char* d, std::size_t n) { return ::recv(s, d, static_cast<int>(n), 0); }
void sockClose(socket_t s) { ::closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
int sockSend(socket_t s, const char* d, std::size_t n) {
    return static_cast<int>(::send(s, d, n, 0));
}
int sockRecv(socket_t s, char* d, std::size_t n) { return static_cast<int>(::recv(s, d, n, 0)); }
void sockClose(socket_t s) { ::close(s); }
#endif

constexpr const char* kCtlSchema = "emu-ctl/2";
// RFC 6455 §1.3 的公开测试向量：key ⇒ accept（⛔ 不为了算它在本文件里塞一份 SHA-1）。
constexpr const char* kWsKey = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr const char* kWsAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool sendAll(socket_t s, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const int n = sockSend(s, data.data() + off, data.size() - off);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// =====================================================================================
// 裸 WS 客户端（upgrade + 帧解析 + 掩码发送）
// =====================================================================================
struct WsClient {
    socket_t fd = kInvalidSocket;
    std::string rx;  // 已经 recv 到但还没被帧解析消费的字节
    std::string accept;  // 握手响应里的 Sec-WebSocket-Accept 小写值
    int status = 0;      // 握手响应的状态码

    ~WsClient() { close(); }
    void close() {
        if (fd != kInvalidSocket) {
            sockClose(fd);
            fd = kInvalidSocket;
        }
    }

    bool connectTo(std::uint16_t port) {
        fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == kInvalidSocket) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr) != 1) return false;
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            close();
            return false;
        }
#ifdef _WIN32
        DWORD tv = 4000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_sec = 4;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        return true;
    }

    // 读完 HTTP 头（到 \r\n\r\n），解析状态码 + Sec-WebSocket-Accept。返回 101 才算握手成功。
    bool handshake(const std::string& token, bool with_origin) {
        std::string req = "GET /?token=" + token + " HTTP/1.1\r\n";
        req += "Host: 127.0.0.1\r\n";
        req += "Upgrade: websocket\r\n";
        req += "Connection: Upgrade\r\n";
        req += std::string("Sec-WebSocket-Key: ") + kWsKey + "\r\n";
        req += "Sec-WebSocket-Version: 13\r\n";
        if (with_origin) req += "Origin: http://evil.example\r\n";
        req += "\r\n";
        if (!sendAll(fd, req)) return false;
        // 读到 header 结束
        for (;;) {
            const std::size_t end = rx.find("\r\n\r\n");
            if (end != std::string::npos) {
                const std::string head = rx.substr(0, end);
                rx.erase(0, end + 4);
                const std::size_t sp = head.find(' ');
                if (sp != std::string::npos && head.size() >= sp + 4) {
                    status = std::atoi(head.c_str() + sp + 1);
                }
                const std::string low = lower(head);
                const std::size_t a = low.find("sec-websocket-accept:");
                if (a != std::string::npos) {
                    std::size_t vb = a + std::strlen("sec-websocket-accept:");
                    while (vb < low.size() && (low[vb] == ' ' || low[vb] == '\t')) ++vb;
                    const std::size_t ve = low.find("\r\n", vb);
                    accept = low.substr(vb, ve == std::string::npos ? std::string::npos : ve - vb);
                }
                return status == 101 && accept == lower(kWsAccept);
            }
            char buf[2048];
            const int n = sockRecv(fd, buf, sizeof(buf));
            if (n <= 0) return false;
            rx.append(buf, static_cast<std::size_t>(n));
        }
    }

    bool fill(std::size_t need) {
        while (rx.size() < need) {
            char buf[4096];
            const int n = sockRecv(fd, buf, sizeof(buf));
            if (n <= 0) return false;
            rx.append(buf, static_cast<std::size_t>(n));
        }
        return true;
    }

    // 读一帧（服务端帧不掩码；带掩码也支持 —— 容错，不靠"服务端一定不掩码"的假设）。
    bool readFrame(int* opcode, std::string* payload) {
        if (!fill(2)) return false;
        const unsigned b0 = static_cast<unsigned char>(rx[0]);
        const unsigned b1 = static_cast<unsigned char>(rx[1]);
        const bool masked = (b1 & 0x80u) != 0;
        std::uint64_t len = b1 & 0x7Fu;
        std::size_t off = 2;
        if (len == 126) {
            if (!fill(4)) return false;
            len = (static_cast<std::uint64_t>(static_cast<unsigned char>(rx[2])) << 8) |
                  static_cast<unsigned char>(rx[3]);
            off = 4;
        } else if (len == 127) {
            if (!fill(10)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) {
                len = (len << 8) | static_cast<unsigned char>(rx[2 + static_cast<std::size_t>(i)]);
            }
            off = 10;
        }
        if (len > (1u << 20)) return false;  // 本用例的帧都远小于此
        std::size_t mask_off = off;
        if (masked) off += 4;
        if (!fill(off + static_cast<std::size_t>(len))) return false;
        std::string out = rx.substr(off, static_cast<std::size_t>(len));
        if (masked) {
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i] = static_cast<char>(out[i] ^ rx[mask_off + (i % 4)]);
            }
        }
        rx.erase(0, off + static_cast<std::size_t>(len));
        *opcode = static_cast<int>(b0 & 0x0Fu);
        *payload = std::move(out);
        return true;
    }

    // 发一帧（客户端帧**必须**掩码）。
    bool writeFrame(int opcode, const std::string& payload) {
        std::string f;
        f.push_back(static_cast<char>(0x80 | opcode));
        const std::size_t n = payload.size();
        if (n < 126) {
            f.push_back(static_cast<char>(0x80u | n));
        } else if (n <= 0xFFFF) {
            f.push_back(static_cast<char>(0x80u | 126u));
            f.push_back(static_cast<char>((n >> 8) & 0xFFu));
            f.push_back(static_cast<char>(n & 0xFFu));
        } else {
            f.push_back(static_cast<char>(0x80u | 127u));
            for (int i = 7; i >= 0; --i) {
                f.push_back(static_cast<char>((static_cast<std::uint64_t>(n) >> (8 * i)) & 0xFFu));
            }
        }
        const char mask[4] = {0x37, static_cast<char>(0xfa), 0x21, 0x3d};
        f.append(mask, 4);
        for (std::size_t i = 0; i < payload.size(); ++i) {
            f.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
        }
        return sendAll(fd, f);
    }

    bool readText(std::string* out) {
        for (;;) {
            int op = 0;
            std::string payload;
            if (!readFrame(&op, &payload)) return false;
            if (op == 1) {
                *out = std::move(payload);
                return true;
            }
            if (op == 8) return false;  // close
            if (op == 9) {  // ping ⇒ 回 pong（本端点本轮不发 ping，但客户端按 RFC 回）
                if (!writeFrame(10, payload)) return false;
            }
        }
    }
};

// =====================================================================================
// 迷你 JSON 字段抽取（⛔ 不为测试再引一个 JSON 库；文档是我们自己写的固定形状）
// =====================================================================================
std::string jsonString(const std::string& doc, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    std::size_t p = doc.find(pat);
    if (p == std::string::npos) return std::string();
    p = doc.find(':', p + pat.size());
    if (p == std::string::npos) return std::string();
    p = doc.find('"', p + 1);
    if (p == std::string::npos) return std::string();
    const std::size_t e = doc.find('"', p + 1);
    if (e == std::string::npos) return std::string();
    return doc.substr(p + 1, e - p - 1);
}

// 取一个**嵌套对象**的原文（`"key":{...}`，花括号配对）。⛔ 只为测试里"同名键在不同层"消歧
// （`screen.frame` 与 `screenshot.frame` 都叫 frame；`present` 在 hello 顶层 / sample 里各有一份）。
std::string jsonObject(const std::string& doc, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    std::size_t p = doc.find(pat);
    if (p == std::string::npos) return std::string();
    p = doc.find('{', p + pat.size());
    if (p == std::string::npos) return std::string();
    int depth = 0;
    for (std::size_t i = p; i < doc.size(); ++i) {
        if (doc[i] == '{') {
            ++depth;
        } else if (doc[i] == '}') {
            if (--depth == 0) return doc.substr(p, i - p + 1);
        }
    }
    return std::string();
}

std::uint64_t jsonUint(const std::string& doc, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    std::size_t p = doc.find(pat);
    if (p == std::string::npos) return 0;
    p = doc.find(':', p + pat.size());
    if (p == std::string::npos) return 0;
    ++p;
    while (p < doc.size() && (doc[p] == ' ' || doc[p] == '\t')) ++p;
    std::uint64_t v = 0;
    bool any = false;
    while (p < doc.size() && doc[p] >= '0' && doc[p] <= '9') {
        v = v * 10 + static_cast<std::uint64_t>(doc[p] - '0');
        ++p;
        any = true;
    }
    return any ? v : 0;
}

std::string readWholeFile(const std::string& path) {
    std::string out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// =====================================================================================
// 注入时钟（判据 6 的确定性读数：暂停 2 s 不需要真睡）
// =====================================================================================
std::uint64_t g_raw_100ns = 0;   // `Clock` 的原始墙钟读数
std::uint64_t fakeRawNow() { return g_raw_100ns; }

// 判据 6：guest 时钟冻结（暂停区间对 guest 不可见）。
void caseClockFreeze() {
    ::lift_test::g_current_case = "clock-freeze";

    // ---- ① guest 时钟：冻结区间对 guest 不可见 ----
    lift::Clock clk;
    clk.setRawNowForTest(&fakeRawNow);
    g_raw_100ns = 10'000'000;  // 随便一个起点（100ns 单位 = 1 s）
    const std::uint32_t before_ms = clk.nowMs();
    const std::uint64_t before_100ns = clk.now100ns();

    clk.freeze();
    g_raw_100ns += 20'000'000;  // 2 s（2e7 × 100ns）
    const std::uint32_t during_ms = clk.nowMs();
    const std::uint64_t during_100ns = clk.now100ns();
    const std::uint64_t paused_ms_during = clk.pausedTotalMs();
    clk.unfreeze();
    const std::uint64_t paused_ms_after = clk.pausedTotalMs();
    g_raw_100ns += 50'000;  // 解冻后再走 5 ms
    const std::uint32_t after_ms = clk.nowMs();

    std::printf(
        "[judge6] guest_ms: before=%u during=%u after=%u | now100ns delta during=%lld | "
        "paused_total_ms during=%llu after=%llu\n",
        before_ms, during_ms, after_ms,
        static_cast<long long>(during_100ns) - static_cast<long long>(before_100ns),
        static_cast<unsigned long long>(paused_ms_during),
        static_cast<unsigned long long>(paused_ms_after));

    LIFT_CHECK(during_100ns == before_100ns);              // 冻结期间 guest 时间**一位都不前进**
    LIFT_CHECK(during_ms == before_ms);
    LIFT_CHECK(paused_ms_during >= 2000 && paused_ms_during <= 2001);
    LIFT_CHECK(paused_ms_after >= 2000 && paused_ms_after <= 2001);
    LIFT_CHECK(after_ms >= before_ms + 5 && after_ms <= before_ms + 6);  // 解冻后照常前进
}

// =====================================================================================
// 主线程取数口的**替身**：只走真协议 + 真 `fillControlScriptSample`（空内存 ⇒ 身份读不到）。
//   ★ 这里**不**伪造 script 块：判据 3d 的 `why` 必须来自 engine_marker 的真路径。
//   ★ 截图只造一个 7 字节文件（BMP 编码在 present 侧；`dumpLastFrame` 的字节一致性由
//     `lift-control-snapshot` 的 BMP 用例测）。
// =====================================================================================
struct FakeHostSide final : lift::ControlHostSide {
    lift::GuestMemory mem;  // 空（没 reserve）⇒ 任何 guest 读都失败 = "boot 之前"
    lift::Vfs vfs;
    lift::MarkerSampler sampler;
    std::atomic<std::uint64_t> frame{0};

    void fillControlSnapshot(lift::ControlSnapshot& out) override {
        out = lift::ControlSnapshot{};
        out.insns = 1000;
        out.frame = frame.load();
        out.last_hash = 0x1234567890ABCDEFull;
        out.present = "hidden";
        lift::fillControlScriptSample(mem, sampler, vfs, &out.script);
    }
    lift::ControlShotStatus controlScreenshot(const std::string& path, lift::ControlScreenshot& out,
                                              std::string* err) override {
        // ★ 与真宿主同一条三态契约：一次 Present 都没有 ⇒ `NoFrameYet`（数据没到，⛔ 不是 io）。
        if (frame.load() == 0) {
            if (err != nullptr) *err = "no frame presented yet";
            return lift::ControlShotStatus::NoFrameYet;
        }
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr) {
            if (err != nullptr) *err = "替身写文件失败：" + path;
            return lift::ControlShotStatus::IoFailed;
        }
        std::fputs("BMP-ish", f);
        std::fclose(f);
        out.path = path;
        out.bytes = 7;
        out.frame = frame.load();
        out.hash_still_matches = true;
        out.source = "guest-backbuffer";
        return lift::ControlShotStatus::Ok;
    }
    std::uint64_t controlPresentFrame() const override { return frame.load(); }
    // ★ `resume` 的宿主即时动作计数器（`REQ-01M7JFDZK1SZW72SBAW6BC3DRT` 的判据面）：
    //   生产实现里这一步是"把恢复时刻记到 sink 上"（`AotHost::controlOnResume` → `presenter_` → `sink_`），
    //   替身只数**被调到了几次** —— 缺陷的形状不是"记错了时刻"，是"**一次都没被调**"。
    std::atomic<unsigned> resume_noted{0};
    void controlOnResume() override { resume_noted.fetch_add(1, std::memory_order_relaxed); }
};

// =====================================================================================
// 端点：生命周期 / 身份 / 安全 / 协议 / 文件面
// =====================================================================================
void caseEndpointSurface() {
    ::lift_test::g_current_case = "endpoint-surface";

    namespace fs = std::filesystem;
    const fs::path root = fs::path("control-endpoint-work") / "ctl";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(fs::is_directory(root, ec));

    // ⑥ 的机制面：同目录手放一个**不属于端点**的文件，任何收口 / gc 都不许碰它。
    const fs::path keep = root / "keep.txt";
    {
        std::FILE* f = std::fopen(keep.string().c_str(), "wb");
        LIFT_CHECK(f != nullptr);
        if (f != nullptr) {
            std::fputs("not an endpoint file\n", f);
            std::fclose(f);
        }
    }

    lift::Control a;
    lift::Control::Options opts;
    opts.root = root.string();
    opts.port = 0;
    opts.idle_timeout_s = 300.0;
    opts.orphan_timeout_s = 10.0;
    opts.predicate_timeout_s = 0.5;  // 等不到的谓词要把 CLI 从挂死里救出来（判据 4 的 timeout 面）
    opts.session = "sess-A";
    opts.exe_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    opts.shape = "test-shape";
    opts.present = "hidden";
    std::string err;
    LIFT_CHECK(a.start(opts, &err));
    if (!a.started()) {
        std::fprintf(stderr, "start err: %s\n", err.c_str());
        return;
    }
    std::printf("[endpoint] local=%s file=%s\n", a.localEndpoint().c_str(),
                a.controlFilePath().c_str());
    const std::uint16_t port = a.localPort();
    LIFT_CHECK(port != 0);

    // ② 只绑 loopback（判据 10 前半；⛔ 不用 lsof）
    LIFT_CHECK(a.localEndpoint().compare(0, 10, "127.0.0.1:") == 0);
    LIFT_CHECK(a.localEndpoint() == "127.0.0.1:" + std::to_string(port));

    // ① bind 两次同一端口 ⇒ 第二次失败并**点名端口**（判据 12 的形态）
    {
        lift::Control b;
        lift::Control::Options ob = opts;
        ob.port = port;
        ob.session = "sess-B";
        std::string berr;
        const bool ok = b.start(ob, &berr);
        std::printf("[endpoint] second bind on %u ⇒ ok=%d err=%s\n", static_cast<unsigned>(port),
                    ok ? 1 : 0, berr.c_str());
        LIFT_CHECK(!ok);
        LIFT_CHECK(berr.find("127.0.0.1:" + std::to_string(port)) != std::string::npos);
    }

    // ⑥ 文件命名：`ctl.<pid>.<nonce8>.json`，内容与 hello 同源
    const fs::path ctl_path = a.controlFilePath();
    const std::string ctl_name = ctl_path.filename().string();
    LIFT_CHECK(ctl_name.rfind("ctl.", 0) == 0);
    LIFT_CHECK(ctl_name.size() > 5 && ctl_name.compare(ctl_name.size() - 5, 5, ".json") == 0);
    LIFT_CHECK(fs::exists(ctl_path));
    const std::string ctl_doc = readWholeFile(ctl_path.string());
    LIFT_CHECK(!ctl_doc.empty());
    const std::string file_nonce = jsonString(ctl_doc, "nonce");
    const std::string file_token = jsonString(ctl_doc, "token");
    const std::uint64_t file_pid = jsonUint(ctl_doc, "pid");
    const std::uint64_t file_port = jsonUint(ctl_doc, "port");
    LIFT_CHECK(file_nonce.size() == 16);
    LIFT_CHECK(!file_token.empty());
    LIFT_CHECK(file_port == port);
    LIFT_CHECK(jsonString(ctl_doc, "schema") == kCtlSchema);
    LIFT_CHECK(jsonString(ctl_doc, "session") == "sess-A");
    LIFT_CHECK(jsonString(ctl_doc, "shape") == "test-shape");
    // 文件名里的 nonce8 = nonce 前 8 hex；pid 与文件内容一致
    LIFT_CHECK(ctl_name.find("." + std::to_string(file_pid) + "." + file_nonce.substr(0, 8) + ".json") !=
               std::string::npos);

    // 同 root 再起一个（`--control-port 0`）⇒ 成功、新端口、新文件（判据 9b 的"不打架"面）
    lift::Control c;
    {
        lift::Control::Options oc = opts;
        oc.port = 0;
        oc.session = "sess-C";
        std::string cerr;
        const bool ok = c.start(oc, &cerr);
        LIFT_CHECK(ok);
        if (ok) {
            LIFT_CHECK(c.localPort() != port);
            LIFT_CHECK(c.controlFilePath() != a.controlFilePath());
            LIFT_CHECK(fs::exists(c.controlFilePath()));
        }
    }

    // ③ 错 token ⇒ 握手失败；带 Origin ⇒ 拒
    {
        WsClient wrong;
        LIFT_CHECK(wrong.connectTo(port));
        const bool hs = wrong.handshake("00000000000000000000000000000000", false);
        std::printf("[endpoint] wrong-token handshake ok=%d status=%d\n", hs ? 1 : 0, wrong.status);
        LIFT_CHECK(!hs);
        LIFT_CHECK(wrong.status == 401);
    }
    {
        WsClient origin;
        LIFT_CHECK(origin.connectTo(port));
        const bool hs = origin.handshake(file_token, true);
        std::printf("[endpoint] origin handshake ok=%d status=%d\n", hs ? 1 : 0, origin.status);
        LIFT_CHECK(!hs);
        LIFT_CHECK(origin.status == 403);
    }

    // 正常握手：服务端先发 hello
    WsClient ws;
    LIFT_CHECK(ws.connectTo(port));
    LIFT_CHECK(ws.handshake(file_token, false));
    std::string hello;
    LIFT_CHECK(ws.readText(&hello));
    std::printf("[endpoint] hello=%s\n", hello.c_str());
    LIFT_CHECK(jsonString(hello, "schema") == kCtlSchema);
    const std::string hello_nonce = jsonString(hello, "nonce");
    LIFT_CHECK(hello_nonce == file_nonce);                     // ④ 两个来源一致
    LIFT_CHECK(jsonUint(hello, "pid") == file_pid);            // ④
    LIFT_CHECK(jsonUint(hello, "port") == file_port);          // ④
    LIFT_CHECK(jsonUint(hello, "port") == a.localPort());
    LIFT_CHECK(jsonString(hello, "session") == "sess-A");
    // ---- 判据 2 的父级裁决：`hello.present` 在**顶层**且 ∈ {visible,hidden}（⛔ 不在 `control` 里）----
    const std::string hello_present = jsonString(hello, "present");
    std::printf("[judge2] hello.present(top-level)=%s\n", hello_present.c_str());
    LIFT_CHECK(hello_present == "visible" || hello_present == "hidden");
    const std::string hello_control = jsonObject(hello, "control");
    LIFT_CHECK(!hello_control.empty());
    LIFT_CHECK(hello_control.find("\"present\"") == std::string::npos);  // 从 control 挪走了
    // `capabilities` 只列真的实现了的（S2/S3 之后含 `sample` / `screenshot`；M1 之后含 `click`；
    // 2026-11-19 起含 `move`；typed global 之后含 `global-get` / `global-set`）
    LIFT_CHECK(hello.find(
                   "\"capabilities\":[\"pause\",\"resume\",\"sample\",\"screenshot\",\"move\",\"click\","
                   "\"global-get\",\"global-set\",\"quit\"]") != std::string::npos);

    // ⑤ 未知 op ⇒ unknown_op（`sample` 本轮没实现 ⇒ 它必须被拒）；每条 res 的 nonce 回显
    struct Res {
        std::string text;
        std::uint64_t id = 0;
        bool ok = false;
        std::string code;
        std::string nonce;
    };
    auto roundTrip = [&](const std::string& req, Res* out) -> bool {
        if (!ws.writeFrame(1, req)) return false;
        if (!ws.readText(&out->text)) return false;
        out->id = jsonUint(out->text, "id");
        out->ok = out->text.find("\"ok\":true") != std::string::npos;
        out->code = jsonString(out->text, "code");
        out->nonce = jsonString(out->text, "nonce");
        return true;
    };
    {
        // `key` 是 M1 裁决**不做**的（用户口径："click 够用即可"）⇒ 必须 `unknown_op`（不静默）。
        // ★ 2026-11-27：`click` 的落地面（立即队列 / 坐标映射）住在 `InputPlan` 里，那两条子用例随
        //   `--input-plan` 退役已删 ⇒ 这里只钉"未知 op 不静默"，⛔ 不再用它冒充 click 的判据。
        Res r;
        LIFT_CHECK(roundTrip("{\"id\":7,\"op\":\"key\",\"vk\":65}", &r));
        std::printf("[endpoint] unknown-op res=%s\n", r.text.c_str());
        LIFT_CHECK(!r.ok);
        LIFT_CHECK(r.code == "unknown_op");
        LIFT_CHECK(r.id == 7);
        LIFT_CHECK(r.nonce == hello_nonce);  // ⑤ 每条 res 回显 hello.nonce
    }
    {
        Res r;
        LIFT_CHECK(roundTrip("{\"id\":8,\"op\":\"pause\"", &r));  // 坏 JSON
        LIFT_CHECK(!r.ok);
        LIFT_CHECK(r.code == "bad_request");
        LIFT_CHECK(r.nonce == hello_nonce);
    }

    // ⑦ pause → 真等 2.1 s → resume：guest_ms 差 < 5 ms、paused_total_ms 增 ≈ 2000
    //   主线程 = 本用例的 gate 线程（与 AOT 的块边界汇点同形：`pending()` / `watching()` ⇒ `serve()`）。
    FakeHostSide fake;
    std::atomic<bool> gate_quit{false};
    std::atomic<bool> gate_done{false};
    bool stop_flag = false;
    std::thread gate([&] {
        lift::WindowManager wm;   // 主线程独占对象（本线程 = 汇点线程）
        lift::Clock clk;
        lift::Control::Context ctx;
        ctx.clock = &clk;
        ctx.windows = &wm;
        ctx.stop_requested = &stop_flag;
        ctx.paused_at = "block";
        ctx.host = &fake;
        while (!gate_quit.load()) {
            if (a.pending() || a.watching()) {
                if (a.serve(ctx)) {
                    gate_quit.store(true);
                    break;
                }
                // 谓词轮询（`KeepRunning` 时 serve 立刻返回）⇒ 别忙等。
                if (a.watching()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        gate_done.store(true);
    });

    // ⑨ `pause` 幂等（父级裁决：已暂停态再 `pause` ⇒ ok:true，⛔ bad_request）
    Res r_pause;
    LIFT_CHECK(roundTrip("{\"id\":9,\"op\":\"pause\"}", &r_pause));
    std::printf("[endpoint] pause res=%s\n", r_pause.text.c_str());
    LIFT_CHECK(r_pause.ok);
    const std::uint64_t state1 = jsonUint(r_pause.text, "state_seq");
    const std::uint64_t guest1 = jsonUint(r_pause.text, "guest_ms");
    LIFT_CHECK(jsonString(r_pause.text, "paused_at") == "block");
    LIFT_CHECK(r_pause.text.find("\"paused\":true") != std::string::npos);
    {
        Res again;
        LIFT_CHECK(roundTrip("{\"id\":10,\"op\":\"pause\"}", &again));
        std::printf("[pause-idempotent] second pause res=%s\n", again.text.c_str());
        LIFT_CHECK(again.ok);
        LIFT_CHECK(again.text.find("\"paused\":true") != std::string::npos);
        LIFT_CHECK(jsonUint(again.text, "state_seq") == state1);  // 没有状态转移
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2100));

    Res r_resume;
    LIFT_CHECK(roundTrip("{\"id\":11,\"op\":\"resume\"}", &r_resume));
    std::printf("[endpoint] resume res=%s\n", r_resume.text.c_str());
    LIFT_CHECK(r_resume.ok);
    const std::uint64_t state2 = jsonUint(r_resume.text, "state_seq");
    const std::uint64_t guest2 = jsonUint(r_resume.text, "guest_ms");
    const std::uint64_t paused_total = jsonUint(r_resume.text, "paused_total_ms");
    const long long delta = static_cast<long long>(guest2) - static_cast<long long>(guest1);
    std::printf(
        "[judge6-endpoint] pause 2100 ms: guest_ms %llu → %llu (delta=%lld ms) paused_total_ms=%llu "
        "state_seq %llu → %llu\n",
        static_cast<unsigned long long>(guest1), static_cast<unsigned long long>(guest2), delta,
        static_cast<unsigned long long>(paused_total), static_cast<unsigned long long>(state1),
        static_cast<unsigned long long>(state2));
    LIFT_CHECK(delta >= 0 && delta < 5);                    // ★ 暂停对 guest 不可见
    LIFT_CHECK(paused_total >= 2000 && paused_total <= 2600);
    LIFT_CHECK(state2 == state1 + 1);                      // ★ 每次离开暂停态严格 +1
    LIFT_CHECK(r_resume.text.find("\"paused\":false") != std::string::npos);

    // `resume` 在运行态 ⇒ not_paused（且 state_seq 不动）
    {
        Res r;
        LIFT_CHECK(roundTrip("{\"id\":12,\"op\":\"resume\"}", &r));
        LIFT_CHECK(!r.ok);
        LIFT_CHECK(r.code == "not_paused");
    }

    // ---- ⑩ `sample`：读不到时是枚举 + 同一暂停点连采两次逐字节相同（判据 3d / 3b）----
    {
        Res rp;
        LIFT_CHECK(roundTrip("{\"id\":13,\"op\":\"pause\"}", &rp));
        LIFT_CHECK(rp.ok);
        LIFT_CHECK(jsonUint(rp.text, "state_seq") == state2 + 1);
    }
    Res s1;
    Res s2;
    // ★ 同一条请求发两次（id 也相同）⇒ **整条 res 文本**必须逐字节相同（判据 3b 的机制面：
    //   ⛔ 没有 wall_ms / 序号自增；`paused_total_ms` 用已结算口径）。
    LIFT_CHECK(roundTrip("{\"id\":14,\"op\":\"sample\"}", &s1));
    LIFT_CHECK(roundTrip("{\"id\":14,\"op\":\"sample\"}", &s2));
    std::printf("[judge3b] sample#1=%s\n", s1.text.c_str());
    std::printf("[judge3b] sample#2==#1: %d\n", s1.text == s2.text ? 1 : 0);
    LIFT_CHECK(s1.ok);
    LIFT_CHECK(s1.text == s2.text);
    const std::string sample_obj = jsonObject(s1.text, "sample");
    LIFT_CHECK(!sample_obj.empty());
    const std::string script_obj = jsonObject(sample_obj, "script");
    LIFT_CHECK(!script_obj.empty());
    const std::string why = jsonString(script_obj, "why");
    const std::vector<std::string> kExistingWhy = {
        "identity-invalid",      "engine-unpublished", "frame-record-uncommitted",
        "body-base-unpublished", "pc-outside-body",    "pc-outside-region",
        "pc-offset-over-max",    "pos-overflow",
    };
    bool why_known = false;
    for (const std::string& w : kExistingWhy) {
        if (why == w) why_known = true;
    }
    std::printf("[judge3d] script.available field present=%d why=%s known=%d sha_why=%s\n",
                script_obj.find("\"available\":false") != std::string::npos ? 1 : 0, why.c_str(),
                why_known ? 1 : 0, jsonString(script_obj, "script_sha_why").c_str());
    LIFT_CHECK(script_obj.find("\"available\":false") != std::string::npos);
    LIFT_CHECK(why_known);  // ★ 只能是 engine_marker 里既有的 8 个之一
    LIFT_CHECK(jsonString(script_obj, "script_sha_why") == "no_identity");
    LIFT_CHECK(script_obj.find("\"script_sha\":null") != std::string::npos);  // 缺省（JSON null）
    LIFT_CHECK(jsonUint(sample_obj, "state_seq") == state2 + 1);
    LIFT_CHECK(s1.text.find("\"paused\":true") != std::string::npos);

    // ---- ⑪ `screenshot` 的协议形状（真 BMP 见 lift-control-snapshot 的 dumpLastFrame 用例）----
    {
        namespace fs2 = std::filesystem;
        fake.frame.store(3);  // 替身："已经 Present 过"（否则按三态契约回 unavailable，见新用例）
        const fs2::path shot_path = fs2::path("control-endpoint-work") / "shot.bmp";
        std::error_code sec;
        fs2::remove(shot_path, sec);
        Res rs;
        const std::string req =
            "{\"id\":15,\"op\":\"screenshot\",\"path\":\"" + shot_path.generic_string() + "\"}";
        LIFT_CHECK(roundTrip(req, &rs));
        std::printf("[screenshot] res=%s\n", rs.text.c_str());
        LIFT_CHECK(rs.ok);
        const std::string shot = jsonObject(rs.text, "screenshot");
        LIFT_CHECK(!shot.empty());
        LIFT_CHECK(jsonUint(shot, "bytes") == 7);
        LIFT_CHECK(jsonString(shot, "source") == "guest-backbuffer");  // ★ 事实字段
        LIFT_CHECK(shot.find("\"hash_still_matches\":true") != std::string::npos);
        LIFT_CHECK(fs2::exists(shot_path));
        fs2::remove(shot_path, sec);
    }

    // ---- ⑪ `resume --for frames N`：谓词成立 ⇒ 重停（state_seq +2：离开 + 进入）----
    {
        Res rr;
        LIFT_CHECK(roundTrip("{\"id\":16,\"op\":\"resume\"}", &rr));
        LIFT_CHECK(rr.ok);
        const std::uint64_t before = jsonUint(rr.text, "state_seq");
        Res rp;
        LIFT_CHECK(roundTrip("{\"id\":17,\"op\":\"pause\"}", &rp));
        LIFT_CHECK(rp.ok);
        fake.frame.store(0);
        // 发出去**先不读**：答复要等谓词（帧号 +1）。
        LIFT_CHECK(ws.writeFrame(1, "{\"id\":18,\"op\":\"resume\",\"for_frames\":1}"));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        fake.frame.store(1);  // 下一帧 Present 发生
        Res rf;
        LIFT_CHECK(ws.readText(&rf.text));
        rf.ok = rf.text.find("\"ok\":true") != std::string::npos;
        std::printf("[judge4-for-frames] res=%s\n", rf.text.c_str());
        LIFT_CHECK(rf.ok);
        LIFT_CHECK(rf.text.find("\"paused\":true") != std::string::npos);
        // 计数：`before` 是 id=16 的 resume 之后；id=17 pause(+1) → id=18 离开(+1)
        // ⇒ 整条 `resume --for-*` = **+1**（判据 4 的步进口径；重停不另计）。
        LIFT_CHECK(jsonUint(rf.text, "state_seq") == before + 2);
        LIFT_CHECK(jsonString(rf.text, "paused_at") == "block");

        // ---- 谓词等不到 ⇒ timeout + **取消谓词**（帧不推进的屏上不能"以后自己停住"）----
        Res rr2;
        LIFT_CHECK(roundTrip("{\"id\":19,\"op\":\"resume\"}", &rr2));
        LIFT_CHECK(rr2.ok);
        Res rp2;
        LIFT_CHECK(roundTrip("{\"id\":20,\"op\":\"pause\"}", &rp2));
        LIFT_CHECK(rp2.ok);
        LIFT_CHECK(ws.writeFrame(1, "{\"id\":21,\"op\":\"resume\",\"for_frames\":1}"));
        const auto t_to = std::chrono::steady_clock::now();
        Res rto;
        LIFT_CHECK(ws.readText(&rto.text));  // 谓词超时（本用例 predicate_timeout=0.5 s）
        const double to_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_to).count();
        std::printf("[judge4-timeout] res=%s (waited %.0f ms)\n", rto.text.c_str(), to_ms);
        LIFT_CHECK(rto.text.find("\"ok\":false") != std::string::npos);
        LIFT_CHECK(jsonString(rto.text, "code") == "timeout");
        LIFT_CHECK(to_ms < 3000.0);
        // ★ 谓词已取消：① 不会在"帧后来推进"时自己停；② 第二次 `resume` 是 `not_paused`（不是旧谓词）。
        fake.frame.store(99);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        LIFT_CHECK(!stop_flag);
        Res rr3;
        LIFT_CHECK(roundTrip("{\"id\":22,\"op\":\"resume\"}", &rr3));
        LIFT_CHECK(!rr3.ok);
        LIFT_CHECK(rr3.code == "not_paused");

        // ---- `resume --for-ms` 的解析 / 形状（guest 时钟口径由 lift-control-snapshot 的纯用例保证）----
        Res rp3;
        LIFT_CHECK(roundTrip("{\"id\":23,\"op\":\"pause\"}", &rp3));
        LIFT_CHECK(rp3.ok);
        LIFT_CHECK(roundTrip("{\"id\":24,\"op\":\"resume\",\"for_frames\":0}", &rp3));
        LIFT_CHECK(!rp3.ok);
        LIFT_CHECK(rp3.code == "bad_request");  // 0 不是合法步长
    }

    // 判据 14 的形态：暂停态 `quit` ⇒ 汇点置停（serve 返回 true）⇒ 端点收口
    {
        Res rp;
        LIFT_CHECK(roundTrip("{\"id\":25,\"op\":\"pause\"}", &rp));
        LIFT_CHECK(rp.ok);
    }
    Res r_quit;
    LIFT_CHECK(roundTrip("{\"id\":26,\"op\":\"quit\"}", &r_quit));
    std::printf("[endpoint] quit res=%s\n", r_quit.text.c_str());
    LIFT_CHECK(r_quit.ok);
    LIFT_CHECK(r_quit.text.find("\"quit\":true") != std::string::npos);

    // gate 线程应当立刻回来（serve 返回 true），`stop_requested` 被置上
    for (int i = 0; i < 200 && !gate_done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    LIFT_CHECK(gate_done.load());
    LIFT_CHECK(stop_flag);
    if (gate.joinable()) gate.join();

    // 收口：`bye` 先于摘要（本用例没有摘要；判据 = bye 帧真的到了且 reason 来自 run 结果）
    const auto t0 = std::chrono::steady_clock::now();
    a.shutdown("stopped");
    const double shutdown_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[endpoint] shutdown took %.1f ms\n", shutdown_ms);
    LIFT_CHECK(shutdown_ms < 2000.0);  // 判据 14：不挂
    std::string bye;
    LIFT_CHECK(ws.readText(&bye));
    std::printf("[endpoint] bye=%s\n", bye.c_str());
    LIFT_CHECK(bye.find("\"event\":\"bye\"") != std::string::npos);
    LIFT_CHECK(jsonString(bye, "reason") == "stopped");
    LIFT_CHECK(jsonString(bye, "requested_by") == "client");
    LIFT_CHECK(jsonString(bye, "nonce") == hello_nonce);
    ws.close();

    // ⑥ 收口只删**自己那一个**文件；keep.txt 与另一个实例的文件都不许被碰
    LIFT_CHECK(!fs::exists(ctl_path));
    LIFT_CHECK(fs::exists(keep));
    if (c.started()) {
        const fs::path c_path = c.controlFilePath();
        LIFT_CHECK(fs::exists(c_path));
        c.shutdown("stopped");
        LIFT_CHECK(!fs::exists(c_path));
        LIFT_CHECK(fs::exists(keep));  // 另一个实例的收口也不碰 keep.txt
    }
    LIFT_CHECK(readWholeFile(keep.string()) == "not an endpoint file\n");
}

// =====================================================================================
// ⑫ `screenshot` 的两条契约（判据 5 / 客户端 `emu-ctl` 的实测偏差）：
//   ① **自己会暂停**：未暂停态直接 `screenshot` ⇒ `state_seq +1`、`paused_at ∈ {present,block}`；
//   ② **没帧可写 = unavailable**（数据没到），⛔ 不是 `io`（io 只留给"有像素但写不出去"）。
// ★ 真帧/真 BMP 那一段由 `lift-control-snapshot` 的 `dumpLastFrame` 用例 + 真 AOT 读数覆盖
//   （报告 §补）；这里测的是**控制层状态机 + 错误码映射**（pure，无需 GPU / 安装树）。
// =====================================================================================
void caseScreenshotSemantics() {
    ::lift_test::g_current_case = "screenshot-smoke-and-unavailable";
    namespace fs = std::filesystem;
    const fs::path root = fs::path("control-endpoint-work") / "ctl-shot";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    lift::Control d;
    lift::Control::Options opts;
    opts.root = root.string();
    opts.port = 0;
    opts.idle_timeout_s = 300.0;
    opts.orphan_timeout_s = 10.0;
    opts.predicate_timeout_s = 0.5;
    opts.session = "sess-shot";
    opts.exe_sha256 = "shot";
    opts.shape = "test-shape";
    std::string err;
    LIFT_CHECK(d.start(opts, &err));
    if (!d.started()) {
        std::fprintf(stderr, "start err: %s\n", err.c_str());
        return;
    }
    const std::string token = jsonString(readWholeFile(d.controlFilePath()), "token");

    FakeHostSide fake;  // frame = 0 ⇒ "从未 Present"
    std::atomic<bool> gate_quit{false};
    std::atomic<bool> gate_done{false};
    bool stop_flag = false;
    std::thread gate([&] {
        lift::WindowManager wm;
        lift::Clock clk;
        lift::Control::Context ctx;
        ctx.clock = &clk;
        ctx.windows = &wm;
        ctx.stop_requested = &stop_flag;
        ctx.paused_at = "block";  // 本用例只有块边界汇点
        ctx.host = &fake;
        while (!gate_quit.load()) {
            if (d.pending() || d.watching()) {
                if (d.serve(ctx)) {
                    gate_quit.store(true);
                    break;
                }
                if (d.watching()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        gate_done.store(true);
    });

    WsClient ws;
    LIFT_CHECK(ws.connectTo(d.localPort()));
    LIFT_CHECK(ws.handshake(token, false));
    std::string hello;
    LIFT_CHECK(ws.readText(&hello));

    struct Res {
        std::string text;
        bool ok = false;
        std::string code;
    };
    auto roundTrip = [&](const std::string& req, Res* out) -> bool {
        if (!ws.writeFrame(1, req)) return false;
        if (!ws.readText(&out->text)) return false;
        out->ok = out->text.find("\"ok\":true") != std::string::npos;
        out->code = jsonString(out->text, "code");
        return true;
    };
    const std::string shot_path = (root / "shot.bmp").generic_string();

    // ---- ② 从未 Present：screenshot ⇒ unavailable + 人话（⛔ 不是 io）----
    {
        Res r;
        LIFT_CHECK(roundTrip("{\"id\":1,\"op\":\"screenshot\",\"path\":\"" + shot_path + "\"}", &r));
        std::printf("[judge5-unavailable] res=%s\n", r.text.c_str());
        LIFT_CHECK(!r.ok);
        LIFT_CHECK(r.code == "unavailable");
        LIFT_CHECK(jsonString(r.text, "message").find("frame") != std::string::npos);
        LIFT_CHECK(!fs::exists(shot_path));
        // 它仍先把自己停住（与 sample 同一条路径）：用 sample 读状态（这里已暂停 ⇒ 幂等 +0）
        Res smp;
        LIFT_CHECK(roundTrip("{\"id\":2,\"op\":\"sample\"}", &smp));
        LIFT_CHECK(smp.ok);
        LIFT_CHECK(smp.text.find("\"paused\":true") != std::string::npos);
        LIFT_CHECK(jsonString(smp.text, "paused_at") == "block");
    }

    // ---- ① 未暂停态直接 screenshot：state_seq +1、paused_at ∈ {present,block}、有帧 ----
    fake.frame.store(5);  // "已经有 Present 过"
    Res rr;
    LIFT_CHECK(roundTrip("{\"id\":3,\"op\":\"resume\"}", &rr));
    LIFT_CHECK(rr.ok);
    const std::uint64_t seq_before = jsonUint(rr.text, "state_seq");
    Res rs;
    LIFT_CHECK(roundTrip("{\"id\":4,\"op\":\"screenshot\",\"path\":\"" + shot_path + "\"}", &rs));
    std::printf("[judge5-screenshot-pauses] res=%s (state_seq %llu → %llu)\n", rs.text.c_str(),
                static_cast<unsigned long long>(seq_before),
                static_cast<unsigned long long>(jsonUint(rs.text, "state_seq")));
    LIFT_CHECK(rs.ok);
    LIFT_CHECK(rs.text.find("\"paused\":true") != std::string::npos);
    const std::string paused_at = jsonString(rs.text, "paused_at");
    // ★ 这里**不**强制 `paused_at == "present"` —— 这是**父级裁决**，不是实现妥协
    //   （单据 `REQ-01M7HX0CJ202MJ4PZXW80VFA0J`，2026-11-16 裁决：判据 5 的字面改成
    //   `paused_at ∈ {present,block}` + 必须 `hash_still_matches:true` + 同一暂停点两张 `shasum`
    //   相同 + `source:"guest-backbuffer"`）。
    //   理由：`paused_at` 是"**哪个汇点先消费了暂停请求**"的**事实字段**（块边界 = block /
    //   Present 闸门 = present），它不是"必须停在哪"的条件。强制 present 会让 `screenshot` 在
    //   **从不 Present 的屏**上永远等不到（TITLE 轮询循环就是 frame=0）—— 那是把事实字段当成了
    //   必须满足的条件。判据 5 真正要的是"**同一帧**"：`hash_still_matches` 与同一暂停点两拍
    //   字节相同（本用例上面已经断言 `hash_still_matches:true`；真帧/真 BMP 的两拍 `shasum`
    //   由 `lift-control-snapshot` 的 `dumpLastFrame` 用例 + 真 AOT 读数覆盖）。
    //   ⛔ 不要再放宽这条断言（放宽 = 丢判据），也⛔ 不要收紧回 `== "present"`（那是把事实字段
    //   当必须条件）；要改判据字面请走单据裁决。
    LIFT_CHECK(paused_at == "present" || paused_at == "block");
    LIFT_CHECK(jsonUint(rs.text, "state_seq") == seq_before + 1);  // ★ 自己会暂停 ⇒ +1
    const std::string shot = jsonObject(rs.text, "screenshot");
    LIFT_CHECK(!shot.empty());
    LIFT_CHECK(jsonUint(shot, "bytes") == 7);
    LIFT_CHECK(shot.find("\"hash_still_matches\":true") != std::string::npos);
    LIFT_CHECK(fs::exists(shot_path));

    Res q;
    LIFT_CHECK(roundTrip("{\"id\":5,\"op\":\"quit\"}", &q));
    LIFT_CHECK(q.ok);
    for (int i = 0; i < 200 && !gate_done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    LIFT_CHECK(gate_done.load());
    if (gate.joinable()) gate.join();
    d.shutdown("stopped");
    fs::remove_all(root, ec);
}

// =====================================================================================
// ★ `resume` 必须真的**通知宿主**（`REQ-01M7JFDZK1SZW72SBAW6BC3DRT`，🐞S2）
//
// 洞：`ControlHostSide::controlOnResume()` 声明了、`AotHost` 也实现了（→ presenter → sink 记"恢复时刻"），
//   但 `Control::serve()` 的 Resume 分支**一次都没调它** ⇒ 生产路径上 `stale_input_before_ns` 恒 0 ⇒
//   `classifyStaleEvent` 恒 `Keep`：暂停 10 s 里的点击 / 拖动 / 关窗会被 resume 首轮当"刚发生"灌进 guest。
//   `present-window` 的用例只证明**纯映射 + sink 一侧**成立（它直接调 `win.noteControlResume()`），
//   证明不了"控制层到宿主这条线存在" —— 那正是本条用例的位置：**真协议 + 真 `serve()`**。
//
// 判据：paused → running 的**每一次**转移 arm 一次，其余命令不 arm。
//   ① `pause` 不 arm（进暂停态没有"首轮灌洪水"这回事）；
//   ② `resume` arm 一次；
//   ③ 运行态再 `resume`（`not_paused`）不 arm —— 没有状态转移就没有"恢复时刻"；
//   ④ `sample`（暂停态取数）不 arm；
//   ⑤ `resume --for-ms` **也** arm：它同样放行 guest 一轮，那一轮同样会把队列里的洪水吃进来；
//   ⑥ 再 `resume` ⇒ 又多一次（计到 3）。
// 红了意味着：恢复时刻没被记下来 ⇒ 陈旧事件闸门是死代码（本缺陷的形状；`rm` 掉 serve 里那一行就红）。
// =====================================================================================
void caseResumeArmsHostHook() {
    ::lift_test::g_current_case = "control-resume-arms-host-hook";

    namespace fs = std::filesystem;
    const fs::path root = fs::path("control-endpoint-work") / "ctl-resume-hook";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(fs::is_directory(root, ec));

    lift::Control a;
    lift::Control::Options opts;
    opts.root = root.string();
    opts.port = 0;
    opts.idle_timeout_s = 300.0;
    opts.orphan_timeout_s = 10.0;
    opts.predicate_timeout_s = 2.0;
    opts.session = "sess-resume-hook";
    opts.exe_sha256 = "resume-hook";
    opts.shape = "test-shape";
    std::string err;
    LIFT_CHECK(a.start(opts, &err));
    if (!a.started()) {
        std::fprintf(stderr, "start err: %s\n", err.c_str());
        return;
    }

    const std::string token = jsonString(readWholeFile(a.controlFilePath()), "token");
    WsClient ws;
    LIFT_CHECK(ws.connectTo(a.localPort()));
    LIFT_CHECK(ws.handshake(token, false));
    std::string hello;
    LIFT_CHECK(ws.readText(&hello));

    FakeHostSide fake;
    std::atomic<bool> gate_quit{false};
    bool stop_flag = false;
    // 主线程 = 本用例的 gate 线程（与 AOT 的块边界汇点同形：`pending()` / `watching()` ⇒ `serve()`）。
    std::thread gate([&] {
        lift::WindowManager wm;
        lift::Clock clk;
        lift::Control::Context ctx;
        ctx.clock = &clk;
        ctx.windows = &wm;
        ctx.stop_requested = &stop_flag;
        ctx.paused_at = "block";
        ctx.host = &fake;
        while (!gate_quit.load()) {
            if (a.pending() || a.watching()) {
                if (a.serve(ctx)) {
                    gate_quit.store(true);
                    break;
                }
                if (a.watching()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    });

    auto call = [&](const char* req) -> std::string {
        std::string out;
        if (!ws.writeFrame(1, req)) return out;
        if (!ws.readText(&out)) return std::string();
        return out;
    };

    // ① 进暂停态：不 arm
    const std::string p1 = call("{\"id\":1,\"op\":\"pause\"}");
    LIFT_CHECK(p1.find("\"ok\":true") != std::string::npos);
    LIFT_CHECK(fake.resume_noted.load() == 0u);

    // ② 第一次恢复：arm 一次
    const std::string r1 = call("{\"id\":2,\"op\":\"resume\"}");
    LIFT_CHECK(r1.find("\"ok\":true") != std::string::npos);
    LIFT_CHECK(fake.resume_noted.load() == 1u);

    // ③ 运行态再 `resume` ⇒ `not_paused`，且**不** arm（没有状态转移）
    const std::string r2 = call("{\"id\":3,\"op\":\"resume\"}");
    LIFT_CHECK(r2.find("\"not_paused\"") != std::string::npos);
    LIFT_CHECK(fake.resume_noted.load() == 1u);

    // ④ 暂停后取数（`sample`）不 arm
    const std::string p2 = call("{\"id\":4,\"op\":\"pause\"}");
    LIFT_CHECK(p2.find("\"ok\":true") != std::string::npos);
    const std::string s1 = call("{\"id\":5,\"op\":\"sample\"}");
    LIFT_CHECK(s1.find("\"ok\":true") != std::string::npos);
    LIFT_CHECK(fake.resume_noted.load() == 1u);

    // ⑤ `resume --for-ms` 也放行 guest ⇒ 也要 arm；谓词成立后**重停**
    const std::string r3 = call("{\"id\":6,\"op\":\"resume\",\"for_ms\":30}");
    LIFT_CHECK(r3.find("\"ok\":true") != std::string::npos);
    LIFT_CHECK(r3.find("\"paused\":true") != std::string::npos);
    LIFT_CHECK(fake.resume_noted.load() == 2u);

    // ⑥ 再恢复到运行态 ⇒ 第三次
    const std::string r4 = call("{\"id\":7,\"op\":\"resume\"}");
    LIFT_CHECK(r4.find("\"ok\":true") != std::string::npos);
    const unsigned noted = fake.resume_noted.load();
    std::printf("[judge-resume-hook] pause/resume(not_paused)/sample/resume--for-ms/resume 之后 "
                "controlOnResume 被调 %u 次（期望 3）\n", noted);
    LIFT_CHECK(noted == 3u);

    gate_quit.store(true);
    if (gate.joinable()) gate.join();
    a.shutdown("stopped");
    ws.close();
}

// =====================================================================================
// idle / orphan 两条超时（判据 9d 的形态）：两者都只**请求**收口（⛔ 控制线程里不杀进程），
// 且 `bye.requested_by` 必须能区分（idle vs orphan）—— 否则"谁把这一轮收了"无法回答。
// =====================================================================================
void caseTimeouts() {
    namespace fs = std::filesystem;
    const fs::path root = fs::path("control-endpoint-work") / "ctl-timeout";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // ---- ① idle：暂停态且无未决命令超过 idle_timeout ⇒ 端点自己 quit ----
    {
        ::lift_test::g_current_case = "idle-timeout";
        lift::Control a;
        lift::Control::Options opts;
        opts.root = root.string();
        opts.port = 0;
        opts.idle_timeout_s = 0.25;
        opts.orphan_timeout_s = 10.0;
        opts.session = "sess-idle";
        opts.exe_sha256 = "idle";
        opts.shape = "test-shape";
        std::string err;
        LIFT_CHECK(a.start(opts, &err));
        if (!a.started()) {
            std::fprintf(stderr, "start err: %s\n", err.c_str());
            return;
        }
        const std::string token = jsonString(readWholeFile(a.controlFilePath()), "token");
        WsClient ws;
        LIFT_CHECK(ws.connectTo(a.localPort()));
        LIFT_CHECK(ws.handshake(token, false));
        std::string hello;
        LIFT_CHECK(ws.readText(&hello));

        std::atomic<bool> gate_done{false};
        bool stop_flag = false;
        std::thread gate([&] {
            lift::WindowManager wm;
            lift::Clock clk;
            lift::Control::Context ctx;
            ctx.clock = &clk;
            ctx.windows = &wm;
            ctx.stop_requested = &stop_flag;
            while (true) {
                if (a.pending()) {
                    if (a.serve(ctx)) break;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            gate_done.store(true);
        });

        LIFT_CHECK(ws.writeFrame(1, "{\"id\":1,\"op\":\"pause\"}"));
        std::string res;
        LIFT_CHECK(ws.readText(&res));
        LIFT_CHECK(res.find("\"ok\":true") != std::string::npos);

        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 400 && !gate_done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const double waited_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[idle] pause → 端点自己收口用了 %.0f ms（idle=250 ms）\n", waited_ms);
        LIFT_CHECK(gate_done.load());
        LIFT_CHECK(stop_flag);
        LIFT_CHECK(waited_ms < 2000.0);
        if (gate.joinable()) gate.join();

        a.shutdown("stopped");
        std::string bye;
        LIFT_CHECK(ws.readText(&bye));
        std::printf("[idle] bye=%s\n", bye.c_str());
        LIFT_CHECK(jsonString(bye, "requested_by") == "idle");  // ★ 与 orphan 可区分
        LIFT_CHECK(jsonString(bye, "reason") == "stopped");
    }

    // ---- ② orphan：连接断了但端点还暂停 ⇒ orphan_timeout 后自行 quit（⛔ 不抱着 GPU 停着）----
    {
        ::lift_test::g_current_case = "orphan-timeout";
        lift::Control b;
        lift::Control::Options opts;
        opts.root = root.string();
        opts.port = 0;
        opts.idle_timeout_s = 300.0;  // 故意给大：能收口只可能是 orphan 那条
        opts.orphan_timeout_s = 0.25;
        opts.session = "sess-orphan";
        opts.exe_sha256 = "orphan";
        opts.shape = "test-shape";
        std::string err;
        LIFT_CHECK(b.start(opts, &err));
        if (!b.started()) {
            std::fprintf(stderr, "start err: %s\n", err.c_str());
            return;
        }
        const std::string token = jsonString(readWholeFile(b.controlFilePath()), "token");
        WsClient ws;
        LIFT_CHECK(ws.connectTo(b.localPort()));
        LIFT_CHECK(ws.handshake(token, false));
        std::string hello;
        LIFT_CHECK(ws.readText(&hello));

        std::atomic<bool> gate_done{false};
        bool stop_flag = false;
        std::thread gate([&] {
            lift::WindowManager wm;
            lift::Clock clk;
            lift::Control::Context ctx;
            ctx.clock = &clk;
            ctx.windows = &wm;
            ctx.stop_requested = &stop_flag;
            while (true) {
                if (b.pending()) {
                    if (b.serve(ctx)) break;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            gate_done.store(true);
        });

        LIFT_CHECK(ws.writeFrame(1, "{\"id\":1,\"op\":\"pause\"}"));
        std::string res;
        LIFT_CHECK(ws.readText(&res));
        LIFT_CHECK(res.find("\"ok\":true") != std::string::npos);
        ws.close();  // ★ 客户端消失（= kill -9 emu-ctl 的形态）：端点还停在暂停态

        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 400 && !gate_done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const double waited_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[orphan] 断连后端点自己收口用了 %.0f ms（orphan=250 ms；idle=300 s）\n", waited_ms);
        LIFT_CHECK(gate_done.load());
        LIFT_CHECK(stop_flag);
        LIFT_CHECK(waited_ms < 2000.0);
        if (gate.joinable()) gate.join();
        b.shutdown("stopped");
    }
}

}  // namespace

int main() {
    caseClockFreeze();
    caseEndpointSurface();
    caseScreenshotSemantics();
    caseResumeArmsHostHook();
    caseTimeouts();
    return lift_test::finish("lift-control-endpoint");
}
