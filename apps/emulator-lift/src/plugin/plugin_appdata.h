// emulator-lift · **`ctx.appdata`** 的宿主实现（`PluginHost` 持有；只读 stat）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.5 / §4.6 / §6 行 38；结果值类型在
//       `runtime/appdata.h`（`AppDataStat` / `AppDataStatError`）。
//
// ★ 只做**一件事**：把 `<appdata 根>/<rel>` 的 `stat` 结果翻成稳定错误码。⛔ 不做 read / write /
//   列目录 —— 插件里的任意文件读会绕开 guest 语义（合同 §4.5 的"操作类唯一路径"）。
// ★ **只 appdata 子树**：绝对路径 / `..` / NUL 一律拒绝；canonical（`weakly_canonical`，目录可以
//   还不存在）之后必须仍在根下 ⇒ 符号链接 / junction 指向外面是 `escape`。
// ★ 根由 `attach()` 绝对化（`Vfs::setAppDataDir` 存的是 CLI 原串，不做绝对化）；没有 `--appdata`
//   ⇒ `available()=false`，`stat` 一律 `no_appdata`（⛔ 不编一个假根）。
// ★ 本类**不** include `quickjs.h`、不调 `input::` / `writeGlobal`（core-shape ④）。
#pragma once

#include <string>

#include "runtime/appdata.h"

namespace lift::plugin {

class PluginAppData {
public:
    // 接根。`appdata_dir` = CLI 原串（空 = 没给，正常）；`install_dir` = `--game-dir`（只用来自检
    // "appdata 落在只读安装树里"这一种退化配置）。返回 false ⇒ `*err` 非空且 `available()=false`
    // （调用方按 **warn + 继续** 处理：插件能力缺失不是启动失败）。
    bool attach(const std::string& appdata_dir, const std::string& install_dir, std::string* err);

    bool available() const { return available_; }
    // 绝对宿主路径（UTF-8；宿主分隔符口径）；不可用 ⇒ 空串。
    const std::string& path() const { return path_; }

    // 同步只读 `stat`；`rel` 相对 appdata 根（`/` 或 `\` 都收；`.` / 连续分隔符归一化）。
    AppDataStat stat(const std::string& rel) const;

private:
    bool available_ = false;
    std::string path_;   // 绝对化 + lexically_normal（给 JS 的 `ctx.appdata.path`）
    std::string canon_;  // `weakly_canonical(根)`（escape 判据；不存在也算到最近的存在前缀）
};

}  // namespace lift::plugin
