// emulator-lift · AOT 控制层的**唯一**传输 TU（WebSocket over boost::asio / boost::beast）
//
// ★ 本文件是**唯一** include `<boost/beast/websocket.hpp>` + `<nlohmann/json.hpp>` 的 TU
//   （design-ws §2.1：beast 头会让一个 TU 多花 3–10 s；`runtime/control.h` 用 PIMPL 挡在外面）。
//
// ★ 线程形状（design-ws §2.3 的骨架，逐条落在这里）：
//   · **控制线程**（一条）= 唯一碰 socket / beast 对象 / JSON 的线程。形状是"async + 10 ms 轮询"：
//     `io_context.run_for(10ms)` 收 accept / HTTP upgrade / 帧，轮询间隔里做两件主线程交办的事
//     （把答复序列化发出去、收口时发 `bye`）。⛔ 不在控制线程里阻塞读（那会让 `quit` 的 join 挂死）。
//   · **主线程** = 唯一碰 guest 状态 / `VirtualClock` / `WindowManager` 的线程（`serve()` 在汇点里跑）。
//   · 两边只共享一个 mutex + 两个 cv + 一个小状态块（`has_req` / `reply` / `client_connected`）。
//
// ★ 安全边界（design-ws §2.7）：
//   ① 只绑 `127.0.0.1`（⛔ 不写 0.0.0.0）；② ⛔ 不开 `reuse_addr`
//   （Windows 上 `SO_REUSEADDR` 会让别的进程**抢占**同一端口）；
//   ③ 握手校验 `?token=`（不匹配 ⇒ 401 且**不升级协议**）；④ **带 `Origin` 头一律拒**
//   （浏览器一定发；Node 内置客户端不发 ⇒ 这条挡住"网页偷偷连本机端点"）；
//   ⑤ `read_message_max = 1 MiB`。
//
// ★ 协议（design-ws §2.4）：服务端先发 `hello`，之后一问一答；每条 `res` 回显 `nonce`；
//   `error.code` 是**枚举**（`controlErrorName`；⛔ 不写自由文本）；未知字段忽略、未知 `op` ⇒ `unknown_op`。
//
// ★ 端点发现（§2.5）：`<root>/ctl.<pid>.<nonce8>.json`，`listen()` 成功之后才写（`.tmp` → rename），
//   正常退出删；`--control-port 0` 由系统给端口。文件名带 pid + nonce ⇒ 同一 root 多实例不互相覆盖，
//   且"创建"是幂等的新建（⛔ 不改已有文件、⛔ 不按通配删任何东西）。
#include "runtime/control.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include "runtime/clock.h"
#include "runtime/cursor_map.h"  // kReferenceFrameW/H（`click` 的参考系上界；⛔ 这里不做除法）
#include "runtime/host_window.h"
#include "runtime/input_plan.h"  // M1：`click` 入的是**同一个** InputPlan 的立即队列

#ifdef _WIN32
#include <process.h>
#define LIFT_CTL_PID static_cast<unsigned long long>(::_getpid())
#else
#include <unistd.h>
#define LIFT_CTL_PID static_cast<unsigned long long>(::getpid())
#endif

namespace lift {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// 一帧 JSON 文本的上限（design-ws §2.4）。超限 ⇒ beast 自带 `message_too_big` ⇒ 关连接。
constexpr std::size_t kReadMessageMax = 1024u * 1024u;
// 控制线程的轮询粒度：只影响"答复/bye 最多晚多久发出去"，⛔ 不影响 guest。
constexpr std::chrono::milliseconds kPollInterval{10};
// 收口时 `bye` 的**有界**写预算（design-review §5-1：⛔ 不让半死 peer 拖住摘要）。
constexpr std::chrono::milliseconds kByeBudget{300};

std::string randomHex(std::size_t bytes) {
    static const char* kHex = "0123456789abcdef";
    std::random_device rd;
    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        const unsigned v = static_cast<unsigned>(rd() & 0xFFu);
        out.push_back(kHex[(v >> 4) & 0xFu]);
        out.push_back(kHex[v & 0xFu]);
    }
    return out;
}

std::string utcNowIso() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string utcNowCompact() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::chrono::milliseconds toMs(double seconds) {
    if (!(seconds > 0.0)) return std::chrono::milliseconds(0);  // 0 / 负 / NaN ⇒ 关掉这条计时器
    const double ms = seconds * 1000.0;
    const double capped = ms > 86'400'000.0 ? 86'400'000.0 : ms;  // 1 天封顶（避免溢出）
    return std::chrono::milliseconds(static_cast<long long>(capped));
}

// 控制线程解析出来的命令（只含值；主线程执行）。`for_*` 只对 `resume` 有意义。
struct Command {
    ControlOp op = ControlOp::None;
    std::string id = "null";
    bool for_frames = false;
    bool for_ms = false;
    std::uint64_t frames = 0;
    double ms = 0.0;
    std::string path;  // screenshot 的目标路径
    // `click`：**1280×720 参考坐标**（整数，已在解析期校验）+ 按钮（缺省左键）。
    int x = 0;
    int y = 0;
    MouseButton button = MouseButton::Left;
};

// 主线程填、控制线程序列化的答复（⛔ 不含 guest 指针 / 不含自由文本错误码）。
//   ★ `snapshot` / `shot` 是**值拷贝**（`control_snapshot.h`：没有 guest 指针）⇒ 控制线程
//     只序列化，⛔ 不读 guest 内存。`error_detail` 是给人看的一句话（code 仍是枚举）。
struct Reply {
    ControlError error = ControlError::None;
    std::string error_detail;
    bool paused = false;
    const char* paused_at = "";
    std::uint64_t state_seq = 0;
    std::uint64_t guest_ms = 0;
    std::uint64_t paused_total_ms = 0;
    bool quit = false;
    bool has_snapshot = false;
    ControlSnapshot snapshot;
    bool has_screenshot = false;
    ControlScreenshot shot;
    // `click`：入队回执（`ok:true` = **已入队**，不是"已施加"——施加点是下一个 guest 读点）。
    bool has_click = false;
    int click_x = 0;
    int click_y = 0;
    const char* click_button = "left";
    std::size_t click_queued = 0;
};

}  // namespace

// =====================================================================================
// Impl
// =====================================================================================
struct Control::Impl {
    Options opts;
    // ---- 身份（`hello` 与 `ctl.*.json` 的**同一份**来源）----
    std::string nonce;
    std::string token;
    std::string started_utc;
    std::string ctl_path;
    std::string ctl_tmp_path;
    std::uint16_t port = 0;
    std::string local_host = "127.0.0.1";
    bool started = false;

