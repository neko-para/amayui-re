// emulator-lift · VFS：Win32 guest 路径 ↔ 宿主路径（唯一根 = `--game-dir`；大小写不敏感）
//
// ★ 口径（plan §6 S6 / §9 R2）：
//   · `--game-dir` 是**唯一的根**。guest 的绝对路径先剥掉盘符与 guest 根前缀，再拼到 game_dir；
//     `..` 不许逃出根（逃出 = 响亮失败，⛔ 不静默夹回根里）。
//   · **大小写不敏感**：逐段在宿主目录里找唯一的大小写不敏感匹配；同一段有多个候选 ⇒ 报歧义
//     （Windows 上不会发生；Linux/macOS 上会发生 ⇒ 必须显式失败，不能按目录顺序猜）。
//   · 枚举顺序**排序后返回**（plan §8.3-4：`FindFirstFileA` 的顺序必须确定性）。
//   · 通配符只实现游戏实际用到的形态：`*` / `?` / `*.*`（Windows 语义：`*.*` 匹配全部）。
//     其它非法形态（`**`、`\` 后无名字等）⇒ 报错，⛔ 不静默忽略。
//   · 宿主路径一律用 **UTF-8** 字符串（Windows 上经 `std::filesystem::u8path`）。
// ★ 这是**叶子设施**：不碰 guest 内存；shim 负责读写与错误码。解析侧有**三层**记忆（都建立在
//   同一条显式假设上："写覆盖层启用时安装树只读" —— 见 `invalidatePathCache` 与私有段
//   `PositiveEntry` 上方）：
//     ① **安装树目录索引**（每个目录**一次** `directory_iterator` ⇒ 折叠名 → 候选；没有失效语义，
//        `setGameDir` 清空）—— 消掉"失败探测每次都整目录枚举"与"逐段精确 stat"；
//     ② **负**解析缓存（只存失败结论；按影子代失效）；
//     ③ **正**解析缓存（只存宿主路径；与负表**同一把键、同一代**）。
//   ⛔ 目录枚举本身（`find()`）不缓存（`FindFirstFile` 的排序 / 增删语义另说）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lift {

struct HostDirEntry {
    std::string name;      // 宿主目录里的**真实**文件名（UTF-8）
    bool is_dir = false;
    bool is_file = false;
    std::uint64_t size = 0;
    std::uint32_t attributes = 0;  // FILE_ATTRIBUTE_* 的跨平台近似
    // 三个宿主时间（自 1601-01-01 起的 100ns = Windows FILETIME）；取不到时回退成 write、
    // 或按文档留 0（"文件系统不支持"）。
    std::uint64_t create_time_100ns = 0;
    std::uint64_t access_time_100ns = 0;
    std::uint64_t write_time_100ns = 0;
};

// 一个宿主路径的三个时间（FILETIME 100ns）。
struct HostFileTimes {
    std::uint64_t create_100ns = 0;
    std::uint64_t access_100ns = 0;
    std::uint64_t write_100ns = 0;
};

// 取宿主文件的三个时间。★ 平台抽象走 **SDL3 的 `SDL_GetPathInfo`**（Windows / APFS / Linux 都实现，
// 含创建时间与访问时间；`std::filesystem` 只有 mtime），实现里**不写 `#ifdef` 平台分支**。
// 失败（路径不存在 / SDL 不可用）⇒ false，调用方回退到 `std::filesystem::last_write_time`。
bool hostFileTimes(const std::string& host_path, HostFileTimes* out);

// =======================================================================================
// ★ 诊断：**解析内部**的段查询 / 目录枚举 / 负缓存命中计数（`LIFT_VFS_TIMING=1` 门控）
//
//   为什么要有：`CreateFileA` 的 184 µs/次 里有多少是"路径解析"、其中又有多少是**整目录枚举**，
//   只能从解析内部读数 —— 外面只能看到总时间。⛔ 只读计量：不改任何解析结论、不参与判据；
//   关档时每次 `lookupSegment` 只多一次静态 bool 判定（不取时、不记账）。
//   ⚠ 计数是**全进程**的（`find()` 也走 `lookupSegment`）⇒ 读数里包含目录枚举的份额，不是只有 `resolve`。
// =======================================================================================
struct VfsDiagCounters {
    std::uint64_t segment_lookups = 0;   // 段查询次数（每次都先做一次精确 stat）
    std::uint64_t dir_enumerations = 0;  // 整目录枚举次数（精确 stat 未命中后的回退）
    std::uint64_t negative_hits = 0;     // 负解析缓存命中次数
};
bool vfsDiagOn();
VfsDiagCounters vfsDiagCounters();

// 按 **UTF-8 宿主路径**打开文件（Windows 上走 `_wfopen_s`）。
// ★ 为什么不能直接用 `std::fopen`：MSVC 的窄 `fopen` 按 **ANSI 代码页**解释路径，而本仓路径一律
//   UTF-8（游戏目录本身是日文名）⇒ Windows 上非 ASCII 的 `--exe` / `--lst` / `--game-dir` 会打不开。
std::FILE* openHostFileUtf8(const std::string& host_path, const char* mode);

// 一次宿主 stat：**同时**给出 size 与 write time（100 ns 自 1601），且只在"它是普通文件"时成功。
// ★ 为什么不是两次 `std::filesystem` 调用：内容缓存的**每次命中**都要付这一笔（目标 2–5 µs），
//   两次 stat 就翻倍；`SDL_GetPathInfo` 一次拿全（与 `hostFileTimes` 同一个来源 ⇒ 存与查的口径一致）。
bool hostFileInfo(const std::string& host_path, std::uint64_t* size, std::uint64_t* write_100ns);

