// emulator-lift · Shape A / T1a：`emulator-lift-gen`（构建期 host tool）
//
// 职责（只有两件）：
//   ① **分区**：把 `--roots` 给的入口 EA 集合扩成"入口 = 函数入口 ∪ 每条 call 的下一条 EA"，
//      再按**分量级直跳**切成互不重叠的入口函数（一个分量 = 一个 `.ll` 函数）；
//      同一块被两个入口函数同时声明 ⇒ **响亮**报错并把它提升成新入口 EA 重跑，直到收敛。
//   ② **发射**：交给 `lift_gen_emit.cpp`（语义只照解释器臂抄；T1c 起"不 inline"的指令走
//      单指令宿主原语 `@lift_one`，⛔ 不再 `llvm.trap`；`llvm.trap` 只留给生成器内部不一致）。
//
// ★ D10 P3（T1c）：**派生 leader 可丢、根不可丢**。`--roots` 里的 EA 解不出 / 不在可执行节
//   ⇒ exit 4；派生 leader（call 返回 / jmp·jcc 目标 / jcc 落点）解不出或落在已发射块的字节
//   区间内 ⇒ 丢弃 + 计数 + 样例（`--verbose` / 汇总行），它们的边发 `ret i32 <EA>` 交回 trampoline。
//
// ★ D14（T3a）：**静态调用闭包**（默认开，`--no-close-calls` 关）。解码过程中凡 `call rel32` 的
//   **目标**，若 ① 不在当前入口集 ② 不是 shim 桩 EA（`GuestLayout::shim_page` 那 32 KiB）③ 落在
//   可执行节内 ⇒ 自动成为一个新的 unit 根（**软根**），加进工作集迭代到不动点（单调 ⇒ 收敛）。
//   ⇒ "时序上没走到、但静态可达"的调用目标一次补掉（T3a 的 `0x004AA1D0` 就是这一类）。
//   软根**解不出** / 落在**已发射块的字节区间内** ⇒ 丢弃 + 计数 + 样例（⛔ 不硬红、⛔ 不静默）；
//   `--roots` 的根仍是**硬**：解不出 / 越界 ⇒ exit 4（现状不变，D14）。
//
// ★ D15（G1）：**第二类软根 = 代码指针立即数**（默认开，`--no-close-stores` 关）。解码过程中凡
//   **写内存的 `mov`**（`mov [reg(+disp(+index*scale))], imm32`，含 `disp=0` 与绝对地址形态）的那个
//   **imm32**，若 ① 落在可执行节内 ② 能解码 ③ **不落在已发射块的字节区间内** ⇒ 收成软根，
//   同样加进工作集迭代到不动点。判据"这个 imm 是内存写的值"的**形态**只写在 `offerStoreImmTarget`
//   一处（`op == Mov && op0().kind == Mem && op1().kind == Imm`，对应 `exec_integer.cpp` 的
//   `wr(op0, rd(op1))`）—— ⛔ 不是"任意 4 字节常量"：`push <imm32>` / `cmp eax, <imm32>` /
//   `mov eax, <imm32>` 的立即数**都不收**（它们不是"写内存的值"）。
//   ⇒ 补掉"函数指针表初始化"这一类（运行期经 `call reg/mem` 到达，全 PE 没有一条 `call rel32`
//   指向它；G1 的 `0x00422E00` 就是这一类：`0x00416A24` 的
//   `mov [esi+0A5884h], offset sub_422E00`）。
//   丢弃照 D14，但**按类分开记**：越界 / 解不出 / 重叠 三类各一个数，⛔ 不与 call 类合并成
//   一个数（两类软根的账必须能分别读出）。
//
// ★ D17 / D18（G3）：**第三类软根 = 非可执行节里静态初始化的代码指针**（默认见下；`--no-close-data`
//   关、`--close-data` 开）。判据（父级给定，只有 `scanDataPointers` 一处）：
//   **非可执行节**里**4 字节对齐**的 dword，值落在**可执行节**内 ⇒ 软根候选。这一类与 D14/D15
//   **都正交**：`.data` 里 PE 阶段就写好的函数指针表，没有任何静态代码边指向它，也不是运行期
//   `mov [mem], imm32` 建出来的（G2 取证：`0x004D2410` 在 `.data` 里 7 处、`call rel32` 命中 0、
//   imm32 写内存站点 0）。收/丢与三档丢弃**照 D14/D15 同一套**（`offerSoftRoot` 一处判据）：
//   解不出 / 落在已发射块字节区间内 ⇒ 丢弃 + 计数 + 样例（⛔ 不硬红）。候选集是**固定集合**
//   ⇒ 在代码闭包收敛之后一次性塞进工作集（这样 D14/D15 的账与 G2 逐项可比，data-ptr 只记净增）。
//   ★ 尺寸护栏（父级硬性要求）：这一类一次可能塞进近 3000 个根 ⇒ `.ll` 字节数 / clang 时间 /
//   obj 字节数 / entries·blocks·insns 增量全部要报。若 `.ll` > 250 MB 或 `clang -c -O2` > 300 s
//   ⇒ **默认改成关**（`--close-data` 才开）。⛔ 不许为让读数好看而裁剪候选（少收就是漏）。
//
// ★ D18：`--dump-blocks <file>` —— 只读覆盖率读数：把**最终 units 的块集合**（每块 start EA +
//   字节长度 + 指令数，升序、文本、确定性）写一个文件。⛔ 不影响 `.ll` / 派发表（0 派发成本），
//   用途是让 `.tmp/aot/sweep/` 那套脚本能算"闭包还会不会漏"（不用再从 `bb_` 标签 + llvm 长度反推）。
//
// ★ D21（第四类）：**可执行节里的跳表 dword**。`.text` 里的跳表（`jmp ds:jpt_xxx[eax*4]` 的操作数表）
//   存的是 case 目标的 EA；运行期经 `ret i32 <EA>` 回 trampoline 时，若那个 EA 不在入口集 ⇒
//   查不到 ⇒ 响亮停（G3 §8.1 的 `0x004C76FC`）。判据：**可执行节**里 4 字节对齐的 dword，值
//   **等于某个已建块的首 EA** ⇒ 只把它加进**拥有那个块的分量**的 `entries`（switch 多一个 case +
//   派发表多一行），**0 新块 / 0 新指令**（这一趟不解码，只查表）。⛔ 不做成新软根（那会引入新块）。
//   落点：`addJumpTableEntries`（在分区收敛之后跑 —— 判据要"拥有该块的分量"）。
//
// ★ D26（第五类）：**可执行节里的跳表 dword → 尚未解码的代码**（默认见下；`--no-close-jmptab` 关）。
//   D21 的判据要求"值**已经是**已发射块的首 EA"⇒ 它结构性覆盖不到"跳表目标**从未被解码**"那一类
//   （间接 `jmp` 没有静态后继，静态闭包走不到那些字节）。WndProc 的消息跳表 `jpt_4B9338`
//   （`.text:004BA0DC`，13 项）就是这一类：只有 `004B95CC` 是已建块首，其余 12 个（含
//   `WM_KEYDOWN=0x100` → `004B9367`、`WM_CLOSE=0x10` → `004B95EC`）**根本没有块** ⇒ 按一个键就
//   `decode-error`、guest 驱动的关窗也停。判据（与 D14/D15/D17 **同一套**机制，区别只在"会新增块"）：
//   **可执行节**里 4 字节对齐的 dword，值 ① 落在**可执行节**内 ② **不在入口集** ③ **不是已建块首**
//   （第四类已处理）④ **能解码** ⑤ **不落在已发射块的字节区间内** ⇒ 收成**软根**（新 unit 根）。
//   收/丢全部走 `offerSoftRoot`（越界 / 解不出 / 重叠 三档分开计数 + 3 个样例，⛔ 不硬红、⛔ 不静默）。
//   ★ 与 D17 一样在"建块之后"判（③⑤ 需要块），不同点是**会新增块** ⇒ 新块又可能挡住别的候选
//     ⇒ 在 `buildUnits` 里**迭代到不动点**（`rejected_targets_` 单调 ⇒ 必然收敛）。
//   ★ 尺寸护栏：`.ll` > 320 MB **或** `clang -c -O2` > 400 s ⇒ 缺省**关**（`--close-jmptab` 才开）。
//
// ★ D26 的第二条扫描（同一类软根，不是第六类）：**可执行节里任意偏移的指令 imm32**
//   （`push imm32` = `68 id` · `mov r32, imm32` = `B8+rd id` / `C7 /0 id`）。上面那条"4 字节对齐
//   dword"结构性看不到**指令流里的 imm32**（它的地址由 opcode 位置决定）。本语料三条实证：
//     · `0x004F80E0`（`LanguageEnumProc`）：唯一引用是 `0x4F822A` 的 `push $0x4F80E0`，
//       imm32 在 `0x4F822B`（mod 4 = 3）⇒ 旧扫描抓不到，以前靠 `closure.txt` 的手工根；
//     · `0x00513B90`（`SEH_412290`，`sub_412290` 的 SEH handler chunk）：唯一引用是 `0x412295` 的
//       `push offset SEH_412290`，imm32 在 `0x412296`（mod 4 = 2）⇒ TITLE→Quit 的 C++ throw 展开到它
//       时"派发表里没有这个 EA"；
//     · `0x0041389A`（`sub_412290` 的 `catch(Command_Exit_Exception)` 续延）：唯一引用是
//       `0x413562` 的 `mov eax, offset loc_41389A`（catch 体把续延地址交给 `___CxxFrameHandler3`），
//       imm32 在 `0x413563`（mod 4 = 3）⇒ push 扫不到它，Quit 的下一跳又会停在派发表外。
//   ⇒ 判据：可执行节里**任意偏移**的 `68` / `B8..BF`（/ `C7` + `mod=11, reg=000`）+ 紧随 4 字节
//     （小端）落在可执行节内 ⇒ 候选目标，与对齐 dword 的候选**并集**，收/丢仍走 `offerSoftRoot`
//     （越界 / 解不出 / 重叠三档分开记），输入面读数（站点 / 落在执行节内 / 仅这条扫描发现）
//     **单独报**。⛔ 不为此新增 SoftKind。
//   ★ 为什么不是"任意偏移的任意 4 字节"：只认**会携带 imm32 的 opcode**（`68` / `B8..BF` / `C7 /0`），
//     这是"这个 imm32 是代码指针"的最窄形态证据；opcode 字节作为别的指令的操作数字节 / 数据常量
//     会撞进来，由 `offerSoftRoot` 的"在可执行节 + 能解码 + 不与已发射块重叠"三道过滤器接住
//     （本语料 push 802 站点 / 539 目标、mov 里 126 个新目标，handler chunk / catch 续延 /
//     线程 `StartAddress` 等都是真入口）。
//   ★ 与 D15 的边界：D15 只认 `mov [mem], imm32`（"写内存的值"）；`push imm32` / `mov reg, imm32`
//     **不是**写内存，两者不重叠、账也分开。
//
// CLI：
//   --exe <PE>            字节真源（PE 节表映射进 guest 后交给 `Decoder`）
//   --roots <file>        一行一个 hex EA（`#` 起注释）；本轮**只**认这一种输入形状
//   --out <file.ll>       生成的 LLVM IR（落 `out/aot/`，⛔ 不 commit）
//   --no-close-calls      关掉静态调用闭包（D14 的对照档：应与"只按 closure.txt"逐字节相同）
//   --close-data / --no-close-data   第三类软根（`.data` 静态代码指针）显式开 / 关（默认见 usage）
//   --close-jmptab / --no-close-jmptab  第五类软根（可执行节跳表 dword → 尚未解码的代码）显式开 / 关
//   --dump-blocks <file>  只读读数：最终块的 start EA + 字节长度 + 指令数（升序文本）
//   --max-components N    分量数上限（超过 ⇒ 参数错；缺省不限）
//   --verbose             逐分量读数
//   --help
//
// 退出码（父级要求可分辨）：
//   0 成功 / 2 参数或输入文件错 / 3 有 unsupported（`.ll` 仍写全）/ 4 解码失败 / 1 生成器内部不一致
//
// ★ 解码入口**只有一条**：`Decoder::decode`（`decode.h:34`）。⛔ 不另写操作数文法。
// ★ 本工具**不**做 census、不建 `closure.txt`、不把批量 IR 链进 AOT（那是 T1c）。
// ★ 确定性：所有容器按 EA 升序迭代、SSA 临时名按发射顺序编号、无时间戳/路径进 `.ll` ⇒ 同输入同字节。
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "aot/aot_hook_ea.h"
#include "aot/closure_scan.h"
#include "aot/lift_aot_text.h"
#include "aot/lift_gen_emit.h"
#include "aot/lift_iat.h"
#include "aot/lift_struct.h"
#include "runtime/decode.h"
#include "runtime/exec.h"
#include "runtime/host_encoding.h"
#include "runtime/image.h"
#include "runtime/memory.h"
#include "runtime/sha256.h"  // `--split` 清单里每份的 sha256（与 PE pin 同一份实现）
#include "shims/register.h"

// ★ S7-IAT（REQ-01M86NMT0YJ9V78XS2DZZV364Q）：`call *<IAT 槽>` 的清理字节建模开关，
//   与 `aot/lift_struct.cpp` 的**同一个**宏（`CMakeLists.txt` 的缺省 OFF；OFF ⇒ 8 份 `.ll`
//   与不带本机制时逐字节相同）。宏没定义也当 OFF。
#ifndef LIFT_S7_IAT
#define LIFT_S7_IAT 0
#endif

namespace {

using lift::aot::GenBlock;
using lift::aot::GenComponent;
using lift::aot::GenEdge;
using lift::aot::GenEmitStats;
using lift::aot::GenIrTarget;
using lift::aot::GenUnsupported;

// 8 位大写 hex（定义在下面"⑤ --pin / --table"块里；识别 / 审计的 EA 打印要先用到它）
std::string hexEa(std::uint32_t ea);

// =====================================================================================
// ★ 尺寸护栏的**唯一落点**（D17 / D18）：第三类软根（非可执行节里静态初始化的代码指针）的
//   **缺省开关**。父级判据（硬性）：`.ll` > 250 MB **或** `clang -c -O2` > 300 s ⇒ 缺省**关**
//   （`--close-data` 才开）；否则缺省开（`--no-close-data` 关）。
//   ⛔ 改这一行必须同时在报告里贴 `.ll` 字节数 / clang 墙钟 / obj 字节数（读数落 `.tmp/aot/g3/`）。
//   G3 实测（2026-11-14/15，win32-x64 / RelWithDebInfo / clang 22.1.2）：`.ll` = 181,563,949 B（< 250 MB）、
//   `clang -c -O2` = 139.5 s（< 300 s）、obj = 13,276,296 B ⇒ **默认开**。读数见 `.tmp/aot/g3/readings.md` §3。
// =====================================================================================
constexpr bool kCloseDataByDefault = true;

// =====================================================================================
// ★ 尺寸护栏的**唯一落点**（D26）：第五类软根（可执行节跳表 dword → 尚未解码的代码）的
//   **缺省开关**。父级判据（硬性）：`.ll` > 320 MB **或** `clang -c -O2` > 400 s ⇒ 缺省**关**
//   （`--close-jmptab` 才开）；否则缺省开（`--no-close-jmptab` 关）。
//   ⛔ 不许为让读数好看而裁剪候选（少收一个就少一个可派发入口）；这一行只决定"默认开还是默认关"。
//   C2 实测（2026-11-15，win32-x64 / RelWithDebInfo / clang 22.1.2）见 `.tmp/aot/c2/readings.md`：
//   默认档 off/on 的 `.ll` 字节、clang 墙钟、obj 字节都记在那里。
// =====================================================================================
constexpr bool kCloseJmptabByDefault = true;

// =====================================================================================
// ★ `--quiet`（CMake 的 AOT 生成命令带它；**默认关** = 输出与今天逐字相同）
//   口径：只静音**非错误**的进度 / 对账打印 —— `[jmptab]` 每表行、`[jmptab/summary|exclude|items|guard]`、
//   `lift-gen: …` 的各条读数、`[S7 downgrade]` 每单元一行、提升 / 冲突的逐条进度、soft-root 样例…
//   **不静音**（⛔ 一个字节都不许少）：
//     · stderr 的错误 / 守卫失败：`emulator-lift-gen: …`、`[jmptab/P-*]`、`[jmptab/reject]`、
//       `[jmptab/bad-item]`、`unsupported … (llvm.trap)`（= exit 3）；
//     · `--closure-gate` 的汇总行（构建期硬闸的判据面，只有一行）；
//     · `--help` / 参数错误 / `--check`·`--verify-split`·`--verify-ir-target` 的单行结论。
//   ★ 只改**打印**：退出码 / 判据 / 产物一个字节都不动（`--jmptab-audit` 的 CTest 与 golden
//     生成命令都不传它 ⇒ 它们看到的输出完全不变）。
// =====================================================================================
bool g_quiet = false;
bool quiet() { return g_quiet; }

struct Options {
    std::string exe;
    std::string roots;
    std::string out;
    std::string check;  // `--check <closure.txt>`：只校验入库件，不发射 IR
    // ★ T2b（D1）：`--pin <lift_pin.h>` / `--table <lift_table.inc>` —— AOT 侧要的两份**机械**数据：
    //   ① 所读 PE 的 sha256 + entry（D1：exe 不是 AOT 的自由参数，启动先核对再 bring-up）；
    //   ② `EA → lift_cc_<entry 所在分量的根>` 派发表（AOT 的 trampoline 唯一查找面）。
    //   ⛔ 两份都可以省略（`lift-gen` 的其它调用点 —— 测试夹具的 golden / `--check` —— 不需要它们）。
    //   ★ 为什么是 `.inc` 片段而不是 `.cpp`：见写盘那一段的注释（宏展开两次 = 声明 + 表）。
    std::string pin;
    std::string table;
    // ★ 构建期库内建表（入库件 `aot/intrinsics.txt`）：EA → 内建名。**缺省不给**（golden / x87 /
    //   ops / s7 这些小根集模块不需要它；给了就会要求表里的 EA 必须是**已发射单元的根**）。
    //   CMake 的 AOT 生成命令显式传 `--intrinsics`；另有 `--no-intrinsics` 只关运行期闸门（A/B）。
    std::string intrinsics;
    // ★ D14：静态调用闭包（`call rel32` 目标 ⇒ 新的 unit 根）。默认开；`--no-close-calls` 关。
    bool close_calls = true;
    // ★ D15：第二类软根 —— 写内存的 `mov` 的 imm32（代码指针立即数）。默认开；`--no-close-stores` 关。
    //   两个开关**各自独立**：`--no-close-calls` 只关 D14 那一类（它的对照档语义不变），要回到
    //   "只按 --roots 展开"的旧口径必须两个都关。
    bool close_stores = true;
    // ★ D17 / D18：第三类软根 —— 非可执行节里静态初始化的代码指针（`.data` 指针表）。
    //   缺省值由**尺寸护栏**决定（见 main 前面的 `kCloseDataByDefault`）：`.ll` > 250 MB 或
    //   `clang -c -O2` > 300 s ⇒ false（`--close-data` 才开），否则 true。
    bool close_data = kCloseDataByDefault;
    // ★ D26：第五类软根 —— 可执行节里"值指向尚未解码的代码"的 4 字节对齐 dword。
    //   缺省值同样由**尺寸护栏**决定（见 `kCloseJmptabByDefault`）：`.ll` > 320 MB 或
    //   `clang -c -O2` > 400 s ⇒ false（`--close-jmptab` 才开），否则 true。
    bool close_jmptab = kCloseJmptabByDefault;
    // =================================================================================
    // ★ BUG-24（S1/S2/S3）：跳表识别与两条消费。**四个开关各自独立**（与 D14/D15/D17/D26
    //   同族，⛔ 不合成总开关）。
    //   · `jmptab_exclude`（P1 / S2）：把识别出的表区间从**解码**里排除（表首不再是 leader、
    //     直线扫描撞到区间就停、`call` 的返回 EA 落在区间里就不进入口集）。`--no-jmptab-exclude` 关。
    //   · `close_jmptab_items`（P2 / S3）：表项在**建块之前**就成 leader/入口（`SoftKind::JumpTableItem`）。
    //     `--no-close-jmptab-items` 关。
    //   · `jmptab_audit`（P0）：只读审计模式（只吃 `--exe` + `--roots`；跑 ①–③ + 分区 + D21 到
    //     `dispatch_entries` 定型后**提前返回**，不发射 IR、不写 `.ll`/派发表）。退出码 = P-1..P-4。
    //   · `jmptab_max_passes`：两趟重启的轮数上限（缺省 4；撞到上限 = 识别器不收敛 ⇒ exit 1）。
    //   ★ 前两个都是**默认开**：P1/P2 是修 BUG-24 的正路，关掉只是为了做"改前/改后"的对照档。
    //   ★ 尺寸护栏沿用 `kCloseJmptabByDefault` 那一式（唯一落点常量 + 注释）：本轮的实测读数见
    //     `.tmp/aot/bug24/impl-report.md`（`.ll` 字节 / `clang -c -O2` 墙钟 / obj 字节）。
    // =================================================================================
    bool jmptab_exclude = true;
    bool close_jmptab_items = true;
    bool jmptab_audit = false;
    unsigned long jmptab_max_passes = 4;
    // ★ D18：只读覆盖率读数（块清单；⛔ 不参与 `.ll` / 派发表）。
    std::string dump_blocks;
    unsigned long max_components = 0;
    bool verbose = false;
    // ★ `--quiet`（纯开关）：只静音非错误的进度 / 对账打印（口径见文件上方 `g_quiet` 的头注）。
    //   默认 **false** = 输出逐字不变；CMake 的 AOT 生成命令带它。
    bool quiet = false;
    // ★ S7（D53）：一对一结构翻译（`ret`→宿主 ret / `call`→宿主 call / 间接→`lift_dyn`）。
    //   默认**开**；`--no-s7` 关（= 回到 D55 之前的形状：每个 `call`/`ret` 都回 trampoline）。
    //   ★ 为什么需要这个开关：**协议形状**的差分夹具（`lift-ir-gen-shim` 的 `golden_gen.ll`）
    //     断言的是"派发序列 / call 后 esp 与 [esp]"—— 那是 S7 **故意改变**的形状。让那些夹具
    //     继续跑旧形状（它们自己的判据一字不改），S7 的形状由 `lift-s7-structured` + AOT 臂覆盖。
    bool s7 = true;
    // =================================================================================
    // ★ AOT 拆分（`.tmp/aot/irsplit/design.md`）：`--split N` + `--out-dir DIR`。
    //   · 缺省 **1** = 今天的单份路径，`--out` 一字不变（逐字节同产物）。
    //   · N > 1 时把**同一个全局分析**的结果按分量根 EA 升序、贪心分桶写成 N 份 `.ll`
    //     （`<prefix>.part00.ll` …）+ 一份清单 `<prefix>.split.txt`。
    //   ★★ 红线：`entry_owner` / `block_flags` / `structure.structured_roots` 一律**全局算一次**
    //     （`emitModuleSplit` 只做输出分区）。⛔ 不许每份重算 —— 那会让跨份静态 `call` 静默
    //     退化成 `ret i32`、并改变 flags 物化（语义还"等价"，但**没人会喊**）。
    std::size_t split = 1;
    // `--out-dir`：N > 1 时的落点目录；分片名 = `lift_aot.part<NN>.ll`（NN = 00..N-1）。
    std::string out_dir;
    // `--split-prefix`：分片文件名的前缀（缺省 `lift_aot`）⇒ `<dir>/<prefix>.part00.ll`。
    std::string split_prefix = "lift_aot";
    // ★ `--verify-split <aot-dir>`：**独立模式**——只读 `out/aot/` 的产物，验分片清单自洽
    //   （CTest `lift-aot-split-manifest` 的被测入口；六项断言见下面 verifyAotSplit 的头注）。
    //   ⛔ 不需要 `--exe` / `--roots` / `--out`；没有生成物时 skip（exit 0）。
    std::string verify_split;
    // ★ 本单：AOT `.ll` 的**函数属性**（`"target-cpu"` / `"target-features"`）—— 与运行库 TU 对齐。
    //   · 取值**唯一来源** = configure 期的 clang 探针（`CMakeLists.txt` 的 `ir_target_probe.c`）⇒
    //     CMake 用 `--ir-target-cpu/--ir-target-features` 传进来。⛔ 不写死 `apple-m1`（win 上根本不是它）。
    //   · **两者任一为空 ⇒ 产物逐字节等于今天**（不发 `attributes #0`、define 行不加 ` #0`）：
    //     既有夹具 / `--check` / 直接调 API 的路径都不传这两个参数。
    //   · 为什么是属性而不是 `-mcpu`：见 `lift_gen_emit.h` 的 `GenIrTarget` 头注（`-mcpu` 实测无效）。
    GenIrTarget ir_target;
    // ★ `--verify-ir-target <aot-dir>`：**独立模式**——只读 `out/aot/` 的产物，验"每个
    //   `define i32 @lift_cc_*` 都挂着与运行库一致的 target 属性组"（CTest `lift-aot-ir-target-attrs`
    //   的被测入口）。⛔ 不需要 `--exe/--roots/--out`；没有生成物时 skip（exit 0），与 `--verify-split` 同口径。
    std::string verify_ir_target;
    // ★ `--note-write`（CMake `LIFT_AOT_NOTE_WRITE`，**缺省 false**）：发射 `lift_note_write`（SMC 记账）。
    //   AOT 臂的 `cpu.cache == nullptr` ⇒ 它是 no-op；缺省关 = Release / RelWithDebInfo 里完全不发
    //   （call / `declare` / `used_note_write` 都不出现，也不为它做 `syncBeforeHostCall()`）。
    //   只在 debug / 诊断（`LIFT_WATCH` 观测 SMC）构建里开。完整理由见 `GenIrTarget::note_write` 的头注。
    bool note_write = false;
    // ★ 入库件硬闸（`--closure-gate`）：**生成这一趟同时**做 `--check` 的五条校验并打同一行汇总。
    //   CMake 的 AOT 生成命令带它 ⇒ 坏闭包在**构建期**硬红，且只分析一次（省 48.6 s）。
    //   ⛔ 缺省关：测试夹具的 roots 文件不是 `0x`+8 位严格文法（`tests/ir/*_roots.txt`），
    //   无条件开会把它们误红 —— 那是**另一件事**（fixture 输入，不是入库件）。
    bool closure_gate = false;
};

void usage() {
    std::printf(
        "emulator-lift-gen - Shape A / T1a: entry EA set + PE bytes -> LLVM IR (`.ll`)\n"
        "  --exe <PE>            byte truth source (mapped into guest, then handed to the decoder)\n"
        "  --roots <file>        one hex EA per line (`#` starts a comment)\n"
        "  --out <file.ll>       output (recommended: out/aot/lift_aot.ll; NOT tracked in git)\n"
        "  --check <file>        validate closure.txt only (format/ascending/decodable/in .text/entry present) => one summary line; does not emit\n"
        "  --closure-gate        run the five --check hard gates on the generation pass (same summary line; the CMake AOT generation command passes it)\n"
        "  --split N             split the output into N `.ll` files (default 1 = the single-file path; bucketing = greedy over ascending root EAs)\n"
        "  --out-dir <dir>       output directory when --split N>1 (parts `lift_aot.partNN.ll` + manifest `lift_aot.split.txt`)\n"
        "  --split-prefix <s>    part filename prefix (default lift_aot => `<dir>/lift_aot.partNN.ll`)\n"
        "  --verify-split <dir>  standalone mode: read <dir> artifacts and verify the split manifest (CTest\n"
        "                        `lift-aot-split-manifest` entry point; no artifacts => skip, exit 0)\n"
        "  --ir-target-cpu <s>   write the `\"target-cpu\"` function attribute into `.ll` (configure-time clang probe value;\n"
        "                        default empty => no attribute, byte-identical output to omitting this flag)\n"
        "  --ir-target-features <s>  same, `\"target-features\"` (verbatim string, not escaped)\n"
        "  --verify-ir-target <dir>  standalone mode: verify every `define i32 @lift_cc_*` carries the same\n"
        "                        target attribute group as the runtime TU (CTest `lift-aot-ir-target-attrs` entry point;\n"
        "                        requires --split N together with --ir-target-cpu/--ir-target-features;\n"
        "                        no artifacts => skip, exit 0)\n"
        "  --pin <file.h>        also write the exe sha256 + entry (AOT startup check, D1; NOT tracked in git)\n"
        "  --table <file.inc>    also write the dispatch table `EA -> lift_cc_<root>` (the AOT EA->LiftFn lookup; NOT tracked in git)\n"
        "  --intrinsics <file>   build-time library-intrinsic table (checked-in `aot/intrinsics.txt`):\n"
        "                        each EA must be an emitted unit root; the entry gets a gated host fast path\n"
        "                        (gate declines => the original lifted body, byte-for-byte)\n"
        "  --note-write          emit `lift_note_write` (SMC accounting) before every guest store (CMake\n"
        "                        LIFT_AOT_NOTE_WRITE; default OFF = nothing emitted in release builds; ON = debug/diagnostic)\n"
        "  --no-close-calls      disable the static call closure (D14 defaults on; control mode = expand --roots only)\n"
        "  --no-close-stores     disable soft roots of kind 2 (D15: imm32 code pointers stored into memory; defaults on)\n"
        "  --close-data          enable soft roots of kind 3 (D17: statically initialised code pointers in non-executable sections = `.data` pointer tables)\n"
        "  --no-close-data       disable soft roots of kind 3 (default %s: see kCloseDataByDefault and .tmp/aot/g3/readings.md)\n"
        "  --close-jmptab        enable soft roots of kind 5 (D26: executable-section jump-table dword -> **not yet decoded** code; may add blocks)\n"
        "  --no-close-jmptab     disable soft roots of kind 5 (default %s: see kCloseJmptabByDefault and .tmp/aot/c2/readings.md)\n"
        "  --no-jmptab-exclude   disable P1 (S2): do not exclude recognised jump-table ranges from decoding (control mode; defaults on)\n"
        "  --no-close-jmptab-items  disable P2 (S3): table items are not made leaders before block building (control mode; defaults on)\n"
        "  --jmptab-audit        read-only audit (P0): takes only --exe + --roots, runs until dispatch is final and returns early;\n"
        "                        does not emit IR / write .ll / dispatch table; exit code = 0 if P-1..P-4 all pass, else 1\n"
        "  --jmptab-max-passes N max restarts of the two-pass loop (default 4; no convergence => exit 1, never silently truncate)\n"
        "  --dump-blocks <file>  read-only coverage readout: `start EA / byte length / instruction count` per final block (ascending text; does not change .ll / dispatch table)\n"
        "  --no-s7               disable S7 one-to-one structural translation (default on): back to the shape before D55\n"
        "                        (every call/ret goes back through the trampoline; protocol-shape fixtures use it)\n"
        "  --max-components N    component count limit (exceeding => exit code 2; default unlimited)\n"
        "  --verbose             per-component readout\n"
        "  --quiet               suppress non-error progress/accounting output (errors, guard failures and the\n"
        "                        one-line --closure-gate summary still print; default off = verbatim today's output)\n"
        "  --help\n"
        "exit codes: 0 success - 2 argument/input error - 3 llvm.trap remains (= generator internal inconsistency: control fell into the default path)\n"
        "        - 4 root (EA from --roots) failed to decode / out of range (a derived leader that fails to decode is only dropped + counted, no exit-code effect)\n"
        "        - 1 internal inconsistency\n",
        kCloseDataByDefault ? "on" : "off", kCloseJmptabByDefault ? "on" : "off");
}

// =====================================================================================
// ① leader / 入口 EA 的发现（只做**可达性**，不做分区）
// =====================================================================================
// ★ D10 P3：派生 leader 可以丢，根不能丢。
//   · **根**（`--roots` / `closure.txt` 里的 EA）解不出 / 不在可执行节 ⇒ **硬红**（exit 4）；
//   · **派生** leader（call 的下一条 EA / jmp·jcc 目标 / jcc 落点）解不出 ⇒ 丢弃 + 计数 + 样例
//     （典型：不返回的 call 的返回 EA 落在 `align 10h` 的填充上）；
//   · **派生** leader 落在**已发射块的字节区间内** ⇒ 丢弃 + 计数 + 样例
//     （典型：`db 15h…` 数据块里两条互相重叠的解码链，各解出 ~200 条 `adc`）。
//   ⇒ 同一站点不再被发射两次（T1b2 的两处账差就是这么收口的）。
struct DropStats {
    std::set<std::uint32_t> dropped;        // 全部被丢弃的派生 leader（升序）
    std::set<std::uint32_t> undecodable;    // 原因 ①：解不出
    std::set<std::uint32_t> inside_block;   // 原因 ②：落在已发射块的字节区间内