    // ---- 主线程 / 控制线程共享状态（`mu` 保护）----
    std::mutex mu;
    std::condition_variable cv_req;  // 主线程：等命令 / 等收口
    bool has_req = false;
    ControlOp req_op = ControlOp::None;
    std::string req_id = "null";
    // `resume --for-*` 的参数（只在这一条命令被消费时读）。
    bool req_for_frames = false;
    std::uint64_t req_frames = 0;
    bool req_for_ms = false;
    double req_ms = 0.0;
    std::string req_path;  // screenshot
    // `click`（只在这一条命令被消费时读）。
    int req_x = 0;
    int req_y = 0;
    MouseButton req_button = MouseButton::Left;
    Reply reply;
    bool reply_ready = false;
    bool client_connected = false;
    std::atomic<bool> shutting_down{false};
    std::string run_reason = "stopped";
    std::string requested_by = "none";  // client | idle | orphan | none
    Clock::time_point last_activity = Clock::now();
    // ---- 仅主线程状态 ----
    bool paused = false;
    const char* paused_at = "";
    std::uint64_t state_seq = 0;
    // ★ **重停谓词**（`resume --for-ms` / `--for frames`；design-review §4）：
    //   armed ⇒ guest 在跑、答复**还没发**；完成 ⇒ 重停（进入暂停态 +1）；等不到 ⇒ 宿主墙钟兜底
    //   回 `timeout` 并**清掉谓词**（⛔ 之后"自己停住"）。被放弃（客户端断了）⇒ 清掉且不进暂停。
    struct Watch {
        bool armed = false;
        bool for_frames = false;
        std::uint64_t frame_target = 0;
        std::uint64_t guest_100ns_target = 0;
        Clock::time_point deadline{};
    } watch;
    std::atomic<bool> watching_flag{false};

    // ---- 仅控制线程状态 ----
    asio::io_context ioc;
    std::unique_ptr<tcp::acceptor> acceptor;
    std::unique_ptr<websocket::stream<tcp::socket>> ws;
    std::unique_ptr<http::request_parser<http::string_body>> parser;
    beast::flat_buffer req_buf;
    beast::flat_buffer rbuf;
    std::deque<std::string> outq;
    std::string wtext;
    bool writing = false;
    bool awaiting_reply = false;
    bool session_dead = false;
    bool bye_sent = false;
    std::thread thread;
    std::atomic<bool> pending_flag{false};
    std::atomic<bool> stop_thread{false};

    // ③ 热路径唯一入口
    bool pending() const { return pending_flag.load(std::memory_order_acquire); }

    // ---- 启动 / 收口 ----
    bool startAll(const Options& o, std::string* err);
    void shutdownAll(const std::string& reason, const char* requested_by_override);
    bool writeCtlFile(std::string* err);

    // ---- 控制线程 ----
    void threadMain();
    void doAccept();
    void onAccept(beast::error_code ec, tcp::socket sock);
    void onHttpRead(beast::error_code ec);
    void respondHttpAndDrop(http::status status);
    void startWsSession();
    void startRead();
    void onRead(beast::error_code ec, std::size_t n);
    void handleFrame(const std::string& text);
    bool parseCommand(const json& j, const std::string& id, Command* out, ControlError* err);
    void sendText(std::string text);
    void pumpWrite();
    void pumpReply();
    void dropSessionSoon();
    void finishSession(bool accept_again);

    // ---- 序列化（只在控制线程调）----
    std::string helloJson() const;
    std::string ctlJson() const;
    std::string byeJson() const;
    std::string resJson(const std::string& id, const Reply& r) const;
    std::string errorJson(const std::string& id, ControlError e) const;
    bool tokenMatches(const std::string& target) const;

    // ---- 主线程（汇点）----
    bool serve(Context& ctx);
    bool waitForCommandLocked(std::unique_lock<std::mutex>& lk, std::string* cause);
    void publishLocked(const Reply& r);
    void consumeLocked();  // 消费命令但**不发答复**（`resume --for-*`：答复等谓词完成）
    void requestQuit(Context& ctx, const std::string& cause);
    Reply stateReply() const;
    void fillClock(Context& ctx, Reply* r) const;
    // 谓词轮询（只主线程）：KeepRunning = 放行 guest；其余三种各对应一个收口动作。
    enum class WatchPoll { KeepRunning, Completed, TimedOut, Abandoned };
    WatchPoll pollWatchLocked(Context& ctx) const;
};

// =====================================================================================
// 身份 / 文件
// =====================================================================================
std::string Control::Impl::ctlJson() const {
    json j;
    j["schema"] = "emu-ctl/2";
    j["url"] = "ws://127.0.0.1:" + std::to_string(port) + "/?token=" + token;
    j["pid"] = LIFT_CTL_PID;
    j["port"] = port;
    j["nonce"] = nonce;
    j["token"] = token;
    j["session"] = opts.session;
    j["exe_sha256"] = opts.exe_sha256;
    j["shape"] = opts.shape;
    j["started_utc"] = started_utc;
    return j.dump(2) + "\n";
}

std::string Control::Impl::helloJson() const {
    json j;
    j["schema"] = "emu-ctl/2";
    j["pid"] = LIFT_CTL_PID;
    j["session"] = opts.session;
    j["nonce"] = nonce;
    j["port"] = port;
    j["started_utc"] = started_utc;
    j["exe_sha256"] = opts.exe_sha256;
    j["shape"] = opts.shape;
    // ★ 只列**真的实现了**的（S0+S1+S2+S3+S4 + M1 的 `click`；`key` 本轮不做 ⇒ 不列）。
    j["capabilities"] = json::array({"pause", "resume", "sample", "screenshot", "click", "quit"});
    // ★ 判据 2 的裁决（父级 2026-11-16）：`present` 在**顶层**（⛔ 不塞进 `control`）：
    //   它是**连接时的事实**，值取 `windowHidden()` / `describe()` 那条既有判据（⛔ 不由 --present 推）。
    j["present"] = opts.present;
    json control;
    control["clock"] = "freeze";
    control["idle_timeout_s"] = opts.idle_timeout_s;
    control["orphan_timeout_s"] = opts.orphan_timeout_s;
    control["predicate_timeout_s"] = opts.predicate_timeout_s;
    j["control"] = control;
    return j.dump();
}

std::string Control::Impl::byeJson() const {
    json j;
    j["event"] = "bye";
    j["reason"] = run_reason;
    j["requested_by"] = requested_by;
    j["nonce"] = nonce;
    return j.dump();
}

