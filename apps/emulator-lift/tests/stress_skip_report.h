// emulator-lift · **S9 的 skip 报表**：从一段子进程 stdout **文本**里提取拒批原因并按格式报错
//   （**pure**：不跑游戏、不建子进程、不碰文件系统 —— 输入 = 文本 + 阈值，输出 = 判定 + 消息）
//
// ★ 为什么要有这一份（2026-11-14 用户裁决，收尾第 1 条）：
//   `stress-gpu-coverage`（S9）的红路径**必须逐条列出原因**（`⚠ gpu` / `skipped top:` / `gpu :` /
//   `unsupported :`），否则"少画了"在输出里会退化成"一个数字变了"，没人知道少了哪一类。但那条
//   用例要真跑 600 帧 × 3 遍（单遍 ~155 s，整条 ~6 min）才可能红一次 ⇒ "红的时候真的会列原因"
//   过去**没有任何机械证据**（只有"应该会"）。
//   ⇒ 把"提取 + 报错"抽成纯函数：`tests/stress_gpu_coverage.cpp` 用**真日志**调它；
//     `tests/dispatch_tax.cpp` 的 `stress/skip-report` 用**合成文本**三组断言钉住它
//     （默认档 / 顶层无条件注册 ⇒ 每次 `ctest` 都跑，⛔ 不需要开 `LIFT_STRESS`、⛔ 不跑真游戏）。
//
// ★ 它只做**解析 + 格式化**：喂什么文本就报什么。⛔ 不含任何业务判断（"到了某界面 / 画面对不对"）；
//   ⛔ 不读文件、不 spawn、不看环境变量 —— 所以它能在任何 pure 测试里被断言。
//
// 口径（与 `stress_gpu_coverage.cpp` 文件头 ②③④⑤ 一一对应；阈值全部来自 R2：计数类 = 0）：
//   ① `read      : … gpu=draws N/tri N/skipped N`  ← 总数面（默认档也有这一行）
//   ② `gpu       : … skipped=N`                    ← 总数面（Verbose 摘要才有）
//   ③ `            sprite: … skipped=N`            ← 精灵面（精灵被拒批不 +skipped_draws ⇒ 必须单独判）
//   ④ `            skipped top: [N×] 原因 ; …`      ← 原因表（按原因计数，最多 5 条）
//   ⑤ `⚠ gpu       : skipped=… gpu_failures=…`     ← 后端失败面（`read :` 里没有这一栏）
//   ★ 读点缺失（①②③ 任一没解析出来）也是**失败**，不是"当 0 处理"——那正是假绿的形状。
//
// ★ 失败的判据分两层，**不要混**：
//   · `ok == false` 与 `failed_faces` 是**机器可读**的面（谁不成立）；
//   · `message` 是**人可读**的报错正文：原样带出上面四类行 + 逐条原因 + （必要时）"原因缺失"。
//     `message` 的每一行原因都是日志行**原文**（调用方可以逐条 `find` 它）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace lift_stress {

// 门槛（S9 的 ②③④⑤）：全部是"计数类"，按 R2 取 0。
struct SkipThresholds {
    std::uint64_t skipped_draws = 0;    // ① `read :` 的 `/skipped N` 与 ② `gpu :` 的 `skipped=N`
    std::uint64_t sprite_skipped = 0;   // ③ `sprite: … skipped=N`
    std::size_t skipped_top_lines = 0;  // ④ `skipped top:` 的行数（原因表为空）
};

// 从文本里读出来的东西（原样行留着 —— 红的时候要逐条带出来）。
struct SkipReadings {
    std::uint64_t read_skipped = 0;    // ① `read      : … gpu=draws N/tri N/skipped N`
    std::uint64_t gpu_skipped = 0;     // ② `gpu       : … skipped=N`
    std::uint64_t sprite_skipped = 0;  // ③ `sprite: … skipped=N`
    bool read_ok = false;
    bool gpu_ok = false;
    bool sprite_ok = false;
    std::string read_line;
    std::string gpu_line;
    std::string sprite_line;
    std::string unsupported_line;
    std::string warn_gpu_line;
    std::vector<std::string> skipped_top_lines;
};

struct SkipVerdict {
    bool ok = true;
    std::vector<std::string> failed_faces;  // 机器可读：read_skipped / gpu_skipped / sprite_skipped /
                                            // skipped_top_lines / warn_gpu / readings/…
    std::string message;                    // 人可读：通过 = 一行摘要；失败 = 逐条列原因
    SkipReadings readings;
};

