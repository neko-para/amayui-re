// emulator-lift · `--plugin <id|path>` 的**纯解析器**（S1）
//
// 判据（红了意味着什么）：`--plugin` 的 spec 分类 / id 搜索根 / 歧义判据被改松或改紧 ——
//   · 改松（`lib/` 也搜 / 递归子目录 / 取第一个命中）⇒ 用户装了**别的**同名文件而没有任何症状；
//   · 改紧（把 `.js` 后缀当 id）⇒ 今天的 `--plugin x.js` 从"cwd 相对路径"静默变成"白名单根里的文件"。
//
// ★ 机械判据只有一条（`AGENTS.md` §6 的"能机械判的写进代码，不写进散文"）：
//     id   := `^[A-Za-z0-9_-]{1,64}$`
//     path := 其余一切（含 `.` / `/` / `\` / `:` / 绝对路径 / UNC / 以 `.js` 结尾 / 空串）
//   ⇒ `.js` 后缀天然是路径；空串也走路径（`--plugin ""` 的失败面与今天逐字相同：
//     交给 `PluginHost::initFromFile` 报"装载失败 exit 2"）。
//
// ★ 搜索根**只有两处、非递归**（用户口径 2026-11-25，比设计报告 A2 更硬）：
//     ① `<exeDir>scripts/plugins/<id>.js`（发行；与 `--overlay` 的 `<exeDir>scripts/ui/hud.js` 同形）
//     ② `LIFT_PLUGIN_DIR/<id>.js`（开发；CMake 注入**源码** `apps/emulator-lift/scripts/plugins`）
//   ⛔ **不查 `lib/`**（库只被 import，永远不是 `--plugin` 的 id 目标）、
//   ⛔ **不递归子目录**、⛔ 不查 `cwd` / `PATH` / 系统目录 / `--appdata`。
//
// ★ 命中 1 处 ⇒ `path` = **绝对路径**（交给 `initFromFile`）；命中 ≥2 ⇒ `Ambiguous`（⛔ 不静默取第一个）；
//   0 ⇒ `NotFound`（`searched` 里是**每个**候选路径，错误文案由调用方按它拼）。
// ★ 路径形式**原样返回**（⛔ 不绝对化、⛔ 不做存在性检查）：cwd 相对 / 绝对 / 装载失败 exit 2
//   三条行为与 S1 之前**逐字相同**。
// ★ 纯函数：除了按传入的 roots 读文件系统（`is_regular_file`）之外没有任何全局状态 ⇒ 可以被
//   `tests/plugin_resolve.cpp` 用临时目录注入 roots 直接判（不需要 exe / GPU / 资产）。
#pragma once

#include <string>
#include <vector>

namespace lift::plugin {

/** spec 的两类（`id` / `path`；判据见文件头）。 */
enum class PluginSpecKind {
    Path,  // 原样交给 `initFromFile`（cwd 相对 / 绝对 / `.js` 后缀 / 空串）
    Id,    // 文件名 stem：只查两个白名单根，命中后换成绝对路径
};

struct PluginResolveRoots {
    /** ① 发行根：`<exeDir>scripts/plugins/` 的**目录部分**（含或不含尾分隔符都可以；空 = 跳过这个根）。 */
    std::string exe_dir;
    /** ② 开发根：编译期 `LIFT_PLUGIN_DIR`（源码树 `scripts/plugins`；空 = 跳过这个根）。 */
    std::string dev_dir;
};

struct PluginResolveResult {
    /** `Ok` = 可以装载；`NotFound` / `Ambiguous` = 调用方必须 `exit 2` 并点名候选。 */
    enum class Status { Ok, NotFound, Ambiguous };

    Status status = Status::Ok;
    PluginSpecKind kind = PluginSpecKind::Path;
    /** spec 原文（错误文案与日志都点它）。 */
    std::string spec;
    /** `kind == Id` 时非空（path 形式为空）。 */
    std::string id;
    /**
     * `Ok` 时的装载路径：`Id` ⇒ 绝对路径；`Path` ⇒ **spec 原样**（⛔ 不绝对化）。
     * 非 `Ok` 时为空。
     */
    std::string path;
    /** `kind == Id`：**按顺序**构造出来的全部候选（`NotFound` 的文案据此逐行列出）。 */
    std::vector<std::string> searched;
    /** `kind == Id`：命中的候选（`Ok` 时恰好 1 条；`Ambiguous` 时 ≥2 条，全部是绝对路径）。 */
    std::vector<std::string> hits;
};

/** `^[A-Za-z0-9_-]{1,64}$`（空串 / 含 `.` `/` `\` `:` / 非 ASCII / >64 字符都 false）。 */
bool isPluginId(const std::string& spec);

/**
 * 解析 `--plugin` 的 spec。
 *
 * @param spec   CLI 原串（UTF-8）
 * @param roots  两个白名单根（`exe_dir` / `dev_dir`；空串 = 跳过该根）
 * @return       分类 + （id 时）全部候选 / 命中；`Path` 永远 `Ok` 且 `path == spec`
 */
PluginResolveResult resolvePluginSpec(const std::string& spec, const PluginResolveRoots& roots);

}  // namespace lift::plugin