std::string Control::Impl::resJson(const std::string& id, const Reply& r) const {
    json j;
    j["nonce"] = nonce;
    json id_json = json::parse(id, nullptr, false);
    j["id"] = id_json.is_discarded() ? json(id) : id_json;
    if (r.error != ControlError::None) {
        j["ok"] = false;
        json err{{"code", controlErrorName(r.error)}};
        // ★ `code` 是枚举（判据 / 客户端分支）；`message` 只给**人**看（⛔ 不许客户端按它分支）。
        if (!r.error_detail.empty()) err["message"] = r.error_detail;
        j["error"] = err;
        return j.dump();
    }
    j["ok"] = true;
    json data;
    data["paused"] = r.paused;
    data["paused_at"] = r.paused_at;
    data["state_seq"] = r.state_seq;
    data["guest_ms"] = r.guest_ms;
    data["paused_total_ms"] = r.paused_total_ms;
    if (r.quit) data["quit"] = true;
    if (r.has_snapshot) {
        // ★ 序列化是**纯函数**（`controlSnapshotJson`；同输入 ⇒ 逐字节同输出）。
        data["sample"] = json::parse(controlSnapshotJson(r.snapshot), nullptr, false);
    }
    if (r.has_screenshot) {
        data["screenshot"] = json{{"path", r.shot.path},
                                  {"bytes", r.shot.bytes},
                                  {"frame", r.shot.frame},
                                  {"hash_still_matches", r.shot.hash_still_matches},
                                  {"source", r.shot.source}};
    }
    if (r.has_click) {
        // ★ `queued` = 入队后的深度（判据 5 的读数面；上界见 `InputPlan::kImmediateClickCapacity`）。
        //   ⚠ 它只是**回执**：真正施加发生在下一个 guest 读点（⛔ 不等于"已点中"）。
        data["click"] = json{{"x", r.click_x},
                             {"y", r.click_y},
                             {"button", r.click_button},
                             {"queued", r.click_queued}};
    }
    j["data"] = data;
    return j.dump();
}

std::string Control::Impl::errorJson(const std::string& id, ControlError e) const {
    Reply r;
    r.error = e;
    return resJson(id, r);
}

bool Control::Impl::tokenMatches(const std::string& target) const {
    // target 形如 `/?token=<hex>`（也接受 `?token=<hex>`；多余查询串忽略）。
    const std::size_t q = target.find('?');
    if (q == std::string::npos) return false;
    std::size_t i = q + 1;
    while (i < target.size()) {
        const std::size_t amp = target.find('&', i);
        const std::string pair = target.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == "token") {
            return pair.substr(eq + 1) == token;  // 定长比较足够：token 是本地文件里读出来的
        }
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    return false;
}

bool Control::Impl::writeCtlFile(std::string* err) {
    {
        std::ofstream out(ctl_tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            *err = "无法写控制端点文件（" + ctl_tmp_path + "）";
            return false;
        }
        const std::string text = ctlJson();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            *err = "控制端点文件写入失败（" + ctl_tmp_path + "）";
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(ctl_tmp_path, ctl_path, ec);
    if (ec) {
        // 目标已存在（同一 pid + nonce 不可能）⇒ 不走删除 + 重试（⛔ 不动已有文件）。
        *err = "控制端点文件 rename 失败（" + ctl_path + "）：" + ec.message();
        std::filesystem::remove(ctl_tmp_path, ec);
        return false;
    }
    return true;
}

// =====================================================================================
// 启动 / 收口（主线程调）
// =====================================================================================
bool Control::Impl::startAll(const Options& o, std::string* err) {
    opts = o;
    if (opts.root.empty()) {
        *err = "--control-root 为空";
        return false;
    }
    if (!(opts.port <= 65535u)) {
        *err = "控制端口越界（--control-port 0..65535）";
        return false;
    }
    nonce = randomHex(8);
    token = randomHex(16);
    started_utc = utcNowIso();
    if (opts.session.empty()) {
        opts.session = "run-" + std::to_string(LIFT_CTL_PID) + "-" + utcNowCompact();
    }

    // ---- 目录：缺省建；建不出来要**响亮**（design-review §8-6：端点自己 mkdir）----
    std::error_code ec;
    std::filesystem::create_directories(opts.root, ec);
    if (!std::filesystem::is_directory(opts.root, ec)) {
        *err = "无法创建 / 使用 --control-root 目录：" + opts.root + "（" + ec.message() + "）";
        return false;
    }

    // ---- listen：只绑 127.0.0.1、⛔ 不开 reuse_addr ----
    const asio::ip::address loopback = asio::ip::make_address("127.0.0.1");
    const tcp::endpoint ep(loopback, opts.port);
    acceptor = std::make_unique<tcp::acceptor>(ioc);
    beast::error_code opt_ec;
    acceptor->open(ep.protocol(), opt_ec);
    if (opt_ec) {
        *err = "控制端点 socket 打开失败：" + opt_ec.message();
        acceptor.reset();
        return false;
    }
    // ★ 显式钉住（缺省就是 false）：Windows 上 `SO_REUSEADDR` 会让别的进程**抢占**同一端口。
    acceptor->set_option(asio::socket_base::reuse_address(false), opt_ec);  // 显式钉住
    if (opt_ec) {
        *err = "控制端点设置 reuse_address(false) 失败：" + opt_ec.message();
        acceptor.reset();
        return false;
    }
    beast::error_code bind_ec;
    acceptor->bind(ep, bind_ec);
    if (bind_ec) {
        // ★ 判据 12：**点名端口**，⛔ 不静默换端口、⛔ 不降级成"没有控制层"。
        *err = "控制端点 bind 失败：127.0.0.1:" + std::to_string(opts.port) + " ⇒ " + bind_ec.message() +
               "（端口被占用？⛔ 不静默换端口，请换 --control-port）";
        acceptor.reset();
        return false;
    }
    beast::error_code listen_ec;
    acceptor->listen(asio::socket_base::max_listen_connections, listen_ec);
    if (listen_ec) {
        *err = "控制端点 listen 失败：127.0.0.1:" + std::to_string(opts.port) + " ⇒ " + listen_ec.message();
        acceptor.reset();
        return false;
    }
    beast::error_code ep_ec;
    port = acceptor->local_endpoint(ep_ec).port();
    if (ep_ec || port == 0) {
        *err = "控制端点 listen 成功但读不到实际端口：" + ep_ec.message();
        acceptor.reset();
        return false;
    }

    // ---- 端点文件（listen 成功**之后**才写；`.tmp` → rename）----
    const std::string name =
        "ctl." + std::to_string(LIFT_CTL_PID) + "." + nonce.substr(0, 8) + ".json";
    ctl_path = (std::filesystem::path(opts.root) / name).string();
    ctl_tmp_path = ctl_path + ".tmp";
    if (!writeCtlFile(err)) {
        beast::error_code cl;
        acceptor->close(cl);
        acceptor.reset();
        return false;
    }

    // ---- 控制线程 ----
    started = true;
    thread = std::thread([this] { threadMain(); });
    return true;
}

void Control::Impl::shutdownAll(const std::string& reason, const char* requested_by_override) {
    if (!started) return;
    {
        std::lock_guard<std::mutex> lk(mu);
        run_reason = reason.empty() ? std::string("stopped") : reason;
        // ★ `bye` 两个字段各管一件事：reason = run 为什么结束；requested_by = 谁请求的收口。
        //   窗口关闭这类宿主停机由调用方显式传 `client`（其余情况保留命令侧已经记下的那一个）。
        if (requested_by_override != nullptr && requested_by_override[0] != '\0') {
            requested_by = requested_by_override;
        }
        shutting_down = true;
    }
    cv_req.notify_all();
    if (thread.joinable()) thread.join();
    // 删**精确文件名**（⛔ 不按通配 / 不删同目录里别人的文件：判据 9b 的机制面）。
    std::error_code ec;
    if (!ctl_tmp_path.empty()) std::filesystem::remove(ctl_tmp_path, ec);
    if (!ctl_path.empty()) std::filesystem::remove(ctl_path, ec);
    started = false;
}

// =====================================================================================
// 控制线程
// =====================================================================================
void Control::Impl::threadMain() {
    doAccept();
    Clock::time_point bye_deadline{};
    while (!stop_thread.load(std::memory_order_acquire)) {
        ioc.run_for(kPollInterval);
        if (session_dead) finishSession(!shutting_down.load());
        pumpReply();
        if (shutting_down.load()) {
            if (bye_deadline == Clock::time_point{}) bye_deadline = Clock::now() + kByeBudget;
            if (awaiting_reply) {
                // 未决请求等不到汇点（run 已经返回）⇒ 兜底答复 `shutting_down`（⛔ 让 CLI 挂死）。
                std::string id;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    id = req_id;
                    has_req = false;
                    awaiting_reply = false;
                }
                sendText(errorJson(id, ControlError::ShuttingDown));
            }
            if (!bye_sent) {
                bye_sent = true;
                sendText(byeJson());
            }
            if ((outq.empty() && !writing) || Clock::now() >= bye_deadline) break;
        }
        ioc.restart();
    }
    // 收尾：取消未完成的异步操作并排空它们的 handler（再析构 beast 对象才安全）。
    if (ws) {
        beast::error_code ec;
        ws->next_layer().cancel(ec);
    }
    ioc.restart();
    ioc.run_for(kPollInterval);
    finishSession(false);
    {
        beast::error_code ec;
        if (acceptor) acceptor->close(ec);
        acceptor.reset();
    }
    ioc.stop();
}