namespace skip_report_detail {

// 按 `\n` 切行（去掉行尾 `\r`）。原 `tests/stress_harness.h` 的 `linesStartingWith(text, "")` 同口径，
// 但这里**故意不引它**：那份是压力档专用（还带 windows.h / subprocess），
// 本份要能被默认档的 pure 测试直接 include（`stress_harness.h` 已随 6 条压力用例退役删除，2026-11-16）。
inline std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

inline bool startsWith(const std::string& line, const char* prefix) {
    return line.rfind(prefix, 0) == 0;
}

// `key` 后面跟一个十进制数（`key` 要带分隔符一起给，如 `"skipped="`）。
inline bool readU64(const std::string& line, const char* key, std::uint64_t* out) {
    const std::size_t at = line.find(key);
    if (at == std::string::npos) return false;
    const char* begin = line.c_str() + at + std::strlen(key);
    char* end = nullptr;
    const unsigned long long v = std::strtoull(begin, &end, 10);
    if (end == begin) return false;
    *out = static_cast<std::uint64_t>(v);
    return true;
}

}  // namespace skip_report_detail

// `read      :` 行里的 `gpu=draws <N>/tri <N>/skipped <N>`。
// ★ 与 S9 主程序**共用同一份实现**（那边不再自己写一份解析）。
inline bool parseReadGpu(const std::string& read_line, std::uint64_t* draws, std::uint64_t* tri,
                         std::uint64_t* skipped) {
    const std::size_t at = read_line.find("gpu=draws ");
    if (at == std::string::npos) return false;
    const char* p = read_line.c_str() + at + 10;
    char* end = nullptr;
    const unsigned long long d = std::strtoull(p, &end, 10);
    if (end == p || std::strncmp(end, "/tri ", 5) != 0) return false;
    p = end + 5;
    const unsigned long long t = std::strtoull(p, &end, 10);
    if (end == p || std::strncmp(end, "/skipped ", 9) != 0) return false;
    p = end + 9;
    const unsigned long long s = std::strtoull(p, &end, 10);
    if (end == p) return false;
    *draws = d;
    *tri = t;
    *skipped = s;
    return true;
}

// 从整段文本里取出五个面（①–⑤）的读数与原文行。⛔ 不做判定。
inline SkipReadings readSkipReport(const std::string& text) {
    SkipReadings r;
    for (const std::string& line : skip_report_detail::splitLines(text)) {
        if (r.read_line.empty() && skip_report_detail::startsWith(line, "read      :")) {
            r.read_line = line;
        } else if (r.gpu_line.empty() && skip_report_detail::startsWith(line, "gpu       : draws=")) {
            r.gpu_line = line;
        } else if (r.warn_gpu_line.empty() && skip_report_detail::startsWith(line, "⚠ gpu       :")) {
            r.warn_gpu_line = line;
        } else if (r.unsupported_line.empty() && skip_report_detail::startsWith(line, "unsupported :")) {
            r.unsupported_line = line;
        } else if (skip_report_detail::startsWith(line, "            skipped top:")) {
            r.skipped_top_lines.push_back(line);
        } else if (r.sprite_line.empty() && line.find("sprite: draws=") != std::string::npos) {
            // 缩进 12 空格；用"含 `sprite: draws=`"来找，⛔ 不写死缩进数
            // （逐帧行的 `sprite_gpu=` 不含 `sprite:`，不会误命中）。
            r.sprite_line = line;
        }
    }
    std::uint64_t draws = 0;
    std::uint64_t tri = 0;
    r.read_ok = !r.read_line.empty() && parseReadGpu(r.read_line, &draws, &tri, &r.read_skipped);
    if (!r.gpu_line.empty()) {
        r.gpu_ok = skip_report_detail::readU64(r.gpu_line, "skipped=", &r.gpu_skipped);
    }
    if (!r.sprite_line.empty()) {
        r.sprite_ok = skip_report_detail::readU64(r.sprite_line, "skipped=", &r.sprite_skipped);
    }
    return r;
}