// =======================================================================================
// ★ guest 路径文本的编码 —— **唯一规则**（2026-11-05 缺陷单 `🐞S2 VFS 不认 guest 根前缀`）
//
//   guest 内存里的路径：A 版 = **CP932 字节**、W 版 = **UTF-16**；Vfs 内部 / 宿主 = **UTF-8**。
//
//   实测（2026-11-05 boot）：`version!GetFileVersionInfoSizeA` 把 guest 内存里的 CP932 字节**原样**
//   交给了 `Vfs::resolve`（版本族是唯一没经过 A 版解码的调用方）⇒ 缺省 guest 根
//   `E:\Games\Eushully\天結` 里的 `天結` 在两侧是**两套字节**（CP932 `93 56 8C 8B` / UTF-8
//   `E5 A4 A9 E7 B5 90`）⇒ 根前缀剥不掉 ⇒ `[notdir] 段 "Games"` ⇒ 版本读不到 ⇒ 引擎弹框后
//   `ExitProcess(0)`。同一件事在日志里表现为 `天結` 打成 `?V??`（CP932 字节直接进 UTF-8 流）。
//
//   ⇒ 规则只有一条，**解析入口与日志打印共用**（同一条规则、不是两套：只给日志用的话，解析侧
//     还得再写一遍同样的判断，两份迟早漂——本仓已经因为"A 版字节 / UTF-8 文本混用"红过一次）：
//        合法 UTF-8 ⇒ 原样返回；否则按 **CP932** 解码（guest 内存里的 A 版字节）。
//   ★ 为什么不按调用方区分：guest 只有 CP932 这一种 A 版码页（`GetACP()=932`，见 runtime/cp932.h）。
//     UTF-8 优先是因为 Vfs 内部与宿主路径都是 UTF-8，而纯 ASCII 在两种编码下逐字节相同。
//   ⚠ 诚实项（边界）：CP932 字节**碰巧**也是合法 UTF-8 的极少数组合会被当成 UTF-8 读 ——
//     只可能出现在非法 / 控制字符段上，那种输入在真机上本来也不是一条有效路径。
// =======================================================================================
std::string guestPathForLog(std::string_view guest_path);

// =======================================================================================
// ★ guest 特殊目录 / 环境块 —— **唯一文本真源**（2026-11-05 用户裁决）
//
//   环境块（`setupProcessStructures` 写进 guest 的那一块）、`GetEnvironmentVariableA/W`、
//   `GetEnvironmentStringsA/W`、`SHGetSpecialFolderPathA` / `SHGetFolderPathA` 全部从这一处取。
//   ⛔ 不许在 shim 里再写一份路径文本 —— 两套各说各话正是"引擎拿不到 LOCALAPPDATA、回退到
//   安装目录"那个 bug 的成因。
//
//   · guest 文本固定为 Windows 形状（`C:\Users\Amayui\...`），由 Vfs::resolve 映射：
//     `…\AppData\Local` → `<--appdata>/Local`、`…\AppData\Roaming` → `<--appdata>/Roaming`、
//     `C:\Users\Amayui\Documents` → `<--appdata>/Documents`；其余（USERPROFILE 本身）留在
//     `--game-dir` 的单一根下（⚠ 不会自动存在：那是 guest 世界的文本，不是宿主目录树）。
//   · **没有 `--appdata` 时，AppData 派生的变量一律不提供**（与 `SHGetSpecialFolderPathA` 的
//     FALSE + 5 一致；⛔ 绝不指向宿主真实 AppData）。理由：两个视图说同一件事比"env 有一个
//     解析时必然失败的路径"更容易诊断 —— 失败发生在取变量处，而不是几步之后的 VFS 解析里。
// =======================================================================================
const char* guestUserProfileText();    // C:\Users\Amayui
const char* guestAppDataText();        // C:\Users\Amayui\AppData\Roaming
const char* guestLocalAppDataText();   // C:\Users\Amayui\AppData\Local
const char* guestDocumentsText();      // C:\Users\Amayui\Documents
// TEMP / TMP / GetTempPathA 的共同落点（`…\AppData\Local\Temp`；需要 --appdata 才可解析）
std::string guestTempDirText();

// guest 环境块：有序 `NAME=VALUE` 列表（顺序 = 大小写不敏感的字典序，**确定性**）。
// `have_appdata=false` ⇒ 不含 APPDATA / LOCALAPPDATA / TEMP / TMP（见上面的理由）。
std::vector<std::string> guestEnvironment(bool have_appdata);
// 同一份内容的 `;` 连接文本：`ProcessSetup::environment_ansi` 与 A4 的 trace 输入哈希用它。
std::string guestEnvironmentText(bool have_appdata);

// ★ 2026-11-05（`REQ-01M6KNBE5FP87DAC93ZRQ1K1DK`）：**写覆盖层** —— `--game-dir` 只读
//
//   引擎把安装树当成可写目录（`CreateFileA(CREATE_ALWAYS)` / `MakeSureDirectoryPathExists` 写
//   `Eushully\天結…\$$AGE.TMP` / `SAVE\…`）。`--game-dir` 是**只读**来源 ⇒ 写不许落到那里。
//   规则（**只有这一条接缝**；⛔ 不是通用 `--work-dir`，plan D5 不变）：
//     · 有 `--appdata`：**落点本来在 --game-dir 的写**改到 `<--appdata>/Game/<相对路径>`（影子目录）。
//       guest 路径 / guest 根不变；影子目录按需创建（⛔ 不在安装树里造目录）。
//     · **读先影子、再安装树**：整条影子路径存在就用影子，否则回落到安装树
//       （"引擎先写、再读自己写的文件"能读到）；`find()` 把两边**合并**（同名影子优先）。
//     · `createFile` 的写打开会 **copy-up**（安装树 → 影子）：`OPEN_ALWAYS` / `r+b` 读改写不丢内容。
//     · ⚠ 有意偏差：`createFile` 的"会新建"分支允许**父目录按需创建** ⇒ 即使 guest 世界里那条
//       父目录还不存在也会成功（引擎的形态：先 `MakeSureDirectoryPathExists` 再写；覆盖层不能
//       反过来要求安装树里先有目录）。`CreateDirectoryA` 仍然要求父目录（影子 ∪ 安装树）存在。
//     · 没有 `--appdata`：**保持旧行为**（写仍落 `--game-dir`），但 shim 每次写都打一行警告 ——
//       ⛔ 不静默写安装树（`tests/file_io.cpp` 的夹具就是无 `--appdata` 的形态）。
//   ⚠ 已知边界（本轮只做接缝，**不是**完整 overlay）：**没有 whiteout** —— 只在安装树里存在的
//     文件 / 目录，`DeleteFileA` / `MoveFileA` / `RemoveDirectoryA` / `SetFileAttributesA` 会
//     **响亮失败**（⛔ 不动安装树）；引擎自己写出来的东西在影子里，照常可删 / 可改。
//   ⚠ 读侧优先级：**exe overlay（①）仍高于影子** —— 写 AGE.EXE 会进影子，但读仍读 `--exe` 拆壳件
//     （⛔ 绝不让写把正在跑的那份字节换掉）。
enum class VfsAccess { Read, Write };

