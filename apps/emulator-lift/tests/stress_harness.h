// emulator-lift · **LIFT_STRESS 四场景共用件**（只给 `tests/stress_*.cpp` 用；默认档不引它）
//
// 这一份只做"采集与落盘"，⛔ 不做任何判据：判据留在各场景的 `main` 里，好让"红了意味着什么"
// 与"阈值由哪条规则 + 哪条读数得出"写在同一个文件里（AGENTS.md §10：读数不是断言）。
//
// 组成（照 `tools/lift.mjs` 的 `.cache/lift-bench/*.json` 形状）：
//   ① `runRedirected`：起真子进程并**只**把 stdout+stderr 重定向到文件（⛔ 不走管道：
//      受限沙箱里捕获管道会 EPERM —— 与 `tests/feeder_seg_e2e.cpp` / `tests/movie_logo.cpp` 同一口径）；
//   ② `captureCommand`：同一套重定向机制起任意命令行（只给 `git status --porcelain` 用来算工作树指纹）；
//   ③ `Sha256`：exe / 脚本 / 工作树指纹的哈希（**自己算**，不依赖 `certutil` / `shasum` 在不在）；
//   ④ `Json`：最小 JSON 构造器（对象 / 数组 / 数 / 串 / 布尔），只为本文件服务；
//   ⑤ 统计：`median` + `p95`（定义写在 `statsOf` 上；⛔ 不用均值、⛔ 不挑最好一次）；
//   ⑥ 日志读点：`[frame]` 行 / `[input-script]` 打点 / `key=value` 两族（含数字与浮点）；
//   ⑦ 每次 run 的**全新 `--appdata`**、UTC 时间戳、CMake 构建类型 / 负载档 / 宿主三件事。
//
// ★ 平台口径：这四条用例的阈值是**在这台 Windows x64 / RelWithDebInfo 机器上量出来的**
//   （`§A1.0` 的 N=3 口径 + `§C-9` 明说平台不对称 ⇒ ⛔ 不把 macOS 的数当 Windows 的门槛）。
//   各场景在 `main` 开头对"非 Windows / 非 RelWithDebInfo"自己 `LIFT_SKIP`(77)。
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "check.h"
#include "cmdline.h"  // `lift_test::quoteArg`（命令行拼装只有一份实现，见 tests/cmdline.h）

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lift_stress {

namespace fs = std::filesystem;

// =====================================================================================
// ① 子进程：stdout+stderr 都落到**文件**（⛔ 不捕获管道）
// =====================================================================================
struct ChildResult {
    int exit_code = -1;
    bool started = false;
    double wall_ms = 0.0;  // 宿主墙钟：从 CreateProcess 返回到进程收口（S1 的判据就是它）
};

// ★ 命令行拼装只有一份实现（`tests/cmdline.h`）：`--game-dir` / `--appdata` 都可能带尾反斜杠。
inline std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

#if defined(_WIN32)
// ★ 重定向句柄必须**可继承**，且只给子进程（`bInheritHandle=TRUE` + `CreateProcessW(TRUE)`）。
inline HANDLE openReducedLog(const fs::path& log_path) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    return ::CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

inline ChildResult runRedirected(const fs::path& exe, const std::vector<std::string>& args,
                                 const fs::path& log_path, unsigned timeout_ms = 0) {
    ChildResult r;
    const HANDLE h = openReducedLog(log_path);
    if (h == INVALID_HANDLE_VALUE) return r;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h;
    si.hStdError = h;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = lift_test::quoteArg(exe.wstring());
    for (const std::string& a : args) cmd += L" " + lift_test::quoteArg(std::wstring(a.begin(), a.end()));
    const auto t0 = std::chrono::steady_clock::now();
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     nullptr, &si, &pi);
    ::CloseHandle(h);
    if (!ok) return r;
    r.started = true;
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, timeout_ms == 0 ? INFINITE : timeout_ms);
    if (waited != WAIT_OBJECT_0) {
        ::TerminateProcess(pi.hProcess, 991u);
        ::WaitForSingleObject(pi.hProcess, 10000u);
    }
    r.wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    DWORD code = 0xFFFFFFFFu;
    ::GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return r;
}