// **判定 + 报错正文**：这就是 S9 失败时打出来的东西（以及 `stress/skip-report` 三组断言的对象）。
inline SkipVerdict judgeSkipReport(const std::string& text, const SkipThresholds& th) {
    SkipVerdict v;
    v.readings = readSkipReport(text);
    const SkipReadings& r = v.readings;

    // ---- 哪个面不成立（机器可读；顺序稳定，测试可以逐项 find）----
    if (!r.read_ok) v.failed_faces.push_back("readings/read-line");
    if (!r.gpu_ok) v.failed_faces.push_back("readings/gpu-line");
    if (!r.sprite_ok) v.failed_faces.push_back("readings/sprite-line");
    if (r.read_skipped > th.skipped_draws) v.failed_faces.push_back("read_skipped");
    if (r.gpu_skipped > th.skipped_draws) v.failed_faces.push_back("gpu_skipped");
    if (r.sprite_skipped > th.sprite_skipped) v.failed_faces.push_back("sprite_skipped");
    if (r.skipped_top_lines.size() > th.skipped_top_lines) v.failed_faces.push_back("skipped_top_lines");
    if (!r.warn_gpu_line.empty()) v.failed_faces.push_back("warn_gpu");
    v.ok = v.failed_faces.empty();

    const std::string face_limits = "门槛 skipped=" + std::to_string(th.skipped_draws) +
                                    " / sprite_skipped=" + std::to_string(th.sprite_skipped) +
                                    " / `skipped top:` " + std::to_string(th.skipped_top_lines) + " 行";

    if (v.ok) {
        v.message = "skip 报表：通过（skipped=" + std::to_string(r.gpu_skipped) +
                    " / sprite_skipped=" + std::to_string(r.sprite_skipped) + " / `skipped top:` " +
                    std::to_string(r.skipped_top_lines.size()) + " 行 / 无 `⚠ gpu`；" + face_limits + "）";
        return v;
    }

    std::string m = "skip 报表：GPU 准入拒批不是 0 ⇒ 这是**潜在缺画**（没有 CPU 补救路径）\n";
    m += "  面（哪一条不成立）：";
    bool first_face = true;
    auto add_face = [&](const std::string& f) {
        m += (first_face ? "" : "、");
        m += f;
        first_face = false;
    };
    if (!r.read_ok) add_face("`read :` 的 `gpu=draws N/tri N/skipped N` 没解析出来");
    if (!r.gpu_ok) add_face("`gpu       :` 行没解析出来（Verbose 摘要缺失？）");
    if (!r.sprite_ok) add_face("`sprite: draws=` 行没解析出来");
    if (r.read_skipped > th.skipped_draws) {
        add_face("`read :` skipped=" + std::to_string(r.read_skipped) + " > " +
                 std::to_string(th.skipped_draws));
    }
    if (r.gpu_skipped > th.skipped_draws) {
        add_face("`gpu :` skipped=" + std::to_string(r.gpu_skipped) + " > " +
                 std::to_string(th.skipped_draws));
    }
    if (r.sprite_skipped > th.sprite_skipped) {
        add_face("`sprite:` skipped=" + std::to_string(r.sprite_skipped) + " > " +
                 std::to_string(th.sprite_skipped));
    }
    if (r.skipped_top_lines.size() > th.skipped_top_lines) {
        add_face("`skipped top:` " + std::to_string(r.skipped_top_lines.size()) + " 行 > " +
                 std::to_string(th.skipped_top_lines) + " 行");
    }
    if (!r.warn_gpu_line.empty()) add_face("`⚠ gpu` 行出现（skipped_draws≠0 或 gpu_failures≠0）");
    m += "（" + face_limits + "）\n";

    m += "  读数原样（同一份日志里的行）：\n";
    if (!r.warn_gpu_line.empty()) m += "       " + r.warn_gpu_line + "\n";
    if (!r.skipped_top_lines.empty()) {
        for (const std::string& l : r.skipped_top_lines) m += "       " + l + "\n";
        m += "       ↑ 每一条都是**一处潜在缺画**的原因与次数（格式：`[N×] 原因 ;`）\n";
    }
    if (!r.gpu_line.empty()) m += "       " + r.gpu_line + "\n";
    if (!r.unsupported_line.empty()) m += "       " + r.unsupported_line + "\n";
    if (!r.sprite_line.empty()) m += "       " + r.sprite_line + "\n";

    // ★ 计数说"有拒批"、原因表却是空 ⇒ **读点缺失**，⛔ 不是通过。
    //   （这条比"静默通过"重要：没有原因表时，前四个面只给出"少了 3 批"，说不出少了哪一类。）
    const bool any_count_over = r.read_skipped > th.skipped_draws || r.gpu_skipped > th.skipped_draws ||
                                r.sprite_skipped > th.sprite_skipped;
    if (any_count_over && r.skipped_top_lines.empty()) {
        m += "  原因缺失：计数说\"有拒批\"（skipped=" + std::to_string(r.gpu_skipped) +
             " / sprite_skipped=" + std::to_string(r.sprite_skipped) +
             "），但日志里一行 `skipped top:` 都没有 ——\n";
        m += "     只有总数、没有按原因的分类。⛔ 这不是通过，是**读点缺失**：要么原因表没打印，\n";
        m += "     要么这一行的格式变了（`            skipped top: [N×] 原因 ;`，见"
             " `gpuSkippedTopLine()`）。\n";
    }
    v.message = std::move(m);
    return v;
}

}  // namespace lift_stress
