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
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "aot/lift_aot_text.h"
#include "aot/lift_gen_emit.h"
#include "runtime/decode.h"
#include "runtime/exec.h"
#include "runtime/host_encoding.h"
#include "runtime/image.h"
#include "runtime/memory.h"

namespace {

using lift::aot::GenBlock;
using lift::aot::GenComponent;
using lift::aot::GenEdge;
using lift::aot::GenEmitStats;
using lift::aot::GenUnsupported;

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
    // ★ D18：只读覆盖率读数（块清单；⛔ 不参与 `.ll` / 派发表）。
    std::string dump_blocks;
    unsigned long max_components = 0;
    bool verbose = false;
};

void usage() {
    std::printf(
        "emulator-lift-gen —— Shape A / T1a：入口 EA 集合 + PE 字节 → LLVM IR（`.ll`）\n"
        "  --exe <PE>            字节真源（映射进 guest 后交给解码器）\n"
        "  --roots <file>        一行一个 hex EA（`#` 起注释）\n"
        "  --out <file.ll>       产物（建议 out/aot/lift_aot.ll；⛔ 不入库）\n"
        "  --check <file>        只校验 closure.txt（格式/升序/可解码/在 .text/entry 必在）⇒ 一行汇总；⛔ 不发射\n"
        "  --pin <file.h>        额外写 exe 的 sha256 + entry（AOT 启动核对，D1；⛔ 不入库）\n"
        "  --table <file.inc>    额外写派发表 `EA → lift_cc_<根>`（AOT 的 EA→LiftFn 查找面；⛔ 不入库）\n"
        "  --no-close-calls      关掉静态调用闭包（D14 默认开；对照档 = 只按 --roots 展开）\n"
        "  --no-close-stores     关掉第二类软根（D15：写内存 mov 的 imm32 代码指针；默认开）\n"
        "  --close-data          打开第三类软根（D17：非可执行节里静态初始化的代码指针 = `.data` 指针表）\n"
        "  --no-close-data       关掉第三类软根（缺省 %s：见 kCloseDataByDefault 与 .tmp/aot/g3/readings.md）\n"
        "  --close-jmptab        打开第五类软根（D26：可执行节跳表 dword → **尚未解码**的代码；会新增块）\n"
        "  --no-close-jmptab     关掉第五类软根（缺省 %s：见 kCloseJmptabByDefault 与 .tmp/aot/c2/readings.md）\n"
        "  --dump-blocks <file>  只读覆盖率读数：最终块的 `start EA / 字节长度 / 指令数`（升序文本；⛔ 不改 .ll / 派发表）\n"
        "  --max-components N    分量数上限（超过 ⇒ 退出码 2；缺省不限）\n"
        "  --verbose             逐分量读数\n"
        "  --help\n"
        "退出码：0 成功 · 2 参数/输入错 · 3 仍有 llvm.trap（= 生成器内部不一致：控制转移掉进默认路径）\n"
        "        · 4 根（--roots 里的 EA）解码失败 / 越界（派生 leader 解不出只丢弃 + 计数，不影响退出码）\n"
        "        · 1 内部不一致\n",
        kCloseDataByDefault ? "开" : "关", kCloseJmptabByDefault ? "开" : "关");
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
};

// 软根的**来源**（= 哪一条判据收的它）。丢弃时必须能回答"这是哪一类的账"。
// ★ 新增类**只许追加在末尾**（⛔ 不重排既有值：`SoftKind` 的值进不了产物，但重排会让
//   "哪一类"的判读随版本漂）。
enum class SoftKind : std::uint8_t { None = 0, Call = 1, StoreImm = 2, DataPtr = 3, JumpTab = 4 };

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
    out.prefix_max.resize(out.spans.size(), 0);
    for (std::size_t i = 0; i < out.spans.size(); ++i) {
        out.prefix_max[i] = (i == 0) ? out.spans[i].second : std::max(out.prefix_max[i - 1], out.spans[i].second);
    }
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
    // `soft_call` / `soft_store` / `soft_data` / `soft_jumptab` = 四类软根**各自的账**
    // （⛔ 不合并：D14 / D15 / D17 / D26 分开报）。
    Explorer(const lift::Decoder& decoder, const lift::GuestMemory& mem,
             const std::vector<lift::aot::ExecRange>& exec_ranges, std::uint32_t shim_begin,
             std::uint32_t shim_end, bool close_calls, bool close_stores, bool close_data,
             bool close_jmptab, SoftStats* soft_call, SoftStats* soft_store, SoftStats* soft_data,
             SoftStats* soft_jumptab)
        : decoder_(decoder),
          mem_(mem),
          exec_ranges_(exec_ranges),
          shim_begin_(shim_begin),
          shim_end_(shim_end),
          close_calls_(close_calls),
          close_stores_(close_stores),
          close_data_(close_data),
          close_jmptab_(close_jmptab),
          soft_call_(soft_call),
          soft_store_(soft_store),
          soft_data_(soft_data),
          soft_jumptab_(soft_jumptab) {}

    // 第一阶段：只展开 D14 / D15 两类软根（`call rel32` 目标 / 写内存 mov 的 imm32）到不动点。
    // ★ D17 的第三类候选**故意不在这里**：它的判据要用"已发射块"（见 `seedDataPointerRoots`），
    //   而块是在这一阶段收敛之后才建的 ⇒ 由 `buildUnits` 在两阶段之间调用。
    bool run(const std::set<std::uint32_t>& roots, DropStats* drops, std::string* err,
             std::uint32_t* err_ea) {
        entries_ = roots;
        leaders_ = roots;
        std::set<std::uint32_t> pending(roots.begin(), roots.end());
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
                        if (err != nullptr) *err = "派生 leader " + std::to_string(leader) + " 反复解码失败";
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
                            const std::uint32_t ret = in.ea + in.length;
                            entries_.insert(ret);
                            leaders_.insert(ret);
                            pending.insert(ret);
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
                            }
                            break;  // `jmp reg/mem` 的目标运行期才知道，静态无后继
                        case lift::Op::Jcc: {
                            const std::uint32_t target = lift::aot::genBranchTarget(in);
                            const std::uint32_t fall = in.ea + in.length;
                            leaders_.insert(target);
                            pending.insert(target);
                            leaders_.insert(fall);
                            pending.insert(fall);
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
                // 这条直线段的后缀已经被别的 leader 扫过 ⇒ 它的控制流后继也登记过了
                if (insns_.find(cur) != insns_.end()) {
                    reached_end = true;
                    break;
                }
            }
            if (!reached_end) {
                if (err != nullptr) *err = "直线扫描超过 1e6 条上限（可能落在数据里）";
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
        return SoftKind::None;
    }
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
                if (soft_data_ != nullptr) soft_data_->overlap.insert(ea);
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
    const std::set<std::uint32_t>& jumpTabSeeds() const { return jumptab_seeds_; }
    std::uint64_t jumpTabSites() const { return jumptab_sites_; }
    std::uint64_t jumpTabInExec() const { return jumptab_in_exec_; }
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
                if (soft_jumptab_ != nullptr) soft_jumptab_->overlap.insert(ea);
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
    void rejectSoftRoot(std::uint32_t ea) {
        soft_roots_.erase(ea);
        soft_roots_call_.erase(ea);
        soft_roots_store_.erase(ea);
        soft_roots_data_.erase(ea);
        soft_roots_jumptab_.erase(ea);
        entries_.erase(ea);
        leaders_.erase(ea);
        rejected_targets_.insert(ea);
    }

