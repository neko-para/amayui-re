// emulator-lift · **AOT 控制层端点**的 PIMPL 门面（只读控制面：停住 / 继续 / 收口）
//
// ★ 本头**故意**很薄：⛔ 不 include beast / nlohmann / machine.h / aot_host.h。
//   唯一 include `<boost/beast/websocket.hpp>` + `<nlohmann/json.hpp>` 的 TU 是
//   `src/runtime/control_ws.cpp`（design-ws §2.1 的硬约束：beast 头会让一个 TU 多花 3–10 s）。
// ★ 线程归属（design-ws §2.3）：
//   · **控制线程**（一条）= 唯一碰 socket / beast 对象 / JSON 的线程；
//   · **主线程**（`AotHost::run` 的 `for(;;)`）= 唯一碰 guest 状态 / 时钟 / `WindowManager` 的线程。
//     命令在控制线程解析成枚举 + 参数，主线程在汇点里用 `serve()` 执行并回结果。
// ★ 热路径成本：`pending()` = **一次 atomic load**；调用方先判 `Control* == nullptr`
//   ⇒ 不带 `--control-root` 的进程连这次原子读都不付（与 `presenter_` 的空指针判据同形）。
//
// ★ 能力面（**真源** = `runtime/capability.h` 的 `kControlCapabilities`；⛔ 本注释不再维护清单）：
//   `hello.capabilities`、`controlOpName` 与启动 readout 的 `caps=` 都从那张表生成（⛔ 不要在任何一处
//   再抄一份 —— `tools/test/capability-matrix.test.mjs` 会红）。
//   `capabilities` 只列**真的实现了**的（`hello` 如实报这一份）。
//   ★ `move` = **纯 move**（只把光标放到参考坐标，⛔ 不按键）；`click` = **暂停态入队**，由**下一个 guest
//     读点**（`ControlInput::atPoint`）施加 —— 控制线程与 `serve()` 都**不碰** guest 状态。
//   `key` 本轮**不做** ⇒ `unknown_op`；`wait` 是 `emu-ctl` 的**客户端糖**（= `resume --for-ms`），
//   ⛔ 不是服务端 op、不进能力表。
//   ★ `global-get` / `global-set` = **哑原语**：按 id + 显式 kind 读写一格 script global。
//     寻址 / ENC-DEC / 类型编码在 `runtime/script_globals.h`（**AGE 引擎层**，可替换；QJS 侧将来共用）；
//     本层只做「JSON → 参数校验（含 0x 十六进制口径）→ 调模块 → 组返回」。
//     ⛔ 不猜类型 / ⛔ 不挑时机 / ⛔ 不重试 / ⛔ 不写后校验 / ⛔ 不为任何 global 开特例。
// ★ 快照的值类型与"主线程取数口"在 `runtime/control_snapshot.h`（只含值，⛔ 无 guest 指针 /
//   无 beast / 无 nlohmann）⇒ 控制线程拿到的是一份拷贝。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "runtime/control_snapshot.h"

namespace lift {

class Clock;
class WindowManager;

// 协议错误码（**枚举**，⛔ 不写自由文本；名字与 `design-ws.md` §2.4 的表一一对应）。
enum class ControlError : std::uint8_t {
    None = 0,
    BadRequest,   // 不是 JSON / 没有 op / 参数非法
    UnknownOp,    // op 不认识（⛔ 不静默）
    NotPaused,    // 只在暂停态才合法的命令（`resume` / M1 的 `click`）
    Busy,         // 同一时刻已有未决命令
    Unavailable,  // 端点这一侧不具备该能力（数据缺失不算：那是数据不是错误）
    Timeout,      // 命令等不到汇点
    Io,           // 写截图等 IO 失败
    Internal,     // 宿主内部不一致
    ShuttingDown, // 收口中（未决请求的兜底答复）
};
const char* controlErrorName(ControlError e);

// 命令（控制线程解析成枚举；主线程执行）。
//   ★ `Move`（2026-11-19）= **纯 move**（只把光标放到参考坐标，⛔ 不按键）：`click` 的第一段
//     必须能与按下分开表达 —— "先悬停、过一个读点、再按"是引擎位置锁存（`+0x74A4`）要求的形状。
enum class ControlOp : std::uint8_t {
    None = 0,
    Pause,
    Resume,
    Sample,
    Screenshot,
    Move,
    Click,
    GlobalGet,  // 读一格 script global（id + 显式 kind；哑原语）
    GlobalSet,  // 写一格 script global（只写一个数据槽；⛔ 不写 cpu_）
    Quit,
};
const char* controlOpName(ControlOp op);

// =====================================================================================
// Control —— 端点（构造即可用；`start()` 之后才有 socket / 线程 / 端点文件）
// =====================================================================================
class Control {
public:
    // `--control-*` 的宿主侧配置（CLI 解析在 `aot_main.cpp`）。
    struct Options {
        std::string root;                        // 端点文件的目录（**必须显式给**；缺省 = 不启动控制层）
        std::uint16_t port = 0;                  // 0 = 由系统给
        double idle_timeout_s = 300.0;           // 暂停态且无未决命令的超时
        double orphan_timeout_s = 10.0;          // 连接断了但端点还暂停 ⇒ 自行 quit
        // ★ **被放弃的重停谓词**的超时（`resume --for-ms` / `--for frames` 等不到汇点时不能挂死）：
        //   缺省 30 s；到点回 `error.code:"timeout"` **并清掉谓词**（⛔ 之后"自己停住"）。
        //   用**宿主墙钟**（它是"客户端还能等多久"的口径，不是 guest 时钟）。
        double predicate_timeout_s = 30.0;
        std::string session;                     // 空 = `run-<pid>-<utc>`
        std::string exe_sha256;                  // 执行源的 sha256（与 pin 同源）
        std::string shape;                       // `LIFT_AOT_SHAPE` 的读数
        std::string present = "hidden";          // "visible" | "hidden"（取自 `describe()`，⛔ 不由 --present 推）
    };