void Control::Impl::doAccept() {
    if (!acceptor || shutting_down.load()) return;
    acceptor->async_accept([this](beast::error_code ec, tcp::socket sock) {
        try {
            onAccept(ec, std::move(sock));
        } catch (...) {
            dropSessionSoon();
        }
    });
}

void Control::Impl::onAccept(beast::error_code ec, tcp::socket sock) {
    if (ec) {
        if (!shutting_down.load() && !stop_thread.load()) doAccept();
        return;
    }
    ws = std::make_unique<websocket::stream<tcp::socket>>(std::move(sock));
    ws->read_message_max(kReadMessageMax);  // ★ 1 MiB 上限
    req_buf.clear();
    parser = std::make_unique<http::request_parser<http::string_body>>();
    parser->body_limit(kReadMessageMax);
    http::async_read(ws->next_layer(), req_buf, *parser, [this](beast::error_code e, std::size_t) {
        try {
            onHttpRead(e);
        } catch (...) {
            dropSessionSoon();
        }
    });
}

void Control::Impl::respondHttpAndDrop(http::status status) {
    if (!ws) {
        dropSessionSoon();
        return;
    }
    auto res = std::make_shared<http::response<http::string_body>>(status, 11);
    res->set(http::field::content_type, "text/plain; charset=utf-8");
    res->set(http::field::connection, "close");
    res->body() = (status == http::status::forbidden) ? "origin not allowed\n" : "unauthorized\n";
    res->prepare_payload();
    http::async_write(ws->next_layer(), *res, [this, res](beast::error_code, std::size_t) {
        dropSessionSoon();
    });
}

void Control::Impl::onHttpRead(beast::error_code ec) {
    if (ec) {
        dropSessionSoon();
        return;
    }
    const http::request<http::string_body>& req = parser->get();
    // ★ 安全边界：带 `Origin` 头的握手**一律拒**（浏览器一定发；Node 内置客户端不发）。
    if (req.find(http::field::origin) != req.end()) {
        respondHttpAndDrop(http::status::forbidden);
        return;
    }
    if (!tokenMatches(std::string(req.target()))) {
        respondHttpAndDrop(http::status::unauthorized);
        return;
    }
    ws->async_accept(req, [this](beast::error_code ae) {
        if (ae) {
            dropSessionSoon();
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            client_connected = true;
        }
        sendText(helloJson());  // ★ 服务端先发 hello
        startRead();
    });
}

void Control::Impl::startRead() {
    if (!ws || session_dead) return;
    rbuf.clear();
    ws->async_read(rbuf, [this](beast::error_code e, std::size_t) {
        try {
            onRead(e, 0);
        } catch (...) {
            dropSessionSoon();
        }
    });
}

void Control::Impl::onRead(beast::error_code ec, std::size_t) {
    if (ec) {
        dropSessionSoon();
        return;
    }
    std::string text = beast::buffers_to_string(rbuf.data());
    rbuf.consume(rbuf.size());
    handleFrame(text);
}