// 起一条**命令行原文**（PATH 里找 exe），stdout+stderr 重定向到文件。只给 `git` 用。
inline bool captureCommand(const std::string& cmdline, const fs::path& log_path, int timeout_ms = 120000) {
    const HANDLE h = openReducedLog(log_path);
    if (h == INVALID_HANDLE_VALUE) return false;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h;
    si.hStdError = h;
    PROCESS_INFORMATION pi{};
    std::wstring cmd(cmdline.begin(), cmdline.end());
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     nullptr, &si, &pi);
    ::CloseHandle(h);
    if (!ok) return false;
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));
    DWORD code = 0xFFFFFFFFu;
    if (waited != WAIT_OBJECT_0) {
        ::TerminateProcess(pi.hProcess, 991u);
        ::WaitForSingleObject(pi.hProcess, 10000u);
    } else {
        ::GetExitCodeProcess(pi.hProcess, &code);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return waited == WAIT_OBJECT_0 && code == 0;
}

// 子进程环境变量（`_putenv_s`）：本进程与它之后起的所有子进程都看得见。
inline void setEnv(const char* name, const char* value) { _putenv_s(name, value); }
inline void clearEnv(const char* name) { _putenv_s(name, ""); }
#else
// POSIX 分支只为"编译得过"存在：四条用例在 `main` 里就 SKIP（阈值口径 = windows-x64）。
inline ChildResult runRedirected(const fs::path&, const std::vector<std::string>&, const fs::path&,
                                 unsigned = 0) {
    return ChildResult{};
}
inline bool captureCommand(const std::string&, const fs::path&, int = 0) { return false; }
inline void setEnv(const char*, const char*) {}
inline void clearEnv(const char*) {}
#endif