    // 返回 false = 这个 EA 之前已经被丢过（说明同一处反复失败 ⇒ 生成器内部不一致）
    bool add_undecodable(std::uint32_t ea) {
        if (!dropped.insert(ea).second) return false;
        undecodable.insert(ea);
        return true;
    }
    bool add_inside_block(std::uint32_t ea) {
        if (!dropped.insert(ea).second) return false;
        inside_block.insert(ea);
        return true;
    }
};

// ★ D14 / D15 / D17：**软根**（closure.txt 之外的 unit 根）的账。与 `DropStats` **分开**记：
//   `DropStats` 是派生 leader（call 返回 / jmp·jcc 目标 / jcc 落点），这里是"closure.txt 之外
//   新收的 unit 根"。两者的丢弃原因**必须分开报**（要求：越界 / 解不出 / 重叠 各一个数）。
// ★ 软根有**四类**（D14 = call 目标 / D15 = 写内存 mov 的 imm32 / D17 = `.data` 静态代码指针 /
//   D26 = 可执行节里"指向尚未解码的代码"的 dword）⇒ 本结构**各一份实例**：四类的 added 与三档丢弃
//   **不合并**（一个数说不清"是哪一类出的问题"）。
struct SoftStats {
    std::set<std::uint32_t> added;         // 被接受为软根（= 新的 unit 根）
    std::set<std::uint32_t> outside;       // 丢弃：目标不在任何可执行节
    std::set<std::uint32_t> undecodable;   // 丢弃：软根解不出
    std::set<std::uint32_t> overlap;       // 丢弃：软根落在已发射块的字节区间内（D10 P3 ②）
    std::set<std::uint32_t> shim_targets;  // 跳过：目标是 shim 桩页里的 EA（不是 guest 单元）
    // ★ BUG-24 / P1（S2）：跳过：目标落在**被排除的跳表区间**内（那是数据，不是代码）。
    //   ★ 这一档**不写** `rejected_targets_`：表区间是"当前这趟的识别结果"，下一趟可能变；
    //     写成"判过就永不重判"会把 BUG-24 的放大器（`rejected_targets_` 单调永久）复制一份。
    std::set<std::uint32_t> table_range;
};

// 软根的**来源**（= 哪一条判据收的它）。丢弃时必须能回答"这是哪一类的账"。
// ★ 新增类**只许追加在末尾**（⛔ 不重排既有值：`SoftKind` 的值进不了产物，但重排会让
//   "哪一类"的判读随版本漂）。
// ★ BUG-24 / P2（S3）：`JumpTableItem = 5` 追加在**末尾**。它与 D26（`JumpTab = 4`）的区别是
//   **顺序**：表项在 `Explorer::run()` 之前就进 `entries_/leaders_/pending`（建块之前），
//   于是 `buildBlocks` 在表项处**断块**而不是"事后发现它落在别人块里再丢"。
enum class SoftKind : std::uint8_t {
    None = 0,
    Call = 1,
    StoreImm = 2,
    DataPtr = 3,
    JumpTab = 4,
    JumpTableItem = 5,
};

// 一个 EA **怎么变成 leader 的**（审计模式的 `boundary=` 字段：回答"表首是怎么进去的"）。
// ★ 优先级 = 枚举顺序（数值小的赢）：一个 EA 多半同时来自几处（例如表项 + call 返回），
//   报"最自然的那一个"——根 > call 返回 > call 目标 > jcc > jmp > data-ptr > store-imm > 表项。
enum class LeaderBoundary : std::uint8_t {
    None = 0,
    Root = 1,
    CallRet = 2,
    Call = 3,
    Jcc = 4,
    Jmp = 5,
    DataPtr = 6,
    StoreImm = 7,
    JumpTabItem = 8,
};

// 五类软根的账**各一份**（⛔ 不合并：读数要能回答"是哪一类出的问题"）。
struct SoftAccounts {
    SoftStats* call = nullptr;
    SoftStats* store = nullptr;
    SoftStats* data = nullptr;
    SoftStats* jumptab = nullptr;
    SoftStats* item = nullptr;  // ★ BUG-24 / P2：表项（`SoftKind::JumpTableItem`）的账

    SoftStats* of(SoftKind kind) const {
        switch (kind) {
            case SoftKind::Call: return call;
            case SoftKind::StoreImm: return store;
            case SoftKind::DataPtr: return data;
            case SoftKind::JumpTab: return jumptab;
            case SoftKind::JumpTableItem: return item;
            default: return nullptr;
        }
    }
};

// 已发射块的**字节区间索引**：`spans` = [start, end) 升序，`prefix_max[i]` = 前 i+1 个 span 的
// 最大 end。判据"EA 落在**某个**已发射块的字节区间内"**只此一份** —— 两处共用：
//   ① `dropLeadersInsideBlocks`（D10 P3 ②，**事后**：候选是扫描过程中才发现的）；
//   ② `Explorer::seedDataPointerRoots`（D17，**事前**：第三类的候选集是静态的，扫一遍 PE 就有）。
// ★ 允许块任意重叠/嵌套（T1a §4.3：块互不重叠并不成立）⇒ 用"前缀最大 end"而不是"上一个 span"。
struct BlockSpans {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> spans;
    std::vector<std::uint32_t> prefix_max;

    bool inside(std::uint32_t ea) const {
        const auto it = std::lower_bound(spans.begin(), spans.end(), std::make_pair(ea, 0u));
        if (it == spans.begin()) return false;
        const std::size_t idx = static_cast<std::size_t>(it - spans.begin()) - 1;
        return spans[idx].first < ea && prefix_max[idx] > ea;
    }

    // ★ BUG-24 / P1（S2）：**同一套判据**的第二个边界口径。数据区间是**闭开** `[start, end)`
    //   而`inside` 是"严格在区间内部"（块首 EA 自己不算在块里）——表首本身恰好是第一个要排除的
    //   字节，所以这里必须把起点算进来。⛔ 不要写第二份区间判定：`spans` / `prefix_max` /
    //   `finalize()` 只有一份，两个谓词只差一个 `<=`。
    bool covers(std::uint32_t ea) const {
        // 第一个 start > ea 的位置；前一个就是"start <= ea"的最后一个 span（按 start 升序）。
        const auto it = std::upper_bound(spans.begin(), spans.end(), ea,
                                         [](std::uint32_t v, const std::pair<std::uint32_t, std::uint32_t>& s) {
                                             return v < s.first;
                                         });
        if (it == spans.begin()) return false;
        const std::size_t idx = static_cast<std::size_t>(it - spans.begin()) - 1;
        return prefix_max[idx] > ea;
    }

    // spans 必须已按 (start, end) 升序排好；prefix_max 是"前 i+1 个 span 的最大 end"。
    void finalize() {
        prefix_max.assign(spans.size(), 0);
        for (std::size_t i = 0; i < spans.size(); ++i) {
            prefix_max[i] = (i == 0) ? spans[i].second
                                     : std::max(prefix_max[i - 1], spans[i].second);
        }
    }
};

inline BlockSpans buildBlockSpans(const std::map<std::uint32_t, GenBlock>& blocks) {
    BlockSpans out;
    out.spans.reserve(blocks.size());
    for (const auto& kv : blocks) {
        if (kv.second.insns.empty()) continue;
        const lift::Insn& last = kv.second.insns.back();
        out.spans.emplace_back(kv.first, last.ea + last.length);
    }
    std::sort(out.spans.begin(), out.spans.end());
    out.finalize();
    return out;
}

// =====================================================================================
// ★ D26（第五类）：**可执行节里的 dword → 尚未解码的代码**
//
// 候选面（判据的**输入面**读数，⛔ 不是判据）：扫**可执行节**里 **4 字节对齐**的 dword，值落在
// **可执行节**内 ⇒ 候选目标。这与 `scanJumpTableTargets`（D21）扫的是**同一批站点**，区别只在
// 过滤器：D21 要求"值**已经是**已建块首"，这里要求"值**还不是**块首"（其余过滤器见
// `seedJumpTableCodeRoots`）。
// ★ 为什么扫**文件字节**：与 D17 / D21 同一式 —— 跳表在 PE 里就是**静态初值**（`.text` 里的
//   `jpt_xxx` 那张 dword 表），真源是文件 raw 字节。`.bss` 尾巴（`raw_size < vsize`）没有初值 ⇒ 跳过。
// ★ 对齐口径：节内偏移 4 字节对齐 = EA 4 字节对齐（`raw_offset` 是 FileAlignment 对齐、节 VA 是
//   SectionAlignment 对齐、ImageBase 也 4 字节对齐）⇒ `i += 4` 这一步就是判据的一半。
// -------------------------------------------------------------------------------------
// ★ **未对齐的指令 imm32**（同一类 D26 的第二条扫描）：
//   上面那条"4 字节对齐 dword"漏掉了**指令流里的 imm32**（地址由 opcode 位置决定，不保证对齐）。
//   判据、opcode 集、三条语料实证（`0x4F80E0` / `0x513B90` / `0x41389A`）与"为什么不是任意 4 字节"
//   的长注释**统一在** `aot/closure_scan.h` 的文件头（守卫 `tests/closure_imm32.cpp` 也读那一份）
//   ⇒ ⛔ 不在这里再抄第二份（两份判据迟早漂）。
//   `lift_gen.cpp` 只做：把 `scanInstructionImmediates()` 的候选**并进** `jumptab_seeds_`，
//   走 `offerSoftRoot` 的同一套收/丢判据；输入面读数（push / mov / 仅这条扫描发现）单独报。
// =====================================================================================
struct JumpTabScan {
    std::uint64_t sites = 0;          // 扫过的可执行节 4 字节对齐 dword 站点数
    std::uint64_t in_exec = 0;        // 其中值落在可执行节内的站点数（= 候选站点数）
    std::set<std::uint32_t> targets;  // 不同候选目标（升序 ⇒ 确定性）
};

JumpTabScan scanExecCodeDwords(const lift::PeImage& image,
                               const std::vector<lift::aot::ExecRange>& exec_ranges) {
    JumpTabScan out;
    for (const lift::PeSectionInfo& sec : image.sections) {
        if (!sec.executable()) continue;  // 判据：**可执行节**
        const std::uint64_t vsize = sec.vsize != 0 ? sec.vsize : sec.raw_size;
        const std::uint64_t n = std::min<std::uint64_t>(sec.raw_size, vsize);
        if (n < 4) continue;
        const std::uint64_t raw_end = static_cast<std::uint64_t>(sec.raw_offset) + n;
        if (raw_end > image.bytes.size()) continue;  // `loadPeFile` 已全量校验；这里只是不许越界读
        for (std::uint64_t i = 0; i + 4 <= n; i += 4) {
            const std::uint8_t* p = image.bytes.data() + sec.raw_offset + i;
            const std::uint32_t value = static_cast<std::uint32_t>(p[0]) |
                                        (static_cast<std::uint32_t>(p[1]) << 8) |
                                        (static_cast<std::uint32_t>(p[2]) << 16) |
                                        (static_cast<std::uint32_t>(p[3]) << 24);
            ++out.sites;
            if (lift::aot::execRangeOf(exec_ranges, value) == nullptr) continue;
            ++out.in_exec;
            out.targets.insert(value);
        }
    }
    return out;
}

// =====================================================================================
// ★ BUG-24 / S1：**跳表识别**（两趟重启的 `recognizeJumpTables`；只读、不发射）
//
// 识别单元 = **上一趟解码流里**的一条间接 `jmp`（`Op::Jmp && op0().kind == Mem &&
// mem.index != None && mem.scale == 4 && mem.base == None`）—— 本语料 56/56 都是绝对 disp32。
// `disp32` 即表地址，且**必须在可执行节内**。⛔ 不用字节模式预扫（`FF 24 SIB disp32`）当站点来源：
// 实测 94 个命中里至少 6 个是假表，且它的错误是**哑的**（设计准备 §4 的裁决）。
//
// 表长（三选一，A 优先 / B 兜底，**必须记账是哪个策略定的长度**）：
//   · **A**：从 jmp 往回（≤ 4 个基本块）找最近一条定义同一索引寄存器的 `cmp <idx>, imm`，
//            且它之后同块内的第一条控制流是无符号 `ja` ⇒ 项数 = `imm + 1`。
//   · **B**：从表地址**逐项向后读**，直到某个 dword 不像代码指针（值不在可执行节 / 解不出 /
//            自指 / 离表 > 64 KiB）。
//   · ~~C：用 `.lst` 的 `jpt_*` 标签~~ —— ⛔ **产品路径不许**（`.lst` 是外部语料，不是可执行真源）。
//
// 交叉验证（缺一不认；**③ 用 ③′**，设计单的 ③ 在本语料上只过 16/42、已证坏）：
//   ① `[ea, ea+4n)` 落在**同一个可执行节**内；
//   ② 每个表项值在可执行节内**且**能解码；
//   ③′ `ja` 的目标在可执行节内 ∧ 能解码 ∧ **不在表自身区间内**（`src=B` 没有 `ja` ⇒ 不适用）；
//   ④ 表项不落在表自身区间内（防自指）。
//   ★ ⑤（父级加的**安全不变量**）：若区间覆盖了任何 `closure.txt` 的根 EA，或任何 D14 收下的
//     `call` 目标软根 ⇒ **响亮拒绝**（单独计数 + 打 EA + ⛔ 不进 `data_ranges_`）。理由：这是
//     "假表把真代码从解码里排除 ⇒ 静默丢覆盖"的唯一硬闸。
//     ★ `call` 的**返回** EA 落在区间内是 BUG-24 本来要的形状（那 2 条）⇒ **不算**违反 ⑤。
// =====================================================================================
struct JmpTab {
    std::uint32_t ea = 0;        // 表地址 = `jmp` 的 disp32
    std::uint32_t n = 0;         // 项数（`A` = imm+1；`B` = 逐项读到的个数）
    bool src_b = false;          // true = 策略 B（逐项读）；false = 策略 A（bounds check 反推）
    std::uint32_t site = 0;      // 认下它的 `jmp` 站点 EA（同表多站点时取最小的）
    std::uint32_t cmp_ea = 0;    // 策略 A 用的 `cmp <idx>, imm`（`src_b` ⇒ 0）
    std::uint32_t ja_ea = 0;     // 策略 A 用的 `ja`（`src_b` ⇒ 0）
    std::uint32_t ja_target = 0; // `ja` 的目标 EA（`src_b` ⇒ 0）
    std::vector<std::uint32_t> items;
    std::uint32_t items_in_exec = 0;  // 判据 ② 的第一半
    std::uint32_t items_decode = 0;   // 判据 ② 的第二半（Zydis 能解出）
    std::uint32_t items_bad = 0;      // 在可执行节内但 `decode()` 失败（⛔ 不许静默：审计单独列）
    std::uint32_t items_selfref = 0;  // 判据 ④
    bool v1 = false, v2 = false, v3 = false, v4 = false;
    bool covers_protected = false;              // 判据 ⑤
    std::vector<std::uint32_t> protected_hits;  // ⑤ 命中的根 / D14 call 目标软根（升序）

    std::uint32_t range_end() const { return ea + 4u * n; }
};

// 被拒的候选（审计要**响亮**列出来，⛔ 不许只计数）。
struct JmpTabReject {
    std::uint32_t ea = 0;
    std::uint32_t site = 0;
    std::uint32_t n = 0;
    bool src_b = false;
    std::string why;
};

struct JmpTabRecognition {
    std::uint64_t sites = 0;                    // 解码流里的间接 jmp 站点数
    std::set<std::uint32_t> candidates;         // 不同 disp32（识别面）
    std::map<std::uint32_t, JmpTab> tables;     // 通过 ①②③′④ 且未触发 ⑤（按 ea 升序）
    std::vector<JmpTabReject> rejects;          // 其余候选（按 ea 升序）
    std::uint64_t rejected_covers_protected = 0;  // ⑤ 单独计数
    std::set<std::uint32_t> items() const {
        std::set<std::uint32_t> out;
        for (const auto& kv : tables) out.insert(kv.second.items.begin(), kv.second.items.end());
        return out;
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges() const {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
        out.reserve(tables.size());
        for (const auto& kv : tables) out.emplace_back(kv.first, kv.second.range_end());
        return out;
    }
};

// 从 PE 文件 raw 字节读一个 dword（**真源**；与 `scanExecCodeDwords` / `scanDataPointers` 同一式）。
// 返回 false = 这个 EA 不在任何"文件真有字节"的节区间内（`.bss` 尾巴 / 节外）。
bool peDword(const lift::PeImage& image, std::uint32_t ea, std::uint32_t* out) {
    for (const lift::PeSectionInfo& sec : image.sections) {
        const std::uint64_t vsize = sec.vsize != 0 ? sec.vsize : sec.raw_size;
        const std::uint64_t n = std::min<std::uint64_t>(sec.raw_size, vsize);
        if (n < 4) continue;
        const std::uint64_t begin = static_cast<std::uint64_t>(image.image_base) + sec.va;
        const std::uint64_t end = begin + n;
        if (static_cast<std::uint64_t>(ea) < begin || static_cast<std::uint64_t>(ea) + 4 > end) continue;
        const std::uint8_t* p = image.bytes.data() + sec.raw_offset + (ea - begin);
        *out = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        return true;
    }
    return false;
}

// 这条指令是否**重定义**了 `r` 所在的 32 位 GPR（目的操作数是 op0 的那些形态；子寄存器按 GPR
// 下标算同一只）。用途只有一个：策略 A 往回扫时，"cmp 之前索引寄存器被改过" ⇒ A 失败
// （本语料 13 个两级 switch 的形态是 `movzx reg, ds:byte_表[reg]`）。⛔ 不是完整的写集分析。
bool writesGpr32(const lift::Insn& in, lift::Reg r) {
    if (r == lift::Reg::None) return false;
    if (!lift::isGpr32(r)) return false;
    switch (in.op) {
        case lift::Op::Mov:
        case lift::Op::Movzx:
        case lift::Op::Movsx:
        case lift::Op::Lea:
        case lift::Op::Add:
        case lift::Op::Sub:
        case lift::Op::And:
        case lift::Op::Or:
        case lift::Op::Xor:
        case lift::Op::Adc:
        case lift::Op::Sbb:
        case lift::Op::Imul:
        case lift::Op::Mul:
        case lift::Op::Div:
        case lift::Op::Idiv:
        case lift::Op::Shl:
        case lift::Op::Shr:
        case lift::Op::Sar:
        case lift::Op::Rol:
        case lift::Op::Ror:
        case lift::Op::Rcl:
        case lift::Op::Rcr:
        case lift::Op::Shld:
        case lift::Op::Shrd:
        case lift::Op::Inc:
        case lift::Op::Dec:
        case lift::Op::Neg:
        case lift::Op::Not:
        case lift::Op::Bswap:
        case lift::Op::Xchg:
        case lift::Op::Pop:
        case lift::Op::Cmovcc:
        case lift::Op::Setcc:
        case lift::Op::Xadd:
        case lift::Op::Cmpxchg:
        case lift::Op::Bts:
        case lift::Op::Btr:
        case lift::Op::Btc:
            break;
        default:
            return false;
    }
    const lift::Operand& d = in.op0();
    if (d.kind != lift::OpKind::Reg) return false;
    if (!lift::isGpr(d.reg)) return false;
    return lift::regSpec(d.reg).index == lift::regSpec(r).index;
}

// 这条指令是不是 `cmp <idx>, imm`（策略 A 的锚点）。
bool isCmpIdxImm(const lift::Insn& in, lift::Reg r, std::uint32_t* imm) {
    if (in.op != lift::Op::Cmp) return false;
    if (in.op0().kind != lift::OpKind::Reg || in.op1().kind != lift::OpKind::Imm) return false;
    if (!lift::isGpr(in.op0().reg) || !lift::isGpr32(r)) return false;
    if (lift::regSpec(in.op0().reg).index != lift::regSpec(r).index) return false;
    *imm = static_cast<std::uint32_t>(in.op1().imm);
    return true;
}

// 指令级 CFG（**只读**；不进 `blocks`）。切块判据与 `.lst` 的 CFG 同一式：
//   块首 = 分支目标 / 终结指令之后 / 断链处；块在**终结指令**（不含 `call`，它线性落穿）后结束。
// 用途只有一个：策略 A 要"从 jmp 所在块往回，最多 4 个块"找 `cmp <idx>, imm` + `ja`。
struct InsnCfg {
    struct Block {
        std::vector<std::uint32_t> eas;
        std::uint32_t start = 0;
        std::uint32_t end = 0;
    };
    std::vector<Block> blocks;
    std::map<std::uint32_t, std::size_t> block_of;
    // 前驱：每项 = (前驱块下标, 是否 fallthrough 边)
    std::vector<std::vector<std::pair<std::size_t, bool>>> preds;
};

InsnCfg buildInsnCfg(const std::map<std::uint32_t, lift::Insn>& insns) {
    InsnCfg out;
    std::set<std::uint32_t> starts;
    // 断链：EA 到 EA+length 之间没有指令 ⇒ 后面那个 EA 是块首（数据洞 / 别人块的中间）。
    if (!insns.empty()) starts.insert(insns.begin()->first);
    std::map<std::uint32_t, std::vector<std::uint32_t>> ends_at;  // 结束 EA → 指令 EA
    for (const auto& kv : insns) {
        const lift::Insn& in = kv.second;
        ends_at[in.ea + in.length].push_back(in.ea);
        if (in.op != lift::Op::Call && lift::opIsControl(in.op)) starts.insert(in.ea + in.length);
        if ((in.op == lift::Op::Jmp || in.op == lift::Op::Jcc) &&
            in.op0().kind == lift::OpKind::Imm) {
            starts.insert(lift::aot::genBranchTarget(in));
        }
    }
    for (const auto& kv : insns) {
        if (starts.count(kv.first) != 0) continue;
        // 有一条"会落穿到它"的前驱指令吗？（`jmp`/`ret`/... 不落穿）
        bool has_fall = false;
        const auto it = ends_at.find(kv.first);
        if (it != ends_at.end()) {
            for (const std::uint32_t prev : it->second) {
                const auto pit = insns.find(prev);
                if (pit == insns.end()) continue;
                const lift::Op op = pit->second.op;
                if (op != lift::Op::Jmp && op != lift::Op::Ret && op != lift::Op::Hlt &&
                    op != lift::Op::Int3 && op != lift::Op::Int) {
                    has_fall = true;
                    break;
                }
            }
        }
        if (!has_fall) starts.insert(kv.first);
    }
    // 建块（块首升序 ⇒ 确定性）
    for (const std::uint32_t start : starts) {
        const auto first = insns.find(start);
        if (first == insns.end()) continue;  // 目标落在没有解码的字节上（表区间 / 数据）
        InsnCfg::Block b;
        b.start = start;
        std::uint32_t cur = start;
        for (int guard = 0; guard < 1000000; ++guard) {
            const auto it = insns.find(cur);
            if (it == insns.end()) break;
            b.eas.push_back(cur);
            const lift::Insn& in = it->second;
            b.end = cur + in.length;
            if (in.op != lift::Op::Call && lift::opIsControl(in.op)) break;
            cur = in.ea + in.length;
            if (starts.count(cur) != 0) break;
        }
        if (b.eas.empty()) continue;
        const std::size_t bi = out.blocks.size();
        for (const std::uint32_t ea : b.eas) out.block_of.emplace(ea, bi);
        out.blocks.push_back(std::move(b));
    }
    // 前驱（只用静态边：jmp/jcc 的 target、jcc/call 的 fallthrough）
    out.preds.resize(out.blocks.size());
    for (std::size_t bi = 0; bi < out.blocks.size(); ++bi) {
        const InsnCfg::Block& b = out.blocks[bi];
        if (b.eas.empty()) continue;
        const lift::Insn& last = insns.at(b.eas.back());
        auto add = [&](std::uint32_t target, bool fall) {
            const auto it = out.block_of.find(target);
            if (it == out.block_of.end() || it->second == bi) return;
            out.preds[it->second].emplace_back(bi, fall);
        };
        if ((last.op == lift::Op::Jmp || last.op == lift::Op::Jcc) &&
            last.op0().kind == lift::OpKind::Imm) {
            add(lift::aot::genBranchTarget(last), false);
        }
        if (last.op == lift::Op::Jcc || last.op == lift::Op::Call) {
            add(last.ea + last.length, true);
        }
    }
    return out;
}

// 策略 A 的逐站点结果（`found == false` 时 `why` 必须能读懂"为什么"）。
struct StrategyA {
    bool found = false;
    std::uint32_t cmp_ea = 0;
    std::uint32_t ja_ea = 0;
    std::uint32_t ja_target = 0;
    std::uint32_t imm = 0;
    std::uint32_t n = 0;
    unsigned hops = 0;
    std::string why;
};

StrategyA strategyA(const InsnCfg& cfg, const std::map<std::uint32_t, lift::Insn>& insns,
                     std::uint32_t site, lift::Reg idx) {
    StrategyA out;
    const auto site_it = cfg.block_of.find(site);
    if (site_it == cfg.block_of.end()) {
        out.why = "site is not in any basic block";
        return out;
    }
    std::set<std::size_t> visited;
    std::size_t cur = site_it->second;
    for (unsigned hop = 0; hop <= 3; ++hop) {
        const InsnCfg::Block& b = cfg.blocks[cur];
        std::size_t hit = b.eas.size();
        for (std::size_t k = b.eas.size(); k-- > 0;) {
            const lift::Insn& in = insns.at(b.eas[k]);
            std::uint32_t imm = 0;
            if (isCmpIdxImm(in, idx, &imm)) {
                hit = k;
                break;
            }
            if (writesGpr32(in, idx)) {
                out.why = std::string("index register is redefined before the cmp (") + lift::opName(in.op) + ")";
                out.hops = hop;
                return out;
            }
        }
        if (hit < b.eas.size()) {
            const lift::Insn& cmp = insns.at(b.eas[hit]);
            std::uint32_t imm = 0;
            isCmpIdxImm(cmp, idx, &imm);
            // cmp 之后同块内的第一条控制流（`call` 也算：与 `.lst` 的 CFG 同一口径）
            const lift::Insn* cf = nullptr;
            for (std::size_t k = hit; k < b.eas.size(); ++k) {
                const lift::Insn& in = insns.at(b.eas[k]);
                if (lift::opIsControl(in.op)) {
                    cf = &in;
                    break;
                }
            }
            out.cmp_ea = cmp.ea;
            out.hops = hop;
            if (cf == nullptr) {
                out.why = "no control flow in the same block after the cmp";
                return out;
            }
            out.ja_ea = cf->ea;
            if (cf->op != lift::Op::Jcc || cf->cc != lift::Cond::A) {
                out.why = std::string("first control flow after the cmp is not ja (it is ") + lift::opName(cf->op) + ")";
                return out;
            }
            if (imm >= 0x100000u) {
                out.why = "cmp immediate is too large to be an item count";
                return out;
            }
            out.ja_target = lift::aot::genBranchTarget(*cf);
            out.imm = imm;
            out.n = imm + 1;
            out.found = true;
            return out;
        }
        // 本块没有 cmp ⇒ 往回退一个块（与 `.lst` 的分析同一套选前驱规则）
        const auto& pr = cfg.preds[cur];
        std::size_t pick = pr.size();
        for (std::size_t k = 0; k < pr.size(); ++k) {
            if (pr[k].second && cfg.blocks[pr[k].first].end == b.start) {
                pick = k;
                break;
            }
        }
        if (pick == pr.size()) {
            std::uint32_t best_end = 0;
            for (std::size_t k = 0; k < pr.size(); ++k) {
                const std::uint32_t e = cfg.blocks[pr[k].first].end;
                if (e <= b.start && (pick == pr.size() || e > best_end)) {
                    best_end = e;
                    pick = k;
                }
            }
        }
        if (pick == pr.size() && !pr.empty()) pick = 0;
        if (pick == pr.size()) {
            out.why = "went back " + std::to_string(hop) + " block(s) and still found no cmp defining the index register";
            out.hops = hop;
            return out;
        }
        const std::size_t next = pr[pick].first;
        if (!visited.insert(next).second) {
            out.why = "predecessor chain is cyclic";
            out.hops = hop;
            return out;
        }
        cur = next;
    }
    out.why = "went back 3 blocks and still found no cmp defining the index register";
    out.hops = 3;
    return out;
}

// 策略 B：从表地址**逐项向后读**，直到某个 dword 不像代码指针。
//   像 = ① 落在这个可执行节内 ② 能解码 ③ 不指向表自身 ④ 离表 ≤ 64 KiB。⛔ 不读 `.lst`。
std::uint32_t strategyB(const lift::PeImage& image,
                        const std::vector<lift::aot::ExecRange>& exec_ranges,
                        const lift::Decoder& decoder, const lift::GuestMemory& mem,
                        std::uint32_t disp) {
    const lift::aot::ExecRange* const sec = lift::aot::execRangeOf(exec_ranges, disp);
    if (sec == nullptr) return 0;  // 表地址不在可执行节 ⇒ 不认
    std::uint32_t n = 0;
    for (std::uint32_t k = 0; k < 4096; ++k) {
        const std::uint64_t ea = static_cast<std::uint64_t>(disp) + 4ull * k;
        if (ea + 4 > sec->end) break;  // 越过节尾
        std::uint32_t v = 0;
        if (!peDword(image, static_cast<std::uint32_t>(ea), &v)) break;
        if (lift::aot::execRangeOf(exec_ranges, v) == nullptr) break;  // ① 值必须在可执行节内
        if (k > 0 && v >= disp && v < disp + 4u * k) break;            // ③ 自指
        if (v > disp ? (v - disp > 0x10000u) : (disp - v > 0x10000u)) break;  // ④ ±64 KiB 窗口
        lift::Insn probe;
        std::string why;
        if (!decoder.decode(mem, v, &probe, &why)) break;  // ② 能解码
        ++n;
    }
    return n;
}

// ★ S1 的唯一落点。`protected_eas` = `closure.txt` 的根 ∪ 本趟 D14 收下的 `call` 目标软根
//   （判据 ⑤ 的输入）。⛔ 不读 `.lst`、⛔ 不看 `.ll`、⛔ 不为任何 EA 开特例。
JmpTabRecognition recognizeJumpTables(const std::map<std::uint32_t, lift::Insn>& insns,
                                      const lift::PeImage& image,
                                      const std::vector<lift::aot::ExecRange>& exec_ranges,
                                      const lift::Decoder& decoder, const lift::GuestMemory& mem,
                                      const std::set<std::uint32_t>& protected_eas) {
    JmpTabRecognition out;
    // ---- 站点：解码流里的间接 jmp（`scale == 4` / `base == None` / 有 index）----
    std::map<std::uint32_t, std::vector<std::uint32_t>> sites_of_table;  // 表地址 → 站点（升序）
    for (const auto& kv : insns) {
        const lift::Insn& in = kv.second;
        if (in.op != lift::Op::Jmp) continue;
        const lift::Operand& o = in.op0();
        if (o.kind != lift::OpKind::Mem) continue;
        if (o.mem.index == lift::Reg::None) continue;
        if (o.mem.scale != 4) continue;
        if (o.mem.base != lift::Reg::None) continue;
        ++out.sites;
        if (lift::aot::execRangeOf(exec_ranges, o.mem.disp) == nullptr) continue;  // 表必须在可执行节内
        out.candidates.insert(o.mem.disp);
        sites_of_table[o.mem.disp].push_back(in.ea);
    }
    // ---- 逐张候选表：A 优先、A 失败才 B ----
    const InsnCfg cfg = buildInsnCfg(insns);
    for (auto& kv : sites_of_table) {
        const std::uint32_t ea = kv.first;
        JmpTab tab;
        tab.ea = ea;
        tab.site = kv.second.front();
        StrategyA a;
        for (const std::uint32_t site : kv.second) {
            const lift::Insn& jmp = insns.at(site);
            const lift::Reg idx = jmp.op0().mem.index;
            const StrategyA probe = strategyA(cfg, insns, site, idx);
            if (probe.found) {
                a = probe;
                tab.site = site;
                break;
            }
        }
        if (a.found) {
            tab.n = a.n;
            tab.cmp_ea = a.cmp_ea;
            tab.ja_ea = a.ja_ea;
            tab.ja_target = a.ja_target;
            tab.src_b = false;
        } else {
            tab.n = strategyB(image, exec_ranges, decoder, mem, ea);
            tab.src_b = true;
        }
        // ---- 交叉验证 ①②③′④ ----
        const lift::aot::ExecRange* const sec = lift::aot::execRangeOf(exec_ranges, ea);
        tab.v1 = (sec != nullptr) &&
                 (static_cast<std::uint64_t>(ea) + 4ull * tab.n <= sec->end);
        tab.items.reserve(tab.n);
        for (std::uint32_t k = 0; k < tab.n; ++k) {
            std::uint32_t v = 0;
            if (!peDword(image, ea + 4u * k, &v)) {
                tab.items.push_back(0);
                continue;
            }
            tab.items.push_back(v);
        }
        for (const std::uint32_t v : tab.items) {
            if (lift::aot::execRangeOf(exec_ranges, v) != nullptr) ++tab.items_in_exec;
            lift::Insn probe;
            std::string why;
            if (decoder.decode(mem, v, &probe, &why)) {
                ++tab.items_decode;
            } else {
                ++tab.items_bad;
            }
            if (v >= ea && v < ea + 4u * tab.n) ++tab.items_selfref;
        }
        tab.v2 = (tab.n != 0 && tab.items_in_exec == tab.n && tab.items_bad == 0);
        if (tab.src_b) {
            tab.v3 = true;  // 策略 B 没有 `ja` ⇒ ③′ 不适用（但 `src=B` 必须记账）
        } else {
            const bool ja_in_exec = lift::aot::execRangeOf(exec_ranges, tab.ja_target) != nullptr;
            lift::Insn probe;
            std::string why;
            const bool ja_decodes = ja_in_exec && decoder.decode(mem, tab.ja_target, &probe, &why);
            const bool ja_in_self = tab.ja_target >= ea && tab.ja_target < ea + 4u * tab.n;
            tab.v3 = ja_in_exec && ja_decodes && !ja_in_self;
        }
        tab.v4 = (tab.items_selfref == 0);
        // ---- ⑤ 安全不变量：区间覆盖了根 / D14 call 目标软根 ⇒ 响亮拒绝 ----
        if (tab.n != 0) {
            for (const std::uint32_t p : protected_eas) {
                if (p >= ea && p < ea + 4u * tab.n) tab.protected_hits.push_back(p);
            }
        }
        tab.covers_protected = !tab.protected_hits.empty();
        const bool accepted = tab.v1 && tab.v2 && tab.v3 && tab.v4 && !tab.covers_protected;
        if (accepted) {
            out.tables.emplace(ea, std::move(tab));
        } else {
            JmpTabReject r;
            r.ea = ea;
            r.site = tab.site;
            r.n = tab.n;
            r.src_b = tab.src_b;
            if (tab.covers_protected) {
                ++out.rejected_covers_protected;
                r.why = "(5) range covers a root / a D14 call-target soft root (";
                for (std::size_t i = 0; i < tab.protected_hits.size() && i < 3; ++i) {
                    if (i != 0) r.why += ",";
                    r.why += hexEa(tab.protected_hits[i]);
                }
                if (tab.protected_hits.size() > 3) r.why += ",...";
                r.why += ")";
            } else if (tab.n == 0) {
                r.why = "strategy A failed and strategy B read 0 items (not a table)";
            } else if (!tab.v1) {
                r.why = "criterion (1): range extends outside the executable section";
            } else if (!tab.v2) {
                r.why = "criterion (2): table item is outside the executable section / does not decode (in_exec=" +
                        std::to_string(tab.items_in_exec) + " decode=" +
                        std::to_string(tab.items_decode) + " bad=" +
                        std::to_string(tab.items_bad) + " n=" + std::to_string(tab.n) + ")";
            } else if (!tab.v3) {
                r.why = "criterion (3-prime): ja target is outside the executable section / does not decode / lies inside the table's own range";
            } else {
                r.why = "criterion (4): table item is self-referential";
            }
            out.rejects.push_back(std::move(r));
        }
    }
    std::sort(out.rejects.begin(), out.rejects.end(),
              [](const JmpTabReject& a, const JmpTabReject& b) { return a.ea < b.ea; });
    return out;
}

// ★ D26 的**候选面**（"先量后收"的那一半）：每个候选**只归一类**，五档相加 = `distinct`。
//   过滤顺序就是判据的书写顺序（先越界由扫描保证、再入口集、再 shim、再块首、再重叠、再可解码）：
//     · `in_entries`    已在入口集（含前三类软根 / 提升出来的根）⇒ 不需要收（D14 等已覆盖）；
//     · `shim`          目标落在 shim 桩页（由 trampoline 派发，⛔ 不是 guest 单元）；
//     · `block_starts`  **已是已建块首** ⇒ 第四类（D21）负责"只加 entry"，这一趟**不收**（⛔ 不重复）；
//     · `overlap_pre`   落在**已发射块的字节区间内** ⇒ 收之前就挡下（D10 P3 ② 的判据前移）。
//                       ★ 前移在这里是**必需**的：放进去展开会产生一堆假 `call` 的返回 EA ⇒ 触发
//                       孤儿提升 ⇒ 提升出来的 EA 又落在别人的块里 ⇒ **提升不收敛**（D17 实测）。
//     · `decodable`     剩下的候选里 `Decoder::decode` 成功的（收之前的**试解一次**：这是读数，
//                       ⛔ 不是判据 —— 判据仍是 `offerSoftRoot` + `explore`：解不出会进 `undecodable` 档）。
//   ⇒ `distinct = in_entries + shim + block_starts + overlap_pre + decodable + undecodable_pre`。
struct JumpTabSurface {
    std::uint64_t sites = 0;
    std::uint64_t in_exec = 0;
    std::uint64_t distinct = 0;
    std::uint64_t in_entries = 0;   // 已在入口集（含前三类软根 / 提升出来的根）或被前三类判过
    std::uint64_t shim = 0;
    std::uint64_t block_starts = 0;
    std::uint64_t overlap_pre = 0;
    std::uint64_t decodable = 0;
    std::uint64_t undecodable_pre = 0;  // 试解失败（收之前的读数；收下之后同样进 `jumptab_undecodable`）
};

class Explorer {
public:
    // `exec_ranges` = 可执行节（`call rel32` 目标 / 立即数代码指针的"在不在 `.text`/`poly`"判据）；
    // `shim_begin/end` = shim 桩页（`GuestLayout::shim_page` + `shim_slots * 4`）——桩 EA 由
    // trampoline 派发，⛔ 不是 guest 单元，所以既不收成软根、也不算丢弃。
    // `accounts` = 五类软根**各自的账**（⛔ 不合并：D14 / D15 / D17 / D26 / 表项 分开报）。
    Explorer(const lift::Decoder& decoder, const lift::GuestMemory& mem,
             const std::vector<lift::aot::ExecRange>& exec_ranges, std::uint32_t shim_begin,
             std::uint32_t shim_end, bool close_calls, bool close_stores, bool close_data,
             bool close_jmptab, bool close_jmptab_items, const SoftAccounts& accounts)
        : decoder_(decoder),
          mem_(mem),
          exec_ranges_(exec_ranges),
          shim_begin_(shim_begin),
          shim_end_(shim_end),
          close_calls_(close_calls),
          close_stores_(close_stores),
          close_data_(close_data),
          close_jmptab_(close_jmptab),
          close_jmptab_items_(close_jmptab_items),
          accounts_(accounts) {}