private:
    SoftStats* statsOf(SoftKind kind) const {
        switch (kind) {
            case SoftKind::Call: return soft_call_;
            case SoftKind::StoreImm: return soft_store_;
            case SoftKind::DataPtr: return soft_data_;
            case SoftKind::JumpTab: return soft_jumptab_;
            default: return nullptr;
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

    // ★ D14 / D15 / D17 的**唯一**收/丢判据：一个软根候选（`call rel32` 目标 / 写内存 mov 的 imm32 /
    //   `.data` 静态代码指针）怎么处理（收到入口集 / 跳过 / 丢弃）只在这里写一次。`kind` 决定记到
    //   哪一类的账上。★ 三类的开关**各自独立**（`--no-close-calls` / `--no-close-stores` /
    //   `--close-data`·`--no-close-data`），⛔ 不要合成一个"总开关"。
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
            default: break;
        }
        entries_.insert(target);
        leaders_.insert(target);
        pending->insert(target);
        if (stats != nullptr) stats->added.insert(target);
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
    SoftStats* soft_call_ = nullptr;   // D14 类（call 目标）的账
    SoftStats* soft_store_ = nullptr;  // D15 类（写内存 mov 的 imm32）的账
    SoftStats* soft_data_ = nullptr;   // D17 类（`.data` 静态代码指针）的账
    SoftStats* soft_jumptab_ = nullptr;  // D26 类（可执行节跳表 dword → 尚未解码的代码）的账
    std::map<std::uint32_t, lift::Insn> insns_;
    std::set<std::uint32_t> leaders_;
    std::set<std::uint32_t> entries_;
    std::set<std::uint32_t> soft_roots_;        // 四类软根的**并集**（closure.txt 之外的 unit 根）
    std::set<std::uint32_t> soft_roots_call_;   // 其中由 D14（call rel32 目标）收的
    std::set<std::uint32_t> soft_roots_store_;  // 其中由 D15（写内存 mov 的 imm32）收的
    std::set<std::uint32_t> soft_roots_data_;   // 其中由 D17（`.data` 静态代码指针）收的
    std::set<std::uint32_t> soft_roots_jumptab_;  // 其中由 D26（可执行节跳表 dword）收的
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
                    *err = "leader 的指令流里有一段没解出来（内部不一致）@ " + std::to_string(cur);
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
            if (err != nullptr) *err = "块扫描超过 1e6 条上限";
            if (err_leader != nullptr) *err_leader = leader;
            return false;
        }
        (*blocks)[leader] = std::move(block);
    }
    return true;
}

// 四类软根的账**选一份**（按 `kind`）。⛔ 不要把四类的丢弃合成一个数。
SoftStats* softStatsOf(SoftKind kind, SoftStats* soft_call, SoftStats* soft_store,
                       SoftStats* soft_data, SoftStats* soft_jumptab) {
    switch (kind) {
        case SoftKind::Call: return soft_call;
        case SoftKind::StoreImm: return soft_store;
        case SoftKind::DataPtr: return soft_data;
        case SoftKind::JumpTab: return soft_jumptab;
        default: return nullptr;
    }
}

// 建块，并按 D10 P3 ① / D14 / D15 / D17 / D26 丢弃"派生 leader 或软根的字节流里有一段解不出"的块（根 ⇒ 硬红）。
// ★ 软根的账**按类**记（`explorer->softKindOf`）：call / store-imm / data-ptr / jmptab 四类的"解不出"分开计数。
bool buildBlocksDropping(Explorer* explorer, std::set<std::uint32_t>* leaders,
                         const std::set<std::uint32_t>& roots, SoftStats* soft_call,
                         SoftStats* soft_store, SoftStats* soft_data, SoftStats* soft_jumptab,
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
            // ★ D14 / D15 / D17 / D26：软根解不出 ⇒ 丢弃 + 计数（⛔ 不硬红），记到**自己那一类**的账上。
            const SoftKind kind = explorer->softKindOf(bad);
            explorer->rejectSoftRoot(bad);
            drops->dropped.insert(bad);
            SoftStats* const st = softStatsOf(kind, soft_call, soft_store, soft_data, soft_jumptab);
            if (st != nullptr) st->undecodable.insert(bad);
        } else if (!drops->add_undecodable(bad)) {
            if (err != nullptr) *err = why + "（同一个派生 leader 反复失败）";
            if (err_ea != nullptr) *err_ea = bad;
            return false;
        }
        leaders->erase(bad);
    }
}

