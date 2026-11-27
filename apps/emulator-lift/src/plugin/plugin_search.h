// emulator-lift · `--plugin <id|path>` 的**纯解析器**（S1）
//
// 判据（红了意味着什么）：`--plugin` 的 spec 分类 / id 搜索根 / 歧义判据被改松或改紧 ——
//   · 改松（`lib/` 也搜 / 递归子目录 / 取第一个命中）⇒ 用户装了**别的**同名文件而没有任何症状；
//   · 改紧（把 `.js` 后缀当 id）⇒ 今天的 `--plugin x.js` 从"cwd 相对路径"静默变成"白名单根里的文件"。
//
// ★ 机械判据只有一条（`AGENTS.md` §6 的"能机械判的写进代码，不写进散文"）：
//     id   := `^[A-Za-z0-9_-]{1,64}$`
//     path := 其余一切（含 `.` / `/` / `\` / `:` / 绝对路径 / UNC / 以 `.js` / `.ts` / `.mts` 结尾 / 空串）
//   ⇒ `.js` / `.ts` / `.mts` 后缀天然是路径。
//   ★ **空串在解析器层仍归 `Path`**（它不满足 id 的 `{1,64}`），但**调用方（`aot_main`）在
//     parse 期就拒绝显式的 `--plugin ""`**（exit 2：空串既不是 id、也不是可装载路径）。
//     ⛔ 不要靠"空 spec 走路径 ⇒ 装载失败"兜底：`plugin_path.empty()` 会被整块跳过，
//     静默变成"没有插件"（S1 复审 minor 1 的来路）。
//
// ★ 搜索根**只有两处、非递归**（用户口径 2026-11-25，比设计报告 A2 更硬）：
//     ① `<exeDir>scripts/plugins/<id>.js|.ts|.mts`（发行；与 `--overlay` 的 `<exeDir>scripts/ui/hud.js` 同形）
//     ② `LIFT_PLUGIN_DIR/<id>.js|.ts|.mts`（开发；CMake 注入**源码** `apps/emulator-lift/scripts/plugins`）
//   ★ S1b 候选顺序 = `.js` → `.ts` → `.mts`，**扩展名优先于根**（同 id 同时有 `.js`/`.ts` ⇒ `.js` 优先；
//     ⛔ 不报歧义、⛔ 不静默换源）。命中第一档就停，之前 0 命中的候选仍进 `searched`。
//   ⛔ **不查 `lib/`**（库只被 import，永远不是 `--plugin` 的 id 目标）、
//   ⛔ **不递归子目录**、⛔ 不查 `cwd` / `PATH` / 系统目录 / `--appdata`。
//   ★ `exe_dir` 为空（`argv[0]` 不含分隔符 / PATH 启动）⇒ 该根跳过，**只查 `LIFT_PLUGIN_DIR`**；
//     两个根指向同一路径时**去重**（同一份文件不报成歧义）。
//
// ★ **S2：`hud` 是保留 id**（`--overlay` = `--plugin hud` 预设 ⇒ 两个入口必须同一解析）：
//     · 解析顺序**先于**普通插件根：`<exeDir>/scripts/ui/hud.js`（发行）→
//       `hud_dev_dir/hud.js`（开发回退 = 编译期 `LIFT_UI_SCRIPT_DIR`）；
//     · 但**用户插件根里存在 `hud.js`**（`<exeDir>/scripts/plugins/hud.js` 或 `LIFT_PLUGIN_DIR/hud.js`）
//       ⇒ `ReservedShadowed`（调用方 exit 2 并列出路径）：⛔ 不静默让用户文件被内置件遮住；
//       ★ S1b 裁决：遮蔽检查**只看 `.js`**（`hud.ts` 不参与：保留 id 解析的是内置发行件 `hud.js`，
//         `.ts` HUD 不在本轮范围；参与遮蔽只会把 `--overlay` 变成静默换源）。
//     · 保留 id **不**进普通根的命中集合（普通根只在遮蔽检查里读）；
//     · 保留 id 判据是 **ASCII 大小写不敏感**（`hud` / `HUD` / `Hud` 同一件事）；
//     · 路径形式（含 `.js`）**不**享受保留语义 ⇒ `--plugin <...>/hud.js` 仍是普通路径（behavior 不变）。
//
// ★ 命中 1 处 ⇒ `path` = **绝对路径**（交给 `initFromFile`）；命中 ≥2 ⇒ `Ambiguous`（⛔ 不静默取第一个）；
//   0 ⇒ `NotFound`（`searched` 里是**每个**候选路径，错误文案由调用方按它拼）。
//   ★ 保留 id `hud` 是**例外**：保留根按"发行 → 开发回退"的**优先序**取第一个命中（与 S2 之前的
//     `--overlay` 解析逐字相同；两份都在场时 exe 那份赢，⛔ 不把"发行优先"变成启动失败）。
// ★ 路径形式**原样返回**（⛔ 不绝对化、⛔ 不做存在性检查）：cwd 相对 / 绝对 / 装载失败 exit 2
//   三条行为与 S1 之前**逐字相同**。
// ★ 纯函数：除了按传入的 roots 读文件系统（`is_regular_file`）之外没有任何全局状态 ⇒ 可以被
//   `tests/plugin_resolve.cpp` 用临时目录注入 roots 直接判（不需要 exe / GPU / 资产）。
#pragma once