    // 第一阶段：先注入 P2 的跳表项（建块之前成 leader），再只展开 D14 / D15 两类软根
    // （`call rel32` 目标 / 写内存 mov 的 imm32）到不动点。
    // ★ D17 的第三类候选**故意不在这里**：它的判据要用"已发射块"（见 `seedDataPointerRoots`），
    //   而块是在这一阶段收敛之后才建的 ⇒ 由 `buildUnits` 在两阶段之间调用。
    bool run(const std::set<std::uint32_t>& roots, DropStats* drops, std::string* err,
             std::uint32_t* err_ea) {
        entries_ = roots;
        leaders_ = roots;
        for (const std::uint32_t ea : roots) noteBoundary(ea, LeaderBoundary::Root);
        std::set<std::uint32_t> pending(roots.begin(), roots.end());
        seedJumpTableItems(&pending);  // ★ P2：表项在**建块之前**成 leader（BUG-24 的根治点）
        return explore(&pending, roots, drops, err, err_ea);
    }

    // 工作集循环（可重入：`pending` 由调用方给；`scanned_` 是成员 ⇒ 不会重复扫同一段）。
    // `roots` 只用于"根解不出 ⇒ 硬红"这条判据（软根 / 派生 leader 是丢弃 + 计数）。
    bool explore(std::set<std::uint32_t>* pending_in, const std::set<std::uint32_t>& roots,
                 DropStats* drops, std::string* err, std::uint32_t* err_ea) {
        std::set<std::uint32_t>& pending = *pending_in;
        while (!pending.empty()) {
            const std::uint32_t leader = *pending.begin();
            pending.erase(pending.begin());
            if (!scanned_.insert(leader).second) continue;
            // ★ BUG-24 / P1（S2）①：弹出的 leader 落在**被排除的跳表区间**内 ⇒ 不解码、不产块。
            //   ★ 根例外：⑤ 保证任何被接受表的区间不覆盖根；真发生了也宁可解（覆盖比整齐重要）
            //     并**响亮记账**，⛔ 不静默丢一个根。
            if (inDataRange(leader)) {
                if (roots.count(leader) != 0) {
                    ++data_range_root_hits_;
                } else {
                    ++data_range_leaders_;
                    table_range_leaders_.insert(leader);
                    leaders_.erase(leader);
                    entries_.erase(leader);
                    continue;
                }
            }
            std::uint32_t cur = leader;
            bool reached_end = false;
            for (int guard = 0; guard < 1000000; ++guard) {
                std::string why;
                std::uint32_t bad_ea = 0;
                const lift::Insn* insn = get(cur, &why, &bad_ea);
                if (insn == nullptr) {
                    if (roots.count(leader) != 0) {
                        // **根**解不出 ⇒ 闭包本身错，硬红（⛔ 不许降级成"丢弃 + 警告"）。
                        if (err != nullptr) *err = why;
                        if (err_ea != nullptr) *err_ea = bad_ea;
                        return false;
                    }
                    if (soft_roots_.count(leader) != 0) {
                        // ★ D14 / D15：**软根**解不出 ⇒ 丢弃 + 计数（⛔ 不硬红），按类记账。
                        const SoftKind kind = softKindOf(leader);
                        rejectSoftRoot(leader);
                        drops->dropped.insert(leader);
                        if (SoftStats* st = statsOf(kind)) st->undecodable.insert(leader);
                        reached_end = true;
                        break;
                    }
                    // 派生 leader 解不出 ⇒ 丢弃这一段（保留它已经解出来的部分：语义上无害）。
                    if (!drops->add_undecodable(leader)) {
                        if (err != nullptr) *err = "derived leader " + std::to_string(leader) + " repeatedly failed to decode";
                        if (err_ea != nullptr) *err_ea = bad_ea;
                        return false;
                    }
                    leaders_.erase(leader);
                    reached_end = true;
                    break;
                }
                const lift::Insn in = *insn;
                // ★ D15：**写内存的 `mov` 的 imm32** = 第二类软根候选（判据形态只写在
                //   `offerStoreImmTarget` 一处）。放在控制流分派**之前**：`mov` 不是控制流，
                //   但这样"每条解出来的指令都过一遍"是结构性的，不靠"分支里顺手加一句"。
                if (in.op == lift::Op::Mov) offerStoreImmTarget(in, &pending);
                if (lift::opIsControl(in.op)) {
                    switch (in.op) {
                        case lift::Op::Call: {
                            // 入口 = 每条 call 的下一条 EA（父级裁决 1）
                            // ★ BUG-24 / P1（S2）③：返回 EA 落在**被排除的跳表区间**内 ⇒ 不入
                            //   entries/leaders/pending（那是 noreturn 调用紧邻表首的形状，本语料
                            //   实测 2 条：`0x418DFB→0x418E00` / `0x41B96F→0x41B974`）。
                            //   判据是"形态 + 区间"两条同时成立 ⇒ 不破坏"每条**会返回**的 call 的
                            //   返回 EA 是入口"这条不变量（20,491 条 call 里没有一条正常返回落进区间）。
                            const std::uint32_t ret = in.ea + in.length;
                            if (inDataRange(ret)) {
                                ++call_ret_in_data_range_;
                            } else {
                                entries_.insert(ret);
                                leaders_.insert(ret);
                                pending.insert(ret);
                                noteBoundary(ret, LeaderBoundary::CallRet);
                            }
                            // ★ D14：`call rel32` 的目标若不在入口集 / 不是 shim 桩 / 在可执行节
                            //   内 ⇒ 收成软根（新的 unit 根）。`call reg/mem` 的目标运行期才知道。
                            if (in.op0().kind == lift::OpKind::Imm)
                                offerSoftRoot(lift::aot::genBranchTarget(in), SoftKind::Call, &pending);
                            break;
                        }
                        case lift::Op::Jmp:
                            if (in.op0().kind == lift::OpKind::Imm) {
                                const std::uint32_t target = lift::aot::genBranchTarget(in);
                                leaders_.insert(target);
                                pending.insert(target);
                                noteBoundary(target, LeaderBoundary::Jmp);
                            }
                            break;  // `jmp reg/mem` 的目标运行期才知道，静态无后继
                        case lift::Op::Jcc: {
                            const std::uint32_t target = lift::aot::genBranchTarget(in);
                            const std::uint32_t fall = in.ea + in.length;
                            leaders_.insert(target);
                            pending.insert(target);
                            noteBoundary(target, LeaderBoundary::Jcc);
                            leaders_.insert(fall);
                            pending.insert(fall);
                            noteBoundary(fall, LeaderBoundary::Jcc);
                            break;
                        }
                        default:
                            // ret / hlt / int3 / int：没有后继
                            break;
                    }
                    reached_end = true;
                    break;
                }
                cur = in.ea + in.length;
                // ★ BUG-24 / P1（S2）②：直线扫描撞到**被排除的跳表区间**的边界 ⇒ 停。
                //   （表后第一字节不是代码：继续线性爬就会把表/数据当指令。）
                if (inDataRange(cur)) {
                    reached_end = true;
                    break;
                }
                // 这条直线段的后缀已经被别的 leader 扫过 ⇒ 它的控制流后继也登记过了
                if (insns_.find(cur) != insns_.end()) {
                    reached_end = true;
                    break;
                }
            }
            if (!reached_end) {
                if (err != nullptr) *err = "linear scan exceeded the 1e6 instruction limit (probably fell into data)";
                if (err_ea != nullptr) *err_ea = leader;
                return false;
            }
        }
        return true;
    }

    const std::map<std::uint32_t, lift::Insn>& insns() const { return insns_; }
    const std::set<std::uint32_t>& leaders() const { return leaders_; }
    const std::set<std::uint32_t>& entries() const { return entries_; }
    const std::set<std::uint32_t>& softRoots() const { return soft_roots_; }
    bool isSoftRoot(std::uint32_t ea) const { return soft_roots_.count(ea) != 0; }
    // 这个软根是哪一条判据收的（丢弃时要据此记到**对应那一类**的账上）。
    SoftKind softKindOf(std::uint32_t ea) const {
        if (soft_roots_call_.count(ea) != 0) return SoftKind::Call;
        if (soft_roots_store_.count(ea) != 0) return SoftKind::StoreImm;
        if (soft_roots_data_.count(ea) != 0) return SoftKind::DataPtr;
        if (soft_roots_jumptab_.count(ea) != 0) return SoftKind::JumpTab;
        if (soft_roots_item_.count(ea) != 0) return SoftKind::JumpTableItem;
        return SoftKind::None;
    }
    // ★ BUG-24 / P0：这个 EA **是怎么变成 leader 的**（审计模式的 `boundary=` 字段）。
    LeaderBoundary boundaryOf(std::uint32_t ea) const {
        const auto it = boundary_.find(ea);
        return it == boundary_.end() ? LeaderBoundary::None : it->second;
    }
    const std::map<std::uint32_t, LeaderBoundary>& boundaries() const { return boundary_; }
    // ★ P1（S2）的读数：被"跳表区间"挡下的 leader 数 / 其中根（应为 0，⑤ 的兜底）/ `call` 返回 EA
    //   被挡下的数 / 被挡下的 leader 集合（报告里逐 EA 点名）。
    std::uint64_t dataRangeLeaders() const { return data_range_leaders_; }
    std::uint64_t dataRangeRootHits() const { return data_range_root_hits_; }
    std::uint64_t callRetInDataRange() const { return call_ret_in_data_range_; }
    const std::set<std::uint32_t>& tableRangeLeaders() const { return table_range_leaders_; }
    // ★ P2（S3）的读数：表项种子被接受 / 已是入口 / 越界 / shim / 落在别的表区间。
    std::uint64_t jumpTableItemSeeds() const { return item_seeds_; }
    std::uint64_t jumpTableItemAlreadyEntry() const { return item_already_; }
    std::uint64_t jumpTableItemOutside() const { return item_outside_; }
    std::uint64_t jumpTableItemShim() const { return item_shim_; }
    std::uint64_t jumpTableItemInRange() const { return item_in_range_; }
    // D15 的读数（判据的输入面）：解出来的 `mov [mem], imm32` 站点数 / 其中 imm 落在可执行节内的。
    // ⛔ 不是判据：只是让"越界=多少"这个数能读（站点 − 可执行节内 = 数据常量那一类）。
    std::uint64_t storeImmSites() const { return store_imm_sites_; }
    std::uint64_t storeImmInExec() const { return store_imm_in_exec_; }

    // ★ D17：第三类软根的**候选集**（由 `scanDataPointers` 从 PE 非可执行节扫出来，升序）。
    //   这里只决定"什么时候塞进工作集"（见 `run` 的收敛后一次性塞）；判据本身不在这里。
    //   `sites` / `in_exec` 是判据的**输入面读数**（⛔ 不是判据）。
    void setDataPointerSeeds(std::set<std::uint32_t> seeds, std::uint64_t sites, std::uint64_t in_exec) {
        data_seeds_ = std::move(seeds);
        data_ptr_sites_ = sites;
        data_ptr_in_exec_ = in_exec;
    }
    std::uint64_t dataPtrSites() const { return data_ptr_sites_; }        // 扫过的 dword 站点数
    std::uint64_t dataPtrInExec() const { return data_ptr_in_exec_; }    // 其中值落在可执行节内的站点数
    std::uint64_t dataPtrDistinct() const { return data_seeds_.size(); } // 不同候选目标数（升序集合）
    std::uint64_t dataPtrSkipped() const { return data_ptr_skipped_; }   // 已在入口集 / 已判过 ⇒ 不收

    // ★ BUG-24 / P1（S2）：把识别出的表区间装进 Explorer。判据本身**只有 `BlockSpans` 一份**
    //   （升序 spans + prefix_max；`covers` 是闭开区间口径），这里只负责装配 + 排序。
    void setDataRanges(std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges) {
        std::sort(ranges.begin(), ranges.end());
        data_ranges_.spans = std::move(ranges);
        data_ranges_.finalize();
    }
    // ★ BUG-24 / P2（S3）：表项候选（上一趟的识别结果）。
    //   ⛔ 它们**不走 `offerSoftRoot`**：那里会写 `rejected_targets_`（"判过就永不重判"），
    //   而那正是 BUG-24 从"一条垃圾块"升级成"永久缺块"的放大器。四档在自己的注入路径里记账。
    void setJumpTableItems(std::set<std::uint32_t> items) { jmptab_items_ = std::move(items); }

    // ★ D17：把第三类候选**收成软根**并塞进工作集。
    //   `emitted` = **当前已建好的块**的字节区间索引（判据输入）。调用点在 `buildUnits`：
    //   D14/D15 收敛 + 建块之后、第三类展开之前。
    //   ① 已在入口集 / 已判过 ⇒ `skipped`（⛔ 不重复记账：同一个 EA 的账算先到的那一类）；
    //   ② **落在已发射块的字节区间内** ⇒ `overlap`（**D10 P3 ② 的判据前移**）。
    //      为什么这一类可以在"收"之前判、而 D14/D15 不行：D17 的候选集是**静态集合**（扫一遍 PE
    //      就全知道），D14/D15 的候选是**扫描过程中才发现的**（不扫就不知道有这条 call）。
    //      ★ 前移是必要的，不只是省事：放进去展开会产生一堆假 `call` 的返回 EA，
    //      它们的段只经由被拒的软根可达 ⇒ 触发"孤儿入口提升"（D17 那条）⇒ 提升出来的 EA
    //      本身又落在别人的块里 ⇒ **提升不收敛**（G3 首次跑就撞上：`00404046`）。判据一个字没变，
    //      只是把它用在扩展工作集**之前**。
    //   ③ 其余 ⇒ `offerSoftRoot(..., DataPtr, pending)`（越界 / 解不出 照旧由那一处判据分类）。
    void seedDataPointerRoots(const BlockSpans& emitted, std::set<std::uint32_t>* pending) {
        for (const std::uint32_t ea : data_seeds_) {
            if (entries_.count(ea) != 0 || rejected_targets_.count(ea) != 0) {
                ++data_ptr_skipped_;
                continue;
            }
            if (emitted.inside(ea)) {
                rejected_targets_.insert(ea);
                if (accounts_.data != nullptr) accounts_.data->overlap.insert(ea);
                ++data_ptr_overlap_pre_;
                continue;
            }
            offerSoftRoot(ea, SoftKind::DataPtr, pending);
        }
    }
    // 读数：候选里因"落在已发射块字节区间内"在**收之前**就挡下的站点数（与 `soft_data_->overlap`
    // 同一个集合 ⇒ 计数必然相等；这个访问器只是让调用方不用去摸 SoftStats）。
    std::uint64_t dataPtrOverlapPre() const { return data_ptr_overlap_pre_; }

    // ★ D26：第五类的候选集（由 `scanExecCodeDwords` 扫出来；`sites` / `in_exec` 是判据的输入面读数）。
    void setJumpTableSeeds(std::set<std::uint32_t> seeds, std::uint64_t sites, std::uint64_t in_exec) {
        jumptab_seeds_ = std::move(seeds);
        jumptab_sites_ = sites;
        jumptab_in_exec_ = in_exec;
    }
    // ★ D26 的第二条扫描（未对齐指令 imm32：push / mov）：候选**并入**同一条 `jumptab_seeds_`（收/丢判据
    //   与对齐 dword 那一类完全相同，⛔ 不另写一套），但输入面的读数**分开记**（否则说不清
    //   "新增的这些是从哪条扫描来的"）。调用顺序：先 `setJumpTableSeeds` 再本函数。
    void setInstructionImmediateSeeds(std::set<std::uint32_t> seeds, std::uint64_t push_sites,
                                      std::uint64_t push_in_exec, std::uint64_t mov_sites,
                                      std::uint64_t mov_in_exec) {
        imm32_seeds_ = std::move(seeds);
        imm32_push_sites_ = push_sites;
        imm32_push_in_exec_ = push_in_exec;
        imm32_mov_sites_ = mov_sites;
        imm32_mov_in_exec_ = mov_in_exec;
        jumptab_seeds_.insert(imm32_seeds_.begin(), imm32_seeds_.end());
    }
    const std::set<std::uint32_t>& jumpTabSeeds() const { return jumptab_seeds_; }
    std::uint64_t jumpTabSites() const { return jumptab_sites_; }
    std::uint64_t jumpTabInExec() const { return jumptab_in_exec_; }
    std::uint64_t imm32PushSites() const { return imm32_push_sites_; }
    std::uint64_t imm32PushInExec() const { return imm32_push_in_exec_; }
    std::uint64_t imm32MovSites() const { return imm32_mov_sites_; }
    std::uint64_t imm32MovInExec() const { return imm32_mov_in_exec_; }
    std::uint64_t imm32Distinct() const { return imm32_seeds_.size(); }
    std::uint64_t jumpTabDistinct() const { return jumptab_seeds_.size(); }
    std::uint64_t jumpTabRounds() const { return jumptab_rounds_; }
    // 候选面（"先量后收"的那一半）：由 `buildUnits` 阶段③在**收之前**填一次（⛔ 不是判据的输入）。
    void setJumpTableSurface(const JumpTabSurface& surface) { jumptab_surface_ = surface; }
    const JumpTabSurface& jumpTabSurface() const { return jumptab_surface_; }
    // 读数：**最终仍在入口集**的第五类软根数（`added` 记的是"被接受为软根"，含事后被丢的
    //   —— 与 D14/D15/D17 同一口径；这个数回答"实际新增了几个 unit 根"）。
    std::uint64_t liveJumpTabRoots() const { return soft_roots_jumptab_.size(); }

    // ★ D26 的**候选面**（"先量后收"）：只看不动（⛔ 不往 entries / leaders / pending 里塞任何东西），
    //   所以可以在"收"之前把每一档都读出来。调用点 = `buildUnits` 阶段③之前（块集已定型）。
    //   ★ 试解（`decoder_.decode`）**不进** `insns_` 缓存：它只是读数，不能改变后面展开的输入。
    JumpTabSurface measureJumpTableSurface(const BlockSpans& emitted,
                                           const std::set<std::uint32_t>& block_starts) const {
        JumpTabSurface out;
        out.sites = jumptab_sites_;
        out.in_exec = jumptab_in_exec_;
        out.distinct = jumptab_seeds_.size();
        for (const std::uint32_t ea : jumptab_seeds_) {
            if (entries_.count(ea) != 0 || rejected_targets_.count(ea) != 0) {
                ++out.in_entries;
                continue;
            }
            if (ea >= shim_begin_ && ea < shim_end_) {
                ++out.shim;
                continue;
            }
            if (block_starts.count(ea) != 0) {
                ++out.block_starts;
                continue;
            }
            if (emitted.inside(ea)) {
                ++out.overlap_pre;
                continue;
            }
            lift::Insn probe;
            std::string why;
            if (decoder_.decode(mem_, ea, &probe, &why)) {
                ++out.decodable;
            } else {
                ++out.undecodable_pre;
            }
        }
        return out;
    }

    // ★ D26：把第五类候选**收成软根**并塞进工作集（返回 true = 这一轮有新的被收下）。
    //   判据顺序与 `measureJumpTableSurface` **完全一致**（同一份过滤器，只是这里的重叠/块首会
    //   **记进账**并把该 EA 标成"已判过"，避免下一轮重复收 / 重复计数）。
    //   `emitted` = **当前已建好的块**的字节区间索引；`block_starts` = 当前已建块的首 EA 集合。
    //   ★ 为什么要迭代（`buildUnits` 阶段③的循环）：这一趟**会新增块**，新块又可能把别的候选
    //     挡住（⇒ 进 `overlap`）或把"能解码"的候选变成"落在已发射区间内"。反过来，某个被拒的
    //     软根也可能让它的块消失而放出候选 —— 但 `rejected_targets_` **单调增长**（判过的不再判）
    //     ⇒ 轮数有限、必然收敛。⛔ 不要去掉 `rejected_targets_` 那一行判断：那会来回收/放。
    bool seedJumpTableCodeRoots(const BlockSpans& emitted, const std::set<std::uint32_t>& block_starts,
                                std::set<std::uint32_t>* pending) {
        if (!close_jmptab_) return false;
        ++jumptab_rounds_;
        bool any = false;
        for (const std::uint32_t ea : jumptab_seeds_) {
            if (entries_.count(ea) != 0 || rejected_targets_.count(ea) != 0) continue;
            if (block_starts.count(ea) != 0) continue;           // 已是块首 ⇒ 第四类（D21）
            if (emitted.inside(ea)) {
                // D10 P3 ② 的判据前移（理由见 `measureJumpTableSurface`）：收之前挡下并记账。
                rejected_targets_.insert(ea);
                if (accounts_.jumptab != nullptr) accounts_.jumptab->overlap.insert(ea);
                continue;
            }
            offerSoftRoot(ea, SoftKind::JumpTab, pending);
            if (entries_.count(ea) != 0) any = true;
        }
        return any;
    }

    // ★ G3：把一个入口 EA **退出**入口集（它没有块 ⇒ 不可能成为 unit 根）。
    //   与 `rejectSoftRoot` 的区别：这里不动软根的账（它可能根本不是软根，而是 `call` 的返回 EA），
    //   调用方自己负责记进 `drops.dropped`（语义 = "没有块，指向它的边发 `ret i32`"）。
    void dropEntry(std::uint32_t ea) {
        entries_.erase(ea);
        leaders_.erase(ea);
        rejected_targets_.insert(ea);
    }

    // 软根被丢弃（解不出 / 与已发射块重叠）⇒ 同时退出 entries：它**没有块**，
    // ⛔ 不许留在派发表 / `all_entries` 里 —— 那会让 trampoline 派发到一个 switch 里没有 label
    //   的函数（落 `bad:` = trap）。它要么是"数据当代码"，要么运行期根本到不了。
    // ★ BUG-24 / P2：**表项类**（`SoftKind::JumpTableItem`）**不写 `rejected_targets_`**：
    //   那个集合是"判过就永不重判"（见 `rejected_targets_` 的注释），而表项是否可解码 / 是否被
    //   别的块盖住是**这一趟**的事实，下一趟由识别结果重新决定。写进去就等于把 BUG-24 的放大器
    //   复制到 P2 类上（那正是它从"一条垃圾块"升级成"永久缺块"的机制）。
    void rejectSoftRoot(std::uint32_t ea) {
        const SoftKind kind = softKindOf(ea);
        soft_roots_.erase(ea);
        soft_roots_call_.erase(ea);
        soft_roots_store_.erase(ea);
        soft_roots_data_.erase(ea);
        soft_roots_jumptab_.erase(ea);
        soft_roots_item_.erase(ea);
        entries_.erase(ea);
        leaders_.erase(ea);
        boundary_.erase(ea);
        if (kind != SoftKind::JumpTableItem) rejected_targets_.insert(ea);
    }

private:
    SoftStats* statsOf(SoftKind kind) const { return accounts_.of(kind); }

    // P1（S2）的唯一谓词：这个 EA 落在**被排除的跳表区间**内吗？（`covers` 是闭开区间）
    bool inDataRange(std::uint32_t ea) const {
        return !data_ranges_.spans.empty() && data_ranges_.covers(ea);
    }
    // 记下"这个 EA 是怎么变成 leader 的"（先到先得，但优先级按 `LeaderBoundary` 的枚举顺序）。
    void noteBoundary(std::uint32_t ea, LeaderBoundary b) {
        auto it = boundary_.find(ea);
        if (it == boundary_.end()) {
            boundary_.emplace(ea, b);
            return;
        }
        if (static_cast<std::uint8_t>(b) < static_cast<std::uint8_t>(it->second)) it->second = b;
    }

    // ★ BUG-24 / P2（S3）：表项在**建块之前**成 leader/入口。
    //   ⛔ 不走 `offerSoftRoot`（那里写 `rejected_targets_`）；四档各记各的账：
    //     ① 已是入口（= 根，或同趟里 D14/D15 先收的）⇒ `item_already_`；
    //     ② shim 桩页 ⇒ `item_shim_`；
    //     ③ 不在可执行节 ⇒ `item_outside_`；
    //     ④ 落在**被排除的表区间**内 ⇒ `item_in_range_`（那是数据，不是代码）。
    //     其余 ⇒ 收成 `SoftKind::JumpTableItem` 软根（同时进 entries/leaders/pending）。
    //   ★ "解不出" 不在这里判：由 `explore` / `buildBlocksDropping` 在实际解码时记进
    //     `accounts_.item->undecodable`，且**不写** `rejected_targets_`。
    void seedJumpTableItems(std::set<std::uint32_t>* pending) {
        item_seeds_ = jmptab_items_.size();
        if (!close_jmptab_items_) return;
        for (const std::uint32_t ea : jmptab_items_) {
            if (entries_.count(ea) != 0) {
                ++item_already_;
                continue;
            }
            if (ea >= shim_begin_ && ea < shim_end_) {
                ++item_shim_;
                continue;
            }
            if (lift::aot::execRangeOf(exec_ranges_, ea) == nullptr) {
                ++item_outside_;
                continue;
            }
            if (inDataRange(ea)) {
                ++item_in_range_;
                continue;
            }
            entries_.insert(ea);
            leaders_.insert(ea);
            pending->insert(ea);
            soft_roots_.insert(ea);
            soft_roots_item_.insert(ea);
            if (accounts_.item != nullptr) accounts_.item->added.insert(ea);
            noteBoundary(ea, LeaderBoundary::JumpTabItem);
        }
    }

    // ★ D15 的唯一判据：**写内存的 `mov` 的 imm32** 怎么处理。
    //   形态判据（"这个 imm 是内存写的值"）就在这里，只有这一处：
    //     `op == Mov`               —— 语义是 `wr(op0, rd(op1))`（`exec_integer.cpp:342`），
    //                                  所以"写"发生在 op0、"值"来自 op1；
    //     `op0().kind == Mem`       —— 目的操作数是内存 ⇒ 这是一条**写内存**的 mov
    //                                  （含 `[reg]` / `[reg+disp]` / `[reg+disp+index*scale]` /
    //                                   `[disp32]` 绝对地址 / `fs:`·`gs:` 段覆盖，全在 `MemOperand` 里）；
    //     `op1().kind == Imm`       —— 源是立即数 ⇒ 那个立即数就是被写进内存的 4 字节值。
    //   ⛔ 因此 `mov <reg>, imm32`（op0 是 Reg）/ `push imm32` / `cmp/... imm32` 一律**不进**
    //      这条判据（它们的立即数不是"写内存的值"）。
    //   ★ 宽度：`mov [mem], imm8/imm16` 也过这条判据，但那种值（≤0xFFFF）不可能落在可执行节
    //     （映像基址 0x400000）⇒ 在 ① 处自然落进"越界"，不需要另写一条宽度规则。
    void offerStoreImmTarget(const lift::Insn& in, std::set<std::uint32_t>* pending) {
        if (in.op0().kind != lift::OpKind::Mem) return;
        if (in.op1().kind != lift::OpKind::Imm) return;
        const std::uint32_t value = static_cast<std::uint32_t>(in.op1().imm);
        ++store_imm_sites_;
        if (lift::aot::execRangeOf(exec_ranges_, value) != nullptr) ++store_imm_in_exec_;
        offerSoftRoot(value, SoftKind::StoreImm, pending);
    }

    // ★ D14 / D15 / D17 / D26 的**唯一**收/丢判据：一个软根候选（`call rel32` 目标 / 写内存 mov 的
    //   imm32 / `.data` 静态代码指针 / 可执行节 dword）怎么处理（收到入口集 / 跳过 / 丢弃）只在这里
    //   写一次。`kind` 决定记到哪一类的账上。★ 四类的开关**各自独立**（`--no-close-calls` /
    //   `--no-close-stores` / `--close-data`·`--no-close-data` / `--close-jmptab`），⛔ 不合成一个总开关。
    //   ★ P2 的 `SoftKind::JumpTableItem` **故意不走这里**（它有自己的 `seedJumpTableItems`：
    //     这里会写 `rejected_targets_`，而表项不许写）。
    void offerSoftRoot(std::uint32_t target, SoftKind kind, std::set<std::uint32_t>* pending) {
        switch (kind) {
            case SoftKind::Call: if (!close_calls_) return; break;
            case SoftKind::StoreImm: if (!close_stores_) return; break;
            case SoftKind::DataPtr: if (!close_data_) return; break;
            case SoftKind::JumpTab: if (!close_jmptab_) return; break;
            default: return;
        }
        if (rejected_targets_.count(target) != 0) return;  // 已经判过（⛔ 不重复收/计数）
        if (entries_.count(target) != 0) return;           // ① 已在当前入口集（含另一类已收的软根）
        SoftStats* const stats = statsOf(kind);
        // ★ BUG-24 / P1（S2）：目标落在**被排除的跳表区间**内 ⇒ 单独一档、跳过。
        //   ⛔ 不写 `rejected_targets_`：区间是"这一趟的识别结果"，下一趟可能不一样；
        //   写成"判过就永不重判"会把 BUG-24 的放大器复制过来。
        if (inDataRange(target)) {
            if (stats != nullptr) stats->table_range.insert(target);
            return;
        }
        if (target >= shim_begin_ && target < shim_end_) {  // shim 桩 EA：由 trampoline 派发
            rejected_targets_.insert(target);
            if (stats != nullptr) stats->shim_targets.insert(target);
            return;
        }
        if (lift::aot::execRangeOf(exec_ranges_, target) == nullptr) {  // 可执行节外（越界）
            rejected_targets_.insert(target);
            if (stats != nullptr) stats->outside.insert(target);
            return;
        }
        soft_roots_.insert(target);
        switch (kind) {
            case SoftKind::Call: soft_roots_call_.insert(target); break;
            case SoftKind::StoreImm: soft_roots_store_.insert(target); break;
            case SoftKind::DataPtr: soft_roots_data_.insert(target); break;
            case SoftKind::JumpTab: soft_roots_jumptab_.insert(target); break;
            case SoftKind::JumpTableItem: soft_roots_item_.insert(target); break;
            default: break;
        }
        entries_.insert(target);
        leaders_.insert(target);
        pending->insert(target);
        if (stats != nullptr) stats->added.insert(target);
        switch (kind) {
            case SoftKind::Call: noteBoundary(target, LeaderBoundary::Call); break;
            case SoftKind::StoreImm: noteBoundary(target, LeaderBoundary::StoreImm); break;
            case SoftKind::DataPtr: noteBoundary(target, LeaderBoundary::DataPtr); break;
            default: break;
        }
    }