// `resolveForWrite` 的选项（两个开关分开：**读改写**与**新建**要的东西不一样）
struct VfsWriteOptions {
    // 安装树里已有同名**文件**、而影子里还没有它 ⇒ 先复制进影子（否则 `r+b` 打开会失败 / 内容丢）。
    bool copy_up = false;
    // 影子的父目录按需创建（只有"会新建文件 / 目录"的调用方才开；删 / 改名 / 改属性不开）。
    bool create_parents = false;
};

// =======================================================================================
// 段查询的结果（`lookupSegment` 与 `Vfs::lookupSegmentIndexed` **共用**这一个定义）
//   ★ `Error` ⇒ `error` 里是**完整**的 tag 串（`[notdir]` / `[io]` / `[ambiguous]` / `[invalid]`）；
//     `Missing` ⇒ **不写** `error`（调用方按"最后一段 / 中间段"自己决定 tag，见 `installPathForSegments`）。
//   ★ 放头文件只是因为 `Vfs` 的私有成员函数签名要用它 —— 实现仍在 vfs.cpp 的匿名 namespace 里。
// =======================================================================================
enum class SegLookup { Found, Missing, Error };

class Vfs {
public:
    // 宿主根（绝对路径，UTF-8）。空 ⇒ ready() = false。
    void setGameDir(const std::string& host_dir);
    const std::string& gameDir() const { return game_dir_; }
    // ★ P0：伪造 AppData（--appdata）。只影响特殊目录（AppData / LocalAppData / Personal），
    //   不改变 --game-dir 是唯一游戏根的口径。空 = 未设置（特殊目录解析会响亮失败）。
    //   ★ 设置时会清掉 `<--appdata>/Local/Temp`（A4：`GetTempFileNameA(uUnique=0)` 的序号
    //     跨运行累积会让同一条命令第二次拿到不同名字 ⇒ trace 不再逐字节相同）。
    //   ★ 非空时同时启用**写覆盖层**（见文件上方的规则）；影子目录 `<--appdata>/Game`
    //     **不清理**（它是存档 / 配置的状态，不是可丢弃的临时区 —— 与 `Local/Temp` 相反）。
    void setAppDataDir(const std::string& host_dir);
    const std::string& appDataDir() const { return app_data_dir_; }
    bool ready() const { return !game_dir_.empty(); }
    // guest 环境块的真源（= `guestEnvironment(!appDataDir().empty())`）
    std::vector<std::string> guestEnvironment() const { return lift::guestEnvironment(!app_data_dir_.empty()); }
    std::string guestEnvironmentText() const { return lift::guestEnvironmentText(!app_data_dir_.empty()); }

    // guest 根的文本形式（缺省 `E:\Games\Eushully\天結`；只用于把路径的公共前缀剥掉与日志）
    void setGuestRoot(const std::string& guest_root) {
        guest_root_ = guest_root;
        invalidatePathCache();
    }
    const std::string& guestRoot() const { return guest_root_; }

    // ★ guest 眼里的主模块路径 = **guest 根之下的 `AGE.EXE`**（用户 2026-11-05 口径：
    //   引擎从 exe 路径推导"安装目录"时，必须得到游戏目录本身，而不是它的父目录）。
    //   ⛔ 宿主 `--exe` 字符串不再出现在 `GetModuleFileNameA` / `GetCommandLineA` 里。
    std::string guestExePath() const;

    // ★ VFS overlay（只对**这一个文件**生效；⛔ 不是通用路径重定向）：
    //   guest 打开 `guest_path` 时拿到的是 `host_path` 的字节（本模拟器真正在跑的拆壳件），
    //   而不是 `--game-dir` 下那份加壳原件。匹配 = 绝对化后的 guest 路径按段大小写不敏感相等，
    //   所以 `E:\…\AGE.EXE` / `e:\…\age.exe` / 相对路径（cwd = guest 根）都会命中。
    //   命中时 `resolve` 打一行日志（来源 + sha256 前 8 位）。
    void setExeOverlay(const std::string& guest_path, const std::string& host_path, const std::string& sha256);
    bool hasExeOverlay() const { return !overlay_guest_.empty(); }
    const std::string& overlayGuestPath() const { return overlay_guest_; }
    const std::string& overlayHostPath() const { return overlay_host_; }

    // guest 当前目录（绝对 guest 路径；缺省 = guestRoot）
    void setGuestCwd(const std::string& guest_path);
    const std::string& guestCwd() const { return guest_cwd_; }