    // 主线程一侧的命令上下文：只有 `serve()` 要动的宿主对象（⛔ 不含任何 guest 指针）。
    //   · `clock`   = 暂停期间冻结 guest 时间（`Clock::freeze/unfreeze`）；
    //   · `windows` = `quit` 的 `postQuit(0)`（同一个交付点）；
    //     ★ M1 的 `click` / `move` **入队点**也在这里：`windows->controlInput()`
    //     （`WindowManager` 的非拥有指针）⇒ 入的是**同一个** `ControlInput`，施加点是它自己的
    //     guest 读点（⛔ 控制层不施加）；
    //   · `stop_requested` = 置停（主线程每个块边界读；本轮由 `serve()` 的返回值同时表达）；
    //   · `paused_at` = 本汇点的名字（`"block"` / `"present"`）—— 由**调用点**填，⛔ Control 不猜；
    //   · `host`     = 快照 / 截图 / resume / **global 读写**的宿主取数口（S2/S3/S4；nullptr ⇒ 该能力 `unavailable`）。
    //     ★ `global-set` 是唯一会**写 guest 数据槽**的命令：`serve()` 在主线程块边界调
    //       `ControlHostSide::controlGlobalSet`（→ `runtime/script_globals.h`）；⛔ 不写 cpu_、
    //       ⛔ 不改执行流，下一块自然重新从内存加载 ⇒ 写完立刻对引擎可见。
    struct Context {
        Clock* clock = nullptr;
        WindowManager* windows = nullptr;
        bool* stop_requested = nullptr;
        std::uint32_t quit_code = 0;
        const char* paused_at = "block";
        ControlHostSide* host = nullptr;
    };

    Control();
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;

    // ① bind（只 127.0.0.1、不开 reuse_addr）+ `ctl.<pid>.<nonce8>.json`（listen 成功后才写）+ 起控制线程。
    //    失败 ⇒ false + `err`（**点名端口**；⛔ 不静默换端口、⛔ 不降级成"没有控制层"）。
    bool start(const Options& opts, std::string* err);
    // ② 收口：发 `bye`（`run_reason` = 停机原因；⛔ 不等于命令名）→ 关 socket / accept → join → 删端点文件。
    //    `run_reason` 缺省 = "stopped"。
    //    `requested_by`（可选）= 谁请求的收口（`client` / `idle` / `orphan`）；nullptr = 保留命令侧
    //    已经记下的那一个（`quit` / idle / orphan）。窗口关闭这类**宿主**停机由调用方显式传 `client`
    //    —— `bye` 两个字段各管一件事（reason = run 为什么结束；requested_by = 谁请求的）。
    void shutdown(const std::string& run_reason = std::string(), const char* requested_by = nullptr);

    // ③ **热路径唯一入口**：一次 atomic load（有未决命令 ⇒ true）。
    bool pending() const;
    // ③' 同一个热路径上的第二次 atomic load：**有已武装的重停谓词**（`resume --for-*`）。
    //    调用方据此把汇点节流到已有的检查点（⛔ 每块边界都进来做一次 steady_clock 读）。
    bool watching() const;

    // ④ 在主线程汇点里执行命令。返回 true = **已有置停请求**（调用方立即收口，⛔ 不要再进 next 块）。
    //    · 第一条 `pause` 之后本函数**留在暂停态**等后续命令（`resume` / `quit` / idle / orphan）；
    //    · `resume` 放行 ⇒ 返回 false（主线程继续执行 guest）；
    //    · 调用点保证进入时 `pending() == true`。
    bool serve(Context& ctx);

    // ---- 读数（测试 / 日志；⛔ 不进协议）----
    bool started() const;
    // 实际绑定端点（listen 成功后才有值）：`"127.0.0.1:<port>"`（判据 10 的 `local_endpoint()` 面；
    // ⛔ 不用 lsof / netstat）。
    std::string localEndpoint() const;
    std::uint16_t localPort() const;
    const std::string& nonce() const;
    const std::string& token() const;
    const std::string& controlFilePath() const;
    // ★ `state_seq` 的只读口（暂停态变化的唯一读数列）。
    //   语义不变：只在进入 / 离开暂停时 +1（`control_snapshot.h` 的头注）。**只在主线程读**
    //   （写入点也都在主线程的 `serve()` / `requestQuit`）⇒ 不加锁；没启动 / 没暂停过 ⇒ 0。
    std::uint64_t stateSeq() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lift