    const lift::Insn* get(std::uint32_t ea, std::string* err, std::uint32_t* err_ea) {
        const auto it = insns_.find(ea);
        if (it != insns_.end()) return &it->second;
        lift::Insn in;
        std::string why;
        if (!decoder_.decode(mem_, ea, &in, &why)) {
            if (err != nullptr) *err = why;
            if (err_ea != nullptr) *err_ea = ea;
            return nullptr;
        }
        return &insns_.emplace(ea, in).first->second;
    }

    const lift::Decoder& decoder_;
    const lift::GuestMemory& mem_;
    const std::vector<lift::aot::ExecRange>& exec_ranges_;
    std::uint32_t shim_begin_ = 0;
    std::uint32_t shim_end_ = 0;
    bool close_calls_ = true;
    bool close_stores_ = true;
    bool close_data_ = kCloseDataByDefault;
    bool close_jmptab_ = kCloseJmptabByDefault;
    bool close_jmptab_items_ = true;   // ★ P2（S3）：表项在建块之前成 leader
    SoftAccounts accounts_;            // 五类软根的账（D14 / D15 / D17 / D26 / 表项）
    std::map<std::uint32_t, lift::Insn> insns_;
    std::set<std::uint32_t> leaders_;
    std::set<std::uint32_t> entries_;
    std::set<std::uint32_t> soft_roots_;        // 五类软根的**并集**（closure.txt 之外的 unit 根）
    std::set<std::uint32_t> soft_roots_call_;   // 其中由 D14（call rel32 目标）收的
    std::set<std::uint32_t> soft_roots_store_;  // 其中由 D15（写内存 mov 的 imm32）收的
    std::set<std::uint32_t> soft_roots_data_;   // 其中由 D17（`.data` 静态代码指针）收的
    std::set<std::uint32_t> soft_roots_jumptab_;  // 其中由 D26（可执行节跳表 dword）收的
    std::set<std::uint32_t> soft_roots_item_;   // 其中由 P2（识别出的跳表项）收的
    std::set<std::uint32_t> jmptab_items_;       // P2 候选（上一趟识别出的表项，去重升序）
    BlockSpans data_ranges_;                     // ★ P1：被排除的跳表区间（升序 + prefix_max）
    std::map<std::uint32_t, LeaderBoundary> boundary_;  // ★ P0：leader 的来源（审计的 boundary=）
    std::uint64_t data_range_leaders_ = 0;       // P1 读数：被区间挡下的 leader 数
    std::uint64_t data_range_root_hits_ = 0;     // P1 读数：其中是根（应为 0；⑤ 的兜底）
    std::uint64_t call_ret_in_data_range_ = 0;   // P1 读数：`call` 返回 EA 落在区间 ⇒ 不进入口集
    std::set<std::uint32_t> table_range_leaders_;  // P1 读数：被挡下的 leader 集合（报告里点名）
    std::uint64_t item_seeds_ = 0;               // P2 读数：表项候选数（去重）
    std::uint64_t item_already_ = 0;             // P2 读数：已是入口（根 / 先到的软根）
    std::uint64_t item_shim_ = 0;                // P2 读数：shim 桩页
    std::uint64_t item_outside_ = 0;             // P2 读数：不在可执行节
    std::uint64_t item_in_range_ = 0;            // P2 读数：落在被排除的表区间内
    std::set<std::uint32_t> rejected_targets_;  // 已判过（越界 / shim / 解不出）：⛔ 不重复收/计数
    std::uint64_t store_imm_sites_ = 0;         // D15 读数：`mov [mem], imm32` 站点数
    std::uint64_t store_imm_in_exec_ = 0;       // 其中 imm 落在可执行节内的
    std::set<std::uint32_t> data_seeds_;        // D17 候选集（`scanDataPointers` 给的升序集合）
    std::uint64_t data_ptr_sites_ = 0;          // D17 读数：扫过的 dword 站点数
    std::uint64_t data_ptr_in_exec_ = 0;        // 其中值落在可执行节内的站点数
    std::uint64_t data_ptr_skipped_ = 0;        // 候选里已在入口集 / 已判过 ⇒ 不重复收
    std::uint64_t data_ptr_overlap_pre_ = 0;    // 候选里落在已发射块字节区间内 ⇒ 收之前就挡下
    std::set<std::uint32_t> jumptab_seeds_;     // D26 候选集（`scanExecCodeDwords` 给的升序集合）
    std::uint64_t jumptab_sites_ = 0;           // D26 读数：扫过的可执行节 dword 站点数
    std::uint64_t jumptab_in_exec_ = 0;         // 其中值落在可执行节内的站点数
    // ★ D26 的第二条扫描：未对齐指令 imm32（push / mov；候选已经并进 `jumptab_seeds_`；这里只留读数）。
    std::set<std::uint32_t> imm32_seeds_;
    std::uint64_t imm32_push_sites_ = 0;
    std::uint64_t imm32_push_in_exec_ = 0;
    std::uint64_t imm32_mov_sites_ = 0;
    std::uint64_t imm32_mov_in_exec_ = 0;
    std::uint64_t jumptab_rounds_ = 0;          // D26 读数：不动点迭代跑了几轮（0 = 关 / 无候选）
    JumpTabSurface jumptab_surface_;            // D26 读数：候选面（收之前读一次）
    std::set<std::uint32_t> scanned_;           // 已经扫过的 leader（两次 `explore` 之间共享）
};

// =====================================================================================
// ② 基本块：从 leader 起，到控制流（含）或"下一条是 leader"为止
// =====================================================================================
// ★ 失败时把**出错的 leader** 报出来（`err_leader`）：调用方要据此判"根 ⇒ 硬红 / 派生 ⇒ 丢弃"。
bool buildBlocks(const std::map<std::uint32_t, lift::Insn>& insns,
                 const std::set<std::uint32_t>& leaders, std::map<std::uint32_t, GenBlock>* blocks,
                 std::string* err, std::uint32_t* err_leader) {
    for (const std::uint32_t leader : leaders) {
        GenBlock block;
        block.ea = leader;
        std::uint32_t cur = leader;
        bool closed = false;
        for (int guard = 0; guard < 1000000; ++guard) {
            const auto it = insns.find(cur);
            if (it == insns.end()) {
                if (err != nullptr)
                    *err = "a stretch of the leader instruction stream did not decode (internal inconsistency) @ " + std::to_string(cur);
                if (err_leader != nullptr) *err_leader = leader;
                return false;
            }
            block.insns.push_back(it->second);
            if (lift::opIsControl(it->second.op)) {
                block.terminated = true;
                closed = true;
                break;
            }
            const std::uint32_t next = cur + it->second.length;
            if (leaders.count(next) != 0) {
                closed = true;
                break;
            }
            cur = next;
        }
        if (!closed) {
            if (err != nullptr) *err = "block scan exceeded the 1e6 instruction limit";
            if (err_leader != nullptr) *err_leader = leader;
            return false;
        }
        (*blocks)[leader] = std::move(block);
    }
    return true;
}

// 五类软根的账**选一份**（按 `kind`）。⛔ 不要把五类的丢弃合成一个数。
//（唯一实现在 `SoftAccounts::of`；这一段只是把它按值传下去，⛔ 不写第二份 switch。）

// 建块，并按 D10 P3 ① / D14 / D15 / D17 / D26 / P2 丢弃"派生 leader 或软根的字节流里有一段解不出"的块（根 ⇒ 硬红）。
// ★ 软根的账**按类**记（`explorer->softKindOf`）：call / store-imm / data-ptr / jmptab / 表项 五类的"解不出"分开计数。
bool buildBlocksDropping(Explorer* explorer, std::set<std::uint32_t>* leaders,
                         const std::set<std::uint32_t>& roots, const SoftAccounts& accounts,
                         std::map<std::uint32_t, GenBlock>* blocks, DropStats* drops,
                         std::string* err, std::uint32_t* err_ea) {
    for (;;) {
        blocks->clear();
        std::string why;
        std::uint32_t bad = 0;
        if (buildBlocks(explorer->insns(), *leaders, blocks, &why, &bad)) return true;
        if (roots.count(bad) != 0) {
            if (err != nullptr) *err = why;
            if (err_ea != nullptr) *err_ea = bad;
            return false;  // 根 ⇒ 硬红
        }
        if (explorer->isSoftRoot(bad)) {
            // ★ D14 / D15 / D17 / D26 / P2：软根解不出 ⇒ 丢弃 + 计数（⛔ 不硬红），记到**自己那一类**的账上。
            const SoftKind kind = explorer->softKindOf(bad);
            explorer->rejectSoftRoot(bad);
            drops->dropped.insert(bad);
            SoftStats* const st = accounts.of(kind);
            if (st != nullptr) st->undecodable.insert(bad);
        } else if (!drops->add_undecodable(bad)) {
            if (err != nullptr) *err = why + " (the same derived leader failed repeatedly)";
            if (err_ea != nullptr) *err_ea = bad;
            return false;
        }
        leaders->erase(bad);
    }
}

// D10 P3 ②：派生 leader（以及五类软根）落在**已发射块的字节区间内** ⇒ 丢弃（根永不丢；
// 返回 true = 有新的丢弃）。★ 软根的重叠与"解不出"**分开计数**，且**按类分开**（要求：不要混成一个数）。
// ★ 用"按起点排序 + 前缀最大 end"判定，允许块**任意重叠/嵌套**（T1a §4.3：块互不重叠并不成立）。
bool dropLeadersInsideBlocks(const std::map<std::uint32_t, GenBlock>& blocks,
                             const std::set<std::uint32_t>& roots, Explorer* explorer,
                             std::set<std::uint32_t>* leaders, DropStats* drops,
                             const SoftAccounts& accounts) {
    // ★ 判据只有一份：`buildBlockSpans` + `BlockSpans::inside`（与 D17 的"收之前"那条共用）。
    const BlockSpans spans = buildBlockSpans(blocks);
    std::vector<std::uint32_t> junk;
    for (const std::uint32_t ea : *leaders) {
        if (roots.count(ea) != 0) continue;  // 根永不丢
        if (spans.inside(ea)) junk.push_back(ea);
    }
    bool changed = false;
    for (const std::uint32_t ea : junk) {
        if (explorer->isSoftRoot(ea)) {
            // ★ D14 / D15 / D17 / D26 / P2：软根与已发射块重叠 ⇒ 丢弃（与"解不出"分开计数，且**按类**分开）
            const SoftKind kind = explorer->softKindOf(ea);
            SoftStats* const st = accounts.of(kind);
            if (st != nullptr && st->overlap.insert(ea).second) changed = true;
            explorer->rejectSoftRoot(ea);
            drops->dropped.insert(ea);
        } else if (drops->add_inside_block(ea)) {
            changed = true;
        }
    }
    for (const std::uint32_t ea : junk) leaders->erase(ea);
    return changed;
}

// ①+② 的唯一入口：leader 发现（含 D14 静态调用闭包 + D15 代码指针立即数 + D17 `.data` 静态代码指针
// + D26 可执行节跳表 dword + P2 识别出的跳表项）+ 建块 + 派生/软根丢弃（主路径 / `--check` /
// `--jmptab-audit` **共用同一份判据**，⛔ 不许几处各写一套展开 / 丢弃规则 —— 那样一处的绿就不再
// 说明另一处的绿）。
bool buildUnits(const std::set<std::uint32_t>& roots, Explorer* explorer, const SoftAccounts& accounts,
                std::map<std::uint32_t, GenBlock>* blocks, DropStats* drops, std::string* err,
                std::uint32_t* err_ea) {
    // 建块 + 派生/软根丢弃到不动点（丢弃是单调的 ⇒ 必然收敛）。
    auto blocksToFixpoint = [&](std::set<std::uint32_t>* leaders) -> bool {
        for (;;) {
            if (!buildBlocksDropping(explorer, leaders, roots, accounts, blocks, drops, err, err_ea))
                return false;
            // 丢弃后块边界会变 ⇒ 重建到不动点。
            if (!dropLeadersInsideBlocks(*blocks, roots, explorer, leaders, drops, accounts)) break;
        }
        return true;
    };
    // 只取"**没被丢弃**的 leader"并重建到不动点（D17 阶段的防御性修剪，理由见那里的长注释；
    // D26 阶段每轮复用同一段）。
    auto rebuildFromExplorerLeaders = [&](std::set<std::uint32_t>* leaders) -> bool {
        leaders->clear();
        for (const std::uint32_t ea : explorer->leaders()) {
            if (drops->dropped.count(ea) == 0) leaders->insert(ea);
        }
        return blocksToFixpoint(leaders);
    };

    // ---- 阶段 ①：D14 / D15 两类软根收敛 + 建块 ----
    if (!explorer->run(roots, drops, err, err_ea)) return false;
    std::set<std::uint32_t> leaders = explorer->leaders();
    if (!blocksToFixpoint(&leaders)) return false;

    // ---- 阶段 ②：D17 第三类软根（`.data` 静态代码指针）----
    // ★ 两阶段的理由："落在已发射块的字节区间内"这条判据（D10 P3 ②）要有**已建好的块**才能判；
    //   而第三类的候选集是静态的（扫一遍 PE 就有）⇒ 正好可以在①之后、扩展工作集**之前**判。
    // ★ 只做**一轮**：候选集固定 ⇒ 新展开的代码区不会产生新候选（与 D14/D15 的迭代不动点不同）。
    if (explorer->dataPtrDistinct() != 0) {
        std::set<std::uint32_t> pending;
        explorer->seedDataPointerRoots(buildBlockSpans(*blocks), &pending);
        if (!pending.empty()) {
            if (!explorer->explore(&pending, roots, drops, err, err_ea)) return false;
            // ★ 只取"**没被丢弃**的 leader"（防御性修剪）：①阶段被判过"解不出 / 落在已发射块字节区间内"的
            //   leader 仍留在 `explorer->leaders()` 里 —— `dropLeadersInsideBlocks` 只删**本地** leaders 副本
            //   （`buildBlocksDropping` 也是）。把整份塞回去会把它们**重新建成块**，而那些块不会被任何分量
            //   claim（分区对 `drops.dropped` 里的 EA 一律跳过）⇒ `blocks` 里多出一批"有块但没主人"的项
            //   （`--check` 的 `blocks=` 数的是 `blocks.size()` ⇒ 与主路径的口径差就是这么来的）。
            //   本输入上实测**产物不变**（`.ll` sha256 与修剪前相同）⇒ 纯防御。
            if (!rebuildFromExplorerLeaders(&leaders)) return false;
        }
    }

    // ---- 阶段 ③：D26 第五类软根（可执行节跳表 dword → **尚未解码**的代码）----
    // ★ 与 D17 同样"必须有已建好的块才能判"（判据 ③⑤），区别是**会新增块** ⇒ 新块又可能挡住
    //   别的候选 ⇒ **迭代到不动点**。终止性：每次迭代 `rejected_targets_`（判过的不再判）或
    //   `entries_` 单调增长，候选集有限 ⇒ 轮数有限。`kJumpTabMaxRounds` 只是护栏
    //   （撞到 = 生成器内部不一致 ⇒ 响亮失败，⛔ 不静默截断）。
    constexpr unsigned kJumpTabMaxRounds = 1000;
    if (explorer->jumpTabDistinct() != 0) {
        // ★ **先量后收**：候选面在**收之前**读一次（块集 = ①+② 定型之后）⇒ 五档相加 = `distinct`。
        std::set<std::uint32_t> block_starts;
        for (const auto& kv : *blocks) block_starts.insert(kv.first);
        explorer->setJumpTableSurface(
            explorer->measureJumpTableSurface(buildBlockSpans(*blocks), block_starts));
        for (unsigned round = 0; round < kJumpTabMaxRounds; ++round) {
            block_starts.clear();
            for (const auto& kv : *blocks) block_starts.insert(kv.first);
            std::set<std::uint32_t> pending;
            const bool any =
                explorer->seedJumpTableCodeRoots(buildBlockSpans(*blocks), block_starts, &pending);
            if (!any || pending.empty()) break;
            if (!explorer->explore(&pending, roots, drops, err, err_ea)) return false;
            if (!rebuildFromExplorerLeaders(&leaders)) return false;
        }
        if (explorer->jumpTabRounds() >= kJumpTabMaxRounds) {
            if (err != nullptr) *err = "fixed-point iteration of kind-5 soft roots (D26) exceeded the round limit";
            if (err_ea != nullptr) *err_ea = 0;
            return false;
        }
    }
    return true;
}

// =====================================================================================
// ③ 入口 EA 文本文件（一行一个 hex EA，`#` 起注释）
// =====================================================================================
std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
    return s.substr(b, e - b);
}

bool readRootsFile(const std::string& path, std::set<std::uint32_t>* roots, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *err = "cannot open `" + path + "`";
        return false;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        std::string token = line;
        if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
            token = token.substr(2);
        }
        bool ok = !token.empty() && token.size() <= 8;
        for (const char ch : token) {
            if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) ok = false;
        }
        if (!ok) {
            *err = "line " + std::to_string(lineno) + " is not a hex EA of at most 8 digits: `" + line + "`";
            return false;
        }
        roots->insert(static_cast<std::uint32_t>(std::strtoul(token.c_str(), nullptr, 16)));
    }
    return true;
}

// =====================================================================================
// ★ 构建期**库内建表**（入库件 `aot/intrinsics.txt`）的解析 + 硬校验
//
// 形式（一行一条；`#` 起注释）：
//   pe-sha256 <16..64 hex>                         本表只对这份 PE 有效（= 所读 PE 的 sha256 **前缀**）
//   ea 0x<8 hex> <builtin> <cc> <args> <ret>       一个内建单元根
//     <builtin> ∈ {memcpy}（v1 只有一个；⛔ 发射器不认识的必须**构建期红**，不能静默跳过）
//     <cc>      = cdecl
//     <args>    = dst=esp+4,src=esp+8,n=esp+12
//     <ret>     = eax
//   ★ 为什么把调用约定 / 参数 / 返回值也写进表：发射器里的"读 `[esp+4..12]`、写 EAX、`esp += 4`"
//     是**形态假设**；表里声明过、这里再逐字校验 ⇒ 任何一边漂了都是构建期红（⛔ 不是运行期静默错位）。
//   ★ 判据链：`.lst` 的 FLIRT 名（`_memcpy`）+ 反汇编（cdecl / epilogue / `std;rep movsl;cld`）
//     + 契约测试 `lift-intrinsics`。表只是**落点**，不是判据本身。
// =====================================================================================
struct IntrinsicEntry {
    std::uint32_t ea = 0;
    std::string builtin;
    std::string convention;
    std::string args;
    std::string ret;
};

struct IntrinsicTable {
    std::string path;
    std::string pe_sha256_prefix;  // 小写
    std::vector<IntrinsicEntry> entries;
};

bool parseIntrinsicsFile(const std::string& path, IntrinsicTable* table, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *err = "cannot open `" + path + "`";
        return false;
    }
    table->path = path;
    bool have_sha = false;
    std::set<std::uint32_t> seen;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        std::vector<std::string> tok;
        {
            std::size_t b = 0;
            while (b < line.size()) {
                while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b])) != 0) ++b;
                std::size_t e0 = b;
                while (e0 < line.size() && std::isspace(static_cast<unsigned char>(line[e0])) == 0) ++e0;
                if (e0 > b) tok.push_back(line.substr(b, e0 - b));
                b = e0;
            }
        }
        if (tok.empty()) continue;
        const std::string at = path + ":" + std::to_string(lineno);
        if (tok[0] == "pe-sha256") {
            if (have_sha) {
                *err = at + ": duplicate `pe-sha256` (exactly one, before any `ea` line)";
                return false;
            }
            if (tok.size() != 2) {
                *err = at + ": `pe-sha256` needs exactly one hex value";
                return false;
            }
            if (tok[1].size() < 16 || tok[1].size() > 64 || (tok[1].size() & 1u) != 0) {
                *err = at + ": sha256 prefix must be 16..64 hex chars (even length), got `" + tok[1] + "`";
                return false;
            }
            std::string low;
            low.reserve(tok[1].size());
            for (const char ch : tok[1]) {
                if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
                    *err = at + ": sha256 prefix is not hex: `" + tok[1] + "`";
                    return false;
                }
                low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
            table->pe_sha256_prefix = low;
            have_sha = true;
            continue;
        }
        if (tok[0] != "ea") {
            *err = at + ": unknown directive `" + tok[0] + "` (expected `pe-sha256` or `ea`)";
            return false;
        }
        if (!have_sha) {
            *err = at + ": `ea` before `pe-sha256` (the PE pin must come first)";
            return false;
        }
        if (tok.size() != 6) {
            *err = at + ": `ea <EA> <builtin> <cc> <args> <ret>` needs 6 fields, got " +
                   std::to_string(tok.size());
            return false;
        }
        std::string ea_tok = tok[1];
        if (ea_tok.size() != 10 || ea_tok[0] != '0' || (ea_tok[1] != 'x' && ea_tok[1] != 'X')) {
            *err = at + ": EA must be `0x` + exactly 8 hex digits: `" + ea_tok + "`";
            return false;
        }
        std::uint32_t ea = 0;
        for (std::size_t i = 2; i < 10; ++i) {
            const char ch = ea_tok[i];
            if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
                *err = at + ": EA is not hex: `" + ea_tok + "`";
                return false;
            }
            ea = (ea << 4) | static_cast<std::uint32_t>(
                                  std::isdigit(static_cast<unsigned char>(ch))
                                      ? (ch - '0')
                                      : (std::tolower(static_cast<unsigned char>(ch)) - 'a' + 10));
        }
        if (!seen.insert(ea).second) {
            *err = at + ": duplicate EA " + hexEa(ea);
            return false;
        }
        // ★ v1 发射器**只认这一种形态**。表里写别的 ⇒ 构建期红（⛔ 不许"解析通过、发射时静默跳过"）。
        if (tok[2] != "memcpy") {
            *err = at + ": unsupported builtin `" + tok[2] +
                   "` (v1 emitter knows only `memcpy`)";
            return false;
        }
        if (tok[3] != "cdecl" || tok[4] != "dst=esp+4,src=esp+8,n=esp+12" || tok[5] != "eax") {
            *err = at + ": unsupported signature `" + tok[3] + " " + tok[4] + " " + tok[5] +
                   "` (v1 emitter knows only `cdecl dst=esp+4,src=esp+8,n=esp+12 eax`)";
            return false;
        }
        table->entries.push_back(IntrinsicEntry{ea, tok[2], tok[3], tok[4], tok[5]});
    }
    if (!have_sha) {
        *err = path + ": missing `pe-sha256` line";
        return false;
    }
    return true;
}

// =====================================================================================
// ③' ★ D17：第三类软根的**候选集** = 非可执行节里静态初始化的代码指针
//
// 判据（父级给定；**只有这一处**）：**非可执行节**里 **4 字节对齐**的 dword，其值落在
// **可执行节**内 ⇒ 软根候选。
// ★ 为什么扫**文件字节**（`PeImage::bytes`）而不是映射后的 guest 内存：这一类的语义就是
//   "PE 里就写好的初值"（G2 取证：`0x004D2410` 在 `.data` 里 7 处，不是运行期 `mov [mem], imm32`
//   建的）⇒ 真源是**文件的 raw 字节**；`.bss` 尾巴（`raw_size < vsize`）本来就没有静态初值，
//   跳过它是判据的一部分，不是省事。
// ★ 对齐口径：**节内偏移 4 字节对齐**（`raw_offset` 在 PE 里是 FileAlignment 对齐的，
//   ⇒ 等价于"RVA / EA 4 字节对齐"，也与 G2 的 `data_ptrs.mjs` 保守口径同一式）。
// ★ `sites` / `in_exec` 是判据的**输入面读数**（⛔ 不是判据）：前者是扫过的 dword 数，
//   后者是其中值落在可执行节内的（= 候选站点数）；两者之差就是"数据常量"那一类。
//   `targets` 去重后才是候选集（升序 ⇒ 确定性）。
// ⛔ 不许按节名裁剪候选（例如"只收 `.data`"）：父级**明确**要求按"非可执行节"判，
//   少收一个就是漏一个入口（`.rsrc` / `.idata` 里也可能有代码指针，噪声由三档丢弃去分）。
// =====================================================================================
struct DataPtrScan {
    std::uint64_t sites = 0;           // 扫过的 4 字节对齐 dword 站点数
    std::uint64_t in_exec = 0;         // 其中值落在可执行节内的站点数
    std::set<std::uint32_t> targets;   // 不同目标（升序）= 软根候选
};

DataPtrScan scanDataPointers(const lift::PeImage& image,
                             const std::vector<lift::aot::ExecRange>& exec_ranges) {
    DataPtrScan out;
    for (const lift::PeSectionInfo& sec : image.sections) {
        if (sec.executable()) continue;  // 判据：**非**可执行节
        const std::uint64_t vsize = sec.vsize != 0 ? sec.vsize : sec.raw_size;
        // 节里"文件真有字节"的部分（PE 语义：初值只在 raw 区；vsize 多出来的尾巴是 0 填充）
        const std::uint64_t n = std::min<std::uint64_t>(sec.raw_size, vsize);
        if (n < 4) continue;
        const std::uint64_t raw_end = static_cast<std::uint64_t>(sec.raw_offset) + n;
        if (raw_end > image.bytes.size()) continue;  // `loadPeFile` 已全量校验；这里只是不许越界读
        for (std::uint64_t i = 0; i + 4 <= n; i += 4) {
            const std::uint8_t* p = image.bytes.data() + sec.raw_offset + i;
            const std::uint32_t value = static_cast<std::uint32_t>(p[0]) |
                                        (static_cast<std::uint32_t>(p[1]) << 8) |
                                        (static_cast<std::uint32_t>(p[2]) << 16) |
                                        (static_cast<std::uint32_t>(p[3]) << 24);
            ++out.sites;
            if (lift::aot::execRangeOf(exec_ranges, value) == nullptr) continue;
            ++out.in_exec;
            out.targets.insert(value);
        }
    }
    return out;
}

// =====================================================================================
// ③'' ★ D21（第四类）：**可执行节里的跳表 dword** = 已发射块的首 EA，但该 EA 不在入口集
//
// 现象（G3 §8.1 / W1 §4.3）：`.text` 里的跳表（`jmp ds:jpt_xxx[eax*4]` 的操作数表）存的是
// **case 目标的 EA**。运行期 `mov eax, [jpt + idx*4]` 之后经 `ret i32 <EA>` 回 trampoline，
// 而 trampoline 只认派发表 ⇒ 查不到 ⇒ **响亮停（decode-error）**。这不是语义错：那些块的字节
// 早就 emit 了（它们是 leader，被某条 `jcc` 指向），缺的只是"这个 EA 要能被派发"。
//
// 判据（父级 D21；**只有这一处**）：**可执行节**里 **4 字节对齐**的 dword，值**等于某个已建块的
// 首 EA** ⇒ 若该 EA 不在入口集，就把它加进**拥有那个块的分量**的 `entries`：`switch` 多一个
// case + 派发表多一行。★ **0 新块 / 0 新指令** —— 这一趟**不解码**，只查表。
//   ★ "值必须已经是一个块首"就是强过滤器（与 D17 的 `.data` 指针不同：这里连候选都要求
//     "已经是块首"，所以不可能引入假根、也不会把数据常量收进来）。
//   ★ 与 D14/D15/D17 **正交**：D14 要 `call rel32`、D15 要 `mov [mem], imm32`、D17 要**非**可
//     执行节；这里正是"可执行节里的跳表"，三者都不覆盖。
//   ★ 为什么不放在 `buildUnits`：判据要"**拥有该块的**分量"，而分量/所有权在分区（③）收敛之后
//     才确定 ⇒ 本趟在 `main` 的分区循环里、收敛判定通过之后跑。
//   ⛔ 不做成"新的软根"：软根 = 新 unit 根 = 新块（那会改变 blocks/insns），是另一件事。
//   ★ 块**没有任何分量 claim**（G3 的孤儿形状：块建出来了但谁都不 claim）⇒ 走**既有**的孤儿提升
//     机制（提升成新入口 EA + 重跑分区），⛔ 不另造一套。
// =====================================================================================
struct JumpTableScan {
    std::uint64_t sites = 0;          // 扫过的可执行节 4 字节对齐 dword 站点数
    std::uint64_t block_hits = 0;     // 其中值 == 某个已建块首 EA 的站点数
    std::set<std::uint32_t> targets;  // 不同目标（升序）
};

// 扫**可执行节**（与 `scanDataPointers` 的"非可执行节"互补；两边都是文件 raw 字节 ⇒ 静态初值）。
// `block_starts` = 已建块的首 EA 集合（判据的强过滤器）。
JumpTableScan scanJumpTableTargets(const lift::PeImage& image,
                                   const std::set<std::uint32_t>& block_starts) {
    JumpTableScan out;
    for (const lift::PeSectionInfo& sec : image.sections) {
        if (!sec.executable()) continue;  // 判据：**可执行节**
        const std::uint64_t vsize = sec.vsize != 0 ? sec.vsize : sec.raw_size;
        // `raw_offset` 是 FileAlignment 对齐、节 VA 是 SectionAlignment 对齐、ImageBase 也 4 字节对齐
        // ⇒ 文件偏移 `i += 4` 与 EA `4 字节对齐` 是同一件事（与 D17 的扫描同一式）。
        const std::uint64_t n = std::min<std::uint64_t>(sec.raw_size, vsize);
        if (n < 4) continue;
        const std::uint64_t raw_end = static_cast<std::uint64_t>(sec.raw_offset) + n;
        if (raw_end > image.bytes.size()) continue;  // `loadPeFile` 已全量校验；这里只是不许越界读
        for (std::uint64_t i = 0; i + 4 <= n; i += 4) {
            const std::uint8_t* p = image.bytes.data() + sec.raw_offset + i;
            const std::uint32_t value = static_cast<std::uint32_t>(p[0]) |
                                        (static_cast<std::uint32_t>(p[1]) << 8) |
                                        (static_cast<std::uint32_t>(p[2]) << 16) |
                                        (static_cast<std::uint32_t>(p[3]) << 24);
            ++out.sites;
            if (block_starts.count(value) == 0) continue;
            ++out.block_hits;
            out.targets.insert(value);
        }
    }
    return out;
}

// 第四类的账（与三类软根**分开**：它没有 added/丢弃三档，只有"加了几条 entry"）。
struct JumpTableStats {
    std::uint64_t sites = 0;
    std::uint64_t block_hits = 0;
    std::uint64_t distinct = 0;
    std::set<std::uint32_t> added;      // 新加进某个分量 entries 的 EA
    std::set<std::uint32_t> skipped;    // 已在入口集 / 已判过 ⇒ 不重复加
    std::set<std::uint32_t> unclaimed;  // 是块首，但没有任何分量 claim（⇒ 孤儿提升）
};

// ★ D21 的唯一落点：把"跳表 dword 指向的**已发射块首**"加进拥有它的分量的 `entries`。
// 返回 false = 存在"块首但没有任何分量 claim"的候选 ⇒ 调用方按**既有**孤儿提升机制（提升 + 重跑）
// 处理；此时本函数**不改** `components`（调用方会整轮重建）。
// ★ `emit_entry_set` **不含**这里新加的 EA：它们只进 switch / 派发表，不进"边目标是不是入口"的判据
//   ⇒ 原本在同一分量内 `br` 直跳的静态边**保持直跳**（否则会平白多一次派发）。
bool addJumpTableEntries(const lift::PeImage& image, const std::map<std::uint32_t, GenBlock>& blocks,
                         const std::set<std::uint32_t>& entries,
                         std::vector<GenComponent>* components, JumpTableStats* jump) {
    // 块首 → 拥有它的分量下标（分区保证一个块只属于一个分量）。
    std::map<std::uint32_t, std::size_t> owner;
    for (std::size_t i = 0; i < components->size(); ++i) {
        for (const auto& kv : (*components)[i].blocks) owner.emplace(kv.first, i);
    }
    // 判据的过滤器 = **已建块**的首 EA（含"建出来但没人 claim"的孤儿 ⇒ 单独分类，别静默丢）：
    // 用 `blocks`（全部建出来的块）而不是 `owner`（已发射块），孤儿才看得见。
    std::set<std::uint32_t> starts;
    for (const auto& kv : blocks) starts.insert(kv.first);
    const JumpTableScan scan = scanJumpTableTargets(image, starts);
    jump->sites = scan.sites;
    jump->block_hits = scan.block_hits;
    jump->distinct = scan.targets.size();

    std::vector<std::uint32_t> to_add;
    for (const std::uint32_t ea : scan.targets) {
        if (entries.count(ea) != 0) {  // 已在入口集（含三类软根 / 提升出来的根）
            jump->skipped.insert(ea);
            continue;
        }
        const auto it = owner.find(ea);
        if (it != owner.end()) {
            to_add.push_back(ea);
            continue;
        }
        // 有块、但没有任何分量 claim ⇒ G3 的孤儿形状 ⇒ 交给既有提升机制。
        jump->unclaimed.insert(ea);
    }
    if (!jump->unclaimed.empty()) return false;

    for (const std::uint32_t ea : to_add) {
        (*components)[owner.at(ea)].entries.push_back(ea);
        jump->added.insert(ea);
    }
    // `GenComponent::entries` 的契约是升序（头注）⇒ 加完排一次；对 `.ll` / 派发表无影响
    // （发射器本来就 sort+unique，派发表走 `std::map`）。
    for (GenComponent& comp : *components) {
        std::sort(comp.entries.begin(), comp.entries.end());
        comp.entries.erase(std::unique(comp.entries.begin(), comp.entries.end()), comp.entries.end());
    }
    return true;
}

