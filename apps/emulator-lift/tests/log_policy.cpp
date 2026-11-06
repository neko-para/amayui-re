// emulator-lift · 日志分级 / 分类 / 模式（pure）
//
// 判据：
//   ① 默认 `warn`：Info 不显示；Warn / Error 显示；Error 在 `quiet` / `--log-cat none` 下仍显示。
//   ② 分类过滤只影响非 Error；`file` 打开时 File 可见、D3d9 不可见。
//   ③ 级别 / 分类解析接受既定名字，未知名字返回 false（调用方应退出 2，不静默）。
//   ④ `logSetCategories("none")` 表示"非 Error 全关"，不是回退到 all。
#include <cstring>

#include "check.h"
#include "runtime/log.h"

int main() {
    using namespace lift_test;

    g_current_case = "log-default-warn";
    LIFT_CHECK(lift::logSetLevel("warn"));
    LIFT_CHECK(!lift::logVisible(lift::LogLevel::Info, lift::LogGeneral));
    LIFT_CHECK(lift::logVisible(lift::LogLevel::Warn, lift::LogGeneral));
    LIFT_CHECK(lift::logVisible(lift::LogLevel::Error, 0u));
    LIFT_CHECK(!lift::logVisible(lift::LogLevel::Debug, lift::LogFile));

    g_current_case = "log-level-parse";
    lift::LogLevel level = lift::LogLevel::Quiet;
    LIFT_CHECK(lift::logParseLevel("debug", &level));
    LIFT_CHECK(level == lift::LogLevel::Debug);
    LIFT_CHECK(!lift::logParseLevel("bogus", &level));
    LIFT_CHECK(!lift::logSetLevel("bogus"));
    LIFT_CHECK(lift::logSetLevel("info"));
    LIFT_CHECK(lift::logVisible(lift::LogLevel::Info, lift::LogGeneral));

    g_current_case = "log-cat-filter";
    LIFT_CHECK(lift::logSetCategories("file"));
    LIFT_CHECK(lift::logVisible(lift::LogLevel::Info, lift::LogFile));
    LIFT_CHECK(!lift::logVisible(lift::LogLevel::Info, lift::LogD3d9));
    // Error 不受分类过滤
    LIFT_CHECK(lift::logVisible(lift::LogLevel::Error, lift::LogD3d9));
    LIFT_CHECK(!lift::logSetCategories("bogus"));
    LIFT_CHECK(lift::logSetCategories("file,com,d3d9"));
    LIFT_CHECK(lift::logVisible(lift::LogLevel::Info, lift::LogCom));
    LIFT_CHECK(lift::logVisible(lift::LogLevel::Info, lift::LogD3d9));

    g_current_case = "log-cat-none";
    LIFT_CHECK(lift::logSetCategories("none"));
    LIFT_CHECK(!lift::logVisible(lift::LogLevel::Info, lift::LogFile));
    LIFT_CHECK(!lift::logVisible(lift::LogLevel::Warn, lift::LogFile));
    LIFT_CHECK(lift::logVisible(lift::LogLevel::Error, lift::LogFile));

    g_current_case = "log-quiet-error-floor";
    LIFT_CHECK(lift::logSetLevel("quiet"));
    LIFT_CHECK(lift::logVisible(lift::LogLevel::Error, lift::LogFile));
    LIFT_CHECK(!lift::logVisible(lift::LogLevel::Warn, lift::LogFile));
    LIFT_CHECK(lift::logVisible(lift::LogLevel::Error, 0u));

    // 恢复默认，避免同进程里其他夹具受本夹具影响
    LIFT_CHECK(lift::logSetLevel("warn"));
    LIFT_CHECK(lift::logSetCategories("all"));
    return finish("log-policy");
}