bool Control::Impl::parseCommand(const json& j, const std::string& id, Command* out, ControlError* err) {
    const auto op_it = j.find("op");
    if (op_it == j.end() || !op_it->is_string()) {
        *err = ControlError::BadRequest;
        return false;
    }
    const std::string op_name = op_it->get<std::string>();
    if (op_name == "pause") out->op = ControlOp::Pause;
    else if (op_name == "resume") out->op = ControlOp::Resume;
    else if (op_name == "sample") out->op = ControlOp::Sample;
    else if (op_name == "screenshot") out->op = ControlOp::Screenshot;
    else if (op_name == "click") out->op = ControlOp::Click;
    else if (op_name == "quit") out->op = ControlOp::Quit;
    else {
        *err = ControlError::UnknownOp;  // ★ 未知 op ⇒ 枚举错误，⛔ 不静默
        return false;
    }
    out->id = id;
    // ---- `resume --for-ms N` / `--for frames N`（两个互斥；数值必须严格正）----
    if (out->op == ControlOp::Resume) {
        const auto ff = j.find("for_frames");
        const auto fm = j.find("for_ms");
        if (ff != j.end() && fm != j.end()) {
            *err = ControlError::BadRequest;
            return false;
        }
        if (ff != j.end()) {
            if (!ff->is_number_unsigned() || ff->get<std::uint64_t>() == 0) {
                *err = ControlError::BadRequest;
                return false;
            }
            out->for_frames = true;
            out->frames = ff->get<std::uint64_t>();
        }
        if (fm != j.end()) {
            if (!fm->is_number() || fm->get<double>() <= 0.0 || !(fm->get<double>() <= 86'400'000.0)) {
                *err = ControlError::BadRequest;
                return false;
            }
            out->for_ms = true;
            out->ms = fm->get<double>();
        }
    }
    // ---- `screenshot <path>` ----
    if (out->op == ControlOp::Screenshot) {
        const auto p = j.find("path");
        if (p == j.end() || !p->is_string() || p->get<std::string>().empty()) {
            *err = ControlError::BadRequest;
            return false;
        }
        out->path = p->get<std::string>();
    }
    // ---- `click <x> <y> [button]`（**1280×720 参考坐标**；整数、在界内；按钮缺省 left）----
    //   ★ 这里**只校验参考坐标**，不映射：映射的唯一落点是施加那一刻的
    //     `cursor_map.h::referenceToClientPixel`（⛔ 控制层不写第二份坐标换算）。
    if (out->op == ControlOp::Click) {
        auto readCoord = [&j](const char* key, int limit, int* out) -> bool {
            const auto it = j.find(key);
            if (it == j.end() || !it->is_number_integer()) return false;  // 浮点 / 字符串 / 缺省 = 非法
            if (it->is_number_unsigned()) {
                const std::uint64_t v = it->get<std::uint64_t>();
                if (v >= static_cast<std::uint64_t>(limit)) return false;
                *out = static_cast<int>(v);
                return true;
            }
            const long long v = it->get<long long>();
            if (v < 0 || v >= static_cast<long long>(limit)) return false;
            *out = static_cast<int>(v);
            return true;
        };
        if (!readCoord("x", kReferenceFrameW, &out->x) || !readCoord("y", kReferenceFrameH, &out->y)) {
            *err = ControlError::BadRequest;  // 负数 / 越界 / 非整数 ⇒ 响亮（⛔ 不 clamp、⛔ 不猜）
            return false;
        }
        const auto b = j.find("button");
        if (b != j.end()) {
            if (!b->is_string()) {
                *err = ControlError::BadRequest;
                return false;
            }
            const std::string name = b->get<std::string>();
            if (name == "left") {
                out->button = MouseButton::Left;
            } else if (name == "right") {
                out->button = MouseButton::Right;
            } else {
                *err = ControlError::BadRequest;  // 只有 left / right（`middle` 本轮不支持）
                return false;
            }
        }
    }
    return true;
}

void Control::Impl::handleFrame(const std::string& text) {
    json j = json::parse(text, nullptr, false);  // ★ **不抛异常**（坏帧 = `bad_request`，⛔ 不是 terminate）
    if (j.is_discarded() || !j.is_object()) {
        sendText(errorJson("null", ControlError::BadRequest));
        startRead();
        return;
    }
    std::string id = "null";
    if (j.contains("id")) id = j["id"].dump();
    Command cmd;
    ControlError cerr = ControlError::None;
    if (!parseCommand(j, id, &cmd, &cerr)) {
        sendText(errorJson(id, cerr));
        startRead();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        req_op = cmd.op;
        req_id = cmd.id;
        req_for_frames = cmd.for_frames;
        req_frames = cmd.frames;
        req_for_ms = cmd.for_ms;
        req_ms = cmd.ms;
        req_path = cmd.path;
        req_x = cmd.x;
        req_y = cmd.y;
        req_button = cmd.button;
        reply_ready = false;
        has_req = true;
        last_activity = Clock::now();
    }
    pending_flag.store(true, std::memory_order_release);
    cv_req.notify_all();
    awaiting_reply = true;  // 一问一答：等主线程的回结果，⛔ 不先读下一帧
}

void Control::Impl::pumpReply() {
    if (!awaiting_reply) return;
    Reply r;
    std::string id;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!reply_ready) return;
        r = reply;
        id = req_id;
        reply_ready = false;
    }
    awaiting_reply = false;
    sendText(resJson(id, r));  // ★ 组 JSON 只在控制线程（主线程只填 POD）
    startRead();
}

void Control::Impl::sendText(std::string text) {
    if (!ws || session_dead) return;
    outq.push_back(std::move(text));
    pumpWrite();
}

void Control::Impl::pumpWrite() {
    if (writing || !ws || session_dead || outq.empty()) return;
    writing = true;
    wtext = std::move(outq.front());
    outq.pop_front();
    ws->text(true);
    ws->async_write(asio::buffer(wtext), [this](beast::error_code ec, std::size_t) {
        writing = false;
        if (ec) {
            dropSessionSoon();
            return;
        }
        pumpWrite();
    });
}

void Control::Impl::dropSessionSoon() {
    // ★ 只在这里**标记 + 关 socket**；beast 对象由 `finishSession()` 在 handler 之外析构
    //   （在完成 handler 里析构 stream 是未定义行为）。
    session_dead = true;
    if (ws) {
        beast::error_code ec;
        ws->next_layer().shutdown(tcp::socket::shutdown_both, ec);
        ws->next_layer().close(ec);
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        client_connected = false;
        awaiting_reply = false;
    }
    cv_req.notify_all();
}

void Control::Impl::finishSession(bool accept_again) {
    if (ws) {
        beast::error_code ec;
        ws->next_layer().close(ec);
        ws.reset();
    }
    parser.reset();
    req_buf.consume(req_buf.size());
    rbuf.consume(rbuf.size());
    outq.clear();
    wtext.clear();
    writing = false;
    awaiting_reply = false;
    session_dead = false;
    if (accept_again && !shutting_down.load()) doAccept();
}

// =====================================================================================
// 主线程（汇点）
// =====================================================================================
Reply Control::Impl::stateReply() const {
    Reply r;
    r.paused = paused;
    r.paused_at = paused_at;
    r.state_seq = state_seq;
    return r;
}

// ★ 时钟读数：`paused_total_ms` 在 **`sample` 的 data** 里是"含当前冻结区间"的 S1 口径
//   （pause/resume 的答复）；快照里那一格用**已结算**口径（`control_snapshot.h` 的头注：判据 3b）。
void Control::Impl::fillClock(Context& ctx, Reply* r) const {
    if (ctx.clock == nullptr) return;
    r->guest_ms = ctx.clock->nowMs();
    r->paused_total_ms = ctx.clock->pausedTotalMs();
}

void Control::Impl::publishLocked(const Reply& r) {
    reply = r;
    reply_ready = true;
    has_req = false;
    pending_flag.store(false, std::memory_order_release);
}

// 消费掉命令但**不发答复**（`resume --for-*`：答复要等谓词完成 / 超时）。
void Control::Impl::consumeLocked() {
    has_req = false;
    pending_flag.store(false, std::memory_order_release);
}

void Control::Impl::requestQuit(Context& ctx, const std::string& cause) {
    {
        std::lock_guard<std::mutex> lk(mu);
        requested_by = cause;
        watch = Watch{};
        watching_flag.store(false, std::memory_order_release);
        if (paused) {
            paused = false;
            paused_at = "";
            ++state_seq;  // ★ 离开暂停态也严格 +1
        }
    }
    if (ctx.clock != nullptr) ctx.clock->unfreeze();
    if (ctx.stop_requested != nullptr) *ctx.stop_requested = true;
    if (ctx.windows != nullptr) ctx.windows->postQuit(ctx.quit_code);
}