// D10 P3 ②：派生 leader（以及四类软根）落在**已发射块的字节区间内** ⇒ 丢弃（根永不丢；
// 返回 true = 有新的丢弃）。★ 软根的重叠与"解不出"**分开计数**，且**按类分开**（要求：不要混成一个数）。
// ★ 用"按起点排序 + 前缀最大 end"判定，允许块**任意重叠/嵌套**（T1a §4.3：块互不重叠并不成立）。
bool dropLeadersInsideBlocks(const std::map<std::uint32_t, GenBlock>& blocks,
                             const std::set<std::uint32_t>& roots, Explorer* explorer,
                             std::set<std::uint32_t>* leaders, DropStats* drops, SoftStats* soft_call,
                             SoftStats* soft_store, SoftStats* soft_data, SoftStats* soft_jumptab) {
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
            // ★ D14 / D15 / D17 / D26：软根与已发射块重叠 ⇒ 丢弃（与"解不出"分开计数，且**按类**分开）
            const SoftKind kind = explorer->softKindOf(ea);
            SoftStats* const st = softStatsOf(kind, soft_call, soft_store, soft_data, soft_jumptab);
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
// + D26 可执行节跳表 dword）+ 建块 + 派生/软根丢弃（主路径与 `--check` **共用同一份判据**，
// ⛔ 不许两处各写一套展开 / 丢弃规则 —— 那样 `--check` 的绿就不再说明主路径的绿）。
bool buildUnits(const std::set<std::uint32_t>& roots, Explorer* explorer, SoftStats* soft_call,
                SoftStats* soft_store, SoftStats* soft_data, SoftStats* soft_jumptab,
                std::map<std::uint32_t, GenBlock>* blocks, DropStats* drops, std::string* err,
                std::uint32_t* err_ea) {
    // 建块 + 派生/软根丢弃到不动点（丢弃是单调的 ⇒ 必然收敛）。
    auto blocksToFixpoint = [&](std::set<std::uint32_t>* leaders) -> bool {
        for (;;) {
            if (!buildBlocksDropping(explorer, leaders, roots, soft_call, soft_store, soft_data,
                                     soft_jumptab, blocks, drops, err, err_ea))
                return false;
            // 丢弃后块边界会变 ⇒ 重建到不动点。
            if (!dropLeadersInsideBlocks(*blocks, roots, explorer, leaders, drops, soft_call,
                                         soft_store, soft_data, soft_jumptab))
                break;
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
            if (err != nullptr) *err = "第五类软根（D26）的不动点迭代超过轮数上限";
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
        *err = "打不开 `" + path + "`";
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
            *err = "第 " + std::to_string(lineno) + " 行不是 8 位以内的十六进制 EA：`" + line + "`";
            return false;
        }
        roots->insert(static_cast<std::uint32_t>(std::strtoul(token.c_str(), nullptr, 16)));
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
int checkClosure(const std::string& exe, const std::string& path, bool close_calls, bool close_stores,
                 bool close_data, bool close_jmptab) {
    std::vector<std::string> violations;
    std::size_t format_bad = 0;
    std::size_t order_bad = 0;
    std::vector<std::uint32_t> eas;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "emulator-lift-gen: --check 打不开 `%s`\n", path.c_str());
            return 2;
        }
        std::string line;
        int lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            const std::string token = lift::aot::stripCommentAndTrim(line);
            if (token.empty()) continue;
            std::uint32_t ea = 0;
            if (!lift::aot::strictParseEa(token, &ea)) {
                ++format_bad;
                violations.push_back("第 " + std::to_string(lineno) + " 行不是 `0x` + 8 位十六进制：`" +
                                     token + "`");
                continue;
            }
            if (!eas.empty() && ea <= eas.back()) {
                ++order_bad;
                char buf[128];
                std::snprintf(buf, sizeof(buf), "第 %d 行不是严格升序（%08X 在 %08X 之后 / 重复）", lineno, ea,
                              eas.back());
                violations.push_back(buf);
            }
            eas.push_back(ea);
        }
    }
    std::set<std::uint32_t> roots(eas.begin(), eas.end());
    if (roots.empty()) {
        std::fprintf(stderr, "emulator-lift-gen: --check `%s` 里一个 EA 都没有\n", path.c_str());
        return 2;
    }

    // ---- PE 字节（与主路径同一套：mem.reserve → loadPeFile → mapPeImage → Decoder）----
    lift::GuestMemory mem;
    if (!mem.reserve()) {
        std::fprintf(stderr, "emulator-lift-gen: 预留 guest 地址空间失败：%s\n", mem.lastError());
        return 2;
    }
    lift::PeImage image;
    {
        std::string err;
        if (!lift::loadPeFile(exe, &image, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: 载入 PE 失败（%s）：%s\n", exe.c_str(), err.c_str());
            return 2;
        }
        if (!lift::mapPeImage(mem, image, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: 映射 PE 失败（%s）：%s\n", exe.c_str(), err.c_str());
            return 2;
        }
    }
    lift::Decoder decoder;
    if (!decoder.ok()) {
        std::fprintf(stderr, "emulator-lift-gen: 解码器初始化失败：%s\n", decoder.lastError());
        return 4;
    }
    const std::vector<lift::aot::ExecRange> ranges = lift::aot::execRanges(image);

    // ---- 逐根：在可执行节 + 可解码 ----
    std::size_t out_of_text = 0;
    std::size_t undecodable = 0;
    for (const std::uint32_t ea : roots) {
        if (lift::aot::execRangeOf(ranges, ea) == nullptr) {
            ++out_of_text;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "EA %08X 不在任何可执行节（越界根）", ea);
            violations.push_back(buf);
            continue;
        }
        lift::Insn in;
        std::string why;
        if (!decoder.decode(mem, ea, &in, &why)) {
            ++undecodable;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "EA %08X 不可解码：%s", ea, why.c_str());
            violations.push_back(buf);
        }
    }
    if (roots.count(image.entry_ea) == 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "PE entry %08X 不在闭包里（起跑线口径）", image.entry_ea);
        violations.push_back(buf);
    }

    // ---- 覆盖块数：静态可达性用主路径**同一份** Explorer（⛔ 不另写一份展开规则）----
    // ★ T1c：派生 leader 的丢弃规则也走**同一份**（`buildUnits`）⇒ `blocks` 读数与 `lift-gen`
    //   的 `blocks=` 严格一致（D10 P3 的账差收口就靠这条）。
    std::size_t covered_blocks = 0;
    std::size_t covered_soft_roots_call = 0;
    std::size_t covered_soft_roots_store = 0;
    std::size_t covered_soft_roots_data = 0;
    std::size_t covered_soft_roots_jumptab = 0;
    bool expand_ok = false;
    if (violations.empty()) {
        const lift::GuestLayout layout;
        const std::uint32_t shim_end = layout.shim_page + layout.shim_slots * 4u;
        SoftStats soft_call;
        SoftStats soft_store;
        SoftStats soft_data;
        SoftStats soft_jumptab;
        Explorer explorer(decoder, mem, ranges, layout.shim_page, shim_end, close_calls,
                          close_stores, close_data, close_jmptab, &soft_call, &soft_store, &soft_data,
                          &soft_jumptab);
        // ★ D17：第三类软根的候选集（非可执行节里静态初始化的代码指针）—— 与主路径同一份扫描。
        const DataPtrScan data_scan = scanDataPointers(image, ranges);
        explorer.setDataPointerSeeds(data_scan.targets, data_scan.sites, data_scan.in_exec);
        // ★ D26：第五类软根的候选集（可执行节里值指向可执行节内的 dword）—— 与主路径同一份扫描。
        const JumpTabScan jumptab_scan = scanExecCodeDwords(image, ranges);
        explorer.setJumpTableSeeds(jumptab_scan.targets, jumptab_scan.sites, jumptab_scan.in_exec);
        std::map<std::uint32_t, GenBlock> blocks;
        DropStats drops;
        std::string err;
        std::uint32_t err_ea = 0;
        if (buildUnits(roots, &explorer, &soft_call, &soft_store, &soft_data, &soft_jumptab, &blocks,
                       &drops, &err, &err_ea)) {
            covered_blocks = blocks.size();
            covered_soft_roots_call = soft_call.added.size();
            covered_soft_roots_store = soft_store.added.size();
            covered_soft_roots_data = soft_data.added.size();
            covered_soft_roots_jumptab = soft_jumptab.added.size();
            expand_ok = true;
        } else {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "静态展开解码失败 @ %08X：%s", err_ea, err.c_str());
            violations.push_back(buf);
        }
    }

    for (const std::string& v : violations) std::fprintf(stderr, "emulator-lift-gen --check: %s\n", v.c_str());
    // ★ D15 / D17 / D26：四类软根**分开报**（`soft-roots` 仍是并集，后面括号里是拆分）。
    std::printf(
        "lift-gen --check: roots=%zu soft-roots=%zu(call=%zu store-imm=%zu data-ptr=%zu jmptab=%zu) "
        "blocks=%zu out-of-text=%zu undecodable=%zu order=%s entry=%s => %s\n",
        roots.size(),
        covered_soft_roots_call + covered_soft_roots_store + covered_soft_roots_data +
            covered_soft_roots_jumptab,
        covered_soft_roots_call, covered_soft_roots_store, covered_soft_roots_data,
        covered_soft_roots_jumptab, covered_blocks, out_of_text, undecodable,
        order_bad == 0 ? "ok" : "bad", roots.count(image.entry_ea) == 0 ? "missing" : "present",
        (violations.empty() && expand_ok) ? "PASS" : "FAIL");
    return (violations.empty() && expand_ok) ? 0 : 2;
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
                 std::size_t entries, std::size_t functions, std::string* err) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (err) *err = "写不了 `" + path + "`";
        return false;
    }
    file << "// 生成物（emulator-lift-gen --pin）—— ⛔ 不手改、⛔ 不入库。\n";
    file << "//\n";
    file << "// ★ 这是 D1 的判据载体：lifted 代码在**构建期**把 EA / 块边界 / 操作数语义焊死在这一份 PE\n";
    file << "//   上；换一份 exe 不会\"跑另一个游戏\"，只会静默跑错 ⇒ AOT 启动时先核对 sha256 + entry，\n";
    file << "//   不一致就是 exit 2（⛔ 不降级、不警告后继续）。\n";
    file << "// ★ 只写机械事实：没有时间戳、没有路径、没有主机名 ⇒ 同一份 PE 两次生成同字节。\n";
    file << "#pragma once\n\n";
    file << "#define LIFT_AOT_GENERATED 1\n";
    file << "#define LIFT_AOT_PIN_SHA256 \"" << image.sha256 << "\"\n";
    file << "#define LIFT_AOT_PIN_ENTRY 0x" << hexEa(image.entry_ea) << "u\n";
    file << "#define LIFT_AOT_PIN_ROOTS " << roots << "u\n";
    file << "#define LIFT_AOT_PIN_ENTRIES " << entries << "u\n";
    file << "#define LIFT_AOT_PIN_FUNCTIONS " << functions << "u\n";
    file.close();
    if (!file) {
        if (err) *err = "写 `" + path + "` 失败";
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
                    std::snprintf(buf, sizeof(buf), "入口 EA %08X 同时属于两个分量（%08X / %08X）",
                                  ea, ins.first->second, comp.root);
                    *err = buf;
                }
                return false;
            }
        }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (err) *err = "写不了 `" + path + "`";
        return false;
    }
    file << "// 生成物（emulator-lift-gen --table）—— ⛔ 不手改、⛔ 不入库。\n";
    file << "// 一行一个**入口 EA**：`LIFT_AOT_ENTRY(0x<EA>u, lift_cc_<该 EA 所在分量的根>)`。\n";
    file << "// ★ 同一个函数会重复出现（一个分量有多个 resume EA）—— 那不是重复，是 switch 的多个 label。\n";
    file << "// ★ 只许有真实存在的入口：每一行都在对应函数体的 `switch i32 %ea` 里有 label。\n";
    for (const auto& kv : ea_to_root) {
        // ★ 函数名走 `genFunctionName`（与 `.ll` 的 `define i32 @…` 是同一份拼法）。
        file << "LIFT_AOT_ENTRY(0x" << hexEa(kv.first) << "u, " << lift::aot::genFunctionName(kv.second)
             << ")\n";
    }
    file.close();
    if (!file) {
        if (err) *err = "写 `" + path + "` 失败";
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
        if (err != nullptr) *err = "写不了 `" + path + "`";
        return false;
    }
    file << "# emulator-lift-gen --dump-blocks —— 只读覆盖率读数（⛔ 不参与 .ll / 派发表）\n";
    file << "# 格式：`0x<8 位大写 start EA> <字节长度> <指令数>`，按 start EA 升序；每行 = 一个已发射块\n";
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
        if (err != nullptr) *err = "写 `" + path + "` 失败";
        return false;
    }
    if (out_blocks != nullptr) *out_blocks = rows.size();
    if (out_bytes != nullptr) *out_bytes = bytes;
    if (out_insns != nullptr) *out_insns = insns;
    return true;
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
                std::fprintf(stderr, "emulator-lift-gen: %s 需要一个值\n", flag);
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
        } else if (a == "--pin") {
            if (!needValue("--pin", &opt.pin)) return 2;
        } else if (a == "--table") {
            if (!needValue("--table", &opt.table)) return 2;
        } else if (a == "--no-close-calls") {
            opt.close_calls = false;
        } else if (a == "--no-close-stores") {
            opt.close_stores = false;
        } else if (a == "--max-components") {
            std::string v;
            if (!needValue("--max-components", &v)) return 2;
            char* endp = nullptr;
            const unsigned long n = std::strtoul(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0' || n == 0) {
                std::fprintf(stderr, "emulator-lift-gen: --max-components 需要正整数：`%s`\n", v.c_str());
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
        } else if (a == "--dump-blocks") {
            if (!needValue("--dump-blocks", &opt.dump_blocks)) return 2;
        } else if (a == "--verbose") {
            opt.verbose = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "emulator-lift-gen: 未知参数：%s\n", a.c_str());
            usage();
            return 2;
        }
    }
    // ★ `--check` 是**独立模式**：只吃 `--exe` + 一个 closure.txt，⛔ 不需要 `--roots/--out`。
    if (!opt.check.empty()) {
        if (opt.exe.empty()) {
            std::fprintf(stderr, "emulator-lift-gen: --check 需要 --exe（字节真源）\n");
            return 2;
        }
        return checkClosure(opt.exe, opt.check, opt.close_calls, opt.close_stores, opt.close_data,
                            opt.close_jmptab);
    }
    if (opt.exe.empty() || opt.roots.empty() || opt.out.empty()) {
        std::fprintf(stderr, "emulator-lift-gen: --exe / --roots / --out 都是必需的\n");
        usage();
        return 2;
    }

    // ---- 输入 ①：入口 EA 集合 ----
    std::set<std::uint32_t> roots;
    {
        std::string err;
        if (!readRootsFile(opt.roots, &roots, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: --roots 读不了：%s\n", err.c_str());
            return 2;
        }
    }
    if (roots.empty()) {
        std::fprintf(stderr, "emulator-lift-gen: --roots 里一个 EA 都没有：%s\n", opt.roots.c_str());
        return 2;
    }

    // ---- 输入 ②：PE 字节（映射进 guest 后交给唯一的解码器）----
    lift::GuestMemory mem;
    if (!mem.reserve()) {
        std::fprintf(stderr, "emulator-lift-gen: 预留 guest 地址空间失败：%s\n", mem.lastError());
        return 2;
    }
    lift::PeImage image;
    {
        std::string err;
        if (!lift::loadPeFile(opt.exe, &image, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: 载入 PE 失败（%s）：%s\n", opt.exe.c_str(), err.c_str());
            return 2;
        }
        if (!lift::mapPeImage(mem, image, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: 映射 PE 失败（%s）：%s\n", opt.exe.c_str(), err.c_str());
            return 2;
        }
    }
    lift::Decoder decoder;
    if (!decoder.ok()) {
        std::fprintf(stderr, "emulator-lift-gen: 解码器初始化失败：%s\n", decoder.lastError());
        return 4;
    }

    // ---- ① leader / 入口 EA ----
    // ★ 根必须落在**可执行节**里（越界根 = 闭包本身错 ⇒ 硬红 exit 4；⛔ 不许与派生噪声混在一起丢）。
    const std::vector<lift::aot::ExecRange> exec_ranges = lift::aot::execRanges(image);
    for (const std::uint32_t ea : roots) {
        if (lift::aot::execRangeOf(exec_ranges, ea) == nullptr) {
            std::fprintf(stderr, "emulator-lift-gen: 根 %08X 不在任何可执行节（越界根）\n", ea);
            return 4;
        }
    }
    DropStats drops;
    // ★ D14 / D15 / D17 / D26：四类软根**各一份账**（⛔ 不合并：读数要能回答"是哪一类出的问题"）。
    SoftStats soft_call;
    SoftStats soft_store;
    SoftStats soft_data;
    SoftStats soft_jumptab;
    // ★ D14：shim 桩页 = `GuestLayout::shim_page` 起 `shim_slots * 4` 字节（与 `aot_run.cpp` /
    //   `aot_host.cpp` 同一份判据）。桩 EA 由 trampoline 派发 ⇒ ⛔ 不收成软根、也不算丢弃。
    const lift::GuestLayout layout;
    const std::uint32_t shim_end = layout.shim_page + layout.shim_slots * 4u;
    Explorer explorer(decoder, mem, exec_ranges, layout.shim_page, shim_end, opt.close_calls,
                      opt.close_stores, opt.close_data, opt.close_jmptab, &soft_call, &soft_store,
                      &soft_data, &soft_jumptab);
    // ★ D17：第三类软根的候选集（**非可执行节**里 4 字节对齐、值落在可执行节内的 dword）。
    //   扫描总是做（它是读数：站点数 / 落在可执行节内的站点数），是否**收**由 `offerSoftRoot` 按
    //   `close_data_` 决定 ⇒ 关掉时读数仍在，`.ll` / 派发表一个字节都不多。
    const DataPtrScan data_scan = scanDataPointers(image, exec_ranges);
    explorer.setDataPointerSeeds(data_scan.targets, data_scan.sites, data_scan.in_exec);
    // ★ D26：第五类软根的候选集（**可执行节**里 4 字节对齐、值也落在可执行节内的 dword）。
    //   同样"扫描总是做"（它给出候选面读数），是否**收**由 `seedJumpTableCodeRoots` 按 `close_jmptab_` 决定。
    const JumpTabScan jumptab_scan = scanExecCodeDwords(image, exec_ranges);
    explorer.setJumpTableSeeds(jumptab_scan.targets, jumptab_scan.sites, jumptab_scan.in_exec);

    // ---- ② 基本块（派生 leader / 软根可丢、根不可丢；D10 P3 ①② + D14 + D15 + D17 + D26）----
    std::map<std::uint32_t, GenBlock> blocks;
    {
        std::string err;
        std::uint32_t err_ea = 0;
        if (!buildUnits(roots, &explorer, &soft_call, &soft_store, &soft_data, &soft_jumptab, &blocks,
                        &drops, &err, &err_ea)) {
            std::fprintf(stderr, "emulator-lift-gen: 建块/解码失败 @ %08X：%s\n", err_ea, err.c_str());
            return 4;
        }
    }

    // ---- ③ 分区（入口 = 根 ∪ 软根 ∪ 全部 call 的下一条 EA；冲突 ⇒ 提升成新根重跑）----
    std::set<std::uint32_t> promoted;
    // ★ G3：入口 EA 里"**没有块**"的那些（落在别人的块字节里 / 解不出）—— 提升它们只会让分区
    //   永远收敛不了（G3 首跑实测 `00404046`）⇒ 单独记账、不提升（见下面哨兵那一段）。
    std::set<std::uint32_t> unbuildable;
    std::vector<GenComponent> components;
    std::set<std::uint32_t> all_entries;
    // ★ D21（第四类）：跳表 dword 指向的已发射块首 —— 只进 switch / 派发表，⛔ 不新增块 / 指令。
    JumpTableStats jump_table;
    std::size_t promo_events = 0;
    for (;;) {
        const std::set<std::uint32_t>& resume_entries = explorer.entries();
        std::set<std::uint32_t> root_set = roots;
        // ★ D14：静态闭包收来的软根也是 **unit 根**（一个软根 = 一个 `.ll` 函数）。
        root_set.insert(explorer.softRoots().begin(), explorer.softRoots().end());
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
                    if (drops.dropped.count(edge.ea) != 0) continue;
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
                        std::fprintf(stderr,
                                     "lift-gen: 跳表目标 %08X 是块首但没有任何分量声明"
                                     "⇒ 提升为新入口 EA 并重跑\n",
                                     ea);
                    }
                }
                if (n_jump_promoted == 0) {
                    // 已经在 promoted 里却仍没有分量 claim ⇒ 与"块冲突提升不收敛"同一类内部不一致。
                    std::fprintf(stderr, "emulator-lift-gen: 跳表目标的孤儿提升不收敛\n");
                    return 1;
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
                    drops.dropped.insert(ea);
                    explorer.dropEntry(ea);
                    ++n_unbuildable;
                    continue;
                }
                if (promoted.insert(ea).second) {
                    std::fprintf(stderr,
                                 "lift-gen: 入口 %08X 没有任何分量声明（只在被拒绝的软根段之后可达）"
                                 "⇒ 提升为新入口 EA 并重跑\n",
                                 ea);
                }
            }
            if (n_unbuildable != 0) {
                std::fprintf(stderr,
                             "lift-gen: 入口里有 %zu 个**没有块**（落在已发射块的字节区间内 / 解不出）"
                             "⇒ 不提升、退出入口集（指向它们的边发 ret i32）\n",
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
            drops.dropped.insert(conflict);
            explorer.dropEntry(conflict);
            std::fprintf(stderr,
                         "lift-gen: 块冲突 EA %08X 没有块（落在已发射块字节区间内 / 解不出）"
                         "⇒ 退出入口集（指向它的边发 ret i32）\n",
                         conflict);
            ++promo_events;
            components.clear();
            continue;
        }
        std::fprintf(stderr,
                     "lift-gen: 块 %08X 被两个入口函数同时声明 ⇒ 提升为新入口 EA 并重跑\n", conflict);
        if (!promoted.insert(conflict).second) {
            std::fprintf(stderr, "emulator-lift-gen: 分区提升不收敛（EA %08X 重复冲突）\n", conflict);
            return 1;
        }
        ++promo_events;
        components.clear();
    }

    // 收敛后按 root 升序（std::set 已经保证；这里再按 root 排一次以防未来改动）。
    std::sort(components.begin(), components.end(),
              [](const GenComponent& a, const GenComponent& b) { return a.root < b.root; });

    if (opt.max_components != 0 && components.size() > opt.max_components) {
        std::fprintf(stderr, "emulator-lift-gen: 分量数 %zu 超过 --max-components %lu\n",
                     components.size(), opt.max_components);
        return 2;
    }

    // ★ D21（第四类）：**派发表的入口集** = 原入口集 ∪ 跳表加进来的 EA。
    //   ⛔ 与传给发射器的 `all_entries` 刻意分开：后者是"边目标算不算入口"的判据 ⇒ 若把跳表目标
    //   也放进去，原本在同一分量内 `br` 直跳的静态边会平白变成 `ret i32`（多一次派发）。
    //   两者的关系是机械的：`dispatch_entries.size() == all_entries.size() + jump_table.added.size()`
    //   （`addJumpTableEntries` 只加不在入口集里的）⇒ 派发表行数 = pin 的 entries 读数。
    std::set<std::uint32_t> dispatch_entries = all_entries;
    dispatch_entries.insert(jump_table.added.begin(), jump_table.added.end());

    // ---- ④ 发射 ----
    std::string ir;
    GenEmitStats stats;
    std::vector<GenUnsupported> unsupported;
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
    closure.store_imm_sites = explorer.storeImmSites();
    closure.store_imm_in_exec = explorer.storeImmInExec();
    // ★ D17：第三类软根（`.data` 静态代码指针）—— 与上面两组**分开**，⛔ 不合并。
    closure.close_data = opt.close_data;
    closure.data_soft_roots = soft_data.added.size();
    closure.data_outside = soft_data.outside.size();
    closure.data_undecodable = soft_data.undecodable.size();
    closure.data_overlap = soft_data.overlap.size();
    closure.data_overlap_pre = explorer.dataPtrOverlapPre();
    closure.data_shim_targets = soft_data.shim_targets.size();
    closure.data_ptr_sites = explorer.dataPtrSites();
    closure.data_ptr_in_exec = explorer.dataPtrInExec();
    closure.data_ptr_distinct = explorer.dataPtrDistinct();
    closure.data_ptr_skipped = explorer.dataPtrSkipped();
    closure.unbuildable_entries = unbuildable.size();
    // ★ D26：第五类软根（可执行节跳表 dword → 尚未解码的代码）—— 与上面三组**分开**，⛔ 不合并。
    closure.close_jmptab = opt.close_jmptab;
    closure.jumptab_soft_roots = soft_jumptab.added.size();
    closure.jumptab_outside = soft_jumptab.outside.size();
    closure.jumptab_undecodable = soft_jumptab.undecodable.size();
    closure.jumptab_overlap = soft_jumptab.overlap.size();
    closure.jumptab_shim_targets = soft_jumptab.shim_targets.size();
    closure.jumptab_rounds = explorer.jumpTabRounds();
    closure.jumptab_live_roots = explorer.liveJumpTabRoots();
    {
        const JumpTabSurface& s = explorer.jumpTabSurface();
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
    if (!lift::aot::emitModule(components, all_entries, drops.dropped, closure, &ir, &stats,
                               &unsupported)) {
        std::fprintf(stderr, "emulator-lift-gen: 发射器内部错误\n");
        return 1;
    }

    // 计数（父级要求的读数）
    std::uint64_t block_count = 0;
    std::uint64_t insn_count = 0;
    for (const GenComponent& comp : components) {
        block_count += comp.blocks.size();
        for (const auto& kv : comp.blocks) insn_count += kv.second.insns.size();
    }

    // ---- ⑤ 落盘（`.ll` 不 commit：落点由调用方指定，通常 out/aot/）----
    const std::filesystem::path out_path(opt.out);
    const std::filesystem::path out_dir =
        out_path.has_parent_path() ? out_path.parent_path() : std::filesystem::path(".");
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    {
        std::ofstream file(out_path, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::fprintf(stderr, "emulator-lift-gen: 写不了 `%s`\n", out_path.string().c_str());
            return 2;
        }
        file.write(ir.data(), static_cast<std::streamsize>(ir.size()));
        file.close();
        if (!file) {
            std::fprintf(stderr, "emulator-lift-gen: 写 `%s` 失败\n", out_path.string().c_str());
            return 2;
        }
    }

    // ---- ⑤' `--pin` / `--table`（可选；AOT 构建才要）----
    // ★ 写在 `.ll` 之后、`unsupported` 之前：这两份是"运行期要的机械数据"，与 trap 清单无关。
    //   ⛔ 缺省不写：测试夹具的 golden 生成（`lift-ir-gen-*`）与 `--check` 都不需要它们。
    if (!opt.pin.empty()) {
        std::string err;
        if (!writeAotPin(opt.pin, image, roots.size(), dispatch_entries.size(), components.size(), &err)) {
            std::fprintf(stderr, "emulator-lift-gen: --pin 失败：%s\n", err.c_str());
            return 2;
        }
        std::printf("lift-gen: pin %s（sha256=%s entry=%08X）\n", opt.pin.c_str(), image.sha256.c_str(),
                    image.entry_ea);
    }
    if (!opt.table.empty()) {
        std::string err;
        if (!writeAotTable(opt.table, components, &err)) {
            std::fprintf(stderr, "emulator-lift-gen: --table 失败：%s\n", err.c_str());
            return 1;
        }
        std::size_t rows = 0;
        for (const GenComponent& comp : components) rows += comp.entries.size();
        std::printf("lift-gen: 派发表 %s（%zu 行 = 入口 EA → 分量根函数）\n", opt.table.c_str(), rows);
    }
    // ---- ⑤'' ★ D18：`--dump-blocks`（只读覆盖率读数；⛔ 不改 `.ll` / 派发表）----
    if (!opt.dump_blocks.empty()) {
        std::string err;
        std::size_t dumped_blocks = 0;
        std::uint64_t dumped_bytes = 0;
        std::uint64_t dumped_insns = 0;
        if (!writeBlockDump(opt.dump_blocks, components, &dumped_blocks, &dumped_bytes, &dumped_insns,
                            &err)) {
            std::fprintf(stderr, "emulator-lift-gen: --dump-blocks 失败：%s\n", err.c_str());
            return 2;
        }
        std::printf("lift-gen: 块清单 %s（blocks=%zu bytes=%llu insns=%llu；⛔ 不进 .ll / 派发表）\n",
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
    const std::filesystem::path unsupported_path = out_dir / "unsupported.txt";
    {
        std::ofstream file(unsupported_path, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::fprintf(stderr, "emulator-lift-gen: 写不了 `%s`\n", unsupported_path.string().c_str());
            return 2;
        }
        file << "# emulator-lift-gen（Shape A / T1c）：**仍然 trap** 的 guest 指令\n";
        file << "# 格式：0x<EA> <op> <原因>；这些位置在 .ll 里是 llvm.trap + unreachable\n";
        file << "# ★ T1c 起「未实现的 op」不再落这里（走单指令宿主原语 @lift_one）；这里为空是正常态，\n";
        file << "#   有内容 ⇒ 控制转移掉进了默认路径 = 生成器内部不一致（退出码 3）。\n";
        for (const GenUnsupported& u : unsupported) {
            char ea[16];
            std::snprintf(ea, sizeof(ea), "0x%08X", u.ea);
            file << ea << " " << u.op << " " << u.reason << "\n";
        }
        file.close();
        if (!file) {
            std::fprintf(stderr, "emulator-lift-gen: 写 `%s` 失败\n", unsupported_path.string().c_str());
            return 2;
        }
    }

    // ---- ⑥ 读数 ----
    std::printf(
        "lift-gen: roots=%zu entries=%zu functions=%zu blocks=%llu insns=%llu promotions=%zu "
        "traps=%llu(%llu unsupported + %llu bad)\n",
        roots.size(), dispatch_entries.size(), components.size(),
        static_cast<unsigned long long>(block_count), static_cast<unsigned long long>(insn_count),
        promoted.size(), static_cast<unsigned long long>(stats.traps_unsupported + stats.traps_bad),
        static_cast<unsigned long long>(stats.traps_unsupported),
        static_cast<unsigned long long>(stats.traps_bad));
    std::printf("lift-gen: lift_one 调用点 %llu 处（%zu 个 op；单指令宿主原语，⛔ 不是 trap）\n",
                static_cast<unsigned long long>(stats.lift_one_sites), stats.lift_one_by_op.size());
    {
        std::printf("lift-gen: lift_one 汇总：");
        bool first = true;
        for (const auto& kv : stats.lift_one_by_op) {
            std::printf("%s%s=%llu", first ? "" : " ", kv.first.c_str(),
                        static_cast<unsigned long long>(kv.second));
            first = false;
        }
        std::printf("\n");
    }
    std::printf("lift-gen: 派生 leader 丢弃 %zu 处（%zu 解不出 + %zu 落在已发射块字节内）\n",
                drops.dropped.size(), drops.undecodable.size(), drops.inside_block.size());
    // ★ D14：静态调用闭包的账 —— 与上面"派生 leader"**分开报**（越界 / 解不出 / 重叠各一个数）。
    //   同一条也打到 stderr（要求：不静默），并把每个丢弃类别的 **3 个样例**打出来。
    std::printf(
        "lift-gen: 静态调用闭包 %s soft-roots=%zu（新增 unit 根）· 丢弃：越界=%zu 解不出=%zu "
        "重叠=%zu · shim 目标跳过=%zu\n",
        opt.close_calls ? "on" : "off", soft_call.added.size(), soft_call.outside.size(),
        soft_call.undecodable.size(), soft_call.overlap.size(), soft_call.shim_targets.size());
    std::fprintf(stderr,
                 "lift-gen: [静态调用闭包 D14/%s] soft-roots=%zu 丢弃：越界=%zu 解不出=%zu 重叠=%zu "
                 "· shim 目标跳过=%zu\n",
                 opt.close_calls ? "on" : "off", soft_call.added.size(), soft_call.outside.size(),
                 soft_call.undecodable.size(), soft_call.overlap.size(), soft_call.shim_targets.size());
    // ★ D15：第二类软根（写内存 mov 的 imm32）—— **独立一行**，三档丢弃同样各一个数。
    //   `写内存 mov 的 imm32 站点` 是判据的**输入面**（读数，不是判据）：越界那一个数之所以大，
    //   就是因为绝大多数 `mov [x], <数据常量>` 的 imm 不是代码地址（站点 − 落在可执行节内 ≈ 它们）。
    std::printf(
        "lift-gen: 代码指针立即数（D15）%s soft-roots=%zu（新增 unit 根）· 丢弃：越界=%zu 解不出=%zu "
        "重叠=%zu · shim 目标跳过=%zu\n",
        opt.close_stores ? "on" : "off", soft_store.added.size(), soft_store.outside.size(),
        soft_store.undecodable.size(), soft_store.overlap.size(), soft_store.shim_targets.size());
    std::printf("lift-gen: 写内存 mov 的 imm32：站点=%llu（其中 imm 落在可执行节内=%llu）\n",
                static_cast<unsigned long long>(explorer.storeImmSites()),
                static_cast<unsigned long long>(explorer.storeImmInExec()));
    std::fprintf(stderr,
                 "lift-gen: [代码指针立即数 D15/%s] soft-roots=%zu 丢弃：越界=%zu 解不出=%zu 重叠=%zu "
                 "· shim 目标跳过=%zu · 站点=%llu（imm 落在可执行节内=%llu）\n",
                 opt.close_stores ? "on" : "off", soft_store.added.size(), soft_store.outside.size(),
                 soft_store.undecodable.size(), soft_store.overlap.size(),
                 soft_store.shim_targets.size(),
                 static_cast<unsigned long long>(explorer.storeImmSites()),
                 static_cast<unsigned long long>(explorer.storeImmInExec()));
    // ★ D17：第三类软根（`.data` 静态代码指针）—— **独立一行**，三档丢弃同样各一个数。
    //   两条读数（判据的**输入面**，⛔ 不是判据）：扫过的 dword 站点 / 其中值落在可执行节内的站点；
    //   再去重才是候选数（`distinct`），候选里"已在入口集 / 已判过"的不重复收（`skipped`）。
    //   ⇒ 恒等式（可手工核）：`distinct = added + overlap + skipped`（outside / shim 结构性为 0：
    //   候选的定义就是"值落在可执行节内"、而 shim 桩页不在可执行节里）。
    std::printf(
        "lift-gen: 静态代码指针（D17）%s soft-roots=%zu（新增 unit 根）· 丢弃：越界=%zu 解不出=%zu "
        "重叠=%zu · shim 目标跳过=%zu\n",
        opt.close_data ? "on" : "off", soft_data.added.size(), soft_data.outside.size(),
        soft_data.undecodable.size(), soft_data.overlap.size(), soft_data.shim_targets.size());
    std::printf(
        "lift-gen: 非可执行节的 4 字节对齐 dword：站点=%llu（值落在可执行节内=%llu）· 不同候选=%llu "
        "· 已在入口集/已判过=%llu\n",
        static_cast<unsigned long long>(explorer.dataPtrSites()),
        static_cast<unsigned long long>(explorer.dataPtrInExec()),
        static_cast<unsigned long long>(explorer.dataPtrDistinct()),
        static_cast<unsigned long long>(explorer.dataPtrSkipped()));
    // ★ 恒等式（可手工核）：`不同候选 = 新增 + 重叠 + 已在入口集/已判过`，
    //   其中"重叠"的**收之前挡下**那部分（`data_overlap_pre`）是判据前移挡的，其余是收进来之后
    //   由 `dropLeadersInsideBlocks` 事后判的（那一部分同时也在 `新增` 里 —— `added` 记的是
    //   "被接受为软根"，与 D14/D15 同一口径）。
    std::printf("lift-gen: 其中收之前就挡下（落在已发射块字节区间内）= %llu\n",
                static_cast<unsigned long long>(explorer.dataPtrOverlapPre()));
    std::fprintf(stderr,
                 "lift-gen: [静态代码指针 D17/%s] soft-roots=%zu 丢弃：越界=%zu 解不出=%zu 重叠=%zu "
                 "· shim 目标跳过=%zu · 站点=%llu（值落在可执行节内=%llu）· 不同候选=%llu · 已在入口集/已判过=%llu\n",
                 opt.close_data ? "on" : "off", soft_data.added.size(), soft_data.outside.size(),
                 soft_data.undecodable.size(), soft_data.overlap.size(),
                 soft_data.shim_targets.size(),
                 static_cast<unsigned long long>(explorer.dataPtrSites()),
                 static_cast<unsigned long long>(explorer.dataPtrInExec()),
                 static_cast<unsigned long long>(explorer.dataPtrDistinct()),
                 static_cast<unsigned long long>(explorer.dataPtrSkipped()));
    // ★ D26：第五类软根（可执行节跳表 dword → **尚未解码**的代码）—— **独立一行**，三档丢弃各一个数；
    //   再一行是**候选面**（"先量后收"：每一档只归一类，五档相加 = `不同候选`）。
    //   ★ 与 D17 的本质区别：这里的 `added` 是**新增 unit 根 = 新增块 / 新增指令**（D17 那行不是）。
    std::printf(
        "lift-gen: 可执行节跳表目标（D26）%s soft-roots=%zu（新增 unit 根 = **会新增块**）· 丢弃：越界=%zu "
        "解不出=%zu 重叠=%zu · shim 目标跳过=%zu · 不动点轮数=%llu · 最终仍在入口集=%zu\n",
        opt.close_jmptab ? "on" : "off", soft_jumptab.added.size(), soft_jumptab.outside.size(),
        soft_jumptab.undecodable.size(), soft_jumptab.overlap.size(),
        soft_jumptab.shim_targets.size(),
        static_cast<unsigned long long>(explorer.jumpTabRounds()),
        explorer.liveJumpTabRoots());
    {
        const JumpTabSurface& s = explorer.jumpTabSurface();
        std::printf(
            "lift-gen: 可执行节的 4 字节对齐 dword 候选面：站点=%llu · 值在可执行节内=%llu · 不同候选=%llu "
            "· 已在入口集/已判过=%llu · 已是块首（第四类 D21 处理）=%llu · 可解码=%llu · 解不出=%llu "
            "· 被重叠挡下=%llu · shim 桩=%llu\n",
            static_cast<unsigned long long>(s.sites), static_cast<unsigned long long>(s.in_exec),
            static_cast<unsigned long long>(s.distinct),
            static_cast<unsigned long long>(s.in_entries),
            static_cast<unsigned long long>(s.block_starts),
            static_cast<unsigned long long>(s.decodable),
            static_cast<unsigned long long>(s.undecodable_pre),
            static_cast<unsigned long long>(s.overlap_pre), static_cast<unsigned long long>(s.shim));
        // ★ 恒等式（可手工核）：`不同候选 = 已在入口集/已判过 + shim 桩 + 已是块首 + 被重叠挡下 + 可解码 + 解不出`。
        std::fprintf(stderr,
                     "lift-gen: [可执行节跳表 D26/%s] soft-roots=%zu（最终仍在入口集=%zu）丢弃：越界=%zu "
                     "解不出=%zu 重叠=%zu · shim 目标跳过=%zu · 轮数=%llu · 站点=%llu（值在可执行节内=%llu）"
                     " · 不同候选=%llu · 已在入口集/已判过=%llu · 已是块首=%llu · 可解码=%llu · 解不出=%llu"
                     " · 被重叠挡下=%llu\n",
                     opt.close_jmptab ? "on" : "off", soft_jumptab.added.size(),
                     explorer.liveJumpTabRoots(), soft_jumptab.outside.size(),
                     soft_jumptab.undecodable.size(), soft_jumptab.overlap.size(),
                     soft_jumptab.shim_targets.size(),
                     static_cast<unsigned long long>(explorer.jumpTabRounds()),
                     static_cast<unsigned long long>(s.sites),
                     static_cast<unsigned long long>(s.in_exec),
                     static_cast<unsigned long long>(s.distinct),
                     static_cast<unsigned long long>(s.in_entries),
                     static_cast<unsigned long long>(s.block_starts),
                     static_cast<unsigned long long>(s.decodable),
                     static_cast<unsigned long long>(s.undecodable_pre),
                     static_cast<unsigned long long>(s.overlap_pre));
    }
    // 样例（⛔ 不是"看看"：这三类的形状由 D14 的三条判据定义，样例是规则的实证）
    auto printSamples = [](const char* tag, const std::set<std::uint32_t>& eas) {
        if (eas.empty()) return;
        std::fprintf(stderr, "lift-gen: [%s] %zu 处，样例：", tag, eas.size());
        std::size_t n = 0;
        for (const std::uint32_t ea : eas) {
            if (n++ == 8) {
                std::fprintf(stderr, " …");
                break;
            }
            std::fprintf(stderr, " %08X", ea);
        }
        std::fprintf(stderr, "\n");
    };
    printSamples("丢弃/解不出", drops.undecodable);
    printSamples("丢弃/落在已发射块字节内", drops.inside_block);
    auto printSoftSamples = [](const char* tag, const std::set<std::uint32_t>& eas) {
        if (eas.empty()) return;
        std::fprintf(stderr, "lift-gen: [%s] %zu 处，样例（3）：", tag, eas.size());
        std::size_t n = 0;
        for (const std::uint32_t ea : eas) {
            if (n++ == 3) {
                std::fprintf(stderr, " …");
                break;
            }
            std::fprintf(stderr, " %08X", ea);
        }
        std::fprintf(stderr, "\n");
    };
    printSoftSamples("soft-root/越界（call）", soft_call.outside);
    printSoftSamples("soft-root/解不出（call）", soft_call.undecodable);
    printSoftSamples("soft-root/重叠（call）", soft_call.overlap);
    printSoftSamples("soft-root/越界（store-imm）", soft_store.outside);
    printSoftSamples("soft-root/解不出（store-imm）", soft_store.undecodable);
    printSoftSamples("soft-root/重叠（store-imm）", soft_store.overlap);
    printSoftSamples("soft-root/越界（data-ptr）", soft_data.outside);
    printSoftSamples("soft-root/解不出（data-ptr）", soft_data.undecodable);
    printSoftSamples("soft-root/重叠（data-ptr）", soft_data.overlap);
    printSoftSamples("soft-root/越界（jmptab）", soft_jumptab.outside);
    printSoftSamples("soft-root/解不出（jmptab）", soft_jumptab.undecodable);
    printSoftSamples("soft-root/重叠（jmptab）", soft_jumptab.overlap);
    printSoftSamples("soft-root/已收（jmptab）", soft_jumptab.added);
    // ★ D21（第四类）：**跳表入口**的账 —— 独立一行，且必须能读出"只加 entry、0 新块"这一点。
    //   判据的输入面是"可执行节里 4 字节对齐的 dword"站点数；强过滤器是"值必须已经是块首"。
    std::printf(
        "lift-gen: 跳表入口（D21）added=%zu（只加 entries；⛔ 0 新块 / 0 新指令）· 可执行节 4 字节对齐 "
        "dword 站点=%llu（值=已建块首=%llu，不同目标=%llu）· 已在入口集=%zu · 孤儿提升=%zu\n",
        jump_table.added.size(), static_cast<unsigned long long>(jump_table.sites),
        static_cast<unsigned long long>(jump_table.block_hits),
        static_cast<unsigned long long>(jump_table.distinct), jump_table.skipped.size(),
        jump_table.unclaimed.size());
    std::fprintf(stderr,
                 "lift-gen: [跳表入口 D21] added=%zu（只加 entries、0 新块）· 可执行节 4 字节对齐 dword "
                 "站点=%llu（值=已建块首=%llu，不同目标=%llu）· 已在入口集=%zu · 孤儿提升=%zu\n",
                 jump_table.added.size(), static_cast<unsigned long long>(jump_table.sites),
                 static_cast<unsigned long long>(jump_table.block_hits),
                 static_cast<unsigned long long>(jump_table.distinct), jump_table.skipped.size(),
                 jump_table.unclaimed.size());
    printSoftSamples("跳表入口/已加（D21）", jump_table.added);
    printSoftSamples("跳表入口/孤儿提升（D21）", jump_table.unclaimed);
    // ★ G3：入口集收敛护栏 —— 没有块、退出入口集的入口 EA（⛔ 不静默）。
    std::printf("lift-gen: 入口集收敛护栏：没有块的入口 EA=%zu（⇒ 退出入口集、指向它的边发 ret i32）\n",
                unbuildable.size());
    if (!unbuildable.empty()) {
        std::fprintf(stderr, "lift-gen: [入口集/没有块] %zu 处，样例（3）：", unbuildable.size());
        std::size_t n = 0;
        for (const std::uint32_t ea : unbuildable) {
            if (n++ == 3) {
                std::fprintf(stderr, " …");
                break;
            }
            std::fprintf(stderr, " %08X", ea);
        }
        std::fprintf(stderr, "\n");
    }
    if (opt.verbose) {
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
        std::fprintf(stderr, "lift-gen: unsupported %zu 处（llvm.trap）→ %s\n", unsupported.size(),
                     unsupported_path.string().c_str());
        std::fprintf(stderr, "lift-gen: unsupported 汇总：");
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