// =====================================================================================
// ★ BUG-24（S1/S2/S3）：**两趟重启**的唯一实现 + 分区 + D21
//
// 为什么是两趟（父级裁决 (A)，设计准备 §4）：S1 的识别单元是"**一条已解码的**间接 jmp"，而
// P2 要求表项"在阶段①**之前**"就进 leader —— 阶段①之前什么都还没解码。解开这个循环的唯一办法
// 是把 S1 的定位改成"上一趟的产物"：
//     S1(pass k) 的输入 = pass k-1 的 `Explorer::insns_`；输出（表区间 + 表项）喂给 pass k。
// ⛔ 不用"字节模式预扫"（`FF 24 SIB disp32`）当站点来源：实测 94 个命中里至少 6 个是假表，
//    而且它的错误是**哑的**（假表区间会把真代码从解码里排除 ⇒ 静默丢覆盖）。
//
// 收敛判据（**硬红**，不是 warning）：
//   · `tables_pass ⊄ tables_{pass-1}` ⇒ exit 1 并**点名 EA**（识别器或区间错了）；
//   · `tables_pass == tables_{pass-1}` ⇒ 收敛，break；
//   · 到 `--jmptab-max-passes` 还没收敛 ⇒ exit 1（生成器内部不一致）。
//   ⊆ 的比较键 = `(ea, n)`（表长变了也算"不单调"）。
//
// ★ 主路径 / `--check` / `--jmptab-audit` **三处共用这一份**（⛔ 不许各写一套 —— 那样一处的绿
//   就不再说明另一处的绿）。
// =====================================================================================
struct PartitionResult {
    std::vector<GenComponent> components;
    std::set<std::uint32_t> all_entries;
    std::set<std::uint32_t> dispatch_entries;  // = all_entries ∪ D21 added（派发表的口径）
    std::set<std::uint32_t> promoted;
    std::set<std::uint32_t> unbuildable;
    JumpTableStats jump_table;
    std::size_t promo_events = 0;
};

// 分区（③）+ D21（第四类）：从"已建块 + 入口集"跑到 `dispatch_entries` 定型。
// 与原 `main` 内联的那一段**逐字同顺序**（块冲突 ⇒ 提升 ⇒ 重跑；D21 在收敛后只加 entries）。
bool partitionComponents(const std::set<std::uint32_t>& roots, Explorer* explorer,
                         const std::map<std::uint32_t, GenBlock>& blocks, DropStats* drops,
                         const lift::PeImage& image, PartitionResult* out, std::string* err) {
    std::set<std::uint32_t> promoted;
    // ★ G3：入口 EA 里"**没有块**"的那些（落在别人的块字节里 / 解不出）—— 提升它们只会让分区
    //   永远收敛不了（G3 首跑实测 `00404046`）⇒ 单独记账、不提升。
    std::set<std::uint32_t> unbuildable;
    std::vector<GenComponent> components;
    std::set<std::uint32_t> all_entries;
    // ★ D21（第四类）：跳表 dword 指向的已发射块首 —— 只进 switch / 派发表，⛔ 不新增块 / 指令。
    JumpTableStats jump_table;
    std::size_t promo_events = 0;
    for (;;) {
        const std::set<std::uint32_t>& resume_entries = explorer->entries();
        std::set<std::uint32_t> root_set = roots;
        // ★ D14：静态闭包收来的软根也是 **unit 根**（一个软根 = 一个 `.ll` 函数）。
        root_set.insert(explorer->softRoots().begin(), explorer->softRoots().end());
        root_set.insert(promoted.begin(), promoted.end());
        all_entries = resume_entries;
        all_entries.insert(promoted.begin(), promoted.end());

        std::map<std::uint32_t, std::uint32_t> block_owner;
        std::map<std::uint32_t, std::uint32_t> entry_owner;
        std::vector<GenComponent> comps;
        std::uint32_t conflict = 0;

        for (const std::uint32_t root : root_set) {
            GenComponent comp;
            comp.root = root;
            comp.entries.push_back(root);
            std::set<std::uint32_t> pending;
            pending.insert(root);
            // ★ 块图里有环（`jns` 回自身 / 互跳）。`pending` 会被回边反复插入 ⇒ 必须另记"已展开过"，
            //   否则这一轮永远不结束（T1a 首次跑就踩到：`0x436D31` 的 jns 目标是它自己）。
            std::set<std::uint32_t> processed;
            auto claim = [&](std::uint32_t ea) -> bool {
                const auto it = block_owner.find(ea);
                if (it != block_owner.end() && it->second != root) return false;
                const auto bit = blocks.find(ea);
                if (bit == blocks.end()) return false;
                block_owner[ea] = root;
                comp.blocks[ea] = bit->second;
                return true;
            };
            if (!claim(root)) {
                conflict = root;
                break;
            }
            while (!pending.empty() && conflict == 0) {
                const std::uint32_t ea = *pending.begin();
                pending.erase(pending.begin());
                if (!processed.insert(ea).second) continue;  // 环上的回边：已经展开过
                const GenBlock& block = blocks.at(ea);
                for (const GenEdge& edge : lift::aot::genBlockEdges(block)) {
                    if (edge.dynamic) continue;
                    // ★ 目标是被丢弃的派生 leader ⇒ **不建块、不 claim**：生成器把它发成
                    //   `ret i32 <EA>`（交回 trampoline），运行期由解释器裁决（T2b 的口径：
                    //   未知 EA ⇒ 交回解释器）。⛔ 不要把它塞进 `all_entries` —— 那会让
                    //   trampoline 派发到一个 switch 里没有它的函数（落 `bad:` = trap）。
                    if (drops->dropped.count(edge.ea) != 0) continue;
                    if (edge.call_ret) {
                        const std::uint32_t ret = edge.ea;
                        if (root_set.count(ret) != 0) continue;  // 它自己是另一个分量的根
                        const auto eo = entry_owner.find(ret);
                        if (eo == entry_owner.end()) {
                            entry_owner[ret] = root;
                            comp.entries.push_back(ret);
                        } else if (eo->second != root) {
                            conflict = ret;
                            break;
                        }
                        if (!claim(ret)) {
                            conflict = ret;
                            break;
                        }
                        pending.insert(ret);
                    } else {
                        if (all_entries.count(edge.ea) != 0) continue;  // 目标是入口 EA ⇒ ret
                        if (!claim(edge.ea)) {
                            conflict = edge.ea;
                            break;
                        }
                        pending.insert(edge.ea);
                    }
                }
            }
            if (conflict != 0) break;
            components.push_back(std::move(comp));
        }

        if (conflict == 0) {
            // ★ 哨兵（D15 落地时暴露的一个**既有**漏洞，与 D14/D15 的判据无关）：
            //   `entries_` 里有、但**没有任何分量声明**的入口 EA。两类的来源都是"复用了别人的
            //   resume EA"：某条 `call` 的返回 EA 恰好又是一条真 `call` 的目标 —— 而那条返回 EA
            //   所在的段只经由一个**被拒的软根**才可达（软根重叠 / 解不出），那段的分量不发射，
            //   于是谁都不 claim 它。后果：`.ll` 里发 `ret i32 <EA>`，而该 EA 不在派发表里
            //   ⇒ 运行期在**解释器能跑**的地址上响亮停机（不是语义问题，是覆盖缺口）。
            //   ⇒ 与"块冲突"同一套单调提升：把它提升成 unit 根，整轮重跑。
            // ★ G3 补的**护栏**（D17 之后的必然缺陷）：提升的前提是"这个 EA **能有自己的块**"。
            //   第三类软根（`.data` 静态指针）量级上千，会派生出大量假 `call` 的返回 EA，其中一部分
            //   **落在别人的块字节里**（或根本解不出）⇒ 提升成一个没有块的根 ⇒ `claim(root)` 失败
            //   ⇒ 同一个 EA 再次冲突 ⇒ **不收敛**（G3 首次跑就撞上：`00404046`）。
            //   ⇒ 没有块的那些**不提升**：从入口集里退出来 + 记进 `drops.dropped`（语义 = "没有块，
            //   指向它的边发 `ret i32`"，与其他被丢弃的 leader 完全一致）+ 计数 + 样例（⛔ 不静默）。
            std::set<std::uint32_t> covered;
            for (const GenComponent& c : components) covered.insert(c.entries.begin(), c.entries.end());
            std::vector<std::uint32_t> uncovered;
            for (const std::uint32_t ea : all_entries) {
                if (covered.count(ea) == 0 && promoted.count(ea) == 0) uncovered.push_back(ea);
            }
            if (uncovered.empty()) {
                // ★ D21（第四类）：分区收敛、块集定型之后 —— **只加 entries、不加块**。
                //   判据与理由见 `addJumpTableEntries` 的头注（`.text` 里的跳表 dword 指向已发射
                //   块首，但那个 EA 不在入口集 ⇒ 运行期经 trampoline 回来查不到）。
                if (addJumpTableEntries(image, blocks, all_entries, &components, &jump_table)) break;
                // 有"块首但没有任何分量 claim"的候选（G3 的孤儿形状）⇒ 走**既有**孤儿提升机制：
                //   提升成新入口 EA + 整轮重跑（`addJumpTableEntries` 此时**没改** components）。
                std::size_t n_jump_promoted = 0;
                for (const std::uint32_t ea : jump_table.unclaimed) {
                    if (promoted.insert(ea).second) {
                        ++n_jump_promoted;
                        if (!quiet()) std::fprintf(stderr,
                                     "lift-gen: jump-table target %08X is a block start but no component claims it"
                                     " => promoting to a new entry EA and rerunning\n",
                                     ea);
                    }
                }
                if (n_jump_promoted == 0) {
                    // 已经在 promoted 里却仍没有分量 claim ⇒ 与"块冲突提升不收敛"同一类内部不一致。
                    if (err != nullptr) *err = "orphan promotion of a jump-table target does not converge";
                    return false;
                }
                ++promo_events;
                components.clear();
                continue;
            }
            std::size_t n_unbuildable = 0;
            for (const std::uint32_t ea : uncovered) {
                if (blocks.count(ea) == 0) {
                    // 没有块 ⇒ 不可能成为 unit 根（提升只会让分区永远收敛不了）。
                    unbuildable.insert(ea);
                    drops->dropped.insert(ea);
                    explorer->dropEntry(ea);
                    ++n_unbuildable;
                    continue;
                }
                if (promoted.insert(ea).second) {
                    if (!quiet()) std::fprintf(stderr,
                                 "lift-gen: entry %08X has no component claim (reachable only after a rejected soft-root segment)"
                                 " => promoting to a new entry EA and rerunning\n",
                                 ea);
                }
            }
            if (n_unbuildable != 0) {
                if (!quiet()) std::fprintf(stderr,
                             "lift-gen: %zu entries have **no block** (inside an already-emitted block's byte range / undecodable)"
                             " => not promoting, dropping them from the entry set (edges to them emit ret i32)\n",
                             n_unbuildable);
            }
            ++promo_events;
            components.clear();
            continue;
        }
        // ★ 一个块被两个入口函数同时声明 ⇒ **响亮**报错（⛔ 不静默选一个）：
        //   把它提升成新的入口 EA，然后整轮重跑（提升是单调的 ⇒ 必然收敛）。
        if (blocks.count(conflict) == 0) {
            // 冲突的 EA 根本没有块（落在别人的字节里 / 解不出）⇒ 提升它也拿不到块 ⇒ 退出入口集。
            unbuildable.insert(conflict);
            drops->dropped.insert(conflict);
            explorer->dropEntry(conflict);
            if (!quiet()) std::fprintf(stderr,
                         "lift-gen: conflicting block EA %08X has no block (inside an already-emitted block's byte range / undecodable)"
                         " => dropping it from the entry set (edges to it emit ret i32)\n",
                         conflict);
            ++promo_events;
            components.clear();
            continue;
        }
        if (!quiet()) std::fprintf(stderr, "lift-gen: block %08X is claimed by two entry functions => promoting to a new entry EA and rerunning\n",
                     conflict);
        if (!promoted.insert(conflict).second) {
            if (err != nullptr) *err = "partition promotion does not converge (EA " + hexEa(conflict) + " conflicts repeatedly)";
            return false;
        }
        ++promo_events;
        components.clear();
    }

    // 收敛后按 root 升序（std::set 已经保证；这里再按 root 排一次以防未来改动）。
    std::sort(components.begin(), components.end(),
              [](const GenComponent& a, const GenComponent& b) { return a.root < b.root; });

    out->components = std::move(components);
    out->all_entries = std::move(all_entries);
    out->promoted = std::move(promoted);
    out->unbuildable = std::move(unbuildable);
    out->jump_table = jump_table;
    out->promo_events = promo_events;
    // ★ D21（第四类）：**派发表的入口集** = 原入口集 ∪ 跳表加进来的 EA。
    //   ⛔ 与传给发射器的 `all_entries` 刻意分开：后者是"边目标算不算入口"的判据 ⇒ 若把跳表目标
    //   也放进去，原本在同一分量内 `br` 直跳的静态边会平白变成 `ret i32`（多一次派发）。
    //   两者的关系是机械的：`dispatch_entries.size() == all_entries.size() + jump_table.added.size()`
    //   （`addJumpTableEntries` 只加不在入口集里的）⇒ 派发表行数 = pin 的 entries 读数。
    out->dispatch_entries = out->all_entries;
    out->dispatch_entries.insert(jump_table.added.begin(), jump_table.added.end());
    return true;
}

// 一次完整构建的产物（每趟的账都换成**最后一趟**的；⛔ 不要跨趟累加）。
struct BuildRun {
    std::map<std::uint32_t, GenBlock> blocks;
    DropStats drops;
    SoftStats soft_call;
    SoftStats soft_store;
    SoftStats soft_data;
    SoftStats soft_jumptab;
    SoftStats soft_item;  // ★ P2：表项
    JmpTabRecognition jmptab;
    std::map<std::uint32_t, LeaderBoundary> boundaries;
    unsigned passes = 0;
    bool jmptab_on = false;
    // 最终一趟的读数（Explorer 出循环就销毁 ⇒ 必须在这里抄出来）
    std::uint64_t store_imm_sites = 0;
    std::uint64_t store_imm_in_exec = 0;
    std::uint64_t data_ptr_sites = 0;
    std::uint64_t data_ptr_in_exec = 0;
    std::uint64_t data_ptr_distinct = 0;
    std::uint64_t data_ptr_skipped = 0;
    std::uint64_t data_ptr_overlap_pre = 0;
    JumpTabSurface jumptab_surface;
    std::uint64_t jumptab_rounds = 0;
    std::uint64_t jumptab_live_roots = 0;
    // ★ D26 的第二条扫描（未对齐指令 imm32：push / mov）的输入面读数（候选已并进 `jumptab_*` 的账）。
    std::uint64_t imm32_sites = 0;        // 未对齐 imm32 的总站点数（push + mov）
    std::uint64_t imm32_in_exec = 0;      // 其中值落在可执行节内的
    std::uint64_t imm32_push_sites = 0;   // `push imm32` 站点数
    std::uint64_t imm32_mov_sites = 0;    // `mov reg, imm32` 站点数
    std::uint64_t imm32_distinct = 0;     // 不同候选目标数
    std::uint64_t imm32_only = 0;         // 仅由这条扫描发现（不在对齐 dword 候选里）的目标数
    // ★ P1（S2）的读数
    std::uint64_t data_range_leaders = 0;
    std::uint64_t data_range_root_hits = 0;
    std::uint64_t call_ret_in_data_range = 0;
    std::set<std::uint32_t> table_range_leaders;
    // ★ P2（S3）的读数
    std::uint64_t item_seeds = 0;
    std::uint64_t item_already = 0;
    std::uint64_t item_shim = 0;
    std::uint64_t item_outside = 0;
    std::uint64_t item_in_range = 0;
    PartitionResult part;
};

// ★ BUG-24：**唯一**的 pass 循环。`err_code` 区分两类失败：4 = 解码/建块失败（输入问题），
//   1 = 收敛判据红（生成器内部不一致）。
bool buildAll(const std::set<std::uint32_t>& roots, const lift::PeImage& image,
              const std::vector<lift::aot::ExecRange>& exec_ranges, const lift::Decoder& decoder,
              const lift::GuestMemory& mem, const Options& opt, BuildRun* out, std::string* err,
              std::uint32_t* err_ea, int* err_code) {
    *err_code = 0;
    const lift::GuestLayout layout;
    const std::uint32_t shim_end = layout.shim_page + layout.shim_slots * 4u;
    // D17 / D26 的候选面是**静态**的（扫一遍 PE 就有）⇒ 每趟重建 Explorer 时复用同一份扫描结果。
    const DataPtrScan data_scan = scanDataPointers(image, exec_ranges);
    const JumpTabScan jumptab_scan = scanExecCodeDwords(image, exec_ranges);
    // ★ D26 的第二条扫描：**可执行节里任意偏移的指令 imm32**（push / mov；未对齐立即数）。
    //   与对齐 dword 的候选**并集**进 `jumptab_seeds_`（收/丢判据完全相同）；这里只多记输入面读数。
    const lift::aot::Imm32Scan imm32_scan =
        lift::aot::scanInstructionImmediates(image, exec_ranges);
    out->imm32_sites = imm32_scan.sites;
    out->imm32_in_exec = imm32_scan.in_exec;
    out->imm32_push_sites = imm32_scan.push_sites;
    out->imm32_mov_sites = imm32_scan.mov_sites;
    out->imm32_distinct = imm32_scan.targets.size();
    {
        std::uint64_t only = 0;
        for (const std::uint32_t ea : imm32_scan.targets) {
            if (jumptab_scan.targets.count(ea) == 0) ++only;
        }
        out->imm32_only = only;
    }
    // ★ 审计模式**总是**跑 S1（它的用途就是"只看识别 / 只读审计"）：两个消费开关都关时，
    //   `ranges` / `items` 都是空 ⇒ 识别结果不改变任何东西 ⇒ 这就是"只加 P0、没上 P1/P2"的
    //   改前状态（守卫在那一档必须红）。
    out->jmptab_on = opt.jmptab_exclude || opt.close_jmptab_items || opt.jmptab_audit;

    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    std::set<std::uint32_t> items;
    std::set<std::pair<std::uint32_t, std::uint32_t>> prev_keys;
    for (unsigned pass = 1;; ++pass) {
        out->passes = pass;
        // 每趟**全新**：块 / 丢弃 / 五类软根的账（⛔ 不要跨趟累加：读数必须只描述最终那一趟）。
        out->blocks.clear();
        out->drops = DropStats{};
        out->soft_call = SoftStats{};
        out->soft_store = SoftStats{};
        out->soft_data = SoftStats{};
        out->soft_jumptab = SoftStats{};
        out->soft_item = SoftStats{};
        SoftAccounts accounts;
        accounts.call = &out->soft_call;
        accounts.store = &out->soft_store;
        accounts.data = &out->soft_data;
        accounts.jumptab = &out->soft_jumptab;
        accounts.item = &out->soft_item;
        out->boundaries.clear();

        Explorer explorer(decoder, mem, exec_ranges, layout.shim_page, shim_end, opt.close_calls,
                          opt.close_stores, opt.close_data, opt.close_jmptab,
                          opt.close_jmptab_items, accounts);
        explorer.setDataPointerSeeds(data_scan.targets, data_scan.sites, data_scan.in_exec);
        explorer.setJumpTableSeeds(jumptab_scan.targets, jumptab_scan.sites, jumptab_scan.in_exec);
        explorer.setInstructionImmediateSeeds(imm32_scan.targets, imm32_scan.push_sites,
                                              imm32_scan.push_in_exec, imm32_scan.mov_sites,
                                              imm32_scan.mov_in_exec);
        if (opt.jmptab_exclude) explorer.setDataRanges(ranges);
        if (opt.close_jmptab_items) explorer.setJumpTableItems(items);
        if (!buildUnits(roots, &explorer, accounts, &out->blocks, &out->drops, err, err_ea)) {
            *err_code = 4;
            return false;
        }
        // ---- 读数（最终一趟的；每趟都覆盖，⛔ 不累加）----
        out->boundaries = explorer.boundaries();
        out->store_imm_sites = explorer.storeImmSites();
        out->store_imm_in_exec = explorer.storeImmInExec();
        out->data_ptr_sites = explorer.dataPtrSites();
        out->data_ptr_in_exec = explorer.dataPtrInExec();
        out->data_ptr_distinct = explorer.dataPtrDistinct();
        out->data_ptr_skipped = explorer.dataPtrSkipped();
        out->data_ptr_overlap_pre = explorer.dataPtrOverlapPre();
        out->jumptab_surface = explorer.jumpTabSurface();
        out->jumptab_rounds = explorer.jumpTabRounds();
        out->jumptab_live_roots = explorer.liveJumpTabRoots();
        out->imm32_sites = explorer.imm32PushSites() + explorer.imm32MovSites();
        out->imm32_in_exec = explorer.imm32PushInExec() + explorer.imm32MovInExec();
        out->imm32_push_sites = explorer.imm32PushSites();
        out->imm32_mov_sites = explorer.imm32MovSites();
        out->imm32_distinct = explorer.imm32Distinct();
        out->data_range_leaders = explorer.dataRangeLeaders();
        out->data_range_root_hits = explorer.dataRangeRootHits();
        out->call_ret_in_data_range = explorer.callRetInDataRange();
        out->table_range_leaders = explorer.tableRangeLeaders();
        out->item_seeds = explorer.jumpTableItemSeeds();
        out->item_already = explorer.jumpTableItemAlreadyEntry();
        out->item_shim = explorer.jumpTableItemShim();
        out->item_outside = explorer.jumpTableItemOutside();
        out->item_in_range = explorer.jumpTableItemInRange();

        bool converged = true;
        if (out->jmptab_on) {
            // ---- S1：在**这一趟的解码流**上识别（⑤ 的保护集 = 根 ∪ 本趟 D14 收下的 call 目标）----
            std::set<std::uint32_t> protected_eas = roots;
            protected_eas.insert(out->soft_call.added.begin(), out->soft_call.added.end());
            out->jmptab = recognizeJumpTables(explorer.insns(), image, exec_ranges, decoder, mem,
                                              protected_eas);
            // ---- 收敛判据（硬红）----
            std::set<std::pair<std::uint32_t, std::uint32_t>> keys;
            for (const auto& kv : out->jmptab.tables) keys.emplace(kv.first, kv.second.n);
            if (pass == 1 && keys.empty()) {
                // 一张都没认出来 ⇒ 区间 / 表项都是空，下一趟必然同结果（等价于不动点）。
                converged = true;
            } else if (pass >= 2) {
                if (!std::includes(prev_keys.begin(), prev_keys.end(), keys.begin(), keys.end())) {
                    std::string bad;
                    for (const auto& k : keys) {
                        if (prev_keys.count(k) == 0) {
                            if (!bad.empty()) bad += " ";
                            bad += hexEa(k.first) + "(n=" + std::to_string(k.second) + ")";
                        }
                    }
                    if (err != nullptr)
                        *err = "jump-table recognition surface is not monotone (tables_pass is not a subset of tables_{pass-1}): new/lengthened " + bad +
                               " => the recogniser or table ranges are wrong";
                    *err_code = 1;
                    if (err_ea != nullptr && !keys.empty()) *err_ea = keys.begin()->first;
                    return false;
                }
                converged = (keys == prev_keys);  // 不动点
            } else {
                converged = false;
            }
            if (!converged) {
                if (pass >= opt.jmptab_max_passes) {
                    if (err != nullptr)
                        *err = "the two-pass jump-table recognition did not converge within " + std::to_string(opt.jmptab_max_passes) +
                               " passes => generator internal inconsistency";
                    *err_code = 1;
                    return false;
                }
                prev_keys = keys;
                ranges = out->jmptab.ranges();
                items = out->jmptab.items();
                continue;
            }
        }
        // ---- ③ 分区 + D21：用**这一趟的** Explorer（`dropEntry` 要落在同一份状态上）----
        if (!partitionComponents(roots, &explorer, out->blocks, &out->drops, image, &out->part, err)) {
            *err_code = 1;
            return false;
        }
        return true;
    }
}

// =====================================================================================
// ★ BUG-24 / P0：`--jmptab-audit`（**只读**：不发射 IR、不写 `.ll` / 派发表 / pin）
//
// 逐张表一行 `[jmptab]`（字段与 design-prep §5.1 同一列）+ 一行 `[jmptab/summary]`；
// 被拒的候选 / 解码失败的表项 / 不过的性质都**响亮**打到 stderr（失败必须能只靠日志定位到 EA）。
//
// 退出码 = P-1..P-4 的合取（**全过 0 / 任一不过 1**；CTest 直接拿它当判据）：
//   P-1（结构）`[ea, ea+4n)` ⊆ **同一个可执行节**；
//   P-2（结构）`[ea, ea+4n)` 内**不存在任何已发射块的首 EA**（闭区间 ⇒ 表首本身也不许是块首）；
//   P-3（覆盖）每个表项 v：若 `in_exec(v) ∧ decodes(v)` ⇒ `v ∈ dispatch`；
//   P-4（识别）`items_selfref == 0` ∧ `in_section == 1`。
//   ★ P-3 必须**收窄**：`decode()` 失败的项不进判据（否则守卫会被 0.2% 量级的不可解码项吊死），
//     但必须**响亮列出**（`[jmptab/bad-item]`）—— ⛔ 不许静默跳过。
// =====================================================================================
const char* boundaryName(LeaderBoundary b) {
    switch (b) {
        case LeaderBoundary::Root: return "root";
        case LeaderBoundary::CallRet: return "call-ret";
        case LeaderBoundary::Call: return "call";
        case LeaderBoundary::Jcc: return "jcc";
        case LeaderBoundary::Jmp: return "jmp";
        case LeaderBoundary::DataPtr: return "data-ptr";
        case LeaderBoundary::StoreImm: return "store-imm";
        case LeaderBoundary::JumpTabItem: return "jumptab-item";
        default: return "none";
    }
}

int auditJumpTables(const BuildRun& run, const lift::PeImage& image,
                    const std::vector<lift::aot::ExecRange>& exec_ranges,
                    const lift::Decoder& decoder, const lift::GuestMemory& mem,
                    const std::set<std::uint32_t>& roots) {
    (void)image;
    const BlockSpans emitted = buildBlockSpans(run.blocks);
    std::set<std::uint32_t> block_starts;
    for (const auto& kv : run.blocks) block_starts.insert(kv.first);

    std::uint64_t p1_bad = 0, p2_bad = 0, p3_bad = 0, p4_bad = 0;
    std::uint64_t item_rows = 0, item_distinct = 0, items_in_exec = 0, items_decode = 0;
    std::uint64_t items_bad = 0, items_selfref = 0, items_nondispatch = 0, items_overlap = 0;
    std::uint64_t block_starts_in_range_total = 0;
    std::uint64_t src_a = 0, src_b = 0;
    std::set<std::uint32_t> distinct_items;
    // ★ ② 的响亮化先跑一遍：在可执行节内、但 `decode()` 失败的表项（`items_undet` 的 C++ 口径）。
    for (const auto& kv : run.jmptab.tables) {
        const JmpTab& t = kv.second;
        for (const std::uint32_t v : t.items) {
            if (lift::aot::execRangeOf(exec_ranges, v) == nullptr) continue;
            lift::Insn probe;
            std::string why;
            if (decoder.decode(mem, v, &probe, &why)) continue;
            std::fprintf(stderr, "[jmptab/bad-item] ea=0x%08X item=0x%08X why=%s\n", t.ea, v,
                         why.c_str());
        }
    }
    for (const auto& kv : run.jmptab.tables) {
        const JmpTab& t = kv.second;
        ++(t.src_b ? src_b : src_a);
        std::size_t bsir = 0;
        for (auto it = block_starts.lower_bound(t.ea);
             it != block_starts.end() && *it < t.range_end(); ++it) {
            ++bsir;
        }
        std::size_t overlap = 0, nondispatch = 0, missing = 0;
        std::vector<std::uint32_t> missing_sample;
        for (const std::uint32_t v : t.items) {
            ++item_rows;
            distinct_items.insert(v);
            if (lift::aot::execRangeOf(exec_ranges, v) != nullptr) ++items_in_exec;
            if (emitted.inside(v)) ++overlap;
            if (run.part.dispatch_entries.count(v) == 0) {
                ++nondispatch;
                // P-3 的判据面：只对"在可执行节内 ∧ 能解码"的项要求可派发。
                lift::Insn probe;
                std::string why;
                if (lift::aot::execRangeOf(exec_ranges, v) != nullptr &&
                    decoder.decode(mem, v, &probe, &why)) {
                    ++missing;
                    if (missing_sample.size() < 3) missing_sample.push_back(v);
                }
            }
        }
        items_decode += t.items_decode;
        items_bad += t.items_bad;
        items_selfref += t.items_selfref;
        items_nondispatch += nondispatch;
        items_overlap += overlap;
        block_starts_in_range_total += bsir;

        const bool p1 = t.v1;
        const bool p2 = (bsir == 0);
        const bool p3 = (missing == 0);
        const bool p4 = (t.items_selfref == 0 && t.v1);
        if (!p1) ++p1_bad;
        if (!p2) ++p2_bad;
        if (!p3) ++p3_bad;
        if (!p4) ++p4_bad;

        // ---- 逐张表一行（§5.1 的字段）----
        const auto bit = run.boundaries.find(t.ea);
        const std::string cmp_str = t.src_b ? "-" : hexEa(t.cmp_ea);
        const std::string ja_str = t.src_b ? "-" : hexEa(t.ja_ea);
        if (!quiet()) std::printf("[jmptab] ea=0x%08X n=%u src=%s cmp=%s ja=%s site=0x%08X "
                    "range=[0x%08X,0x%08X) in_section=%d items=%zu items_in_exec=%u "
                    "items_decode=%u items_undet=%u items_overlap=%zu items_nondispatch=%zu "
                    "items_selfref=%u block_starts_in_range=%zu v1=%d v2=%d v3=%d v4=%d "
                    "boundary=%s\n",
                    t.ea, t.n, t.src_b ? "B" : "A", cmp_str.c_str(), ja_str.c_str(), t.site, t.ea,
                    t.range_end(), t.v1 ? 1 : 0, t.items.size(), t.items_in_exec, t.items_decode,
                    t.items_bad, overlap, nondispatch, t.items_selfref, bsir, t.v1 ? 1 : 0,
                    t.v2 ? 1 : 0, t.v3 ? 1 : 0, t.v4 ? 1 : 0,
                    boundaryName(bit == run.boundaries.end() ? LeaderBoundary::None : bit->second));
        // ---- 不过的性质逐条打到 stderr（必须能只靠日志定位到 EA）----
        if (!p1) {
            std::fprintf(stderr, "[jmptab/P-1] ea=0x%08X range=[0x%08X,0x%08X) extends outside the executable section\n", t.ea,
                         t.ea, t.range_end());
        }
        if (!p2) {
            for (auto it = block_starts.lower_bound(t.ea);
                 it != block_starts.end() && *it < t.range_end(); ++it) {
                const auto bnd = run.boundaries.find(*it);
                std::fprintf(stderr,
                             "[jmptab/P-2] ea=0x%08X table range [0x%08X,0x%08X) contains block start 0x%08X "
                             "boundary=%s\n",
                             t.ea, t.ea, t.range_end(), *it,
                             boundaryName(bnd == run.boundaries.end() ? LeaderBoundary::None
                                                                     : bnd->second));
            }
        }
        if (!p3) {
            std::fprintf(stderr, "[jmptab/P-3] ea=0x%08X has %zu table items inside the executable section that decode but cannot be dispatched:",
                         t.ea, missing);
            for (const std::uint32_t v : missing_sample) std::fprintf(stderr, " 0x%08X", v);
            std::fprintf(stderr, "%s\n", missing > missing_sample.size() ? " ..." : "");
        }
        if (!p4) {
            std::fprintf(stderr, "[jmptab/P-4] ea=0x%08X items_selfref=%u in_section=%d\n", t.ea,
                         t.items_selfref, t.v1 ? 1 : 0);
        }
    }
    item_distinct = distinct_items.size();
    // ---- 被拒的候选（响亮；`[jmptab/reject]` 每张一行）----
    for (const JmpTabReject& r : run.jmptab.rejects) {
        std::fprintf(stderr, "[jmptab/reject] ea=0x%08X site=0x%08X n=%u src=%s why=%s\n", r.ea,
                     r.site, r.n, r.src_b ? "B" : "A", r.why.c_str());
    }
    // ---- 汇总（靠它做对账；⚠ 这些数是**读数**，⛔ 不是断言值）----
    if (!quiet()) std::printf(
        "[jmptab/summary] candidates=%zu tables=%zu rejected=%zu covers_protected=%llu srcA=%llu "
        "srcB=%llu items=%llu distinct=%llu items_in_exec=%llu items_decode=%llu items_undet=%llu "
        "items_selfref=%llu items_nondispatch=%llu items_overlap=%llu "
        "block_starts_in_range=%llu passes=%u jmptab_on=%d roots=%zu\n",
        run.jmptab.candidates.size(), run.jmptab.tables.size(), run.jmptab.rejects.size(),
        static_cast<unsigned long long>(run.jmptab.rejected_covers_protected),
        static_cast<unsigned long long>(src_a), static_cast<unsigned long long>(src_b),
        static_cast<unsigned long long>(item_rows), static_cast<unsigned long long>(item_distinct),
        static_cast<unsigned long long>(items_in_exec),
        static_cast<unsigned long long>(items_decode), static_cast<unsigned long long>(items_bad),
        static_cast<unsigned long long>(items_selfref),
        static_cast<unsigned long long>(items_nondispatch),
        static_cast<unsigned long long>(items_overlap),
        static_cast<unsigned long long>(block_starts_in_range_total), run.passes,
        run.jmptab_on ? 1 : 0, roots.size());
    // ---- P1 / P2 的读数（与"改前/改后"的对照直接相关）----
    if (!quiet()) std::printf(
        "[jmptab/exclude] leaders_in_range=%llu (leaders blocked by a range) - root_hits=%llu (should be 0) - "
        "call_ret_in_range=%llu (the shape where a noreturn call is adjacent to the table start)\n",
        static_cast<unsigned long long>(run.data_range_leaders),
        static_cast<unsigned long long>(run.data_range_root_hits),
        static_cast<unsigned long long>(run.call_ret_in_data_range));
    if (!quiet()) std::printf(
        "[jmptab/items] seeds=%llu added=%zu already=%llu shim=%llu outside=%llu in_range=%llu - "
        "undecodable=%zu overlap=%zu\n",
        static_cast<unsigned long long>(run.item_seeds), run.soft_item.added.size(),
        static_cast<unsigned long long>(run.item_already),
        static_cast<unsigned long long>(run.item_shim),
        static_cast<unsigned long long>(run.item_outside),
        static_cast<unsigned long long>(run.item_in_range), run.soft_item.undecodable.size(),
        run.soft_item.overlap.size());
    // ---- 守卫判据（P-1..P-4）----
    const bool ok = (p1_bad == 0 && p2_bad == 0 && p3_bad == 0 && p4_bad == 0);
    if (!quiet()) std::printf(
        "[jmptab/guard] P-1 range inside executable section=%llu bad - P-2 no block start inside range=%llu bad - "
        "P-3 table items dispatchable=%llu bad - P-4 recognition self-consistent=%llu bad => %s\n",
        static_cast<unsigned long long>(p1_bad), static_cast<unsigned long long>(p2_bad),
        static_cast<unsigned long long>(p3_bad), static_cast<unsigned long long>(p4_bad),
        ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    return ok ? 0 : 1;
}


// =====================================================================================
// ⑤ `--check <closure.txt>`：入库件的**红判据**（供 CI 与手工复核；不发射 IR）
// ★ 判据（父级 D5 / T1 §4 候选 A）：
//   1. 格式：非空行必须是 `0x` + **恰好 8 位**十六进制（`#` 起注释；⛔ 不认 6 位 / 无前缀）；
//   2. 升序：严格递增（重复 = 红。排序是为了 diff 稳定，不是语义）；
//   3. 可解码：`Decoder::decode` 在该 EA 上成功（失败把 `why` 打出来）；
//   4. 落在 `.text`：在**可执行节**内（判据在 `lift_aot_text.h`，与 closure 生成器同一份）；
//   5. `entry` 必在（起跑线口径）。
// ★ 汇总**一行**：根数 / 覆盖块数 / 越界根 / 不可解码根 —— 前两项是读数，后两项是红判据的量化。
// ★ 退出码：0 = 全绿；2 = 任一判据红（与"参数/输入错"同一档：这是**输入文件**的问题，不是 guest 代码）。
// =====================================================================================
// ★ 入库件五条硬闸的**唯一实现**（`--check` 独立模式与 `--closure-gate`（生成那一趟）共用）。
//   拆成"解析 + 文法/升序"（只要文件）与"逐根在可执行节 + 可解码 + entry 必在"（要已加载的
//   PE / 解码器）两步：生成那一趟两样都早就有 ⇒ 不必第二遍加载 PE、也不必第二遍 `buildAll`。
//   ★ 五条判据一条不少：① 格式 ② 严格升序 ③ 可解码 ④ 在可执行节 ⑤ `entry` 必在。
struct ClosureGate {
    std::vector<std::uint32_t> eas;  // 文件里的 EA（重复/乱序也原样留着：读数要如实）
    std::size_t format_bad = 0;
    std::size_t order_bad = 0;
    std::size_t out_of_text = 0;
    std::size_t undecodable = 0;
    std::vector<std::string> violations;
};

// ① + ②：文件可读、非空、每行 `0x` + 恰好 8 位十六进制、严格递增。false = 文件都读不了（退出码 2）。
bool parseClosureFile(const std::string& path, ClosureGate* gate) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "emulator-lift-gen: --check cannot open `%s`\n", path.c_str());
        return false;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const std::string token = lift::aot::stripCommentAndTrim(line);
        if (token.empty()) continue;
        std::uint32_t ea = 0;
        if (!lift::aot::strictParseEa(token, &ea)) {
            ++gate->format_bad;
            gate->violations.push_back("line " + std::to_string(lineno) + " is not `0x` + 8 hex digits: `" +
                                       token + "`");
            continue;
        }
        if (!gate->eas.empty() && ea <= gate->eas.back()) {
            ++gate->order_bad;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "line %d is not strictly ascending (%08X comes after %08X / duplicate)", lineno, ea,
                          gate->eas.back());
            gate->violations.push_back(buf);
        }
        gate->eas.push_back(ea);
    }
    if (gate->eas.empty()) {
        std::fprintf(stderr, "emulator-lift-gen: --check `%s` contains no EA\n", path.c_str());
        return false;
    }
    return true;
}