Control::Impl::WatchPoll Control::Impl::pollWatchLocked(Context& ctx) const {
    if (!watch.armed) return WatchPoll::KeepRunning;
    // ① 客户端走了（`kill -9 emu-ctl` / 断开 / 控制线程判死）：清谓词，⛔ 之后"自己停住"。
    if (!client_connected) return WatchPoll::Abandoned;
    // ② 谓词成立？（`frame` 走 presenter 的计数；`ms` 走 **guest 时钟**，⛔ 不是宿主墙钟）
    bool satisfied = false;
    if (watch.for_frames) {
        satisfied = ctx.host != nullptr && ctx.host->controlPresentFrame() >= watch.frame_target;
    } else if (ctx.clock != nullptr) {
        satisfied = ctx.clock->now100ns() >= watch.guest_100ns_target;
    }
    if (satisfied) return WatchPoll::Completed;
    // ③ 等不到（帧不推进的屏上 `--for frames` 必然撞这里）⇒ 放弃超时（宿主墙钟）。
    if (Clock::now() >= watch.deadline) return WatchPoll::TimedOut;
    return WatchPoll::KeepRunning;
}

bool Control::Impl::waitForCommandLocked(std::unique_lock<std::mutex>& lk, std::string* cause) {
    const std::chrono::milliseconds idle_ms = toMs(opts.idle_timeout_s);
    const std::chrono::milliseconds orphan_ms = toMs(opts.orphan_timeout_s);
    bool orphan_armed = false;
    Clock::time_point orphan_deadline{};
    for (;;) {
        if (shutting_down) {
            *cause = "shutdown";
            return false;
        }
        if (has_req) return true;
        const Clock::time_point now = Clock::now();
        if (!client_connected) {
            if (!orphan_armed && orphan_ms.count() > 0) {
                orphan_armed = true;
                orphan_deadline = now + orphan_ms;
            }
        } else {
            orphan_armed = false;
        }
        if (orphan_armed && now >= orphan_deadline) {
            *cause = "orphan";  // 连接断了但端点还暂停 ⇒ 不抱着 GPU 停着
            return false;
        }
        const bool idle_on = idle_ms.count() > 0;
        const Clock::time_point idle_deadline = idle_on ? last_activity + idle_ms : Clock::time_point::max();
        if (idle_on && now >= idle_deadline) {
            *cause = "idle";  // 暂停态且无未决命令
            return false;
        }
        Clock::time_point until = idle_deadline;
        if (orphan_armed && orphan_deadline < until) until = orphan_deadline;
        cv_req.wait_until(lk, until);
    }
}