    // 解析：guest 路径 → 宿主绝对路径。失败 ⇒ false，error 写清"哪一段 / 为什么"。
    //
    // ★ 匹配顺序（**最具体 → 最泛**；改这个顺序 = 改语义，改之前先想清楚）：
    //   ① **overlay**：就是 `setExeOverlay` 的那一个文件（绝对化 + 按段大小写不敏感比较）；
    //   ② **guest 根前缀**：`<guest_root>\…`（大小写 / `\`·`/` / 末尾分隔符变体全认）⇒
    //      余下部分**相对 `--game-dir` 根**，再走**同一套**分段解析（大小写不敏感、`..` 归一化、
    //      overlay、越界 `[escape]` 检查）——⛔ 剥完前缀直接拼宿主路径；
    //   ③ **`--appdata` 前缀**：`C:\Users\Amayui\AppData\{Roaming,Local}` 与 `…\Documents`；
    //   ④ **相对路径**：相对 guest cwd（缺省 = guest 根），剥盘符 / 前导分隔符后落到 `--game-dir`。
    //   ⛔ ② 只认**完整段前缀**：`…\Eushully\…` / `…\天結X` 不算根（不是"包含某一段就剥"）。
    //
    // ★ 写覆盖层（`access == Write`，见文件上方）：非特殊目录的路径**不落安装树** —— 落到
    //   `<--appdata>/Game/…`（父目录按需创建）；没有 `--appdata` 时退回旧行为（安装树路径，
    //   调用方负责打警告）。读侧先查影子（整条路径存在才算），再回落安装树。
    //
    // `must_exist` = false 时最后一段可以不存在（CreateFile(CREATE_NEW) / 写文件）。
    // `resolve_case` = true 时逐段做大小写不敏感匹配；false 时最后一段允许保留调用方大小写
    // （用于创建新文件：父目录仍然要做大小写匹配）。
    bool resolve(const std::string& guest_path, std::string* host_path, std::string* error,
                 bool must_exist = false, bool resolve_case = true,
                 VfsAccess access = VfsAccess::Read) const;

    // ★ 写覆盖层的**唯一入口**（`--game-dir` 只读的接缝；`kernel32_file.cpp` / `imagehlp.cpp` 用）：
    //   返回这条 guest 路径在宿主上的**写目标**。`install_tree` = 返回的宿主路径落在 `--game-dir`
    //   里（**只读**：调用方⛔ 不许写它；只应该按"它已存在"或"响亮失败"处理）。
    //   · 有 `--appdata`、落点是安装树 ⇒ 影子路径 `<--appdata>/Game/…`，`install_tree=false`；
    //   · 影子还没有、安装树里有同名**文件**且 `copy_up` ⇒ 先复制进影子（读改写不丢内容）；
    //   · 安装树里是**目录** ⇒ 返回安装树路径 + `install_tree=true`（调用方按"目标是目录 /
    //     已存在"处理，⛔ 不改它）；
    //   · 没有 `--appdata` ⇒ 旧行为（安装树路径）+ `install_tree=true`（调用方打警告后照旧写）；
    //   · AppData 特殊目录 ⇒ 原映射（本来就写 `--appdata`）+ `install_tree=false`。
    bool resolveForWrite(const std::string& guest_path, std::string* host_path, bool* install_tree,
                         std::string* error, const VfsWriteOptions& opts) const;

    // 写覆盖层是否启用（= `--appdata` 非空）。空 ⇒ shim 走旧行为 + 每次写打一行警告。
    bool writeOverlayEnabled() const { return !app_data_dir_.empty(); }
    // 影子目录的宿主路径 `<--appdata>/Game`（UTF-8；未启用 ⇒ 空串）。日志 / 诊断用。
    std::string writeOverlayDir() const;
    // `host_path` 是否在影子目录下（日志 / toGuest 用）
    bool isWriteOverlayPath(const std::string& host_path) const;
    // `host_path` 是否在 `--game-dir`（安装树）之下 = **只读来源**（写路径的防御性判定：
    // 句柄类写 API —— `SetFileTime` / `WriteFile` 的宿主侧 —— 靠它挡住"写回安装树"）。
    bool isInstallTreePath(const std::string& host_path) const;