// ③ + ④ + ⑤：逐根"在可执行节 + 可解码"，外加 `entry` 必在。
void checkClosureRoots(const lift::PeImage& image, const std::vector<lift::aot::ExecRange>& ranges,
                       const lift::GuestMemory& mem, const lift::Decoder& decoder,
                       const std::set<std::uint32_t>& roots, ClosureGate* gate) {
    for (const std::uint32_t ea : roots) {
        if (lift::aot::execRangeOf(ranges, ea) == nullptr) {
            ++gate->out_of_text;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "EA %08X is not in any executable section (out-of-range root)", ea);
            gate->violations.push_back(buf);
            continue;
        }
        lift::Insn in;
        std::string why;
        if (!decoder.decode(mem, ea, &in, &why)) {
            ++gate->undecodable;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "EA %08X does not decode: %s", ea, why.c_str());
            gate->violations.push_back(buf);
        }
    }
    if (roots.count(image.entry_ea) == 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "PE entry %08X is not in the closure (starting-line invariant)", image.entry_ea);
        gate->violations.push_back(buf);
    }
}

// 汇总**一行**：措辞 / 字段顺序**逐字保留**（`--check` 与 `--closure-gate` 打的是同一行）。
void printClosureSummary(const ClosureGate& gate, const std::set<std::uint32_t>& roots,
                         const lift::PeImage& image, std::size_t soft_call, std::size_t soft_store,
                         std::size_t soft_data, std::size_t soft_jumptab, std::size_t soft_item,
                         std::size_t blocks, std::size_t tables, unsigned passes, bool expand_ok) {
    const bool pass = gate.violations.empty() && expand_ok;
    std::printf(
        "lift-gen --check: roots=%zu soft-roots=%zu(call=%zu store-imm=%zu data-ptr=%zu jmptab=%zu "
        "jmptab-item=%zu) blocks=%zu tables=%zu passes=%u out-of-text=%zu undecodable=%zu order=%s "
        "entry=%s => %s\n",
        roots.size(), soft_call + soft_store + soft_data + soft_jumptab + soft_item, soft_call,
        soft_store, soft_data, soft_jumptab, soft_item, blocks, tables, passes, gate.out_of_text,
        gate.undecodable, gate.order_bad == 0 ? "ok" : "bad",
        roots.count(image.entry_ea) == 0 ? "missing" : "present", pass ? "PASS" : "FAIL");
}

int checkClosure(const std::string& exe, const std::string& path, bool close_calls, bool close_stores,
                 bool close_data, bool close_jmptab, bool jmptab_exclude,
                 bool close_jmptab_items, unsigned long jmptab_max_passes) {
    ClosureGate gate;
    if (!parseClosureFile(path, &gate)) return 2;
    const std::set<std::uint32_t> roots(gate.eas.begin(), gate.eas.end());

    // ---- PE 字节（与主路径同一套：mem.reserve → loadPeFile → mapPeImage → Decoder）----
    lift::GuestMemory mem;
    if (!mem.reserve()) {
        std::fprintf(stderr, "emulator-lift-gen: failed to reserve guest address space: %s\n", mem.lastError());
        return 2;
    }
    lift::PeImage image;
    {
        std::string err;
        if (!lift::loadPeFile(exe, &image, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: failed to load PE (%s): %s\n", exe.c_str(), err.c_str());
            return 2;
        }
        if (!lift::mapPeImage(mem, image, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: failed to map PE (%s): %s\n", exe.c_str(), err.c_str());
            return 2;
        }
    }
    lift::Decoder decoder;
    if (!decoder.ok()) {
        std::fprintf(stderr, "emulator-lift-gen: decoder initialisation failed: %s\n", decoder.lastError());
        return 4;
    }
    const std::vector<lift::aot::ExecRange> ranges = lift::aot::execRanges(image);

    // ---- 逐根：在可执行节 + 可解码 + `entry` 必在（唯一实现在 `checkClosureRoots`）----
    checkClosureRoots(image, ranges, mem, decoder, roots, &gate);

    // ---- 覆盖块数：静态可达性用主路径**同一份** `buildAll`（⛔ 不另写一份展开规则）----
    // ★ T1c：派生 leader 的丢弃规则也走**同一份**（`buildUnits`）⇒ `blocks` 读数与 `lift-gen`
    //   的 `blocks=` 严格一致（D10 P3 的账差收口就靠这条）。
    // ★ BUG-24：两趟重启 / S1 / 分区 / D21 也**只走 `buildAll`**（与主路径逐字节同一条路径）。
    std::size_t covered_blocks = 0;
    std::size_t covered_soft_roots_call = 0;
    std::size_t covered_soft_roots_store = 0;
    std::size_t covered_soft_roots_data = 0;
    std::size_t covered_soft_roots_jumptab = 0;
    std::size_t covered_soft_roots_item = 0;
    std::size_t covered_tables = 0;
    unsigned covered_passes = 0;
    bool expand_ok = false;
    if (gate.violations.empty()) {
        Options expand_opt;  // 只用到这几项；其余取缺省（与主路径同一套缺省值）
        expand_opt.close_calls = close_calls;
        expand_opt.close_stores = close_stores;
        expand_opt.close_data = close_data;
        expand_opt.close_jmptab = close_jmptab;
        expand_opt.jmptab_exclude = jmptab_exclude;
        expand_opt.close_jmptab_items = close_jmptab_items;
        expand_opt.jmptab_max_passes = jmptab_max_passes;
        BuildRun run;
        std::string err;
        std::uint32_t err_ea = 0;
        int err_code = 0;
        if (buildAll(roots, image, ranges, decoder, mem, expand_opt, &run, &err, &err_ea,
                     &err_code)) {
            covered_blocks = run.blocks.size();
            covered_soft_roots_call = run.soft_call.added.size();
            covered_soft_roots_store = run.soft_store.added.size();
            covered_soft_roots_data = run.soft_data.added.size();
            covered_soft_roots_jumptab = run.soft_jumptab.added.size();
            covered_soft_roots_item = run.soft_item.added.size();
            covered_tables = run.jmptab.tables.size();
            covered_passes = run.passes;
            expand_ok = true;
        } else {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "static expansion %s @ %08X: %s",
                          err_code == 4 ? "decoding failed" : "convergence/partition failed", err_ea, err.c_str());
            gate.violations.push_back(buf);
        }
    }

    for (const std::string& v : gate.violations)
        std::fprintf(stderr, "emulator-lift-gen --check: %s\n", v.c_str());
    // ★ D15 / D17 / D26 / P2：五类软根**分开报**（`soft-roots` 仍是并集，后面括号里是拆分）。
    printClosureSummary(gate, roots, image, covered_soft_roots_call, covered_soft_roots_store,
                        covered_soft_roots_data, covered_soft_roots_jumptab, covered_soft_roots_item,
                        covered_blocks, covered_tables, covered_passes, expand_ok);
    return (gate.violations.empty() && expand_ok) ? 0 : 2;
}

// =====================================================================================
// ④ 分区：分量级直跳 + 冲突提升到收敛（实现在 main 内联，便于共用 Explorer 的 entries）
// =====================================================================================

// =====================================================================================
// ⑤ `--pin` / `--table`：AOT 启动要的两份**机械**数据（T2b / D1）
//
// ★ 形态选择与理由：
//   · `lift_pin.h` = 生成物自己的 `#define`。D1 要求"启动先核对再 bring-up"⇒ 拿到的必须是
//     **构建期那份** PE 的 sha256，而不是运行期再算一次（那就成了自证）。宏是零成本的常量，
//     AOT 侧 `#include` 即可，不需要读文件、也不需要第二条解析路径。
//   · `lift_table.inc` = `.inc` 片段（不是 `.cpp`，也不是 IR 全局表）：
//     ① IR 全局表要求 AOT 用 `.ll` 里的符号布局**再写一遍**（多一份偏移假设，且表在 .rodata 里
//        拿不到 `LiftFn` 的类型检查）⇒ 表在 C++ 侧才有"函数指针类型不匹配 = 编译错"这条判据；
//     ② `.cpp` 会让"声明 8927 个 `extern "C"`"与"表"分散在两个文件里，改一处忘一处；
//        同一个 `.inc` 用**两个不同的宏**展开两次（声明 + 表）⇒ 声明与表在结构上不可能漂。
//   · 确定性：EA 升序、名字由 EA 派生、**无时间戳 / 无路径 / 无计数以外的变量** ⇒ 同输入同字节。
// =====================================================================================
// 8 位大写 hex（与 `lift_gen_emit.cpp` 的 `hexEa` 同形；那份是内部链接，本 TU 不能复用）
std::string hexEa(std::uint32_t ea) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", ea);
    return buf;
}

bool writeAotPin(const std::string& path, const lift::PeImage& image, std::size_t roots,
                 std::size_t entries, std::size_t functions,
                 const std::vector<IntrinsicEntry>& intrinsics, std::string* err) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (err) *err = "cannot write `" + path + "`";
        return false;
    }
    file << "// Generated by emulator-lift-gen --pin - do not edit by hand, not tracked in git.\n";
    file << "//\n";
    file << "// This header is the D1 pin: lifted code bakes EA / block boundaries / operand semantics to this PE at **build time**;\n";
    file << "//   a different exe does not \"run another game\", it silently runs wrong => the AOT startup checks sha256 + entry,\n";
    file << "//   and a mismatch is exit 2 (no downgrade, no warning-and-continue).\n";
    file << "// Only mechanical facts: no timestamp, no path, no hostname => the same PE generates the same bytes twice.\n";
    file << "#pragma once\n\n";
    file << "#define LIFT_AOT_GENERATED 1\n";
    file << "#define LIFT_AOT_PIN_SHA256 \"" << image.sha256 << "\"\n";
    file << "#define LIFT_AOT_PIN_ENTRY 0x" << hexEa(image.entry_ea) << "u\n";
    file << "#define LIFT_AOT_PIN_ROOTS " << roots << "u\n";
    file << "#define LIFT_AOT_PIN_ENTRIES " << entries << "u\n";
    file << "#define LIFT_AOT_PIN_FUNCTIONS " << functions << "u\n";
    // ★ 构建期库内建表（`aot/intrinsics.txt` 的**已校验**内容）：AOT 启动 readout 要知道
    //   "这一趟到底带了哪几个内建 EA"，而不必在运行期再读一遍入库件（那是第二条解析路径）。
    //   形态 = X-macro 列表：`aot_dispatch.cpp` 用它展开成 `{EA, 名字}` 常量数组。
    file << "#define LIFT_AOT_PIN_INTRINSICS " << intrinsics.size() << "u\n";
    if (!intrinsics.empty()) {
        file << "#define LIFT_AOT_PIN_INTRINSIC_LIST(X) \\\n";
        for (std::size_t i = 0; i < intrinsics.size(); ++i) {
            file << "    X(0x" << hexEa(intrinsics[i].ea) << "u, \"" << intrinsics[i].builtin << "\")";
            file << (i + 1 == intrinsics.size() ? "\n" : " \\\n");
        }
    }
    file.close();
    if (!file) {
        if (err) *err = "write to `" + path + "` failed";
        return false;
    }
    return true;
}

bool writeAotTable(const std::string& path, const std::vector<GenComponent>& components,
                   std::string* err) {
    // ★ 表里**只许有真实存在的入口**：一行 = 一个 `comp.entries` 里的 EA（那些 EA 在对应函数体的
    //   `switch i32 %ea` 里一定有 label）。不在表里的 EA ⇒ AOT 响亮停机（D10 P4：不设返回哨兵约定）。
    //   分量之间的 entries 按分区定义互不相交 ⇒ 重复 = 生成器内部不一致 ⇒ 拒绝交付（⛔ 不合并去重，
    //   那会把"同一 EA 被两个函数声明"变成静默）。
    std::map<std::uint32_t, std::uint32_t> ea_to_root;
    for (const GenComponent& comp : components) {
        for (const std::uint32_t ea : comp.entries) {
            const auto ins = ea_to_root.emplace(ea, comp.root);
            if (!ins.second && ins.first->second != comp.root) {
                if (err) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "entry EA %08X belongs to two components at once (%08X / %08X)",
                                  ea, ins.first->second, comp.root);
                    *err = buf;
                }
                return false;
            }
        }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (err) *err = "cannot write `" + path + "`";
        return false;
    }
    file << "// Generated by emulator-lift-gen --table - do not edit by hand, not tracked in git.\n";
    file << "// One line per **entry EA**: `LIFT_AOT_ENTRY(0x<EA>u, lift_cc_<root of the component containing that EA>)`.\n";
    file << "// The same function appears more than once (a component has several resume EAs) - that is not duplication, it is several switch labels.\n";
    file << "// Only real entries are allowed: every line has a label in the `switch i32 %ea` of the corresponding function body.\n";
    for (const auto& kv : ea_to_root) {
        // ★ 函数名走 `genFunctionName`（与 `.ll` 的 `define i32 @…` 是同一份拼法）。
        file << "LIFT_AOT_ENTRY(0x" << hexEa(kv.first) << "u, " << lift::aot::genFunctionName(kv.second)
             << ")\n";
    }
    file.close();
    if (!file) {
        if (err) *err = "write to `" + path + "` failed";
        return false;
    }
    return true;
}

// =====================================================================================
// ★ D18：`--dump-blocks <file>` —— **只读覆盖率读数**
//
// 用途（父级口径）：让 `.tmp/aot/sweep/` 那套脚本能直接回答"闭包还会不会漏"，而不用再从
// `.ll` 的 `bb_` 标签 + llvm 指令长度**反推**块区间（sweep 报告 §0 那条"差 0.8%"的近似）。
//
// ★ **0 派发成本**：不发射任何东西给 `.ll`、不进派发表、不改分区 ⇒ shipped 形态一个字节都不变。
// ★ 块集合 = **最终 units 的块**（= `emitModule` 实际发射的那批 = `comp.blocks`），不是"解出来过的块"：
//   被丢弃的派生 leader / 软根的块**不在**里面。
// ★ 格式（文本、确定性、升序）：
//     `# ` 起三行注释（含 `# blocks=<n>` 一行自校验），之后一行一个块：
//       `0x<8 位大写 start EA> <字节长度> <指令数>`（十进制）
//   升序 = start EA 升序。★ 每块只属于一个分量（分区保证）⇒ 不会出现两行同 start EA。
// =====================================================================================
bool writeBlockDump(const std::string& path, const std::vector<GenComponent>& components,
                    std::size_t* out_blocks, std::uint64_t* out_bytes, std::uint64_t* out_insns,
                    std::string* err) {
    struct Row {
        std::uint32_t ea = 0;
        std::uint32_t len = 0;
        std::size_t insns = 0;
    };
    std::vector<Row> rows;
    for (const GenComponent& comp : components) {
        for (const auto& kv : comp.blocks) {
            const GenBlock& b = kv.second;
            if (b.insns.empty()) continue;
            const lift::Insn& last = b.insns.back();
            rows.push_back(Row{kv.first, last.ea + last.length - kv.first, b.insns.size()});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ea < b.ea; });
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (err != nullptr) *err = "cannot write `" + path + "`";
        return false;
    }
    file << "# emulator-lift-gen --dump-blocks - read-only coverage readout (not part of .ll / dispatch table)\n";
    file << "# Format: `0x<8 uppercase hex start EA> <byte length> <instruction count>`, ascending by start EA; one line per emitted block\n";
    file << "# blocks=" << rows.size() << "\n";
    std::uint64_t bytes = 0;
    std::uint64_t insns = 0;
    for (const Row& r : rows) {
        file << "0x" << hexEa(r.ea) << " " << r.len << " " << r.insns << "\n";
        bytes += r.len;
        insns += r.insns;
    }
    file.close();
    if (!file) {
        if (err != nullptr) *err = "write to `" + path + "` failed";
        return false;
    }
    if (out_blocks != nullptr) *out_blocks = rows.size();
    if (out_bytes != nullptr) *out_bytes = bytes;
    if (out_insns != nullptr) *out_insns = insns;
    return true;
}

// =====================================================================================
// ★ `--verify-split <aot-dir>`：**分片清单自洽**（CTest `lift-aot-split-manifest` 的被测入口）
//
// 2026-11-16：这条 CTest 原来跑的是 `tests/cmake/lift_aot_split.cmake`（`COMMAND ${CMAKE_COMMAND}`）
// ⇒ 它没有 target、没有 `main(`，于是 `cmake-test-entry` 的"注册面 ↔ 源文件"判据判不了它
// （那条守卫防的是"空壳用例 ctest 照样绿"）。现在把六项断言搬进**被测二进制自己**：
//
//   1. `Σ functions == LIFT_AOT_PIN_FUNCTIONS`（pin 是启动期核对的同一份计数）
//   2. 各份 `define i32 @lift_cc_*` 的符号集合**两两不交**，并集 == `lift_table.inc` 的函数名集合
//   3. 每份 `.ll` ≤ 20 MB（`design.md` §3.1 的上界）
//   4. 份数 == `--split N`；清单里每份的 sha256 / 字节数与文件实际一致
//
// ⛔ 不断言任何"当前数字"：全部由 `out/aot/` 的产物自己算。没有生成物（缺 clang / 执行源 /
//   closure.txt）时照旧 **skip（exit 0）**，与旧的 `.cmake` 脚本口径一致（AOT 侧的"没有生成物"
//   由 `emulator-lift-aot` 启动时响亮报告并 exit 1 覆盖）。
// 退出码：0 = OK / skip；1 = 判据破了；2 = 参数或读文件错。
// =====================================================================================

std::vector<std::string> splitTextLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (const char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        if (cur.back() == '\r') cur.pop_back();
        lines.push_back(cur);
    }
    return lines;
}

std::vector<std::string> splitWs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : line) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool readTextFile(const std::filesystem::path& p, std::string* out) {
    std::ifstream file(p, std::ios::binary);
    if (!file) return false;
    out->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

/** 一份 `.ll` 里的 `define i32 @lift_cc_<hex>(` 符号名（与 `.cmake` 的 REGEX 同一判据）。 */
std::vector<std::string> liftDefineNames(const std::string& text) {
    std::vector<std::string> names;
    const std::string prefix = "define i32 @";
    for (const std::string& line : splitTextLines(text)) {
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        const std::size_t at = prefix.size();
        const std::size_t lparen = line.find('(', at);
        if (lparen == std::string::npos) continue;
        const std::string name = line.substr(at, lparen - at);
        if (name.compare(0, 8, "lift_cc_") == 0 && name.size() > 8) names.push_back(name);
    }
    return names;
}

bool parseU64(const std::string& s, std::uint64_t* out) {
    if (s.empty()) return false;
    char* endp = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &endp, 10);
    if (endp == s.c_str() || *endp != '\0') return false;
    *out = static_cast<std::uint64_t>(v);
    return true;
}

bool readPinFunctions(const std::filesystem::path& pin, std::uint64_t* out, std::string* err) {
    std::string text;
    if (!readTextFile(pin, &text)) {
        if (err != nullptr) *err = "cannot read `" + pin.string() + "`";
        return false;
    }
    const std::string key = "#define LIFT_AOT_PIN_FUNCTIONS ";
    for (const std::string& line : splitTextLines(text)) {
        if (line.compare(0, key.size(), key) != 0) continue;
        std::string v = line.substr(key.size());
        while (!v.empty() && (v.back() == 'u' || v.back() == 'U')) v.pop_back();
        if (!parseU64(v, out)) {
            if (err != nullptr) *err = "cannot parse LIFT_AOT_PIN_FUNCTIONS: `" + line + "`";
            return false;
        }
        return true;
    }
    if (err != nullptr) *err = "lift_pin.h has no LIFT_AOT_PIN_FUNCTIONS";
    return false;
}

bool readTableFunctions(const std::filesystem::path& table, std::set<std::string>* out,
                        std::string* err) {
    std::string text;
    if (!readTextFile(table, &text)) {
        if (err != nullptr) *err = "cannot read `" + table.string() + "`";
        return false;
    }
    const std::string prefix = "LIFT_AOT_ENTRY(";
    const std::string sep = ", ";
    for (const std::string& line : splitTextLines(text)) {
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        const std::size_t sep_at = line.find(sep, prefix.size());
        const std::size_t close = line.rfind(')');
        if (sep_at == std::string::npos || close == std::string::npos || close <= sep_at) continue;
        out->insert(line.substr(sep_at + sep.size(), close - (sep_at + sep.size())));
    }
    return true;
}

int verifyAotSplit(const std::string& arg, std::size_t split) {
    if (split == 0) {
        std::fprintf(stderr, "emulator-lift-gen: --verify-split requires --split >= 1\n");
        return 2;
    }
    std::error_code ec;
    std::filesystem::path dir(arg);
    if (std::filesystem::is_regular_file(dir, ec)) dir = dir.parent_path();

    const std::filesystem::path pin = dir / "lift_pin.h";
    const std::filesystem::path table = dir / "lift_table.inc";
    if (!std::filesystem::exists(pin) || !std::filesystem::exists(table)) {
        std::printf(
            "lift-gen --verify-split: skip (no lift_pin.h / lift_table.inc: this build has no AOT artifacts)\n");
        return 0;
    }
    auto fail = [](const std::string& msg) {
        std::fprintf(stderr, "emulator-lift-gen --verify-split: %s\n", msg.c_str());
        return 1;
    };

    std::uint64_t pin_functions = 0;
    std::string err;
    if (!readPinFunctions(pin, &pin_functions, &err)) return fail(err);
    std::set<std::string> table_fns;
    if (!readTableFunctions(table, &table_fns, &err)) return fail(err);
    if (table_fns.empty()) return fail("lift_table.inc contains no LIFT_AOT_ENTRY line");

    if (split == 1) {
        // ---- 单份路径（缺省回退形状）：份数必然 1，清单不应存在 ----
        const std::filesystem::path ll = dir / "lift_aot.ll";
        if (!std::filesystem::exists(ll)) return fail("split=1 but `" + ll.string() + "` is missing");
        if (std::filesystem::exists(dir / "lift_aot.split.txt")) {
            return fail("split=1 but `" + (dir / "lift_aot.split.txt").string() +
                        "` exists (stale manifest / two shapes mixed in one tree)");
        }
        std::string text;
        if (!readTextFile(ll, &text)) return fail("cannot read `" + ll.string() + "`");
        const std::vector<std::string> defs = liftDefineNames(text);
        if (defs.size() != pin_functions) {
            return fail("split=1 defines=" + std::to_string(defs.size()) +
                        " != PIN_FUNCTIONS=" + std::to_string(pin_functions));
        }
        const std::set<std::string> uniq(defs.begin(), defs.end());
        if (uniq.size() != defs.size()) return fail("duplicate `define` in the split=1 .ll");
        std::printf("lift-gen --verify-split: OK (single-O2; defines=%zu)\n", defs.size());
        return 0;
    }

    // ---- 多份路径：清单 + 每份文件 ----
    const std::filesystem::path manifest = dir / "lift_aot.split.txt";
    if (!std::filesystem::exists(manifest)) {
        return fail("LIFT_AOT_SPLIT=" + std::to_string(split) + " but `" + manifest.string() + "` is missing");
    }
    std::string manifest_text;
    if (!readTextFile(manifest, &manifest_text)) return fail("cannot read `" + manifest.string() + "`");
    std::vector<std::string> parts;
    for (const std::string& line : splitTextLines(manifest_text)) {
        if (line.compare(0, 5, "part ") == 0) parts.push_back(line);
    }
    if (parts.size() != split) {
        return fail("manifest parts=" + std::to_string(parts.size()) + " != LIFT_AOT_SPLIT=" +
                    std::to_string(split) + " (typical cause: LIFT_AOT_SPLIT changed without regenerating the manifest)");
    }

    constexpr std::uint64_t kMaxPartBytes = 20971520;  // 20 MB（design.md §3.1）
    std::uint64_t sum_functions = 0;
    std::uint64_t max_part_bytes = 0;
    std::set<std::string> seen;
    for (const std::string& line : parts) {
        const std::vector<std::string> f = splitWs(line);
        if (f.size() != 8) {
            return fail("manifest line field count=" + std::to_string(f.size()) + " (expected 8): `" + line + "`");
        }
        const std::string& idx = f[1];
        const std::filesystem::path part_path = dir / f[2];
        const std::string& want_sha = f[3];
        std::uint64_t want_bytes = 0;
        std::uint64_t want_fns = 0;
        if (!parseU64(f[4], &want_bytes) || !parseU64(f[5], &want_fns)) {
            return fail("manifest part " + idx + ": bytes/functions are not decimal integers: `" + line + "`");
        }
        if (!std::filesystem::exists(part_path)) {
            return fail("manifest part " + idx + ": file does not exist: `" + part_path.string() + "`");
        }
        std::string content;
        if (!readTextFile(part_path, &content)) {
            return fail("cannot read manifest part " + idx + ": `" + part_path.string() + "`");
        }
        const std::vector<std::uint8_t> raw(content.begin(), content.end());
        const std::string real_sha = lift::toLower(lift::toHex(lift::sha256(raw)));
        if (real_sha != lift::toLower(want_sha)) {
            return fail("part " + idx + " sha256 mismatch (manifest=" + want_sha + " actual=" + real_sha + ")");
        }
        if (content.size() != want_bytes) {
            return fail("part " + idx + " byte count mismatch (manifest=" + std::to_string(want_bytes) +
                        " actual=" + std::to_string(content.size()) + ")");
        }
        if (content.size() > kMaxPartBytes) {
            return fail("part " + idx + " is " + std::to_string(content.size()) + " B, above the 20 MB bound");
        }
        if (content.size() > max_part_bytes) max_part_bytes = content.size();

        const std::vector<std::string> defs = liftDefineNames(content);
        if (defs.size() != want_fns) {
            return fail("part " + idx + " defines=" + std::to_string(defs.size()) +
                        " != manifest functions=" + std::to_string(want_fns));
        }
        sum_functions += defs.size();
        for (const std::string& fn : defs) {
            if (!seen.insert(fn).second) {
                return fail("symbol `" + fn + "` appears in two parts (pairwise-disjoint invariant broken)");
            }
            if (table_fns.find(fn) == table_fns.end()) {
                return fail("part " + idx + " defines `" + fn + "`, but the dispatch table does not contain it");
            }
        }
    }
    if (sum_functions != pin_functions) {
        return fail("sum functions=" + std::to_string(sum_functions) +
                    " != LIFT_AOT_PIN_FUNCTIONS=" + std::to_string(pin_functions));
    }
    if (seen.size() != table_fns.size()) {
        return fail("part union=" + std::to_string(seen.size()) +
                    " != dispatch-table function set=" + std::to_string(table_fns.size()));
    }
    for (const std::string& fn : table_fns) {
        if (seen.find(fn) == seen.end()) {
            return fail("dispatch table entry `" + fn + "` is not in any part");
        }
    }
    std::printf(
        "lift-gen --verify-split: OK (LIFT_AOT_SPLIT=%zu; sum-functions=%llu; max-part=%llu B)\n", split,
        static_cast<unsigned long long>(sum_functions),
        static_cast<unsigned long long>(max_part_bytes));
    return 0;
}