bool Control::Impl::serve(Context& ctx) {
    for (;;) {
        ControlOp op = ControlOp::None;
        std::string id;
        std::string path;
        bool for_frames = false;
        bool for_ms = false;
        std::uint64_t frames = 0;
        double ms = 0.0;
        int click_x = 0;
        int click_y = 0;
        MouseButton click_button = MouseButton::Left;
        {
            std::unique_lock<std::mutex> lk(mu);
            // ---- ⓪ 已武装的重停谓词先轮询（只有它 armed 时才会走到这条分支）----
            if (!has_req && watch.armed) {
                switch (pollWatchLocked(ctx)) {
                    case WatchPoll::Completed: {
                        // 谓词成立 ⇒ **重停**：回到暂停态；答复这条 `resume --for-*`。
                        // ★ `state_seq` 在这里**不再 +1**：它数的是**客户端可见的状态转移**，
                        //   而"一条 `resume --for-*` = 一次步进"（判据 4 的读数口径：每条命令 +1）。
                        //   离开暂停那一下已经 +1（下面 Resume 分支）⇒ 整条命令恰好 +1；
                        //   重停是这个命令的**完成**，不是新命令（下一次 `pause` 是幂等 +0）。
                        if (ctx.clock != nullptr) ctx.clock->freeze();
                        paused = true;
                        paused_at = ctx.paused_at;
                        watch = Watch{};
                        watching_flag.store(false, std::memory_order_release);
                        Reply rep = stateReply();
                        fillClock(ctx, &rep);
                        last_activity = Clock::now();
                        publishLocked(rep);
                        lk.unlock();
                        cv_req.notify_all();
                        // 重停后留在暂停态等后续命令（与 `pause` 同一条状态机）。
                        continue;
                    }
                    case WatchPoll::TimedOut: {
                        // ★ 被放弃的谓词必须能取消：清掉谓词、**不进暂停**、guest 继续跑。
                        watch = Watch{};
                        watching_flag.store(false, std::memory_order_release);
                        Reply rep;
                        rep.error = ControlError::Timeout;
                        rep.error_detail =
                            "resume --for-* 在超时内没有等到谓词（谓词已取消；guest 继续跑）";
                        publishLocked(rep);
                        lk.unlock();
                        cv_req.notify_all();
                        return false;
                    }
                    case WatchPoll::Abandoned: {
                        // 客户端断了：清谓词、不发答复（没人等）、不进暂停，⛔ 之后"自己停住"。
                        watch = Watch{};
                        watching_flag.store(false, std::memory_order_release);
                        lk.unlock();
                        return false;
                    }
                    case WatchPoll::KeepRunning:
                        lk.unlock();
                        return false;  // ★ 放行 guest；调用方节流后再进来轮询
                }
            }
            if (!has_req) {
                std::string cause = "idle";
                if (!waitForCommandLocked(lk, &cause)) {
                    lk.unlock();
                    requestQuit(ctx, cause);  // 收口 / idle / orphan ⇒ 置停
                    return true;
                }
            }
            if (!has_req) {  // 收口竞态：没有命令可执行
                lk.unlock();
                requestQuit(ctx, "shutdown");
                return true;
            }
            op = req_op;
            id = req_id;
            for_frames = req_for_frames;
            for_ms = req_for_ms;
            frames = req_frames;
            ms = req_ms;
            path = req_path;
            click_x = req_x;
            click_y = req_y;
            click_button = req_button;
            req_for_frames = false;
            req_for_ms = false;
            req_path.clear();
            last_activity = Clock::now();
        }

        Reply rep;
        bool leave_paused = false;
        switch (op) {
            case ControlOp::Pause: {
                // ★ 幂等（父级 2026-11-16 裁决）：已暂停态再 `pause` ⇒ `ok:true`（⛔ `bad_request`），
                //   `state_seq` 不动（没有状态转移）。
                if (!paused) {
                    if (ctx.clock != nullptr) ctx.clock->freeze();  // ★ 冻结区间对 guest 不可见
                    paused = true;
                    paused_at = ctx.paused_at;
                    ++state_seq;  // ★ 进入暂停态严格 +1
                }
                rep = stateReply();
                break;
            }
            case ControlOp::Resume: {
                if (!paused) {
                    rep.error = ControlError::NotPaused;
                    rep.error_detail = "当前没有暂停（`resume` 只在暂停态合法；`pause` 是幂等的）";
                    break;
                }
                if (ctx.clock != nullptr) ctx.clock->unfreeze();
                paused = false;
                paused_at = "";
                ++state_seq;  // ★ 离开暂停态严格 +1
                // ★ **resume 的宿主即时动作**（`REQ-01M7JFDZK1SZW72SBAW6BC3DRT`）：
                //   暂停期间 `pumpEvents` 一次不跑 ⇒ 点/拖/关窗全排在 **Win32 队列**里（还没进 SDL 队列；
                //   下一次 `SDL_PumpEvents` 才转换进来），而 resume 会**放行 guest**
                //   （下面两条出口都放行：普通 resume 与 `--for-*`）。这一刻把"恢复时刻"记到宿主 sink 上
                //   ⇒ 早于它的 OS 输入丢弃、早于它的 ESC 当宿主 stop（判据是纯函数 `classifyStaleEvent`）。
                //   ⚠ 关窗拿不到陈旧戳（`SDL_SendWindowEvent` 置 0 ⇒ `SDL_PushEvent` 填当下）⇒ 不按陈旧处理，
                //     见 `REQ-01M7JNVD59RD49RSKD9N2S9TXW`。
                //   ⛔ 只在这里 arm：`pause` / `sample` / `screenshot` / `click` 都不离开暂停态；
                //     `requestQuit`（idle / orphan）虽然也清 paused，但它同时 `stop_requested = true` ⇒ 没有"首轮"。
                //   ★ 与 `for_*` 无关：`resume --for-ms/-frames` 同样放行 guest 一轮，那一轮同样会把洪水吃进来。
                if (ctx.host != nullptr) ctx.host->controlOnResume();
                if (for_frames || for_ms) {
                    // ★ 留下一条**重停谓词**：答复等它完成 / 超时；这一轮先放行 guest。
                    watch = Watch{};
                    watch.armed = true;
                    watch.for_frames = for_frames;
                    if (for_frames) {
                        watch.frame_target =
                            (ctx.host != nullptr ? ctx.host->controlPresentFrame() : 0) + frames;
                    } else if (ctx.clock != nullptr) {
                        watch.guest_100ns_target =
                            ctx.clock->now100ns() + static_cast<std::uint64_t>(ms * 10000.0);
                    }
                    watch.deadline = Clock::now() + toMs(opts.predicate_timeout_s);
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        consumeLocked();
                    }
                    watching_flag.store(true, std::memory_order_release);
                    cv_req.notify_all();
                    return false;  // 放行 guest；谓词由下一个汇点轮询
                }
                rep = stateReply();
                leave_paused = true;
                break;
            }
            case ControlOp::Sample: {
                // ★ `sample` = **暂停（若未停）** + 一份 POD 快照（主线程填，控制线程只序列化）。
                if (!paused) {
                    if (ctx.clock != nullptr) ctx.clock->freeze();
                    paused = true;
                    paused_at = ctx.paused_at;
                    ++state_seq;
                }
                rep = stateReply();
                if (ctx.host == nullptr) {
                    rep.error = ControlError::Unavailable;
                    rep.error_detail = "这个宿主没有接快照取数口（--control-root 之外的装配）";
                    break;
                }
                ctx.host->fillControlSnapshot(rep.snapshot);
                // 下面这些格由 Control 自己填（它们属于**状态机**，不是 guest 读数）。
                rep.snapshot.paused = paused;
                rep.snapshot.paused_at = paused_at;
                rep.snapshot.state_seq = state_seq;
                if (ctx.clock != nullptr) {
                    rep.snapshot.guest_ms = ctx.clock->nowMs();
                    // ★ **已结算**暂停毫秒（不含当前冻结区间）⇒ 同一暂停点连采两次逐位相同（判据 3b）。
                    rep.snapshot.paused_total_ms = ctx.clock->pausedCommittedMs();
                }
                rep.has_snapshot = true;
                break;
            }
            case ControlOp::Screenshot: {
                // ★ 与 `sample` **同一条**"若未暂停则停在该汇点"的路径（判据 5 / 设计 §4 的命令表：
                //   `screenshot` = 暂停 + 写最后呈现那一帧）。`paused_at` 由**实际停住的汇点**填
                //   （`ctx.paused_at`：块边界 `"block"` / Present 闸门 `"present"`）。
                if (!paused) {
                    if (ctx.clock != nullptr) ctx.clock->freeze();
                    paused = true;
                    paused_at = ctx.paused_at;
                    ++state_seq;  // 进入暂停态严格 +1（与 pause/sample 同一口径）
                }
                rep = stateReply();
                if (ctx.host == nullptr) {
                    rep.error = ControlError::Unavailable;
                    rep.error_detail = "这个宿主没有接截图取数口";
                    break;
                }
                std::string io_err;
                const ControlShotStatus st = ctx.host->controlScreenshot(path, rep.shot, &io_err);
                if (st == ControlShotStatus::NoFrameYet) {
                    // ★ "还没有帧可写"是**数据没到**，不是 IO 失败（判据 5 的客户端契约）。
                    rep.error = ControlError::Unavailable;
                    rep.error_detail = io_err.empty() ? "no frame presented yet" : io_err;
                    break;
                }
                if (st != ControlShotStatus::Ok) {
                    rep.error = ControlError::Io;
                    rep.error_detail = io_err.empty() ? "截图写入失败（没有给出原因）" : io_err;
                    break;
                }
                rep.has_screenshot = true;
                break;
            }
            case ControlOp::Click: {
                // ★ 判据 3：`click` **只**在暂停态入队（未暂停 ⇒ `not_paused`；⛔ 静默丢）。
                if (!paused) {
                    rep.error = ControlError::NotPaused;
                    rep.error_detail = "当前没有暂停（`click` 只在暂停态入队：先 `pause` 再投）";
                    break;
                }
                // ★ 落点 = **同一个** `InputPlan`（`WindowManager` 的非拥有指针）⇒ 施加点是它的
                //   **下一个 guest 读点**（`atPoint` 里先于配方当步）。⛔ 控制线程 / 本汇点都**不**碰
                //   guest 状态；⛔ 也不推进配方的 `next_`（判据 2）。
                InputPlan* plan = ctx.windows != nullptr ? ctx.windows->inputPlan() : nullptr;
                if (plan == nullptr) {
                    rep.error = ControlError::Unavailable;
                    rep.error_detail = "这个宿主没有接输入投递层（没有可施加 click 的 guest 读点）";
                    break;
                }
                if (!plan->enqueueClick(click_x, click_y, click_button)) {
                    // ★ 判据 5：队列**有界**；超界要**响亮**（⛔ 静默丢 / ⛔ 无界扩容）。
                    rep.error = ControlError::BadRequest;
                    char msg[128];
                    std::snprintf(msg, sizeof(msg),
                                  "立即队列已满（上限 %zu 条；等下一个 guest 读点消费后再投）",
                                  InputPlan::kImmediateClickCapacity);
                    rep.error_detail = msg;
                    break;
                }
                rep = stateReply();
                rep.has_click = true;
                rep.click_x = click_x;
                rep.click_y = click_y;
                rep.click_button = click_button == MouseButton::Right ? "right" : "left";
                rep.click_queued = plan->queuedClicks();
                break;
            }
            case ControlOp::Quit:
            default: {
                if (paused) {
                    if (ctx.clock != nullptr) ctx.clock->unfreeze();
                    paused = false;
                    paused_at = "";
                    ++state_seq;
                }
                rep = stateReply();
                rep.quit = true;
                break;
            }
        }

        if (op != ControlOp::Quit) fillClock(ctx, &rep);
        // ★ `sample` 的 data 层也报**已结算**暂停毫秒：它是"快照"的一部分，同一暂停点连采两次
        //   必须逐字节相同（判据 3b）。pause/resume 的答复仍用含当前区间的 S1 口径。
        if (op == ControlOp::Sample && ctx.clock != nullptr) {
            rep.paused_total_ms = ctx.clock->pausedCommittedMs();
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            publishLocked(rep);
            if (op == ControlOp::Quit) requested_by = "client";
        }
        cv_req.notify_all();
        if (op == ControlOp::Quit) {
            if (ctx.stop_requested != nullptr) *ctx.stop_requested = true;
            if (ctx.windows != nullptr) ctx.windows->postQuit(ctx.quit_code);
            return true;
        }
        if (leave_paused) return false;
        // `pause` / `sample` ⇒ 留在暂停态等后续命令（idle / orphan 计时从这一刻起）。
    }
}