    // ===================================================================================
    // ★ C7：**只读安装树内容缓存**（2026-11-23；口径见 vfs.cpp 的"内容缓存"段）
    //
    //   为什么：SAVE11 的读数把 `CreateFileA` 的 184 µs/次 拆开之后，**真正的 CreateFileW
    //   （宿主 open）单独就是 ~200 µs/次**（AV / 文件系统固定成本），`resolve` 再占 ~110 µs/次
    //   ⇒ 同一份只读内容被反复打开是纯浪费。写覆盖层启用时（有 `--appdata`）安装树按合同**只读**
    //   （见 `invalidatePathCache` 上方的显式假设）⇒ 内容可以缓存，校验只要一次 stat。
    //
    //   语义边界（**这些是判据，不许"看起来还能跑"就放宽**）：
    //   · 只对**读**生效，且调用方必须已经判定"纯读 + OPEN_EXISTING + 安装树之下"（判定在 shim，
    //     因为它才看得到 `dwDesiredAccess` / `dwCreationDisposition`）；
    //   · 每次命中都做**一次 stat**（size + write time）校验 —— 不一致就丢弃条目（安装树运行期
    //     不变是既有假设，但"变了就重读"这条兜底不要省：省了就变成静默旧数据）；
    //   · 条目内容**只读**：小文件整块进堆（计入字节预算），大文件 `mmap` 只读（不计入预算，
    //     由 OS 页缓存管）；两者都**不复制**进 guest 内存 —— 复制由 shim 做（那是它的职责）。
    //   · ⛔ 缓存键是**解析后的宿主路径**，⛔ 不是 guest 路径、⛔ 不按扩展名 / 目录名特判。
    // ===================================================================================
    struct ReadCacheEntryView {
        std::uint64_t size = 0;
        const std::uint8_t* data = nullptr;
    };
    // 预算（MiB；0 = **整个缓存关掉**，一次查找都不做）。缺省 256 MiB。
    void setReadCacheBudgetMb(std::uint64_t mb) { read_cache_budget_mb_ = mb; }
    std::uint64_t readCacheBudgetMb() const { return read_cache_budget_mb_; }
    // 缓存是否可能生效（= 预算非 0）。⛔ 这**不是**"这条路径该不该缓存"的判据（那在 shim）。
    bool readCacheEnabled() const { return read_cache_budget_mb_ != 0; }
    // 命中判定：返回 true ⇒ `out` 是条目视图（只读，有效期 = 本次句柄持有期间）。
    //   `cookie` 是条目所有权（引用 +1）—— 由调用方交给句柄，句柄销毁时自动释放。
    //   校验失败（stat 不一致 / 文件不再是普通文件）⇒ 丢弃条目并返回 false。
    bool readCacheLookup(const std::string& host_path, std::shared_ptr<const void>* cookie,
                         ReadCacheEntryView* out) const;    // 把一条内容存进缓存（`fp` 是调用方刚打开的那个只读 `std::FILE*`；位置会被恢复）。
    // 预算为 0 / 读失败 / 超预算 ⇒ 不存（`cookie` 空）。返回时 `out` 有效 ⇔ 返回 true。
    bool readCacheStore(const std::string& host_path, std::FILE* fp,
                        std::shared_ptr<const void>* cookie, ReadCacheEntryView* out) const;
    // 在读视图里定位（`data` 为空的条目 = 不该走到这里）。
    static void readCacheRelease(std::shared_ptr<const void>* cookie) { cookie->reset(); }
    // 缓存计数器（诊断 / 守卫用）：命中 / 未命中（含校验失败）/ 存入 / 逐出 / 当前字节。
    struct ReadCacheStats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t stale = 0;
        std::uint64_t stores = 0;
        std::uint64_t evictions = 0;
        std::uint64_t bytes = 0;       // 堆里的字节（计入预算的那部分）
        std::uint64_t entries = 0;
        std::uint64_t mapped_bytes = 0;  // mmap 条目的地址空间（**不计入**预算，只报出来）
    };
    ReadCacheStats readCacheStats() const;
    // 清空（配置变化 / 守卫用）。
    void clearReadCache();

    // ===================================================================================
    // ★ C6：**读**路径**负**缓存（只缓存失败结论）的失效点。
    //
    //   失效口径 = **按代（generation）**：每次调用 `++path_cache_gen_`，旧代的条目在下次查找时
    //   就地淘汰。⛔ 不需要 O(n) 清表，也不需要知道"改的是哪条路径"。
    //   调用点（**任何改变某条 guest 路径存在性的宿主写之后**）：
    //     · 影子目录 / 文件的创建（`shadowPathForSegments(create_dirs=true)`、copy-up）；
    //     · `makeSureDirectoryPathExists`（影子目录 + AppData 映射脚手架）；
    //     · shim 侧的成功写：`CreateFileA` 新建、`DeleteFileA`、`MoveFileA`、`CopyFileA`、
    //       `CreateDirectoryA`、`RemoveDirectoryA`、`GetTempFileNameA(uUnique=0)`；
    //     · 配置变化（`setGameDir` / `setAppDataDir` / `setGuestRoot` / `setGuestCwd` / `setExeOverlay`）。
    //   ⛔ **安装树**（`--game-dir`）本身不做失效：写覆盖层启用时它按合同是**只读来源**
    //     （资源目录不变 —— 显式假设，见私有段的 `NegativeKey` 上方）；没有 `--appdata` 时
    //     它是写目标 ⇒ 缓存整个停用（`resolve` 里的 `cacheable`）。
    //   ⚠ 漏一个失效点 = 读到旧结论（最常见：**自己刚写的文件读不到**）。本轮只缓存失败 ⇒
    //     删除 / 改名侧的 +1 代是保守项（把"存在性变化"这套钩子补齐，将来做正缓存也靠它）。
    //     新增写路径时必须同时加到这里与 vfs.cpp 的对应位置。
    // ===================================================================================
    void invalidatePathCache() const;

    // ===================================================================================
    // ★ C5：**安装树目录索引** + C8：**正解析缓存**（2026-11-25；实现在 vfs.cpp 的同名段）
    //
    //   为什么（读数见 `.tmp/reveq/REQ-01M86NP7RNP65YCC4709HZ7AN7/analysis.md`）：C7 把
    //   `CreateFileA` 压到命中 88–114 µs 之后，剩下的钱仍花在解析上 —— 命中路径要 2–3 次精确
    //   stat（53–62 µs），而失败探测每次都把安装树根目录**整目录枚举**一遍（542 项；388 次枚举
    //   = 121.5 ms）。两笔各有一层记忆：
    //
    //   · **目录索引**（`install_dir_index_`）：每个目录**一次** `directory_iterator` 建
    //     "折叠名 → 候选"表。折叠规则与 `lookupSegment` **同一条**（`asciiLowerChar` 逐字节，
    //     非 ASCII 原样）；同一个折叠键的**多个候选全部保留** ⇒ `[ambiguous]` 照样会报，
    //     ⛔ 不用单值表静默挑一个。它替的是"逐段精确 stat → 未命中再整目录枚举"这条老路。
    //   · **正解析缓存**（`positive_cache_`）：与负缓存**同一把键、同一代**（`cacheable` 的判据
    //     一行不动；`invalidatePathCache` 只换代，两张表按代一起淘汰）。缓存的是**宿主路径**
    //     （内容缓存是另一件事，见上面的 C7 段）。
    //
    //   ★ 两层共同的**显式假设**（与负缓存同一条，原文见私有段 `PositiveEntry` 上方）：
    //     写覆盖层启用时（有 `--appdata`）`--game-dir` 是**只读**来源，运行中不从外部改它。
    //   ⇒ 没有 `--appdata` 时三层记忆**全部停用**（那时安装树就是写目标）：
    //     负 / 正表看 `resolve` 的 `cacheable`，目录索引看 `dirIndexUsable()`。
    //   ⛔ `find()` / 目录枚举不在本层（`FindFirstFile` 的排序 / 增删语义另说）。
    // ===================================================================================
    // `--vfs-dir-index 0`（A/B 与守卫）：关掉目录索引 ⇒ `installPathForSegments` 回到逐段
    //   `lookupSegment`。缺省开。⛔ 它不是语义开关 —— 两档的 `ok / host / error` 必须逐字节相同。
    //   ⚠ 只在**启动 / 夹具**里设置（不是并发开关：它没有自己的锁，见私有段）。
    void setDirIndexEnabled(bool on);
    bool dirIndexEnabled() const { return dir_index_enabled_; }
    // 安装树目录索引的**显式**失效点：只在"宿主侧改动了安装树"（测试夹具 / 诊断）时调用。
    //   产品路径不需要它（安装树只读）；`setGameDir` 自己会清（根变了）；
    //   `setDirIndexEnabled` 在值**变化**时也清（A/B 两档各自从空表开始，读数才不串）。
    //   ★ 清表**同时把计数归零**（表与计数一起回到"从没建过"；否则"表里条目数"这类读数会被
    //     上一次建表带偏 —— `vfs-dir-index-parity` 就是靠它把两档切开）。
    void clearDirIndex();
    // 正解析缓存的关档（守卫的 A/B 用；缺省开）。⛔ 同一条边界：两档 `ok / host / error` 逐字节同。
    //   ⚠ 同在启动 / 夹具里设置（不是并发开关）。
    void setPositiveCacheEnabled(bool on) { positive_cache_enabled_ = on; }
    bool positiveCacheEnabled() const { return positive_cache_enabled_; }
    // 两层的计数（**不受 `LIFT_VFS_TIMING` 门控**：守卫拿它证明"真的被用到了"，否则
    //   "开↔关逐位相同"可能只是两层都没生效）。⛔ 只读计量，不参与任何判定。
    struct PathResolveStats {
        std::uint64_t dir_index_builds = 0;   // 目录索引建表次数（每个目录一次）
        std::uint64_t dir_index_lookups = 0;  // 走索引的段查询次数
        std::uint64_t dir_index_entries = 0;  // 表里累计记下的目录项数（= 各被建过表的目录的条目数之和）
        std::uint64_t positive_hits = 0;
        std::uint64_t positive_stores = 0;
    };
    PathResolveStats pathResolveStats() const;

    // 反解：宿主路径 → guest 路径（用于 GetCurrentDirectory / GetModuleFileName；
    // 不在根下 ⇒ false）。
    bool toGuest(const std::string& host_path, std::string* guest_path) const;

    // 枚举 guest 目录下匹配通配符的条目（排序；`.` / `..` 不返回）。
    // `pattern` 是 guest 路径（可含 `*` / `?`）；目录部分必须存在。
    bool find(const std::string& guest_pattern, std::vector<HostDirEntry>* out, std::string* error) const;

    // 目录是否存在 / 文件是否存在（大小写不敏感）
    bool isDirectory(const std::string& guest_path) const;
    bool isFile(const std::string& guest_path) const;

    // ★ P4：`imagehlp!MakeSureDirectoryPathExists` 的落点（递归建目录）。
    //   口径（照 Win32 文档 + 引擎的实际用法，见 imagehlp.cpp 的文件头）：
    //   · 路径里**每个分隔符（`\` / `/`）之前**的段都是目录 ⇒ 必须存在；
    //   · **末尾分隔符**把最后一段也标成目录（引擎就是这么调的：`.lst` 0x00490383 先补 `\`）；
    //   · **没有**末尾分隔符时最后一段按**文件名**处理，⛔ 不建它；
    //   · 连续分隔符（`\\`）没有段 ⇒ 跳过；盘符 / guest 根 / `--appdata` 特殊目录这些"根"不创建
    //     （它们本来就在，⛔ 不许为 `E:\Games` 这种根前缀在宿主上造出一棵假目录树）；
    //   · **已存在算成功**（这是它与 `CreateDirectoryA` 的关键区别）。
    //   返回 false 时 `error` 写清是哪一段 / 为什么（调用方映射成 Win32 失败值）。
    bool makeSureDirectoryPathExists(const std::string& guest_path, std::string* error) const;

    // 常用：把 guest 路径规范化成"绝对 guest 路径"（不改大小写；用于日志 / 当前目录）
    std::string absoluteGuest(const std::string& guest_path) const;
    // ★ `GetFullPathNameA` 用：**纯文本**规范化（相对路径相对 guest cwd；折叠 `.` / `..`；
    //   `..` 到根即止；隔离符统一成 `\`），⛔ 不查宿主盘、不做大小写解析 —— Windows 的
    //   GetFullPathName 也不做（文档原话："does not verify that the resulting path ... sees an
    //   existing file"）。空串 ⇒ 返回空串（调用方映射成 ERROR_INVALID_PARAMETER）。
    std::string canonicalGuest(const std::string& guest_path) const;
    // 取父目录（guest 路径）；没有则空串
    static std::string parentGuest(const std::string& guest_path);
    static std::string baseName(const std::string& guest_path);