// =====================================================================================
// ② SHA-256（自算；只用来做"同一份二进制 / 同一棵树才可比"的指纹）
// =====================================================================================
class Sha256 {
  public:
    void update(const void* data, std::size_t len) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        total_ += len;
        while (len > 0) {
            const std::size_t take = std::min<std::size_t>(len, 64 - buf_len_);
            std::memcpy(buf_ + buf_len_, p, take);
            buf_len_ += take;
            p += take;
            len -= take;
            if (buf_len_ == 64) {
                transform(buf_);
                buf_len_ = 0;
            }
        }
    }

    // 收尾；返回 64 位十六进制小写串（反复调用只有第一次有意义）。
    std::string hexDigest() {
        const std::uint64_t bits = total_ * 8;
        const std::uint8_t pad = 0x80;
        update(&pad, 1);
        const std::uint8_t zero = 0;
        while (buf_len_ != 56) update(&zero, 1);
        std::uint8_t len_be[8];
        for (int i = 0; i < 8; ++i) len_be[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        update(len_be, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h_[i]);
        return std::string(out, 64);
    }

  private:
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void transform(const std::uint8_t* p) {
        static const std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
            0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
            0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
            0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
            0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
            0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
            0xc67178f2u};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(p[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        std::uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += hh;
    }

    std::uint32_t h_[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                           0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::uint64_t total_ = 0;
    std::uint8_t buf_[64]{};
    std::size_t buf_len_ = 0;
};

inline std::string sha256Bytes(const std::string& bytes) {
    Sha256 s;
    s.update(bytes.data(), bytes.size());
    return s.hexDigest();
}

inline std::string sha256File(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    Sha256 s;
    char buf[64 * 1024];
    while (in) {
        in.read(buf, sizeof(buf));
        const std::streamsize got = in.gcount();
        if (got > 0) s.update(buf, static_cast<std::size_t>(got));
    }
    return s.hexDigest();
}

// =====================================================================================
// ③ JSON（最小构造器；只为本文件服务）
// =====================================================================================
class Json {
  public:
    Json() = default;
    static Json number(double v) {
        Json j;
        j.kind_ = Kind::Num;
        j.num_ = v;
        return j;
    }
    static Json u64(std::uint64_t v) { return number(static_cast<double>(v)); }
    static Json text(std::string v) {
        Json j;
        j.kind_ = Kind::Str;
        j.str_ = std::move(v);
        return j;
    }
    static Json boolean(bool v) {
        Json j;
        j.kind_ = Kind::Bool;
        j.bool_ = v;
        return j;
    }
    static Json null() { return Json{}; }
    static Json array() {
        Json j;
        j.kind_ = Kind::Arr;
        return j;
    }
    static Json object() {
        Json j;
        j.kind_ = Kind::Obj;
        return j;
    }

    // 对象：插入 / 覆盖（同键后者胜）。
    Json& set(const std::string& key, Json v) {
        if (kind_ != Kind::Obj) {
            kind_ = Kind::Obj;
            obj_.clear();
        }
        for (auto& kv : obj_) {
            if (kv.first == key) {
                kv.second = std::move(v);
                return *this;
            }
        }
        obj_.emplace_back(key, std::move(v));
        return *this;
    }
    Json& operator[](const std::string& key) {
        if (kind_ != Kind::Obj) {
            kind_ = Kind::Obj;
            obj_.clear();
        }
        for (auto& kv : obj_) {
            if (kv.first == key) return kv.second;
        }
        obj_.emplace_back(key, Json{});
        return obj_.back().second;
    }
    // 数组：末尾追加。
    Json& push(Json v) {
        if (kind_ != Kind::Arr) {
            kind_ = Kind::Arr;
            arr_.clear();
        }
        arr_.push_back(std::move(v));
        return *this;
    }
    std::size_t size() const { return kind_ == Kind::Arr ? arr_.size() : obj_.size(); }
    // ★ 数组最后一项的**引用**（`push` 之后取回刚塞进去的那个，好继续往里填字段）。
    //   注意：引用在**下一次** `push` 之后就失效 ⇒ 填完这一遍再 push 下一遍。
    Json& back() { return arr_.back(); }

    std::string dump() const {
        std::string out;
        dumpInto(out, 0);
        out += "\n";
        return out;
    }

  private:
    enum class Kind { Null, Bool, Num, Str, Arr, Obj };

    static std::string escape(const std::string& s) {
        std::string o;
        o.reserve(s.size() + 8);
        for (const char c : s) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                        o += b;
                    } else {
                        o += c;
                    }
            }
        }
        return o;
    }

    void dumpInto(std::string& out, int indent) const {
        const std::string pad(static_cast<std::size_t>(indent + 2), ' ');
        const std::string pad_end(static_cast<std::size_t>(indent), ' ');
        switch (kind_) {
            case Kind::Null: out += "null"; return;
            case Kind::Bool: out += bool_ ? "true" : "false"; return;
            case Kind::Num: {
                char b[64];
                if (std::fabs(num_) < 1e15 && num_ == std::floor(num_)) {
                    std::snprintf(b, sizeof(b), "%.0f", num_);
                } else {
                    std::snprintf(b, sizeof(b), "%.6f", num_);
                }
                out += b;
                return;
            }
            case Kind::Str: out += "\"" + escape(str_) + "\""; return;
            case Kind::Arr:
                if (arr_.empty()) {
                    out += "[]";
                    return;
                }
                out += "[\n";
                for (std::size_t i = 0; i < arr_.size(); ++i) {
                    out += pad;
                    arr_[i].dumpInto(out, indent + 2);
                    if (i + 1 < arr_.size()) out += ",";
                    out += "\n";
                }
                out += pad_end + "]";
                return;
            case Kind::Obj:
                if (obj_.empty()) {
                    out += "{}";
                    return;
                }
                out += "{\n";
                for (std::size_t i = 0; i < obj_.size(); ++i) {
                    out += pad + "\"" + escape(obj_[i].first) + "\": ";
                    obj_[i].second.dumpInto(out, indent + 2);
                    if (i + 1 < obj_.size()) out += ",";
                    out += "\n";
                }
                out += pad_end + "}";
                return;
        }
    }

    Kind kind_ = Kind::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<std::pair<std::string, Json>> obj_;
    std::vector<Json> arr_;
};

// =====================================================================================
// ④ 统计：中位 + p95（⛔ 不用均值 / ⛔ 不挑最好一次）
// =====================================================================================
struct Stats {
    double median = 0.0;
    double p95 = 0.0;
    double min = 0.0;
    double max = 0.0;
    std::size_t n = 0;
};

/**
 * p95 的定义（写死在这里，⛔ 不随场景漂）：升序后取第 `floor(0.95*(n-1))` 个（0 基）。
 * ⇒ n=1211 时是第 1150 个；n=3 时退化成"第二个"（= 中位），这是小样本的必然结果，⛔ 不要读成"p95 很稳"。
 * ⛔ 不用均值：`§0.2 R1` 明说只钉中位 / p95。
 */