// =====================================================================================
// 门面转发
// =====================================================================================
Control::Control() : impl_(std::make_unique<Impl>()) {}
Control::~Control() {
    // 未显式 `shutdown()` 就析构 ⇒ 仍然要 join（⛔ 让 `std::thread` 析构触发 terminate）。
    if (impl_ && impl_->started) impl_->shutdownAll("stopped", nullptr);
}

bool Control::start(const Options& opts, std::string* err) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    if (impl_->started) {
        if (err != nullptr) *err = "控制端点已经启动过（⛔ 不支持二次 start）";
        return false;
    }
    return impl_->startAll(opts, err);
}

void Control::shutdown(const std::string& run_reason, const char* requested_by) {
    if (impl_) impl_->shutdownAll(run_reason, requested_by);
}

bool Control::pending() const { return impl_ && impl_->pending(); }

bool Control::watching() const {
    return impl_ && impl_->watching_flag.load(std::memory_order_acquire);
}

bool Control::serve(Context& ctx) { return impl_ && impl_->serve(ctx); }

bool Control::started() const { return impl_ && impl_->started; }

std::string Control::localEndpoint() const {
    if (!impl_ || impl_->port == 0) return std::string();
    return impl_->local_host + ":" + std::to_string(impl_->port);
}

std::uint16_t Control::localPort() const { return impl_ ? impl_->port : 0; }
const std::string& Control::nonce() const {
    static const std::string kEmpty;
    return impl_ ? impl_->nonce : kEmpty;
}
const std::string& Control::token() const {
    static const std::string kEmpty;
    return impl_ ? impl_->token : kEmpty;
}
const std::string& Control::controlFilePath() const {
    static const std::string kEmpty;
    return impl_ ? impl_->ctl_path : kEmpty;
}

// =====================================================================================
// 枚举名字（协议字段；⛔ 不写自由文本）
// =====================================================================================
const char* controlErrorName(ControlError e) {
    switch (e) {
        case ControlError::None: return "none";
        case ControlError::BadRequest: return "bad_request";
        case ControlError::UnknownOp: return "unknown_op";
        case ControlError::NotPaused: return "not_paused";
        case ControlError::Busy: return "busy";
        case ControlError::Unavailable: return "unavailable";
        case ControlError::Timeout: return "timeout";
        case ControlError::Io: return "io";
        case ControlError::Internal: return "internal";
        case ControlError::ShuttingDown: return "shutting_down";
    }
    return "internal";
}

const char* controlOpName(ControlOp op) {
    switch (op) {
        case ControlOp::None: return "none";
        case ControlOp::Pause: return "pause";
        case ControlOp::Resume: return "resume";
        case ControlOp::Sample: return "sample";
        case ControlOp::Screenshot: return "screenshot";
        case ControlOp::Click: return "click";
        case ControlOp::Quit: return "quit";
    }
    return "none";
}

// =====================================================================================
// `sample` 的 JSON 序列化（**纯函数**；`control_snapshot.h` 的声明）
// ★ 为什么单独抽出来：判据 3b 的机制面（"同一暂停点连采两次逐位相同" = ⛔ 没有自增 / 墙钟字段）
//   要能在**不碰 GPU / 不起进程**的 pure CTest 里断言（`tests/control_snapshot.cpp`）。
// ★ 键序固定（nlohmann 的 `json` 对象缺省按 key 排序）⇒ 同输入逐字节同输出。
// =====================================================================================
std::string controlSnapshotJson(const ControlSnapshot& s) {
    json j;
    j["paused"] = s.paused;
    j["paused_at"] = s.paused_at;
    j["state_seq"] = s.state_seq;
    j["insns"] = s.insns;
    j["present"] = s.present;
    {
        char hash[24];
        std::snprintf(hash, sizeof(hash), "0x%016llX", static_cast<unsigned long long>(s.last_hash));
        j["screen"] = json{{"frame", s.frame}, {"last_hash", hash}};
    }
    j["clock"] = json{{"guest_ms", s.guest_ms}, {"paused_total_ms", s.paused_total_ms}};
    json sc;
    sc["available"] = s.script.available;
    sc["why"] = s.script.available ? json(nullptr)
                                   : json(s.script.why != nullptr ? s.script.why : "identity-invalid");
    sc["script"] = s.script.name.empty() ? json(nullptr) : json(s.script.name);
    sc["script_id"] = s.script.script_id;
    sc["cur"] = s.script.cur;
    sc["script_sha"] = s.script.sha.empty() ? json(nullptr) : json(s.script.sha);
    sc["script_sha_why"] = s.script.sha_why != nullptr ? s.script.sha_why : "no_identity";
    sc["script_sha_source"] = "host-file";
    if (s.script.available) {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%08X", s.script.pos);
        sc["pos"] = s.script.pos;
        sc["pos_hex"] = hex;
        sc["pos_dwords"] = s.script.pos_dwords;
    } else {
        sc["pos"] = nullptr;
        sc["pos_hex"] = nullptr;
        sc["pos_dwords"] = nullptr;
    }
    // ★ `header_len` = `kScriptHeaderLen` 的编译期常量（⛔ 不是"从头里读出来的"；恒真，不作判据）。
    sc["header_len"] = s.script.header_len;
    j["script"] = sc;
    return j.dump();
}

}  // namespace lift