private:
    // ★ overlay 命中：`guest_path` 是否就是被覆盖的那一个文件（绝对化 + 大小写不敏感按段比较）
    bool matchesOverlay(const std::string& guest_path) const;

    // ---- 写覆盖层的内部脚手架（解析规则见文件上方；两份实现只在这里，⛔ shim 不自己拼宿主路径）----
    // guest 路径命中 `--appdata` 特殊目录 ⇒ true；`sub` = Roaming / Local / Documents，
    // `rest` = 前缀之后的文本（`/` 形态），`guest_form` = 命中的 guest 前缀（错误信息用）。
    bool matchSpecialDir(const std::string& guest, std::string* sub, std::string* rest,
                         std::string* guest_form) const;
    // 段 → 安装树（`--game-dir`）宿主路径。**resolve / resolveForWrite / find 共用这一份**。
    bool installPathForSegments(const std::vector<std::string>& segs, bool must_exist,
                                bool resolve_case, std::string* host_path, std::string* error) const;
    // 段 → 影子宿主路径。`create_dirs` = **中间**目录按需创建（最后一段永远不建：它可能是文件）。
    bool shadowPathForSegments(const std::vector<std::string>& segs, bool create_dirs,
                               std::string* host_path, std::string* error) const;
    // 段在影子里**整条都存在** ⇒ true + 宿主路径；不存在 ⇒ false（`error` 只在真错误时非空）。
    bool shadowExistingForSegments(const std::vector<std::string>& segs, std::string* host_path,
                                   std::string* error) const;
    // 影子根 `<--appdata>/Game` 的宿主路径（未启用 / 未设置 ⇒ 空串）
    std::string shadowRoot() const;

    // ---- C5：安装树目录索引（每个目录一张"折叠名 → 候选"表；懒建；口径见 public 段的 C5/C8）----
    //   ★ 键 = `asciiLowerChar` 逐字节折叠（**非 ASCII 原样** = `lookupSegment` 的同一条规则）；
    //     值里**保留全部候选** —— 大小写敏感的宿主上"只差 ASCII 大小写的两个真名"必须照样报
    //     `[ambiguous]`，⛔ 不能用单值 map 静默挑一个（Linux/macOS 上会真的发生）。
    //   ★ `is_dir` 如实记录：`lookupSegmentIndexed` 把它交给 `installPathForSegments`，后者用它
    //     在"中间段其实是文件"时省掉下一次索引建表的尝试（**不提前改判据** —— 那种情况现有实现
    //     会在下一条段上按 `[notdir]` / `[notfound]` 分岔，见 `installPathForSegments` 的 tag 注释）。
    struct DirEntry {
        std::string real;       // 宿主目录里的真实文件名（UTF-8）
        bool is_dir = false;    // 是否目录（符号链接按"跟随之后"算，与 `fs::is_directory` 同）
    };
    using DirIndex = std::unordered_map<std::string, std::vector<DirEntry>>;
    enum class DirIndexStatus { Ready, NotDir, Error };
    // 取（或懒建）`dir_host` 的表。**调用方必须持 `dir_index_mu_`**。返回 nullptr 时看 `*status`：
    //   `NotDir` = 不是可枚举目录（`[notdir]` 的文本由 `lookupSegmentIndexed` 按现有**次序**补 ——
    //   那里才知道该不该报）；`Error` = 枚举失败（`*error` 是 `[io] …`，与现有文本逐字相同）。
    const DirIndex* dirIndexFor(const std::string& dir_host, DirIndexStatus* status,
                                std::string* error) const;
    // 与 `lookupSegment` **逐字等价**的段查询（走索引）：规则见 vfs.cpp 的"目录索引"段。
    //   `dir_is_known_dir=false` ⇒ 调用方已知道这条路径不是可枚举目录（上一段是文件）⇒ 省一次 stat。
    SegLookup lookupSegmentIndexed(const std::string& dir_host, const std::string& seg, bool exact_only,
                                   bool dir_is_known_dir, std::string* real_name, bool* real_is_dir,
                                   std::string* error) const;
    // 索引能不能用 = 开关 + "安装树只读"这条前提（与 `resolve` 的 `cacheable` 同一条判据：
    // 没有 `--appdata` 时安装树就是写目标 ⇒ 建出来的表随时会过期）。
    bool dirIndexUsable() const { return dir_index_enabled_ && writeOverlayEnabled(); }
    mutable std::mutex dir_index_mu_;      // 控制层另一条线程也会 `resolve` ⇒ 懒建必须加锁
    mutable std::unordered_map<std::string, DirIndex> install_dir_index_;  // 键 = 宿主目录 UTF-8
    mutable std::uint64_t dir_index_builds_ = 0;
    mutable std::uint64_t dir_index_lookups_ = 0;
    mutable std::uint64_t dir_index_entries_ = 0;
    bool dir_index_enabled_ = true;

    // ---- C6：读路径负缓存（键 = 已折叠的段序列 + 两个影响结果的开关；**只缓存失败**）-----
    //   ★ 为什么键是段而不是原始文本：`resolve` 走到段序列之后只看 `segs`（影子的逐段查找 +
    //     安装树的逐段查找），而 `..` / `.` / 分隔符 / guest 根前缀 / cwd 都已折叠进 `segs`
    //     ⇒ 同一个键必然走同一条解析路径、得到同一个结论（错误串里的宿主路径也由段决定）。
    //   ★ 为什么把 `must_exist` / `resolve_case` 也放进键：它们改变"最后一段缺失"与
    //     "最后一段大小写"的结果（`installPathForSegments` 的入参）。少一个都会串结论。
    //   ★ 正结论本来**不**缓存（C6 的裁决，读数见 `.tmp/perf-reassess/c6-vfs-cache.md`：那时
    //     命中段只是**一次精确 stat**，收益在噪声内）。C5 的目录索引把这"一次精确 stat"也拿掉
    //     之后，正结论的重复探测就只剩"两次哈希"这一档成本 ⇒ C8 把正表加回来，与负表**同一把键、
    //     同一代**（见下面的 `PositiveEntry`）。⛔ 两张表不许各用一套代号：`invalidatePathCache`
    //     只 `++path_cache_gen_`，任何一边漏查代就会静默返回旧结论。
    //   ★ **显式假设**（正 / 负 / 目录索引三层都依赖它）：写覆盖层启用时（有 `--appdata`），
    //     `--game-dir` 安装树是**只读**来源 —— 运行中不从外部改它（没有热更新的资源目录）。
    //     开发期"运行中换安装树里的文件"不再被支持；不启用 `--appdata` 时三层记忆整个停用
    //     （那时安装树就是写目标）。
    //   ⛔ 不缓存 `find()` / 目录枚举（排序 / 增删语义另说）。
    struct NegativeKey {
        std::vector<std::string> segs;
        bool must_exist = true;
        bool resolve_case = true;
        bool operator==(const NegativeKey& other) const {
            return must_exist == other.must_exist && resolve_case == other.resolve_case &&
                   segs == other.segs;
        }
    };
    struct NegativeKeyHash {
        std::size_t operator()(const NegativeKey& key) const;
    };
    // ★ 正 / 负两张表**共用同一把键**（段序列 + 两个改变结果的开关）⇒ 用一个别名，⛔ 不复制一份
    //   同名结构体（复制出来迟早漂一个字段，那时同一个 `segs` 会在两张表里算出不同结论）。
    using PathKey = NegativeKey;
    struct NegativeEntry {
        std::uint64_t gen = 0;  // 写入时的影子代号；≠ 当前代 ⇒ 结论可能已过期（就地淘汰）
        std::string error;      // **完整**错误串（tag + 说明）：shim 靠 tag 映射 Win32 错误码
    };
    // 命中 ⇒ true（`host_path` 清空、`error` = 缓存的失败原因）；未命中 / 已过期 ⇒ false。
    bool lookupNegative(const std::vector<std::string>& segs, bool must_exist, bool resolve_case,
                        std::string* host_path, std::string* error) const;
    void rememberNegative(const std::vector<std::string>& segs, bool must_exist, bool resolve_case,
                          const std::string& error) const;
    // 缓存条目上限（防御性：引擎会探测大量"可能不存在"的名字；超限就整表丢掉重来）。
    static constexpr std::size_t kMaxNegativeEntries = 8192;
    // ---- C8：正解析缓存（只存**宿主路径**；与负表同一把键 / 同一代 / 同一把锁）----
    //   ★ 存的是"这条段序列解析出来的宿主路径"，⛔ 不是内容（内容在 C7）、⛔ 不是"存在性"。
    //     `must_exist=false` 的成功结果也安全：路径只由段序列决定。
    //   ★ 不存的两类（它们都在段序列之前就返回，天然进不来）：`--exe` overlay 与 `--appdata`
    //     特殊目录（AppData 映射走另一支逐段查找）。
    //   ★ 失效只有一条：`invalidatePathCache()` 的**换代**（影子新建 / copy-up / 删除 / 改名 /
    //     配置变化都会调）—— 写作"删掉换代"的变异必须让 `vfs-resolve-cache-parity` 的写后读红。
    struct PositiveEntry {
        std::uint64_t gen = 0;  // 写入时的影子代号；≠ 当前代 ⇒ 就地淘汰（与 `NegativeEntry` 同一条）
        std::string host;       // 当时解析出来的宿主绝对路径（UTF-8）
    };
    // 命中 ⇒ true（`host_path` = 缓存的宿主路径）；未命中 / 已过期 / 关档 ⇒ false。
    bool lookupPositive(const std::vector<std::string>& segs, bool must_exist, bool resolve_case,
                        std::string* host_path) const;
    void rememberPositive(const std::vector<std::string>& segs, bool must_exist, bool resolve_case,
                          const std::string& host_path) const;
    // 条目上限（与负表同一条防御性口径：超限就整表丢掉重来）。
    static constexpr std::size_t kMaxPositiveEntries = 8192;
    // ★ `resolve` 是 const，而缓存是它的 memo ⇒ 成员必须 mutable。
    // ★ 必须加锁：控制层（`control_snapshot.cpp` 的 `vfs.resolve`）跑在**另一条线程**上；
    //   缓存是读解析时**写**的 ⇒ 无锁就是数据竞争（不是"本线单线程"能兜住的假设）。
    //   ★ 负表 / 正表 / 代**共用这一把锁**：换代与两张表的写入必须是同一个临界区，否则
    //     "刚换完代、旧代的正条目又写进来"会让旧结论活下来。
    mutable std::mutex path_cache_mu_;
    mutable std::uint64_t path_cache_gen_ = 0;
    mutable std::unordered_map<PathKey, NegativeEntry, NegativeKeyHash> negative_cache_;
    mutable std::unordered_map<PathKey, PositiveEntry, NegativeKeyHash> positive_cache_;
    mutable std::uint64_t positive_hits_ = 0;
    mutable std::uint64_t positive_stores_ = 0;
    bool positive_cache_enabled_ = true;

    // ---- C7：只读内容缓存（实现在 vfs.cpp 的"内容缓存"段）----
    //   ★ 条目 = {size, mtime, 内容}，键 = 解析后的宿主路径。LRU 顺序存在 `read_cache_lru_` 里
    //     （**表头 = 最久未用**）；字节预算只算**堆**条目（mmap 条目单列，只报地址空间）。
    //   ★ 内容所有权用 `shared_ptr<const void>`（cookie）：句柄在、内容就在 ⇒ 逐出只是从表里
    //     摘掉，不会把别人正在读的映射 `unmap` 掉（那是 use-after-free）。
    struct ReadCacheEntry {
        std::shared_ptr<const void> cookie;
        const std::uint8_t* data = nullptr;
        std::uint64_t size = 0;
        std::uint64_t mtime = 0;     // 存的时候的宿主 write time（100 ns 自 1601）
        std::uint64_t heap_bytes = 0;  // 计入预算的字节（mmap 条目 = 0）
        std::uint64_t map_bytes = 0;   // mmap 的地址空间（不计入预算，只报出来）
        std::list<std::string>::iterator lru_pos;  // 在 `read_cache_lru_` 里的位置（O(1) 搬移）
    };
    using ReadCacheMap = std::unordered_map<std::string, ReadCacheEntry>;
    // 从表里摘掉一个条目（调用方持锁）。`ReadCacheMap::iterator` 有效。
    void dropReadCacheEntry(ReadCacheMap::iterator it) const;
    mutable std::mutex cache_mu_;
    mutable ReadCacheMap read_cache_;
    mutable std::list<std::string> read_cache_lru_;  // 表头 = 最久未用，表尾 = 最近使用
    mutable ReadCacheStats read_cache_stats_;
    std::uint64_t read_cache_budget_mb_ = 256;       // 0 = 关
    // 条目数上限（防御性：mmap 条目不占字节预算，但每个都占地址空间）。
    static constexpr std::size_t kMaxReadCacheEntries = 512;
    // 小文件阈值：≤ 它整块进堆，> 它走只读 mmap（"别让默认档吃光内存"）。
    static constexpr std::uint64_t kReadCacheSmallMax = 1ull << 20;

    std::string game_dir_;
    std::string app_data_dir_;
    std::string guest_root_ = "E:\\Games\\Eushully\\天結";
    std::string guest_cwd_;
    bool guest_cwd_set_ = false;
    // overlay（只允许一个文件；见 setExeOverlay）
    std::string overlay_guest_;
    std::string overlay_host_;
    std::string overlay_sha256_;
};

// 跨平台 FILE_ATTRIBUTE_*（只实现本线用到的几个）
constexpr std::uint32_t kAttrReadOnly = 0x00000001u;
constexpr std::uint32_t kAttrDirectory = 0x00000010u;
constexpr std::uint32_t kAttrArchive = 0x00000020u;
constexpr std::uint32_t kAttrNormal = 0x00000080u;

}  // namespace lift