inline Stats statsOf(std::vector<double> v) {
    Stats s;
    s.n = v.size();
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    const std::size_t mid = v.size() / 2;
    s.median = (v.size() % 2 == 1) ? v[mid] : (v[mid - 1] + v[mid]) / 2.0;
    const std::size_t p95_at = static_cast<std::size_t>(std::floor(0.95 * static_cast<double>(v.size() - 1)));
    s.p95 = v[std::min(p95_at, v.size() - 1)];
    s.min = v.front();
    s.max = v.back();
    return s;
}

// 计数类（R2）：中位 / 极差 —— ⛔ 不用均值，但计数类本来就不该有噪声。
inline std::uint64_t medianU64(std::vector<std::uint64_t> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t mid = v.size() / 2;
    return (v.size() % 2 == 1) ? v[mid] : (v[mid - 1] + v[mid]) / 2;
}

// =====================================================================================
// ⑤ 日志读点
// =====================================================================================
inline std::vector<std::string> linesStartingWith(const std::string& text, const std::string& prefix) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(prefix, 0) == 0) out.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

// 第一条以 `prefix` 开头的行（没有 ⇒ 空串）。
inline std::string lineStartingWith(const std::string& text, const std::string& prefix) {
    const std::vector<std::string> all = linesStartingWith(text, prefix);
    return all.empty() ? std::string() : all.front();
}

inline bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// `key` 后面跟一个数字（十进制）。`key` 要带分隔符一起给（如 `"skipped="` / `"skipped "`）。
inline bool readU64(const std::string& line, const std::string& key, std::uint64_t* out) {
    const std::size_t at = line.find(key);
    if (at == std::string::npos) return false;
    const char* begin = line.c_str() + at + key.size();
    char* end = nullptr;
    const unsigned long long v = std::strtoull(begin, &end, 10);
    if (end == begin) return false;
    *out = static_cast<std::uint64_t>(v);
    return true;
}

inline bool readDouble(const std::string& line, const std::string& key, double* out) {
    const std::size_t at = line.find(key);
    if (at == std::string::npos) return false;
    const char* begin = line.c_str() + at + key.size();
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin) return false;
    *out = v;
    return true;
}

// 一行 `[frame] n=… wall=…ms insns=…`（`LIFT_FRAME_TRACE=1` 时逐帧打）。
struct FrameRow {
    std::uint64_t n = 0;
    double wall_ms = 0.0;
    std::uint64_t insns = 0;
};

inline std::vector<FrameRow> parseFrames(const std::string& log) {
    std::vector<FrameRow> out;
    for (const std::string& line : linesStartingWith(log, "[frame] ")) {
        FrameRow f;
        if (!readU64(line, "n=", &f.n)) continue;
        if (!readDouble(line, "wall=", &f.wall_ms)) continue;
        if (!readU64(line, "insns=", &f.insns)) continue;
        out.push_back(f);
    }
    return out;
}

// 一行 `[input-script] frame=… guest_ms=… insns=… <动作>`（打点；`wait 0 insn` 就是时间戳）。
struct ScriptMark {
    std::uint64_t frame = 0;
    std::uint64_t guest_ms = 0;
    std::uint64_t insns = 0;
};

inline std::vector<ScriptMark> parseScriptMarks(const std::string& log) {
    std::vector<ScriptMark> out;
    for (const std::string& line : linesStartingWith(log, "[input-script] ")) {
        ScriptMark m;
        if (!readU64(line, "frame=", &m.frame)) continue;
        if (!readU64(line, "guest_ms=", &m.guest_ms)) continue;
        if (!readU64(line, "insns=", &m.insns)) continue;
        out.push_back(m);
    }
    return out;
}

// 去掉"同一 tick 的连续重复点"（脚本尾部 `quit` 与最后一个 `wait` 会同时到期）—— 与
// `tools/lift.mjs` 的 `parseBenchLog` 同一条口径。
inline std::vector<ScriptMark> dedupeMarks(std::vector<ScriptMark> marks) {
    std::vector<ScriptMark> out;
    for (const ScriptMark& m : marks) {
        if (!out.empty() && out.back().guest_ms == m.guest_ms && out.back().insns == m.insns) continue;
        out.push_back(m);
    }
    return out;
}