#include <string>
#include <vector>

namespace lift::plugin {

/** `--overlay` 预设解析的保留 id（也是 `--plugin` 的保留名：用户插件根里出现同名文件 ⇒ exit 2）。 */
inline constexpr const char* kReservedHudId = "hud";

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
    /**
     * 保留 id `hud` 的**开发回退根**：编译期 `LIFT_UI_SCRIPT_DIR`（源码树 `scripts/ui`；空 = 跳过）。
     * 发行侧不需要新字段：`<exe_dir>scripts/ui/hud.js` 由 `exe_dir` 派生（与 `--overlay` 旧布局同形）。
     */
    std::string hud_dev_dir;
};

struct PluginResolveResult {
    /**
     * `Ok` = 可以装载；`NotFound` / `Ambiguous` / `ReservedShadowed` = 调用方必须 `exit 2` 并点名候选。
     * `ReservedShadowed`（S2）= 保留 id `hud` 被用户插件根里的 `hud.js` 遮住（⛔ 不静默换源）。
     */
    enum class Status { Ok, NotFound, Ambiguous, ReservedShadowed };

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
    /**
     * `kind == Id`：命中的候选（`Ok` 时恰好 1 条；`Ambiguous` 时 ≥2 条，全部是绝对路径）。
     * `ReservedShadowed` 时 = **用户插件根里**那些 `hud.js`（全部是绝对路径）。
     */
    std::vector<std::string> hits;
};

/** `^[A-Za-z0-9_-]{1,64}$`（空串 / 含 `.` `/` `\` `:` / 非 ASCII / >64 字符都 false）。 */
bool isPluginId(const std::string& spec);

/**
 * `spec` 是否**保留 id `hud`** —— **ASCII 大小写不敏感**（`hud` / `HUD` / `Hud` 都是）。
 * ★ 为什么不能按精确串：Windows 卷不敏感 ⇒ `--plugin HUD` 会绕开 `ReservedShadowed`，把用户
 *   插件根里的 `hud.js` 当普通 id 装上（Linux 上则 NotFound）——同一份命令两种行为。
 */
bool isReservedHudId(const std::string& spec);

/**
 * 解析 `--plugin` 的 spec。
 *
 * @param spec   CLI 原串（UTF-8）
 * @param roots  白名单根（`exe_dir` / `dev_dir` / 保留 id 的 `hud_dev_dir`；空串 = 跳过该根）
 * @return       分类 + （id 时）全部候选 / 命中；`Path` 永远 `Ok` 且 `path == spec`
 */
PluginResolveResult resolvePluginSpec(const std::string& spec, const PluginResolveRoots& roots);

}  // namespace lift::plugin