// =====================================================================================
// ★ `--verify-ir-target <aot-dir> --split N --ir-target-cpu <cpu> --ir-target-features <feat>`
//   （CTest `lift-aot-ir-target-attrs` 的被测入口）
//
// 判据（**每份** `.ll`；产物名的口径与 `--verify-split` 完全一致：N=1 ⇒ `<dir>/lift_aot.ll`，
//   N>1 ⇒ `<dir>/lift_aot.partNN.ll`）：
//   ① 恰好**一行**以 `attributes #0 = {` 开头，且其中两个值**逐字**等于参数；
//   ② 每个以 `define i32 @lift_cc_` 开头的行都以 ` #0 {` 结尾。
//
// 为什么这两条是判据（⛔ 不是风格检查）：
//   LLVM 的 LTO 内联器 `areInlineCompatible()` 比的是**函数属性**（callee 的 feature 必须是 caller
//   的子集）。运行库 TU 由 C++ 前端编译 ⇒ 每个函数都带 `"target-cpu"` / `"target-features"`；
//   生成物若不带（或只带一半），两边就判"不相容" ⇒ `lift_tick` / `lift_note_write` **一条都不内联**，
//   而**没有任何东西会喊**（构建成功、产物正常、运行结果也一样，只有站点数与墙钟静默不变）。
//   ⇒ 这条用例红了意味着：LTO 一旦打开也买不到任何东西，而你以为买到了。
//
// ★ 没有生成物（缺 clang / 执行源 / closure.txt ⇒ 没有 `lift_pin.h` / `lift_table.inc`）⇒ **skip、exit 0**：
//   与 `--verify-split` 同一条"没有生成物不算红"（AOT 侧"没有生成物"由 `emulator-lift-aot` 启动时
//   响亮报告并 exit 1 覆盖）。
// 退出码：0 = OK / skip；1 = 判据破了；2 = 参数或读文件错。
// =====================================================================================
int verifyAotIrTarget(const std::string& arg, std::size_t split, const GenIrTarget& want) {
    if (split == 0) {
        std::fprintf(stderr, "emulator-lift-gen: --verify-ir-target requires --split >= 1\n");
        return 2;
    }
    std::error_code ec;
    std::filesystem::path dir(arg);
    if (std::filesystem::is_regular_file(dir, ec)) dir = dir.parent_path();
    // ★ 与 `--verify-split` **同一条** skip 口径："本次构建没有 AOT 生成物"。
    if (!std::filesystem::exists(dir / "lift_pin.h") ||
        !std::filesystem::exists(dir / "lift_table.inc")) {
        std::printf(
            "lift-gen --verify-ir-target: skip (no lift_pin.h / lift_table.inc: "
            "this build has no AOT artifacts)\n");
        return 0;
    }
    // ★ 有产物却没有期望值 = CMake 那侧的接线掉了（探针没跑 / 参数没传）。
    //   ⛔ 不许当 skip —— 那正是本用例要防的静默失效（属性整条消失而门禁照样绿）。
    if (want.empty()) {
        std::fprintf(stderr,
                     "emulator-lift-gen --verify-ir-target: expectation is empty (cpu=`%s`; features=%zu chars)"
                     " => CMake did not pass the configure-time probe values `--ir-target-cpu/--ir-target-features`;"
                     " that makes `attributes #0` and the ` #0 {` on define lines disappear together => LTO's "
                     "areInlineCompatible judges incompatibility, so lift_tick / lift_note_write are silently not inlined\n",
                     want.cpu.c_str(), want.features.size());
        return 2;
    }
    auto fail = [](const std::string& msg) {
        std::fprintf(stderr, "emulator-lift-gen --verify-ir-target: %s\n", msg.c_str());
        return 1;
    };
    const std::string attrs_prefix = "attributes #0 = {";
    const std::string cpu_pair = "\"target-cpu\"=\"" + want.cpu + "\"";
    const std::string feat_pair = "\"target-features\"=\"" + want.features + "\"";
    const std::string define_prefix = "define i32 @lift_cc_";
    const std::string define_tail = " #0 {";
    std::uint64_t defines = 0;
    for (std::size_t i = 0; i < split; ++i) {
        std::string name;
        if (split == 1) {
            name = "lift_aot.ll";
        } else {
            char b[64];
            std::snprintf(b, sizeof(b), "lift_aot.part%02zu.ll", i);
            name = b;
        }
        const std::filesystem::path p = dir / name;
        if (!std::filesystem::exists(p)) {
            return fail("part " + std::to_string(i) + " `" + p.string() +
                        "` does not exist (pin / dispatch table are present but this part is not => stale artifact shape: "
                        "LIFT_AOT_SPLIT changed without regenerating the `.ll` files)");
        }
        std::string text;
        if (!readTextFile(p, &text)) {
            return fail("cannot read part " + std::to_string(i) + " `" + p.string() + "`");
        }
        std::size_t attrs_lines = 0;
        std::string attrs_line;
        std::uint64_t part_defines = 0;
        for (const std::string& line : splitTextLines(text)) {
            if (line.compare(0, attrs_prefix.size(), attrs_prefix) == 0) {
                ++attrs_lines;
                attrs_line = line;
            }
            if (line.compare(0, define_prefix.size(), define_prefix) != 0) continue;
            ++part_defines;
            if (line.size() < define_tail.size() ||
                line.compare(line.size() - define_tail.size(), define_tail.size(), define_tail) !=
                    0) {
                const std::size_t lparen = line.find('(');
                const std::string fn = line.substr(0, lparen);
                const std::string tail =
                    line.size() > 48 ? "..." + line.substr(line.size() - 48) : line;
                return fail("part " + std::to_string(i) + " `" + name + "`: define line of `" + fn +
                            "` ends with `" + tail + "` (expected ` #0 {`) => this function has **no** "
                            "target attribute => in LTO the callee features are not a subset of the caller's => "
                            "areInlineCompatible judges incompatibility => lift_tick / lift_note_write are "
                            "silently not inlined (build succeeds, results are the same, only the benefit is lost)");
            }
        }
        if (attrs_lines != 1) {
            return fail(
                "part " + std::to_string(i) + " `" + name + "`: `attributes #0 = {` appears " +
                std::to_string(attrs_lines) + " time(s) (expected exactly 1)" +
                (attrs_lines == 0
                     ? " => attribute group missing: define lines reference #0 but no attributes can be parsed (clang errors out),"
                       " and without #0 the function attributes do not match the runtime TU => LTO judges incompatibility, tick/note silently not inlined"
                     : " => two #0 groups in the same part, which one wins is undefined"));
        }
        if (attrs_line.find(cpu_pair) == std::string::npos) {
            return fail("part " + std::to_string(i) + " `" + name + "`: the attribute group does not contain `" +
                        cpu_pair + "` verbatim (actual line=`" + attrs_line +
                        "`) => target-cpu does not match the runtime TU => areInlineCompatible judges incompatibility");
        }
        if (attrs_line.find(feat_pair) == std::string::npos) {
            return fail("part " + std::to_string(i) + " `" + name +
                        "`: `\"target-features\"` in the attribute group is not **verbatim** equal to the argument string (actual line=`" +
                        attrs_line +
                        "`) => the feature set does not match the runtime TU => areInlineCompatible judges incompatibility "
                        "=> tick/note silently not inlined");
        }
        defines += part_defines;
    }
    if (defines == 0) {
        return fail(
            "no part contains a `define i32 @lift_cc_` line (pin / dispatch table present but zero defines => the artifact is an empty shell, "
            "the two assertions above are vacuously true => this test verified nothing)");
    }
    std::printf(
        "lift-gen --verify-ir-target: OK (parts=%zu; defines=%llu; cpu=%s; features=%zu chars)\n",
        split, static_cast<unsigned long long>(defines), want.cpu.c_str(), want.features.size());
    return 0;
}

}  // namespace