// =====================================================================================
// ⑥ 时间 / 目录 / 宿主
// =====================================================================================
inline std::tm utcNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

// ISO-8601（`2026-11-14T02:45:00Z`）。JSON 的 `timestamp` 用它。
inline std::string utcIso8601() {
    const std::tm tm = utcNow();
    char b[32];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

// 文件名用的紧凑戳（`20261114T024500Z`）—— 与 `tools/lift.mjs` 的 `stamp` 同一形状。
inline std::string utcStamp() {
    const std::tm tm = utcNow();
    char b[32];
    std::snprintf(b, sizeof(b), "%04d%02d%02dT%02d%02d%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

// 每次 run 一个**全新** `--appdata`（`defects.md:377-378`：带上上一轮存档会让 insns 漂）。
inline fs::path freshDir(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

// 宿主 CPU 忙占比（负载档）：`GetSystemTimes` 采两次、间隔 ~300 ms。
// ⛔ 不是负载平均值（Windows 没有 loadavg）；它是"这一刻宿主有多少核在忙"的采样值。
inline bool cpuBusyPercent(double* out) {
#if defined(_WIN32)
    auto sample = [](std::uint64_t* idle, std::uint64_t* kernel, std::uint64_t* user) {
        FILETIME fi{}, fk{}, fu{};
        if (!::GetSystemTimes(&fi, &fk, &fu)) return false;
        auto to64 = [](const FILETIME& f) {
            return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        *idle = to64(fi);
        *kernel = to64(fk);
        *user = to64(fu);
        return true;
    };
    std::uint64_t i0 = 0, k0 = 0, u0 = 0, i1 = 0, k1 = 0, u1 = 0;
    if (!sample(&i0, &k0, &u0)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (!sample(&i1, &k1, &u1)) return false;
    const std::uint64_t d_idle = i1 - i0;
    const std::uint64_t d_total = (k1 - k0) + (u1 - u0);  // kernel 时间**含** idle
    if (d_total == 0) return false;
    *out = 100.0 * (1.0 - static_cast<double>(d_idle) / static_cast<double>(d_total));
    return true;
#else
    (void)out;
    return false;
#endif
}

inline std::uint32_t logicalProcessors() {
#if defined(_WIN32)
    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    return si.dwNumberOfProcessors;
#else
    return 0;
#endif
}

inline const char* hostPlatform() {
#if defined(_WIN32)
    return "win32";
#else
    return "posix";
#endif
}

inline const char* hostArch() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#else
    return "unknown";
#endif
}

inline std::string hostCpuModel() {
    const char* env = std::getenv("PROCESSOR_IDENTIFIER");
    return env != nullptr ? std::string(env) : std::string();
}

// 工作树指纹（同一份 sha256 才可比）：`git status --porcelain` 全文的 sha256 + 行数。
// git 不可用 ⇒ `available:false` + reason（⛔ 不假装）。
inline Json worktreeFingerprint(const fs::path& work_dir) {
    Json j = Json::object();
#if defined(_WIN32)
    const fs::path out = work_dir / "git-status.txt";
    std::error_code ec;
    fs::remove(out, ec);
    const bool ok = captureCommand("git status --porcelain", out);
    const std::string text = ok ? readAllBytes(out) : std::string();
    std::vector<std::string> lines;
    {
        std::size_t pos = 0;
        while (pos < text.size()) {
            const std::size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(line);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
    j.set("available", Json::boolean(ok));
    if (ok) {
        j.set("dirtyLines", Json::u64(lines.size()));
        j.set("porcelainSha256", Json::text(sha256Bytes(text)));
    } else {
        j.set("dirtyLines", Json::null());
        j.set("porcelainSha256", Json::null());
        j.set("reason", Json::text("git status --porcelain 起不来 / 非 0 退出"));
    }
#else
    j.set("available", Json::boolean(false));
    j.set("dirtyLines", Json::null());
    j.set("porcelainSha256", Json::null());
    j.set("reason", Json::text("非 Windows 宿主：本批不在 POSIX 上取工作树指纹"));
#endif
    return j;
}

/**
 * 一次场景的"落盘骨架"（照 `.cache/lift-bench/` 下那些 JSON 的形状）：
 * 语境（build / 二进制 / 工作树 / 宿主 / 负载档）+ `passes[]` + `median` / `spread`。
 * 各场景只填自己的 `readings` 与 `thresholds`。
 */
struct Session {
    fs::path exe;
    fs::path game_dir;
    fs::path work_dir;
    fs::path out_dir;      // 仓库根的 `.cache/lift-stress`
    std::string build_type;
    std::string label;     // 场景名（文件名用它）
    std::string note;
    std::string command_line;
    Json root;
    Json passes = Json::array();
    std::vector<double> pass_wall_ms;
    bool load_start_ok = false;
    double load_start = 0.0;

    bool init(const char* note_in) {
        note = note_in;
        std::error_code ec;
        fs::create_directories(work_dir, ec);
        fs::create_directories(out_dir, ec);
        load_start_ok = cpuBusyPercent(&load_start);
        root = Json::object();
        root.set("schema", Json::number(1));
        root.set("tool", Json::text("apps/emulator-lift 的 CTest（LIFT_STRESS 档）"));
        root.set("note", Json::text(note));
        root.set("timestamp", Json::text(utcIso8601()));
        root.set("label", Json::text(label));
        Json build = Json::object();
        build.set("cmakeBuildType", Json::text(build_type));
        build.set("preset", Json::text("windows-x64"));
        root.set("build", build);
        Json bin = Json::object();
        bin.set("path", Json::text(exe.string()));
        bin.set("sha256", Json::text(sha256File(exe)));
        std::error_code size_ec;
        const std::uintmax_t bytes = fs::file_size(exe, size_ec);
        bin.set("bytes", Json::u64(size_ec ? 0 : static_cast<std::uint64_t>(bytes)));
        root.set("binary", bin);
        root.set("worktree", worktreeFingerprint(work_dir));
        Json host = Json::object();
        host.set("platform", Json::text(hostPlatform()));
        host.set("arch", Json::text(hostArch()));
        host.set("cpus", Json::u64(logicalProcessors()));
        host.set("cpuModel", Json::text(hostCpuModel()));
        root.set("host", host);
        root.set("n", Json::number(0));
        root.set("commandLine", Json::text(command_line));
        return true;
    }

    // 一遍的公共字段（场景再往里塞 `readings`）。
    Json& addPass(int index, const ChildResult& child, const std::string& log_rel) {
        Json p = Json::object();
        p.set("index", Json::number(index));
        p.set("log", Json::text(log_rel));
        p.set("exitCode", Json::number(child.exit_code));
        p.set("wallMs", Json::number(child.wall_ms));
        pass_wall_ms.push_back(child.wall_ms);
        passes.push(std::move(p));
        return passes.back();
    }

    void finish() {
        const std::uint64_t n = passes.size();
        root.set("n", Json::u64(n));
        Json load = Json::object();
        load.set("cpuBusyPctAtStart", load_start_ok ? Json::number(load_start) : Json::null());
        double end = 0.0;
        const bool end_ok = cpuBusyPercent(&end);
        load.set("cpuBusyPctAtEnd", end_ok ? Json::number(end) : Json::null());
        load.set("note", Json::text("负载档 = 宿主 CPU 忙占比的采样值（Windows 没有 loadavg；"
                                    "⛔ 不同负载下的墙钟读数不可横比，见 §0.2 R1）"));
        root.set("load", load);
        root.set("passes", passes);
        const Stats wall = statsOf(pass_wall_ms);
        Json w = Json::object();
        w.set("median", Json::number(wall.median));
        w.set("min", Json::number(wall.min));
        w.set("max", Json::number(wall.max));
        root.set("passWallMs", w);
    }

    // 落盘：`.cache/lift-stress/<场景>-<UTC 时间戳>.json`（⛔ 不进 git：`.cache/` 已 gitignore）。
    fs::path write() const {
        std::error_code ec;
        fs::create_directories(out_dir, ec);
        const fs::path p = out_dir / (label + "-" + utcStamp() + ".json");
        const std::string text = root.dump();
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        return p;
    }
};

// 场景入口的公共口径检查（非 Windows / 非 RelWithDebInfo ⇒ SKIP 77，⛔ 不假绿）。
inline bool buildTypeUsable(const std::string& build_type) {
    return build_type == "RelWithDebInfo";
}

}  // namespace lift_stress