// =====================================================================================
// main
// =====================================================================================
int main(int argc, char** argv) {
    // 与 `src/aot/aot_main.cpp` 同一套进程入口三件事（理由见那些头注）。
    lift::suppressCrashDialogsOnce();
    lift::initConsoleUtf8();
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);

    Options opt;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto needValue = [&](const char* flag, std::string* dst) -> bool {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "emulator-lift-gen: %s requires a value\n", flag);
                return false;
            }
            *dst = args[++i];
            return true;
        };
        if (a == "--exe") {
            if (!needValue("--exe", &opt.exe)) return 2;
        } else if (a == "--roots") {
            if (!needValue("--roots", &opt.roots)) return 2;
        } else if (a == "--out") {
            if (!needValue("--out", &opt.out)) return 2;
        } else if (a == "--check") {
            if (!needValue("--check", &opt.check)) return 2;
        } else if (a == "--closure-gate") {
            opt.closure_gate = true;
        } else if (a == "--split") {
            std::string v;
            if (!needValue("--split", &v)) return 2;
            char* endp = nullptr;
            const unsigned long n = std::strtoul(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0' || n == 0) {
                std::fprintf(stderr, "emulator-lift-gen: --split requires a positive integer: `%s`\n", v.c_str());
                return 2;
            }
            opt.split = static_cast<std::size_t>(n);
        } else if (a == "--out-dir") {
            if (!needValue("--out-dir", &opt.out_dir)) return 2;
        } else if (a == "--split-prefix") {
            if (!needValue("--split-prefix", &opt.split_prefix)) return 2;
        } else if (a == "--verify-split") {
            if (!needValue("--verify-split", &opt.verify_split)) return 2;
        } else if (a == "--ir-target-cpu") {
            if (!needValue("--ir-target-cpu", &opt.ir_target.cpu)) return 2;
        } else if (a == "--ir-target-features") {
            if (!needValue("--ir-target-features", &opt.ir_target.features)) return 2;
        } else if (a == "--verify-ir-target") {
            if (!needValue("--verify-ir-target", &opt.verify_ir_target)) return 2;
        } else if (a == "--pin") {
            if (!needValue("--pin", &opt.pin)) return 2;
        } else if (a == "--table") {
            if (!needValue("--table", &opt.table)) return 2;
        } else if (a == "--intrinsics") {
            if (!needValue("--intrinsics", &opt.intrinsics)) return 2;
        } else if (a == "--note-write") {
            opt.note_write = true;
        } else if (a == "--no-close-calls") {
            opt.close_calls = false;
        } else if (a == "--no-s7") {
            opt.s7 = false;
        } else if (a == "--no-close-stores") {
            opt.close_stores = false;
        } else if (a == "--max-components") {
            std::string v;
            if (!needValue("--max-components", &v)) return 2;
            char* endp = nullptr;
            const unsigned long n = std::strtoul(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0' || n == 0) {
                std::fprintf(stderr, "emulator-lift-gen: --max-components requires a positive integer: `%s`\n", v.c_str());
                return 2;
            }
            opt.max_components = n;
        } else if (a == "--close-data") {
            opt.close_data = true;
        } else if (a == "--no-close-data") {
            opt.close_data = false;
        } else if (a == "--close-jmptab") {
            opt.close_jmptab = true;
        } else if (a == "--no-close-jmptab") {
            opt.close_jmptab = false;
        } else if (a == "--no-jmptab-exclude") {
            opt.jmptab_exclude = false;
        } else if (a == "--no-close-jmptab-items") {
            opt.close_jmptab_items = false;
        } else if (a == "--jmptab-audit") {
            opt.jmptab_audit = true;
        } else if (a == "--jmptab-max-passes") {
            std::string v;
            if (!needValue("--jmptab-max-passes", &v)) return 2;
            char* endp = nullptr;
            const unsigned long n = std::strtoul(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0' || n == 0) {
                std::fprintf(stderr, "emulator-lift-gen: --jmptab-max-passes requires a positive integer: `%s`\n",
                             v.c_str());
                return 2;
            }
            opt.jmptab_max_passes = n;
        } else if (a == "--dump-blocks") {
            if (!needValue("--dump-blocks", &opt.dump_blocks)) return 2;
        } else if (a == "--verbose") {
            opt.verbose = true;
        } else if (a == "--quiet") {
            opt.quiet = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "emulator-lift-gen: unknown argument: %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    g_quiet = opt.quiet;  // ★ 解析完立刻生效：独立模式（--check / --verify-* / --jmptab-audit）也照同一口径
    // ★ `--note-write` 直接落到发射器的 target 旋钮上（`GenIrTarget` 已经一路传到 `FnEmit` ——
    //   ⛔ 不再加一条并行参数链，避免"解析了但没传到"这种静默失效）。
    opt.ir_target.note_write = opt.note_write;
    opt.ir_target.quiet = opt.quiet;  // ★ 同一口径：只静音非错误读数，产物字节不动
    // ★ `--verify-split` 是**独立模式**：只读 `out/aot/` 的产物，⛔ 不需要 `--exe/--roots/--out`。
    if (!opt.verify_split.empty()) {
        return verifyAotSplit(opt.verify_split, opt.split);
    }
    // ★ `--verify-ir-target` 同族（独立模式）：只读 `out/aot/` 的产物 + 那两个期望值。
    if (!opt.verify_ir_target.empty()) {
        return verifyAotIrTarget(opt.verify_ir_target, opt.split, opt.ir_target);
    }
    // ★ `--check` 是**独立模式**：只吃 `--exe` + 一个 closure.txt，⛔ 不需要 `--roots/--out`。
    if (!opt.check.empty()) {
        if (opt.exe.empty()) {
            std::fprintf(stderr, "emulator-lift-gen: --check requires --exe (byte truth source)\n");
            return 2;
        }
        return checkClosure(opt.exe, opt.check, opt.close_calls, opt.close_stores, opt.close_data,
                            opt.close_jmptab, opt.jmptab_exclude, opt.close_jmptab_items,
                            opt.jmptab_max_passes);
    }
    // ★ `--jmptab-audit` 是**独立模式**（与 `--check` 同族：只吃 `--exe` + `--roots`，⛔ 不需要
    //   `--out` / `--pin` / `--table`；跑 ①–③ + 分区 + D21 到 `dispatch_entries` 定型后提前返回）。
    if (opt.jmptab_audit) {
        if (opt.exe.empty() || opt.roots.empty()) {
            std::fprintf(stderr, "emulator-lift-gen: --jmptab-audit requires --exe and --roots\n");
            return 2;
        }
    } else if (opt.exe.empty() || opt.roots.empty() ||
               (opt.split <= 1 ? opt.out.empty() : opt.out_dir.empty())) {
        std::fprintf(stderr,
                     "emulator-lift-gen: --exe / --roots are required; --split 1 needs --out, --split N>1 needs "
                     "--out-dir\n");
        usage();
        return 2;
    }
    if (opt.split > 1 && !opt.out.empty()) {
        // ⛔ 两个落点同时给 = 谁生效说不清 ⇒ 直接拒绝（不猜）。
        std::fprintf(stderr, "emulator-lift-gen: --split N>1 is mutually exclusive with --out (use --out-dir for multiple parts)\n");
        return 2;
    }

    // ---- 输入 ①：入口 EA 集合 ----
    std::set<std::uint32_t> roots;
    {
        std::string err;
        if (!readRootsFile(opt.roots, &roots, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: cannot read --roots: %s\n", err.c_str());
            return 2;
        }
    }
    if (roots.empty()) {
        std::fprintf(stderr, "emulator-lift-gen: --roots contains no EA: %s\n", opt.roots.c_str());
        return 2;
    }

    // ---- 输入 ②：PE 字节（映射进 guest 后交给唯一的解码器）----
    lift::GuestMemory mem;
    if (!mem.reserve()) {
        std::fprintf(stderr, "emulator-lift-gen: failed to reserve guest address space: %s\n", mem.lastError());
        return 2;
    }
    lift::PeImage image;
    {
        std::string err;
        if (!lift::loadPeFile(opt.exe, &image, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: failed to load PE (%s): %s\n", opt.exe.c_str(), err.c_str());
            return 2;
        }
        if (!lift::mapPeImage(mem, image, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: failed to map PE (%s): %s\n", opt.exe.c_str(), err.c_str());
            return 2;
        }
    }
    lift::Decoder decoder;
    if (!decoder.ok()) {
        std::fprintf(stderr, "emulator-lift-gen: decoder initialisation failed: %s\n", decoder.lastError());
        return 4;
    }

    // ---- 输入 ③：构建期库内建表（可选；⛔ 缺省不给 ⇒ 产物与今天逐字节相同）----
    IntrinsicTable intrinsics;
    if (!opt.intrinsics.empty()) {
        std::string err;
        if (!parseIntrinsicsFile(opt.intrinsics, &intrinsics, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: intrinsics table rejected: %s\n", err.c_str());
            return 2;
        }
        // ★ 硬校验 ①：PE 的 sha256 必须与表里 pin 的前缀一致。PE 一换，表里的 EA / 形态假设
        //   就可能不再成立；⛔ 不许"生成成功了，但内建表静默失效"。
        std::string actual = image.sha256;
        for (char& ch : actual) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (actual.rfind(intrinsics.pe_sha256_prefix, 0) != 0) {
            std::fprintf(stderr,
                         "emulator-lift-gen: intrinsics table PE pin mismatch (%s):\n"
                         "  table pe-sha256 prefix = %s\n"
                         "  actual PE sha256        = %s\n"
                         "  => the table describes a different PE; refusing to emit (exit 2)\n",
                         opt.intrinsics.c_str(), intrinsics.pe_sha256_prefix.c_str(), actual.c_str());
            return 2;
        }
    }

    // ---- ① leader / 入口 EA ----
    // ★ 根必须落在**可执行节**里（越界根 = 闭包本身错 ⇒ 硬红 exit 4；⛔ 不许与派生噪声混在一起丢）。
    const std::vector<lift::aot::ExecRange> exec_ranges = lift::aot::execRanges(image);
    for (const std::uint32_t ea : roots) {
        if (lift::aot::execRangeOf(exec_ranges, ea) == nullptr) {
            std::fprintf(stderr, "emulator-lift-gen: root %08X is not in any executable section (out-of-range root)\n", ea);
            return 4;
        }
    }
    // ★ 入库件硬闸（`--closure-gate`）：**生成这一趟自己**做 `--check` 的五条校验 ——
    //   与 `--check` 共用 `parseClosureFile` / `checkClosureRoots`（唯一实现），汇总打**同一行**。
    //   CMake 的 AOT 生成命令带这个开关 ⇒ 坏闭包仍在**构建期**硬红，但**不再**有第二趟 `buildAll`
    //   （省 48.6 s：那两趟调的是同一个 `buildAll`）。
    ClosureGate gate;
    std::set<std::uint32_t> gate_roots;
    if (opt.closure_gate) {
        if (!parseClosureFile(opt.roots, &gate)) return 2;
        gate_roots.insert(gate.eas.begin(), gate.eas.end());
        checkClosureRoots(image, exec_ranges, mem, decoder, gate_roots, &gate);
        if (!gate.violations.empty()) {
            for (const std::string& v : gate.violations)
                std::fprintf(stderr, "emulator-lift-gen --check: %s\n", v.c_str());
            printClosureSummary(gate, gate_roots, image, 0, 0, 0, 0, 0, 0, 0, 0, false);
            return 2;
        }
        if (gate_roots != roots) {
            // ⛔ 不静默：**文法全绿**之后两条解析路径还读出不同集合 = 生成器内部不一致
            //   （文法红的情形已经在上面按"五条判据红"处理，⛔ 不在这里混成"内部错误"）。
            std::fprintf(stderr,
                         "emulator-lift-gen: the --closure-gate root set (%zu) differs from the --roots root set (%zu)\n",
                         gate_roots.size(), roots.size());
            return 1;
        }
    }    // ★ BUG-24：①（leader 发现 / 建块）+ ②（两趟重启 / S1 识别）+ ③（分区）+ D21 —— **一份实现**，
    //   主路径 / `--check` / `--jmptab-audit` 三处共用（见 `buildAll` 的头注）。
    BuildRun run;
    {
        std::string err;
        std::uint32_t err_ea = 0;
        int err_code = 0;
        if (!buildAll(roots, image, exec_ranges, decoder, mem, opt, &run, &err, &err_ea, &err_code)) {
            std::fprintf(stderr, "emulator-lift-gen: %s @ %08X: %s\n",
                         err_code == 4 ? "block building/decoding failed" : "jump-table recognition/partition failed", err_ea, err.c_str());
            return err_code == 0 ? 1 : err_code;
        }
    }
    // ---- ③ 分区的结果（构造完成后就是常量：只读）----
    // ★ `--closure-gate`：到这里 `buildAll` 已经成功 ⇒ 汇总行用**同一份** run 的读数
    //   （与 `--check` 那一趟的字段逐字相同，只是不再跑第二遍）。
    if (opt.closure_gate) {
        printClosureSummary(gate, gate_roots, image, run.soft_call.added.size(),
                            run.soft_store.added.size(), run.soft_data.added.size(),
                            run.soft_jumptab.added.size(), run.soft_item.added.size(),
                            run.blocks.size(), run.jmptab.tables.size(), run.passes, true);
    }
    const std::vector<GenComponent>& components = run.part.components;
    const std::set<std::uint32_t>& all_entries = run.part.all_entries;
    const std::set<std::uint32_t>& dispatch_entries = run.part.dispatch_entries;
    const std::map<std::uint32_t, GenBlock>& blocks = run.blocks;
    const DropStats& drops = run.drops;
    const std::set<std::uint32_t>& promoted = run.part.promoted;
    const std::set<std::uint32_t>& unbuildable = run.part.unbuildable;
    const JumpTableStats& jump_table = run.part.jump_table;
    const std::size_t promo_events = run.part.promo_events;
    const SoftStats& soft_call = run.soft_call;
    const SoftStats& soft_store = run.soft_store;
    const SoftStats& soft_data = run.soft_data;
    const SoftStats& soft_jumptab = run.soft_jumptab;
    const SoftStats& soft_item = run.soft_item;
    // ★ D14：shim 桩页 = `GuestLayout::shim_page` 起 `shim_slots * 4` 字节（与 `aot_run.cpp` /
    //   `aot_host.cpp` 同一份判据）。桩 EA 由 trampoline 派发 ⇒ ⛔ 不收成软根、也不算丢弃。
    const lift::GuestLayout layout;

    // ★ P0：审计模式 —— 跑到 `dispatch_entries` 定型后**提前返回**（⛔ 不发射 IR、不写 .ll）。
    if (opt.jmptab_audit) {
        return auditJumpTables(run, image, exec_ranges, decoder, mem, roots);
    }
    if (opt.max_components != 0 && components.size() > opt.max_components) {
        std::fprintf(stderr, "emulator-lift-gen: component count %zu exceeds --max-components %lu\n",
                     components.size(), opt.max_components);
        return 2;
    }
    // ★ 硬校验 ②：内建表里的每个 EA 必须是**已发射单元的根**。PE / 闭包一变，某个 EA 可能
    //   不再是根（甚至不再被发射）—— 那时闸门永远不会执行，而构建仍然全绿 ⇒ 这是**静默失效**。
    //   判据 = `components` 的根集合（= `emitModule` 真正定义的那批函数），⛔ 不是 `--roots` 文本。
    if (!intrinsics.entries.empty()) {
        std::set<std::uint32_t> component_roots;
        for (const GenComponent& comp : components) component_roots.insert(comp.root);
        for (const IntrinsicEntry& e : intrinsics.entries) {
            if (component_roots.count(e.ea) == 0) {
                std::fprintf(stderr,
                             "emulator-lift-gen: intrinsics table EA %08X (%s) is NOT an emitted unit root\n"
                             "  => the gated fast path would never run (silent no-op); refusing to emit (exit 1)\n"
                             "  (a PE/closure change moved or removed that root; update `%s`)\n",
                             e.ea, e.builtin.c_str(), opt.intrinsics.c_str());
                return 1;
            }
        }
    }
    // ---- ④ 发射 ----
    // ★ S7（D53）：先按函数判"能不能一对一结构翻译"（判据在 `aot/lift_struct.*`，跑在本进程
    //   **自己的 Zydis 解码结果**上；⛔ 不搬 `.lst` 的那套普查）。
    //   `entry_owner` = 派发入口 EA → 它所属单元的根（call 目标归属 / 传染边的输入）。
    std::map<std::uint32_t, std::uint32_t> entry_owner;
    for (const GenComponent& comp : components) {
        for (const std::uint32_t ea : comp.entries) entry_owner.emplace(ea, comp.root);
    }
    // ★ S7-IAT（REQ-01M86NMT0YJ9V78XS2DZZV364Q · design §1.3）：生成期把**运行期同一份**
    //   shim 注册表实例化出来（`registerAotShims` = 唯一装配序列），与 PE 导入表交叉成
    //   `槽 EA → 清理字节`（`aot/lift_iat.h`）。槽 EA = `image_base + PeImportInfo::iat_rva`。
    //   ⛔ 这里**不** `bind()`：本机制不需要 stub EA（关键 —— 避开了"stub EA 由注册顺序决定"
    //   这个耦合）；`registerAotShims` 只做 `add` / `addStub`（无宿主副作用、不碰 guest 内存）。
    //   ⛔ 不按微软文档手写 conv/argc 表：清理字节的真源就是 `ShimDef::conv/argc`（= 运行期
    //   `invokePrepared` 用的那两个字段）。
    //   ★ OFF 档**整段不编**：`analyzeStructure` 在 OFF 时不查询模型（`aot/lift_struct.cpp`
    //     的 `(void)iat`），而 `registerAotShims` 是所有 `shims/*.cpp` 的唯一装配序列 ⇒ 留在
    //     OFF 档会把整条 shim 注册链（d3d9 → present → quickjs …）强链进这个**构建期工具**。
    //     ON 档才需要它（该档允许 gen 多链东西）。
#if LIFT_S7_IAT
    lift::ShimRegistry shim_meta;
    lift::registerAotShims(shim_meta);
    const lift::aot::IatModel iat_model_storage = lift::aot::buildIatModel(shim_meta, image);
    const lift::aot::IatModel* iat_model = &iat_model_storage;
    {
        // 读数（⛔ 不进判据）：`modelled` = 会按 shim 的约定弹参的槽数；其余是未实现导入 /
        // take_control 的 shim（见 `aot/lift_iat.cpp` 的 `kTakeControlImports`）。
        // 两平台比对它就能发现"注册集随宿主平台变"（design §1.4-1）。
        const lift::aot::IatModelStats ms = lift::aot::iatModelStats(*iat_model);
        std::fprintf(stderr,
                     "lift-gen: [shim-meta] imports=%llu modelled=%llu unmodelled=%llu"
                     " (registered=%zu; modelled = fn != nullptr && !take_control)\n",
                     static_cast<unsigned long long>(ms.imports),
                     static_cast<unsigned long long>(ms.modelled),
                     static_cast<unsigned long long>(ms.unmodelled), shim_meta.size());
    }
#else
    const lift::aot::IatModel* iat_model = nullptr;
#endif
    const lift::aot::StructResult structure =
        lift::aot::analyzeStructure(components, entry_owner, drops.dropped, layout.shim_page,
                                    layout.shim_page + layout.shim_slots * 4u, iat_model);
    // ★ S7：交给发射器的**结论**（判据在上面；发射器只消费）。
    lift::aot::GenStructure gen_structure;
    gen_structure.enabled = opt.s7;
    gen_structure.structured_roots = opt.s7 ? structure.structured : std::set<std::uint32_t>{};
    if (!opt.s7) {
        if (!quiet()) std::fprintf(stderr, "lift-gen: [S7] --no-s7 => structural translation disabled (all call/ret go back through the trampoline)\n");
    }
    gen_structure.shim_begin = layout.shim_page;
    gen_structure.shim_end = layout.shim_page + layout.shim_slots * 4u;
    // ★ 构建期库内建表 → 发射器的唯一输入（EA → 内建名）。表已经过 PE pin + 单元根两条硬校验。
    for (const IntrinsicEntry& e : intrinsics.entries) {
        gen_structure.intrinsic_roots.emplace(e.ea, e.builtin);
    }
    // ★ 宿主钩子 EA 只在这一份模块**真的能到达它**时才有意义（小根集模块，如 x87 差分 /
    //   golden，根本不含 WinMain ⇒ 那里没有钩子点）。判据 = "它在入口集里"（能在派发表里查到）：
    //   · 在入口集 ⇒ 指向它的边发 `ret i32 <EA>`（trampoline 跑钩子后再派发回来）；
    //   · 不在入口集 ⇒ 这一份模块里不存在"钩子必须生效"的路径 ⇒ 不列（⛔ 不发 `ret i32`：
    //     那会在 trampoline 里响亮停）。全量模块（`aot/closure.txt`）两个钩子都在入口集。
    for (const std::uint32_t hook : lift::aot::kHostHookEas) {
        if (entry_owner.count(hook) != 0) {
            gen_structure.hook_eas.insert(hook);
        } else {
            if (!quiet()) std::fprintf(stderr,
                         "lift-gen: [S7] host hook EA %08X is not in this module's entry set (small-root-set module) => not listing a checkpoint\n",
                         hook);
        }
    }
    std::string ir;
    GenEmitStats stats;
    std::vector<GenUnsupported> unsupported;
    // ★ `--split N>1`：每份一个完整 `.ll` 文本 + 该份的分量根（升序、两两不交、并集 = 全部根）。
    std::vector<std::string> parts;
    std::vector<std::vector<std::uint32_t>> part_roots;
    const bool split_out = opt.split > 1;
    lift::aot::GenClosureStats closure;
    closure.close_calls = opt.close_calls;
    closure.soft_roots = soft_call.added.size();
    closure.soft_outside = soft_call.outside.size();
    closure.soft_undecodable = soft_call.undecodable.size();
    closure.soft_overlap = soft_call.overlap.size();
    closure.shim_targets = soft_call.shim_targets.size();
    // ★ D15：第二类软根（写内存 mov 的 imm32）—— 与上面 D14 那组**分开**，⛔ 不合并。
    closure.close_stores = opt.close_stores;
    closure.store_soft_roots = soft_store.added.size();
    closure.store_outside = soft_store.outside.size();
    closure.store_undecodable = soft_store.undecodable.size();
    closure.store_overlap = soft_store.overlap.size();
    closure.store_shim_targets = soft_store.shim_targets.size();
    closure.store_imm_sites = run.store_imm_sites;
    closure.store_imm_in_exec = run.store_imm_in_exec;
    // ★ D17：第三类软根（`.data` 静态代码指针）—— 与上面两组**分开**，⛔ 不合并。
    closure.close_data = opt.close_data;
    closure.data_soft_roots = soft_data.added.size();
    closure.data_outside = soft_data.outside.size();
    closure.data_undecodable = soft_data.undecodable.size();
    closure.data_overlap = soft_data.overlap.size();
    closure.data_overlap_pre = run.data_ptr_overlap_pre;
    closure.data_shim_targets = soft_data.shim_targets.size();
    closure.data_ptr_sites = run.data_ptr_sites;
    closure.data_ptr_in_exec = run.data_ptr_in_exec;
    closure.data_ptr_distinct = run.data_ptr_distinct;
    closure.data_ptr_skipped = run.data_ptr_skipped;
    closure.unbuildable_entries = unbuildable.size();
    // ★ D26：第五类软根（可执行节跳表 dword → 尚未解码的代码）—— 与上面三组**分开**，⛔ 不合并。
    closure.close_jmptab = opt.close_jmptab;
    closure.jumptab_soft_roots = soft_jumptab.added.size();
    closure.jumptab_outside = soft_jumptab.outside.size();
    closure.jumptab_undecodable = soft_jumptab.undecodable.size();
    closure.jumptab_overlap = soft_jumptab.overlap.size();
    closure.jumptab_shim_targets = soft_jumptab.shim_targets.size();
    closure.jumptab_rounds = run.jumptab_rounds;
    closure.jumptab_live_roots = run.jumptab_live_roots;
    {
        const JumpTabSurface& s = run.jumptab_surface;
        closure.jumptab_sites = s.sites;
        closure.jumptab_in_exec = s.in_exec;
        closure.jumptab_distinct = s.distinct;
        closure.jumptab_in_entries = s.in_entries;
        closure.jumptab_block_starts = s.block_starts;
        closure.jumptab_overlap_pre = s.overlap_pre;
        closure.jumptab_decodable = s.decodable;
        closure.jumptab_undecodable_pre = s.undecodable_pre;
        closure.jumptab_shim = s.shim;
    }
    // ★ D21：第四类（可执行节里的跳表 dword）—— 它**不是软根**：只加派发入口，0 新块 / 0 新指令。
    closure.jump_table_sites = jump_table.sites;
    closure.jump_table_hits = jump_table.block_hits;
    closure.jump_table_distinct = jump_table.distinct;
    closure.jump_table_added = jump_table.added.size();
    closure.jump_table_skipped = jump_table.skipped.size();
    closure.jump_table_unclaimed = jump_table.unclaimed.size();
    if (split_out) {
        if (opt.split > components.size()) {
            std::fprintf(stderr, "emulator-lift-gen: --split %zu exceeds the component count %zu (never silently truncate)\n",
                         opt.split, components.size());
            return 2;
        }
        // ★★ 一次全局分析（`emitModuleSplit` 内部走 `emitBodies`：entry_owner / block_flags /
        //    structured_roots 全部按**全集**算一次）⇒ 拆分只是**输出的分区**。
        if (!lift::aot::emitModuleSplit(components, all_entries, drops.dropped, closure, gen_structure,
                                        opt.split, &stats, &unsupported, &parts, &part_roots,
                                        opt.ir_target)) {
            std::fprintf(stderr, "emulator-lift-gen: emitter internal error\n");
            return 1;
        }
    } else if (!lift::aot::emitModule(components, all_entries, drops.dropped, closure, gen_structure,
                                      &ir, &stats, &unsupported, opt.ir_target)) {
        std::fprintf(stderr, "emulator-lift-gen: emitter internal error\n");
        return 1;
    }
    // ★ S7（D53）的覆盖率读数（stderr + `.ll` 头部都有；⛔ 只进读数、不进判据）。
    {
        const lift::aot::StructStats& s7 = structure.stats;
        if (!quiet()) std::fprintf(stderr,
                     "lift-gen: [S7 criteria] unit roots=%llu self-pass=%llu structurable=%llu (self-downgrade=%llu - "
                     "callgraph contagion=%llu) - primary causes variable-stack=%llu writes-return-slot=%llu indirect-call=%llu dynamic-return=%llu "
                     "off-table-callee=%llu\n",
                     static_cast<unsigned long long>(s7.functions),
                     static_cast<unsigned long long>(s7.self_ok),
                     static_cast<unsigned long long>(s7.structured),
                     static_cast<unsigned long long>(s7.downgraded_self),
                     static_cast<unsigned long long>(s7.downgraded_callee),
                     static_cast<unsigned long long>(s7.reason_varstack),
                     static_cast<unsigned long long>(s7.reason_return_slot),
                     static_cast<unsigned long long>(s7.reason_indirect_call),
                     static_cast<unsigned long long>(s7.reason_dynamic_ret),
                     static_cast<unsigned long long>(s7.reason_unknown_callee));
        if (!quiet()) std::fprintf(stderr,
                     "lift-gen: [S7 criteria/site accounts] ret=%llu (in structured units=%llu) - call=%llu (static=%llu "
                     "indirect=%llu) => host-call=%llu lift_dyn=%llu shim-dispatch=%llu other-dispatch=%llu - jmp=%llu"
                     "(static=%llu indirect=%llu)\n",
                     static_cast<unsigned long long>(s7.ret_sites),
                     static_cast<unsigned long long>(s7.ret_sites_structured),
                     static_cast<unsigned long long>(s7.call_sites),
                     static_cast<unsigned long long>(s7.call_sites_static),
                     static_cast<unsigned long long>(s7.call_sites_indirect),
                     static_cast<unsigned long long>(s7.call_sites_host),
                     static_cast<unsigned long long>(s7.call_sites_lift_dyn),
                     static_cast<unsigned long long>(s7.call_sites_shim),
                     static_cast<unsigned long long>(s7.call_sites_dispatch),
                     static_cast<unsigned long long>(s7.jmp_sites),
                     static_cast<unsigned long long>(s7.jmp_sites_static),
                     static_cast<unsigned long long>(s7.jmp_sites_indirect));
        // 降级清单（EA + 原因 + 首因站点 + 传染源）。**全量**打 stderr：它是裁决的可复核面。
        std::size_t printed = 0;
        for (const auto& kv : structure.reason) {
            const auto eit = structure.reason_ea.find(kv.first);
            const auto cit = structure.contagious_from.find(kv.first);
            if (!quiet()) std::fprintf(stderr, "lift-gen: [S7 downgrade] %08X reason=%s site=%08X%s%08X\n", kv.first,
                         lift::aot::structReasonName(kv.second),
                         eit != structure.reason_ea.end() ? eit->second : 0u,
                         cit != structure.contagious_from.end() ? " contagious-from=" : "",
                         cit != structure.contagious_from.end() ? cit->second : 0u);
            ++printed;
        }
        if (!quiet()) std::fprintf(stderr, "lift-gen: [S7 downgrade] total %zu unit roots are not structurable (one line each above)\n", printed);
    }

    // 计数（父级要求的读数）
    std::uint64_t block_count = 0;
    std::uint64_t insn_count = 0;
    for (const GenComponent& comp : components) {
        block_count += comp.blocks.size();
        for (const auto& kv : comp.blocks) insn_count += kv.second.insns.size();
    }

    // ---- ⑤ 落盘（`.ll` 不 commit：落点由调用方指定，通常 out/aot/）----
    std::error_code ec;
    if (split_out) {
        // ★ N 份 `.ll`：文件名 = `<prefix>.part<NN>.ll`（序号，**不是**内容哈希 ⇒ 名字确定）。
        //   清单 = `<prefix>.split.txt`（每份 序号 / 文件名 / sha256 / 字节 / functions / 首末根 EA
        //   + 一行汇总）。同输入两次生成 ⇒ 逐字节相同（发射文本确定性 + 分桶是纯函数）。
        const std::filesystem::path parts_dir(opt.out_dir);
        std::filesystem::create_directories(parts_dir, ec);
        std::vector<std::string> names;
        std::vector<std::string> shas;
        std::vector<std::uint64_t> bytes;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            char name[96];
            std::snprintf(name, sizeof(name), "%s.part%02zu.ll", opt.split_prefix.c_str(), i);
            names.push_back(name);
            const std::filesystem::path p = parts_dir / name;
            std::ofstream file(p, std::ios::binary | std::ios::trunc);
            if (!file) {
                std::fprintf(stderr, "emulator-lift-gen: cannot write `%s`\n", p.string().c_str());
                return 2;
            }
            file.write(parts[i].data(), static_cast<std::streamsize>(parts[i].size()));
            file.close();
            if (!file) {
                std::fprintf(stderr, "emulator-lift-gen: write to `%s` failed\n", p.string().c_str());
                return 2;
            }
            const std::vector<std::uint8_t> raw(parts[i].begin(), parts[i].end());
            shas.push_back(lift::toLower(lift::toHex(lift::sha256(raw))));
            bytes.push_back(static_cast<std::uint64_t>(parts[i].size()));
        }
        const std::filesystem::path manifest = parts_dir / (opt.split_prefix + ".split.txt");
        std::ofstream file(manifest, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::fprintf(stderr, "emulator-lift-gen: cannot write `%s`\n", manifest.string().c_str());
            return 2;
        }
        file << "# Generated by emulator-lift-gen --split " << parts.size()
             << " - do not edit by hand, not tracked in git.\n";
        file << "# One line per part: `part <index> <file> <sha256> <bytes> <functions> <first-root> "
                "<last-root>`; last line `split=... functions=... bytes=... max-bytes=...`.\n";
        file << "# Bucketing rule (deterministic pure function): component roots in ascending EA order, greedily filled to sum(body)/N.\n";
        std::uint64_t total_bytes = 0;
        std::uint64_t total_functions = 0;
        std::uint64_t max_bytes = 0;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            total_bytes += bytes[i];
            total_functions += part_roots[i].size();
            if (bytes[i] > max_bytes) max_bytes = bytes[i];
            file << "part " << i << " " << names[i] << " " << shas[i] << " " << bytes[i] << " "
                 << part_roots[i].size() << " "
                 << "0x" << hexEa(part_roots[i].front()) << " " << "0x" << hexEa(part_roots[i].back())
                 << "\n";
        }
        file << "split=" << parts.size() << " functions=" << total_functions << " bytes=" << total_bytes
             << " max-bytes=" << max_bytes << "\n";
        file.close();
        if (!file) {
            std::fprintf(stderr, "emulator-lift-gen: write to `%s` failed\n", manifest.string().c_str());
            return 2;
        }
        if (!quiet()) std::printf("lift-gen: %zu parts (%s/%s.part*.ll - manifest %s - total functions %llu - total bytes %llu - "
                    "largest part %llu)\n",
                    parts.size(), opt.out_dir.c_str(), opt.split_prefix.c_str(), manifest.string().c_str(),
                    static_cast<unsigned long long>(total_functions),
                    static_cast<unsigned long long>(total_bytes),
                    static_cast<unsigned long long>(max_bytes));
    } else {
        const std::filesystem::path out_path(opt.out);
        const std::filesystem::path out_dir =
            out_path.has_parent_path() ? out_path.parent_path() : std::filesystem::path(".");
        std::filesystem::create_directories(out_dir, ec);
        std::ofstream file(out_path, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::fprintf(stderr, "emulator-lift-gen: cannot write `%s`\n", out_path.string().c_str());
            return 2;
        }
        file.write(ir.data(), static_cast<std::streamsize>(ir.size()));
        file.close();
        if (!file) {
            std::fprintf(stderr, "emulator-lift-gen: write to `%s` failed\n", out_path.string().c_str());
            return 2;
        }
    }

    // ---- ⑤' `--pin` / `--table`（可选；AOT 构建才要）----
    // ★ 写在 `.ll` 之后、`unsupported` 之前：这两份是"运行期要的机械数据"，与 trap 清单无关。
    //   ⛔ 缺省不写：测试夹具的 golden 生成（`lift-ir-gen-*`）与 `--check` 都不需要它们。
    if (!opt.pin.empty()) {
        std::string err;
        if (!writeAotPin(opt.pin, image, roots.size(), dispatch_entries.size(), components.size(),
                         intrinsics.entries, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: --pin failed: %s\n", err.c_str());
            return 2;
        }
        if (!quiet()) std::printf("lift-gen: pin %s (sha256=%s entry=%08X)\n", opt.pin.c_str(), image.sha256.c_str(),
                    image.entry_ea);
    }
    if (!opt.table.empty()) {
        std::string err;
        if (!writeAotTable(opt.table, components, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: --table failed: %s\n", err.c_str());
            return 1;
        }
        std::size_t rows = 0;
        for (const GenComponent& comp : components) rows += comp.entries.size();
        if (!quiet()) std::printf("lift-gen: dispatch table %s (%zu rows = entry EA -> component root function)\n", opt.table.c_str(), rows);
    }
    if (!intrinsics.entries.empty()) {
        if (!quiet()) std::printf("lift-gen: intrinsics %s (%zu unit root%s; PE sha prefix %s)\n",
                    opt.intrinsics.c_str(), intrinsics.entries.size(),
                    intrinsics.entries.size() == 1 ? "" : "s", intrinsics.pe_sha256_prefix.c_str());
    }
    // ---- ⑤'' ★ D18：`--dump-blocks`（只读覆盖率读数；⛔ 不改 `.ll` / 派发表）----
    if (!opt.dump_blocks.empty()) {
        std::string err;
        std::size_t dumped_blocks = 0;
        std::uint64_t dumped_bytes = 0;
        std::uint64_t dumped_insns = 0;
        if (!writeBlockDump(opt.dump_blocks, components, &dumped_blocks, &dumped_bytes, &dumped_insns,
                            &err)) {
            std::fprintf(stderr, "emulator-lift-gen: --dump-blocks failed: %s\n", err.c_str());
            return 2;
        }
        if (!quiet()) std::printf("lift-gen: block dump %s (blocks=%zu bytes=%llu insns=%llu; not part of .ll / dispatch table)\n",
                    opt.dump_blocks.c_str(), dumped_blocks,
                    static_cast<unsigned long long>(dumped_bytes),
                    static_cast<unsigned long long>(dumped_insns));
    }

    // unsupported 清单（确定性：按 EA / op / 原因排序）
    std::sort(unsupported.begin(), unsupported.end(),
              [](const GenUnsupported& a, const GenUnsupported& b) {
                  if (a.ea != b.ea) return a.ea < b.ea;
                  const int c = std::strcmp(a.op, b.op);
                  if (c != 0) return c < 0;
                  return a.reason < b.reason;
              });
    unsupported.erase(std::unique(unsupported.begin(), unsupported.end(),
                                  [](const GenUnsupported& a, const GenUnsupported& b) {
                                      return a.ea == b.ea && std::strcmp(a.op, b.op) == 0 &&
                                             a.reason == b.reason;
                                  }),
                      unsupported.end());
    // ★ `unsupported.txt` 与 `.ll` 同目录（单份 = `--out` 的父目录；多份 = `--out-dir`）。
    const std::filesystem::path unsupported_dir =
        split_out ? std::filesystem::path(opt.out_dir)
                  : (std::filesystem::path(opt.out).has_parent_path()
                         ? std::filesystem::path(opt.out).parent_path()
                         : std::filesystem::path("."));
    const std::filesystem::path unsupported_path = unsupported_dir / "unsupported.txt";
    {
        std::ofstream file(unsupported_path, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::fprintf(stderr, "emulator-lift-gen: cannot write `%s`\n", unsupported_path.string().c_str());
            return 2;
        }
        file << "# emulator-lift-gen (Shape A / T1c): guest instructions that **still trap**\n";
        file << "# Format: 0x<EA> <op> <reason>; these sites are llvm.trap + unreachable in .ll\n";
        file << "# Since T1c 'unimplemented op' no longer lands here (it goes through the single-instruction host primitive @lift_one); empty is the normal state,\n";
        file << "#   content => control fell into the default path = generator internal inconsistency (exit code 3).\n";
        for (const GenUnsupported& u : unsupported) {
            char ea[16];
            std::snprintf(ea, sizeof(ea), "0x%08X", u.ea);
            file << ea << " " << u.op << " " << u.reason << "\n";
        }
        file.close();
        if (!file) {
            std::fprintf(stderr, "emulator-lift-gen: write to `%s` failed\n", unsupported_path.string().c_str());
            return 2;
        }
    }

    // ---- ⑥ 读数 ----
    if (!quiet()) std::printf(
        "lift-gen: roots=%zu entries=%zu functions=%zu blocks=%llu insns=%llu promotions=%zu "
        "traps=%llu(%llu unsupported + %llu bad)\n",
        roots.size(), dispatch_entries.size(), components.size(),
        static_cast<unsigned long long>(block_count), static_cast<unsigned long long>(insn_count),
        promoted.size(), static_cast<unsigned long long>(stats.traps_unsupported + stats.traps_bad),
        static_cast<unsigned long long>(stats.traps_unsupported),
        static_cast<unsigned long long>(stats.traps_bad));
    if (!quiet()) std::printf("lift-gen: lift_one call sites %llu (%zu ops; single-instruction host primitives, not traps)\n",
                static_cast<unsigned long long>(stats.lift_one_sites), stats.lift_one_by_op.size());
    if (!quiet()) {
        std::printf("lift-gen: lift_one totals:");
        bool first = true;
        for (const auto& kv : stats.lift_one_by_op) {
            std::printf("%s%s=%llu", first ? "" : " ", kv.first.c_str(),
                        static_cast<unsigned long long>(kv.second));
            first = false;
        }
        std::printf("\n");
    }
    if (!quiet()) std::printf("lift-gen: dropped derived leaders %zu (%zu undecodable + %zu inside already-emitted block bytes)\n",
                drops.dropped.size(), drops.undecodable.size(), drops.inside_block.size());
    // ★ D14：静态调用闭包的账 —— 与上面"派生 leader"**分开报**（越界 / 解不出 / 重叠各一个数）。
    //   同一条也打到 stderr（要求：不静默），并把每个丢弃类别的 **3 个样例**打出来。
    if (!quiet()) std::printf(
        "lift-gen: static call closure %s soft-roots=%zu (new unit roots) - dropped: outside=%zu undecodable=%zu "
        "overlap=%zu - shim targets skipped=%zu\n",
        opt.close_calls ? "on" : "off", soft_call.added.size(), soft_call.outside.size(),
        soft_call.undecodable.size(), soft_call.overlap.size(), soft_call.shim_targets.size());
    if (!quiet()) std::fprintf(stderr,
                 "lift-gen: [static call closure D14/%s] soft-roots=%zu dropped: outside=%zu undecodable=%zu overlap=%zu "
                 "- shim targets skipped=%zu\n",
                 opt.close_calls ? "on" : "off", soft_call.added.size(), soft_call.outside.size(),
                 soft_call.undecodable.size(), soft_call.overlap.size(), soft_call.shim_targets.size());
    // ★ D15：第二类软根（写内存 mov 的 imm32）—— **独立一行**，三档丢弃同样各一个数。
    //   `写内存 mov 的 imm32 站点` 是判据的**输入面**（读数，不是判据）：越界那一个数之所以大，
    //   就是因为绝大多数 `mov [x], <数据常量>` 的 imm 不是代码地址（站点 − 落在可执行节内 ≈ 它们）。
    if (!quiet()) std::printf(
        "lift-gen: code-pointer immediates (D15) %s soft-roots=%zu (new unit roots) - dropped: outside=%zu undecodable=%zu "
        "overlap=%zu - shim targets skipped=%zu\n",
        opt.close_stores ? "on" : "off", soft_store.added.size(), soft_store.outside.size(),
        soft_store.undecodable.size(), soft_store.overlap.size(), soft_store.shim_targets.size());
    if (!quiet()) std::printf("lift-gen: memory-writing mov imm32: sites=%llu (of which imm falls inside an executable section=%llu)\n",
                static_cast<unsigned long long>(run.store_imm_sites),
                static_cast<unsigned long long>(run.store_imm_in_exec));
    if (!quiet()) std::fprintf(stderr,
                 "lift-gen: [code-pointer immediates D15/%s] soft-roots=%zu dropped: outside=%zu undecodable=%zu overlap=%zu "
                 "- shim targets skipped=%zu - sites=%llu (imm inside an executable section=%llu)\n",
                 opt.close_stores ? "on" : "off", soft_store.added.size(), soft_store.outside.size(),
                 soft_store.undecodable.size(), soft_store.overlap.size(),
                 soft_store.shim_targets.size(),
                 static_cast<unsigned long long>(run.store_imm_sites),
                 static_cast<unsigned long long>(run.store_imm_in_exec));
    // ★ D17：第三类软根（`.data` 静态代码指针）—— **独立一行**，三档丢弃同样各一个数。
    //   两条读数（判据的**输入面**，⛔ 不是判据）：扫过的 dword 站点 / 其中值落在可执行节内的站点；
    //   再去重才是候选数（`distinct`），候选里"已在入口集 / 已判过"的不重复收（`skipped`）。
    //   ⇒ 恒等式（可手工核）：`distinct = added + overlap + skipped`（outside / shim 结构性为 0：
    //   候选的定义就是"值落在可执行节内"、而 shim 桩页不在可执行节里）。
    if (!quiet()) std::printf(
        "lift-gen: static code pointers (D17) %s soft-roots=%zu (new unit roots) - dropped: outside=%zu undecodable=%zu "
        "overlap=%zu - shim targets skipped=%zu\n",
        opt.close_data ? "on" : "off", soft_data.added.size(), soft_data.outside.size(),
        soft_data.undecodable.size(), soft_data.overlap.size(), soft_data.shim_targets.size());
    if (!quiet()) std::printf(
        "lift-gen: 4-byte aligned dwords in non-executable sections: sites=%llu (value inside an executable section=%llu) - distinct candidates=%llu "
        "- already in entry set/already judged=%llu\n",
        static_cast<unsigned long long>(run.data_ptr_sites),
        static_cast<unsigned long long>(run.data_ptr_in_exec),
        static_cast<unsigned long long>(run.data_ptr_distinct),
        static_cast<unsigned long long>(run.data_ptr_skipped));
    // ★ 恒等式（可手工核）：`不同候选 = 新增 + 重叠 + 已在入口集/已判过`，
    //   其中"重叠"的**收之前挡下**那部分（`data_overlap_pre`）是判据前移挡的，其余是收进来之后
    //   由 `dropLeadersInsideBlocks` 事后判的（那一部分同时也在 `新增` 里 —— `added` 记的是
    //   "被接受为软根"，与 D14/D15 同一口径）。
    if (!quiet()) std::printf("lift-gen: of which blocked before acceptance (inside already-emitted block byte range) = %llu\n",
                static_cast<unsigned long long>(run.data_ptr_overlap_pre));
    if (!quiet()) std::fprintf(stderr,
                 "lift-gen: [static code pointers D17/%s] soft-roots=%zu dropped: outside=%zu undecodable=%zu overlap=%zu "
                 "- shim targets skipped=%zu - sites=%llu (value inside an executable section=%llu) - distinct candidates=%llu - already in entry set/already judged=%llu\n",
                 opt.close_data ? "on" : "off", soft_data.added.size(), soft_data.outside.size(),
                 soft_data.undecodable.size(), soft_data.overlap.size(),
                 soft_data.shim_targets.size(),
                 static_cast<unsigned long long>(run.data_ptr_sites),
                 static_cast<unsigned long long>(run.data_ptr_in_exec),
                 static_cast<unsigned long long>(run.data_ptr_distinct),
                 static_cast<unsigned long long>(run.data_ptr_skipped));
    // ★ D26：第五类软根（可执行节跳表 dword → **尚未解码**的代码）—— **独立一行**，三档丢弃各一个数；
    //   再一行是**候选面**（"先量后收"：每一档只归一类，五档相加 = `不同候选`）。
    //   ★ 与 D17 的本质区别：这里的 `added` 是**新增 unit 根 = 新增块 / 新增指令**（D17 那行不是）。
    if (!quiet()) std::printf(
        "lift-gen: executable-section jump-table targets (D26) %s soft-roots=%zu (new unit roots = **may add blocks**) - dropped: outside=%zu "
        "undecodable=%zu overlap=%zu - shim targets skipped=%zu - fixed-point rounds=%llu - still in entry set=%llu\n",
        opt.close_jmptab ? "on" : "off", soft_jumptab.added.size(), soft_jumptab.outside.size(),
        soft_jumptab.undecodable.size(), soft_jumptab.overlap.size(),
        soft_jumptab.shim_targets.size(),
        static_cast<unsigned long long>(run.jumptab_rounds),
        static_cast<unsigned long long>(run.jumptab_live_roots));
    {
        const JumpTabSurface& s = run.jumptab_surface;
        // ★ D26 的第二条扫描（未对齐 imm32：`push imm32` / `mov reg, imm32`）：**只有输入面读数**。
        //   候选已经并进下面那行的"不同候选（并集）"，收/丢的账仍然是 `soft_jumptab`
        //   （⛔ 不另立一类软根）。
        if (!quiet()) std::printf(
            "lift-gen: unaligned imm32 candidate surface (D26 second scan): sites=%llu (push imm32=%llu - mov reg, "
            "imm32=%llu) - value inside an executable section=%llu - distinct candidates=%llu (of which found only by this scan=%llu)\n",
            static_cast<unsigned long long>(run.imm32_sites),
            static_cast<unsigned long long>(run.imm32_push_sites),
            static_cast<unsigned long long>(run.imm32_mov_sites),
            static_cast<unsigned long long>(run.imm32_in_exec),
            static_cast<unsigned long long>(run.imm32_distinct),
            static_cast<unsigned long long>(run.imm32_only));
        if (!quiet()) std::printf(
            "lift-gen: executable code-pointer candidate surface (aligned dwords union unaligned imm32): aligned dword sites=%llu - "
            "value inside an executable section=%llu - distinct candidates (union)=%llu "
            "- already in entry set/already judged=%llu - already a block start (handled as kind 4, D21)=%llu - decodable=%llu - undecodable=%llu "
            "- blocked by overlap=%llu - shim stubs=%llu\n",
            static_cast<unsigned long long>(s.sites), static_cast<unsigned long long>(s.in_exec),
            static_cast<unsigned long long>(s.distinct),
            static_cast<unsigned long long>(s.in_entries),
            static_cast<unsigned long long>(s.block_starts),
            static_cast<unsigned long long>(s.decodable),
            static_cast<unsigned long long>(s.undecodable_pre),
            static_cast<unsigned long long>(s.overlap_pre), static_cast<unsigned long long>(s.shim));
        // ★ 恒等式（可手工核）：`不同候选 = 已在入口集/已判过 + shim 桩 + 已是块首 + 被重叠挡下 + 可解码 + 解不出`。
        if (!quiet()) std::fprintf(stderr,
                     "lift-gen: [executable jump table D26/%s] soft-roots=%zu (still in entry set at the end=%llu) dropped: outside=%zu "
                     "undecodable=%zu overlap=%zu - shim targets skipped=%zu - rounds=%llu - aligned dword sites=%llu (value inside an executable section=%llu)"
                     " - unaligned imm32 sites=%llu (push=%llu - mov=%llu - value inside an executable section=%llu, found only by this scan=%llu)"
                     " - distinct candidates (union)=%llu - already in entry set/already judged=%llu - already a block start=%llu - decodable=%llu - undecodable=%llu"
                     " - blocked by overlap=%llu\n",
                     opt.close_jmptab ? "on" : "off", soft_jumptab.added.size(),
                     static_cast<unsigned long long>(run.jumptab_live_roots),
                     soft_jumptab.outside.size(),
                     soft_jumptab.undecodable.size(), soft_jumptab.overlap.size(),
                     soft_jumptab.shim_targets.size(),
                     static_cast<unsigned long long>(run.jumptab_rounds),
                     static_cast<unsigned long long>(s.sites),
                     static_cast<unsigned long long>(s.in_exec),
                     static_cast<unsigned long long>(run.imm32_sites),
                     static_cast<unsigned long long>(run.imm32_push_sites),
                     static_cast<unsigned long long>(run.imm32_mov_sites),
                     static_cast<unsigned long long>(run.imm32_in_exec),
                     static_cast<unsigned long long>(run.imm32_only),
                     static_cast<unsigned long long>(s.distinct),
                     static_cast<unsigned long long>(s.in_entries),
                     static_cast<unsigned long long>(s.block_starts),
                     static_cast<unsigned long long>(s.decodable),
                     static_cast<unsigned long long>(s.undecodable_pre),
                     static_cast<unsigned long long>(s.overlap_pre));
    }
    // 样例（⛔ 不是"看看"：这三类的形状由 D14 的三条判据定义，样例是规则的实证）
    auto printSamples = [](const char* tag, const std::set<std::uint32_t>& eas) {
        if (quiet()) return;
        if (eas.empty()) return;
        std::fprintf(stderr, "lift-gen: [%s] %zu site(s), samples:", tag, eas.size());
        std::size_t n = 0;
        for (const std::uint32_t ea : eas) {
            if (n++ == 8) {
                std::fprintf(stderr, " ...");
                break;
            }
            std::fprintf(stderr, " %08X", ea);
        }
        std::fprintf(stderr, "\n");
    };
    printSamples("dropped/undecodable", drops.undecodable);
    printSamples("dropped/inside already-emitted block bytes", drops.inside_block);
    auto printSoftSamples = [](const char* tag, const std::set<std::uint32_t>& eas) {
        if (quiet()) return;
        if (eas.empty()) return;
        std::fprintf(stderr, "lift-gen: [%s] %zu site(s), samples (3):", tag, eas.size());
        std::size_t n = 0;
        for (const std::uint32_t ea : eas) {
            if (n++ == 3) {
                std::fprintf(stderr, " ...");
                break;
            }
            std::fprintf(stderr, " %08X", ea);
        }
        std::fprintf(stderr, "\n");
    };
    printSoftSamples("soft-root/outside (call)", soft_call.outside);
    printSoftSamples("soft-root/undecodable (call)", soft_call.undecodable);
    printSoftSamples("soft-root/overlap (call)", soft_call.overlap);
    printSoftSamples("soft-root/outside (store-imm)", soft_store.outside);
    printSoftSamples("soft-root/undecodable (store-imm)", soft_store.undecodable);
    printSoftSamples("soft-root/overlap (store-imm)", soft_store.overlap);
    printSoftSamples("soft-root/outside (data-ptr)", soft_data.outside);
    printSoftSamples("soft-root/undecodable (data-ptr)", soft_data.undecodable);
    printSoftSamples("soft-root/overlap (data-ptr)", soft_data.overlap);
    printSoftSamples("soft-root/outside (jmptab)", soft_jumptab.outside);
    printSoftSamples("soft-root/undecodable (jmptab)", soft_jumptab.undecodable);
    printSoftSamples("soft-root/overlap (jmptab)", soft_jumptab.overlap);
    printSoftSamples("soft-root/accepted (jmptab)", soft_jumptab.added);
    // ★ BUG-24 / P2（S3）：**表项**的账 —— 独立一行（⛔ 不与 D26 合并：两类来源不同）。
    if (!quiet()) std::printf(
        "lift-gen: jump-table items (S3/P2) %s candidates=%llu - accepted as leaders (before block building)=%zu - already entries=%llu - "
        "outside=%llu - shim=%llu - inside a table range=%llu - undecodable=%zu - overlaps an emitted block=%zu\n",
        opt.close_jmptab_items ? "on" : "off",
        static_cast<unsigned long long>(run.item_seeds), soft_item.added.size(),
        static_cast<unsigned long long>(run.item_already),
        static_cast<unsigned long long>(run.item_outside),
        static_cast<unsigned long long>(run.item_shim),
        static_cast<unsigned long long>(run.item_in_range), soft_item.undecodable.size(),
        soft_item.overlap.size());
    printSoftSamples("soft-root/undecodable (jmptab-item)", soft_item.undecodable);
    printSoftSamples("soft-root/overlap (jmptab-item)", soft_item.overlap);
    // ★ BUG-24 / S1：**跳表识别**的账（逐表细节走 `--jmptab-audit`；这里只报能对账的汇总）。
    if (!quiet()) std::printf(
        "lift-gen: jump-table recognition (S1) %s - sites=%llu - candidate tables=%zu - recognised=%zu (srcA=%zu srcB=%zu) - "
        "rejected=%zu (of which (5) range covers a root/call target=%llu) - items=%zu - passes=%u - switches exclude=%d items=%d\n",
        run.jmptab_on ? "on" : "off", static_cast<unsigned long long>(run.jmptab.sites),
        run.jmptab.candidates.size(), run.jmptab.tables.size(),
        [&] {
            std::size_t a = 0;
            for (const auto& kv : run.jmptab.tables) if (!kv.second.src_b) ++a;
            return a;
        }(),
        [&] {
            std::size_t b = 0;
            for (const auto& kv : run.jmptab.tables) if (kv.second.src_b) ++b;
            return b;
        }(),
        run.jmptab.rejects.size(),
        static_cast<unsigned long long>(run.jmptab.rejected_covers_protected),
        run.jmptab.items().size(), run.passes, opt.jmptab_exclude ? 1 : 0,
        opt.close_jmptab_items ? 1 : 0);
    // ★ BUG-24 / P1（S2）的读数：被表区间挡下的 leader / `call` 返回 EA。
    if (!quiet()) std::printf("lift-gen: jump-table ranges excluded from decoding (S2/P1) %s - leaders blocked by a range=%llu (root exceptions=%llu) - "
                "call return EAs inside a range=%llu\n",
                opt.jmptab_exclude ? "on" : "off",
                static_cast<unsigned long long>(run.data_range_leaders),
                static_cast<unsigned long long>(run.data_range_root_hits),
                static_cast<unsigned long long>(run.call_ret_in_data_range));
    printSoftSamples("jump-table range/blocked leaders", run.table_range_leaders);
    // 五类软根的"落在表区间"这一档（P1 新增；⛔ 不写 rejected_targets_）
    printSoftSamples("soft-root/inside table range (call)", soft_call.table_range);
    printSoftSamples("soft-root/inside table range (store-imm)", soft_store.table_range);
    printSoftSamples("soft-root/inside table range (data-ptr)", soft_data.table_range);
    printSoftSamples("soft-root/inside table range (jmptab)", soft_jumptab.table_range);
    // ★ D21（第四类）：**跳表入口**的账 —— 独立一行，且必须能读出"只加 entry、0 新块"这一点。
    //   判据的输入面是"可执行节里 4 字节对齐的 dword"站点数；强过滤器是"值必须已经是块首"。
    if (!quiet()) std::printf(
        "lift-gen: jump-table entries (D21) added=%zu (entries only; 0 new blocks / 0 new instructions) - executable-section 4-byte aligned "
        "dword sites=%llu (value==an existing block start=%llu, distinct targets=%llu) - already in entry set=%zu - orphan promotions=%zu\n",
        jump_table.added.size(), static_cast<unsigned long long>(jump_table.sites),
        static_cast<unsigned long long>(jump_table.block_hits),
        static_cast<unsigned long long>(jump_table.distinct), jump_table.skipped.size(),
        jump_table.unclaimed.size());
    if (!quiet()) std::fprintf(stderr,
                 "lift-gen: [jump-table entries D21] added=%zu (entries only, 0 new blocks) - executable-section 4-byte aligned dword "
                 "sites=%llu (value==an existing block start=%llu, distinct targets=%llu) - already in entry set=%zu - orphan promotions=%zu\n",
                 jump_table.added.size(), static_cast<unsigned long long>(jump_table.sites),
                 static_cast<unsigned long long>(jump_table.block_hits),
                 static_cast<unsigned long long>(jump_table.distinct), jump_table.skipped.size(),
                 jump_table.unclaimed.size());
    printSoftSamples("jump-table entry/added (D21)", jump_table.added);
    printSoftSamples("jump-table entry/orphan-promoted (D21)", jump_table.unclaimed);
    // ★ G3：入口集收敛护栏 —— 没有块、退出入口集的入口 EA（⛔ 不静默）。
    if (!quiet()) std::printf("lift-gen: entry-set convergence guard: entry EAs without a block=%zu (=> dropped from the entry set, edges to them emit ret i32)\n",
                unbuildable.size());
    if (!quiet() && !unbuildable.empty()) {
        std::fprintf(stderr, "lift-gen: [entry set/no block] %zu site(s), samples (3):", unbuildable.size());
        std::size_t n = 0;
        for (const std::uint32_t ea : unbuildable) {
            if (n++ == 3) {
                std::fprintf(stderr, " ...");
                break;
            }
            std::fprintf(stderr, " %08X", ea);
        }
        std::fprintf(stderr, "\n");
    }
    if (!quiet() && opt.verbose) {
        for (const GenComponent& comp : components) {
            std::uint64_t ib = 0;
            for (const auto& kv : comp.blocks) ib += kv.second.insns.size();
            std::printf("  fn %08X: entries=%zu blocks=%zu insns=%llu\n", comp.root, comp.entries.size(),
                        comp.blocks.size(), static_cast<unsigned long long>(ib));
        }
        std::printf("  promotions(events)=%zu\n", promo_events);
    }

    if (!unsupported.empty()) {
        std::map<std::string, std::size_t> by_op;
        for (const GenUnsupported& u : unsupported) ++by_op[u.op];
        std::fprintf(stderr, "lift-gen: %zu unsupported site(s) (llvm.trap) -> %s\n", unsupported.size(),
                     unsupported_path.string().c_str());
        std::fprintf(stderr, "lift-gen: unsupported totals:");
        bool first = true;
        for (const auto& kv : by_op) {
            std::fprintf(stderr, "%s%s=%zu", first ? "" : " ", kv.first.c_str(), kv.second);
            first = false;
        }
        std::fprintf(stderr, "\n");
        return 3;
    }
    return 0;
}
