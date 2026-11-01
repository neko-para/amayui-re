// 分派 + 主循环（70-vm）。★ 指令实现不在这个文件：一个 opcode 一个函数，按族分在
// `vm_ops_*.cpp`（影片 0x20F 在 `vm_movie.cpp`）；值/池原语在 `vm_value.cpp`；计时窗在 `vm_timing.cpp`。
// 这个文件只保留：分派表（一个 opcode 一行）、帧协议（长度槽 / 跳转 / 返回栈）、
// `is_scene_mutating`（脏白名单）、构造、`step()` / `run()`。
#include "core/vm.h"

#include "core/encoding.h"
#include "core/install.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace core {
namespace {
// 会改场景（因而置脏、让下一次迭代渲染）的 opcode。按章归组；未实现的也先登记，
// 以后补 handler 时不会漏掉脏标记。
bool is_scene_mutating(std::uint32_t opcode) {
    switch (opcode) {
        // 0x1F7（拆离）不在这里：它只在真的删掉条目时才标脏，由处理器自己写。
        case 0x1F8:  // 空白纹理
        case 0x1F9:  // 设置纹理
        case 0x1FA:  // 释放纹理
        case 0x1FB:  // 写绘制项
        case 0x1FD: case 0x1FE: case 0x1FF:  // 场景对象变换
        case 0x202: case 0x203:              // 绘制项颜色
        case 0x217: case 0x219: case 0x220:  // 3D 记录
        case 0x223: case 0x239:
        case 0x24D: case 0x24F: case 0x250: case 0x251:  // 场景命令表
        case 0x229: case 0x230: case 0x231: case 0x233: case 0x234: case 0x235:
        case 0x23B:                          // CG 数字条
        // ★ `0x258` / `0x259`（1000 项表的两项标志）**不在**这张白名单里：规范说它们
        //   "不把场景标脏"（`CORE-OPS-SCENE-TABLE-FLAGS`）—— 那两格在本作也没有读者。
        case 0x320: case 0x322: case 0x323:  // 网格
        case 0x344:                          // 辅助表 A
            return true;
        default:
            return false;
    }
}

// 「装载帧」的失败分档（`60-ops-frame.md:151` / `70-vm.md:309` 的泵正命令）：
//   · **id 非法 / 打不开** ⇒ 显示消息型异常（类别 7：可重试 / 忽略）；
//   · **头坏 / 装载器返回 0** ⇒ 退出型异常（不重试）。
// 判据按失败点本身分：打开件那几条以文件 / 归档开头，解析头那几条以 `script-` 开头。
bool is_load_open_failure(const std::string& what) {
    return what.rfind("file-id-out-of-range", 0) == 0 || what.rfind("open-failed", 0) == 0 ||
           what.rfind("tell-failed", 0) == 0 || what.rfind("read-failed", 0) == 0 ||
           what.rfind("archive-", 0) == 0;
}
}  // namespace

Machine::Machine(std::uint32_t int_key, const OpcodeTable& table, const Install* install,
                 ConfigStore config)
    : key_(int_key == 0 ? 1u : int_key), table_(table), install_(install), config_(std::move(config)) {
    // 主循环的渲染闸门：`set:DrawMode == 1` 才自动渲染。缺键按配置口径为 0（不自动渲染）。
    draw_mode_ = config_.get_int("set:DrawMode");
    // 参数格（`Engine+0x15290`，`0x74` 写 / `0x7f` 读）的初值 = 配置键 `message:MessageSpeed`
    // —— 启动期有确定写点：`0x417E98` 取键、`0x417EAB` 写格（取证见 ops-forensics 文档 §6）。
    // ★ 这解决了 spec 的 `RES-PARAM-21668-INIT`（"初值没有观察"）：它不是分配残留，是配置。
    param_cell_ = config_.get_int("message:MessageSpeed");
    // 画面模式缓存（`Engine+0xA40D8`）的初值 = 配置 `display:ScreenMode`（引擎启动时由该键写入）。
    display_mode_cache_ = config_.get_int("display:ScreenMode");
    // 启动期登记：键非 0 才认为"备组已分配"（发行数据里该键恒 0 ⇒ 只有异常支可达）。
    memflip_available_ = config_.get_int("set:EnableMemFlip") != 0;
    // 启动时登记引擎重置钩子（`CORE-OPS-FRAME-EXIT-SCRIPT` 的第一项；站点 `0x4165ce`）。
    // ★ 只登记、不执行；`0x9` 拿它做「推进格数 ← 1 + 清状态字第 9 位」。
    engine_reset_hook_registered_ = true;
    // 字体对象的两对描述（构造器把两份变体都按缺省字号写成 `loHeight = −字号` / `loWidth = −字号/2`）。
    // 字面名不建模（引擎缺省值是平台字体名，本版未取证）⇒ 留空串。
    for (auto& variant : font_a_pair_.variants) {
        variant.height = -glyph_height_;
        variant.width = -(glyph_height_ / 2);
    }
    for (auto& variant : font_b_pair_.variants) {
        variant.height = -font_b_height_;
        variant.width = -(font_b_height_ / 2);
    }
    // ③ 槽号 → 键号：`-1` = 未绑定（兄弟指令写 −1 解除绑定）。
    slot_key_table_.fill(-1);
    // ★ 输入系统的初始化还把**槽 0..6 绑到这 7 个固定逻辑键**（`CORE-OPS-SCALAR-TABLE-10C` 的
    //   「七个默认槽依赖的逻辑键」）：槽 0..6 依次 = 200 / 205 / 208 / 203 / 28 / 57 / 14，
    //   经表 B 换成系统键码 38 / 39 / 40 / 37 / 13 / 32 / 8（上 / 右 / 下 / 左 / Enter / Space /
    //   Backspace）。`0xfd` 的"解绑该槽当前逻辑键的键码"要靠这条绑定才成立（spec 的可证伪条：
    //   `0xfd` 把槽 0 改绑 Enter 之后，按 ↑ ⇒ 输入字里不再有第 0 位）。其余槽仍是"未绑定"（−1）。
    slot_key_table_[0] = 200;  // ↑
    slot_key_table_[1] = 205;  // →
    slot_key_table_[2] = 208;  // ↓
    slot_key_table_[3] = 203;  // ←
    slot_key_table_[4] = 28;   // Enter
    slot_key_table_[5] = 57;   // Space
    slot_key_table_[6] = 14;   // Backspace
    // ── 输入系统的初始化（`CORE-INPUT-WORD` 的「各来源的位号合成」；⛔ 不是配置、不是脚本）──
    // ① 系统键码表 256 项全 −1；表 B（逻辑键 → 系统键码）是 **256 格**：整表清 0，再写
    //    `CORE-OPS-SCALAR-TABLE-10C` 的「表 B 的初值」那 **93 个固定条目**（未列出的 163 格恒为 0
    //    ⇒ 逻辑键号落到那些格时换成系统键码 0，与"逻辑键 0"同效）。
    //    ★ 表 B 是**构造期的固定两段查表**、运行期没有第二个写者（配置与脚本都不碰它）。
    key_code_table_.fill(-1);
    logical_key_codes_.fill(0);
    {
        struct KeyCodeEntry {
            std::int32_t logical_key;  // 逻辑键号（0..255）
            std::int32_t system_code;  // 系统键码（Win32 VK）
        };
        // ★ 逐条抄 `data/spec/chapters/60-ops-scalar.md` 的「表 B 的初值」（93 条；表内其余格 = 0）。
        constexpr KeyCodeEntry kTableB[] = {
            {1, 27},   {2, 49},   {3, 50},   {4, 51},   {5, 52},   {6, 53},
            {7, 54},   {8, 55},   {9, 56},   {10, 57},  {11, 48},  {12, 109},
            {14, 8},   {15, 9},   {16, 81},  {17, 87},  {18, 69},  {19, 82},
            {20, 84},  {21, 89},  {22, 85},  {23, 73},  {24, 79},  {25, 80},
            {28, 13},  {29, 17},  {30, 65},  {31, 83},  {32, 68},  {33, 70},
            {34, 71},  {35, 72},  {36, 74},  {37, 75},  {38, 76},  {42, 16},
            {44, 90},  {45, 88},  {46, 67},  {47, 86},  {48, 66},  {49, 78},
            {50, 77},  {54, 16},  {56, 18},  {57, 32},  {59, 112}, {60, 113},
            {61, 114}, {62, 115}, {63, 116}, {64, 117}, {65, 118}, {66, 119},
            {67, 120}, {68, 121}, {69, 144}, {70, 145}, {71, 103}, {72, 104},
            {73, 105}, {74, 109}, {75, 100}, {76, 101}, {77, 102}, {78, 107},
            {79, 97},  {80, 98},  {81, 99},  {82, 96},  {83, 110}, {87, 122},
            {88, 123}, {112, 21}, {121, 28}, {123, 29}, {148, 25}, {156, 13},
            {157, 17}, {181, 111},{184, 18}, {199, 18}, {200, 38}, {201, 33},
            {203, 37}, {205, 39}, {207, 35}, {208, 40}, {209, 34}, {210, 45},
            {219, 91}, {220, 92}, {221, 93},
        };
        for (const auto& entry : kTableB) {
            logical_key_codes_[static_cast<std::size_t>(entry.logical_key)] = entry.system_code;
        }
    }
    // 表 C 的七个默认项**不是硬编码的常数**：它们的系统键码来自表 B（`60-ops-input` 的
    // 「键盘来源的键码表」—— 那 7 项经「逻辑键号 → 系统键码」的表取），这里按 表 A → 表 B 两段查表
    // 把表 C 登记成 位 0..6（上 / 右 / 下 / 左 / Enter / Space / Backspace）。
    // ★ 这 7 个逻辑键在表 B 里**必在**（⇒ 38 / 39 / 40 / 37 / 13 / 32 / 8）；改了槽绑定就必须同步表 B，
    //   守卫 `spec_ops_scalar_key_table_b` 会当场红（它同时断言两段落点）。
    for (std::int32_t slot = 0; slot < 7; ++slot) {
        const auto code = logical_key_code(slot_key_table_[static_cast<std::size_t>(slot)]);
        key_code_table_[static_cast<std::size_t>(code)] = slot;
    }
    // ② 键槽映射（`0x10b` 那张 32 格表）：初始化把它写成**各自的槽号**，另把槽 2 覆写成 30；
    //    组头的**槽数**写成 2。⇒ 槽 0 / 1 的基位号初值 0 / 1，鼠标左右键默认落输入位 4 / 5。
    for (std::size_t i = 0; i < array_d1383_.size(); ++i) {
        array_d1383_[i] = static_cast<std::int32_t>(i);
    }
    array_d1383_[2] = 30;
    key_slot_count_ = 2;
    // ③ 手柄组头的按钮数：向平台查询设备能力（`system:LimitJoy` = 1 ⇒ 整组为空）。
    //    ★ 本模型不自己打开设备：能力查询走 `set_joy_button_count`（窗口路径在那里接 SDL）。
    joy_button_count_ = 0;
    // 重算缩放的**第 ① 个时机**：启动、字体对象建立之后一次。视口缺省 = 客户区 ⇒ 两个分量 1.0，
    // 与缓存初值相等 ⇒ 整趟什么都不写（契约本来就是"相等 ⇒ 不写"）。
    refresh_font_scale();
}

Machine::~Machine() {
    // 文件库析构（`0x454cd0`）：槽位表逐格清 0。本模型没有独立的文件库对象 ⇒ 由这里承担；
    // 测试用 `release_file_library()` 模拟同一步（`0x143` 的可达条件随之消失）。
    release_file_library();
}

void Machine::load_root(const Bytes& script, std::uint32_t script_id) {
    const ScriptHeader header = parse_script_header(script);
    // 根帧的父帧号是哨兵 −1（`CORE-OPS-FRAME-EXIT-SCRIPT`）：装载器只照抄「父帧候选」格 ⇒ 先写它。
    parent_candidate_ = -1;
    apply_frame_record(0, script, header, script_id);
    current_frame_ = 0;
    halted_ = false;
    scene_dirty_ = true;
    stop_ = {};
}

// ★ **同一条装载路径**（`CORE-OPS-FRAME-CALL-SCRIPT` 的成立条件）：`0x3` / `0x6` / `0x9` 重置
//   与泵的正命令都从这里建新帧记录，所以形状只有一份（以前三条各拼一份，于是局部块两格就各写各的）。
void Machine::apply_frame_record(std::size_t index, const Bytes& script, const ScriptHeader& header,
                                 std::uint32_t file_id) {
    Frame& frame = frames_[index];
    // 先按引擎的装载器（`0x40edd9`：装载前**无条件释放**目标帧槽）清掉原有内容：
    // 码区 + 四个池 + 两张按键表 + 两条栈。⛔ 不因槽已被占用而报错。
    frame.code.clear();
    frame.local_int.clear();
    frame.local_float.clear();
    frame.local_string.clear();
    // 字符串族的**裸字叠层**与文本池同寿命（见 `store_raw`）：本帧窗口里的裸字一起清，
    // ⛔ 不让上一个脚本写下的位模式活过装载器。★ 本帧局部字符串池基点 = 帧窗口 + (11 − 9) × 0x100000。
    clear_string_record_words(0x40000000u + static_cast<std::uint32_t>(index) * 0x00800000u + 0x00200000u,
                              0x00100000u);
    frame.key_jumps.clear();
    frame.key_cursor = 0;
    frame.key_limit = 0;
    frame.hold_slots.clear();
    frame.position = 0;
    frame.call_sites.clear();
    // `0x40ee12`：返回栈的**栈顶指针** ← 0（数组内容不写）。容器清空 = "旧位置不再可达"，
    // 与"只把栈顶写 0"没有可观察差别；⛔ 不要把它当成"清内容"这条合同。
    frame.ret_stack.clear();
    frame.loaded = true;
    frame.code.assign(script.begin() + static_cast<std::ptrdiff_t>(header.body_offset), script.end());
    frame.ip = 0;                      // `0x40f6db`：指令指针 ← 新代码区起点
    frame.body_offset = header.body_offset;
    frame.length_slot = 0;             // `0x40f6ee`：分派器不自动推进
    frame.parent = parent_candidate_;  // `0x40ee05`：父帧号 ← 引擎的「父帧候选」格（只照抄）
    frame.script_id = file_id;         // `0x40edeb`：统一文件 id
    frame.local_depth = static_cast<std::int32_t>(file_id);  // 本帧的身份编号
    frame.local_main = -1;             // `0x40ee26`：本地块主进入目标 ← −1（未登记）
    frame.local_sub = -1;              // `0x40ee33`：本地块副进入目标 ← −1（未登记）
}

void Machine::install_script_into(std::size_t slot, std::uint32_t file_id) {
    if (install_ == nullptr) throw CoreError("frame-loader-missing");
    const Bytes script = core::read_install_file(*install_, file_id);
    // 文件库装载登记（`0x455948`）：按**装载文件对象自带的号**（统一 id 的高 8 位 = 区号）填槽 ——
    // `0x143` 的投递源就是这张表。
    register_loaded_file(file_id);
    const ScriptHeader header = core::parse_script_header(script);
    apply_frame_record(slot, script, header, file_id);
}

void Machine::load_frame_for_test(std::uint32_t file_id, std::int32_t target) {
    if (target < 0 || target >= static_cast<std::int32_t>(kFrameCount)) {
        throw CoreError("test-frame-slot-out-of-range:" + std::to_string(target));
    }
    // 只调装载器：**不写**「父帧候选」格（那是调用方的事 —— 泵写 −10、`0x6` 写当前帧号）。
    install_script_into(static_cast<std::size_t>(target), file_id);
}

// ── 待处理命令队列 / 泵（`CORE-OPS-RUN-BRACKET` / `CORE-OPS-EVENT-143`）──

// 队列本体：256 格线性表。判空口径照 spec 是「队列头 ≥ 队列尾」；这里用 `head < tail` 表示非空，
// 与门的「占用标记」是**同一个状态**（⛔ 不另存一个会漂移的标记格）。
void Machine::push_command(std::int32_t command) {
    if (queue_tail_ >= kCommandQueueCapacity && queue_head_ > 0) {
        // 收拢：把 [head, tail) 搬到表头。只为让"头 ≥ 尾 ⇒ 空"这条口径在长会话里仍可用；
        // 规范没有规定满队处置（`0x143` 一趟最多投 255 条 ⇒ 空队列起步装得下）。
        const auto count = queue_tail_ - queue_head_;
        std::copy(command_queue_.begin() + static_cast<std::ptrdiff_t>(queue_head_),
                  command_queue_.begin() + static_cast<std::ptrdiff_t>(queue_tail_),
                  command_queue_.begin());
        queue_head_ = 0;
        queue_tail_ = count;
    }
    if (queue_tail_ >= kCommandQueueCapacity) {
        // 真的满了（头 = 0 且尾 = 256）：记诊断、丢掉这一条。⛔ 不覆盖最旧项，也不静默改头尾。
        report_error("command-queue-overflow");
        return;
    }
    command_queue_[queue_tail_++] = command;
    const auto occupancy = queue_tail_ - queue_head_;
    if (occupancy > queue_high_water_) queue_high_water_ = occupancy;
}

void Machine::enqueue_command(std::int32_t command) {
    push_command(command);
    // 投递口自带的"顺手泵一次"尾巴（`60-ops-scalar.md:177`）：**队列泵占用标记**非 0 时
    // 一次也不泵 —— 那正是 `0x143` 逐格投递期间借用这个标记的用处（`70-vm.md:315`）。
    if (!pump_busy_) pump_command_queue();
}

void Machine::enqueue_command_for_test(std::int32_t command) { push_command(command); }

void Machine::register_loaded_file(std::uint32_t file_id) {
    // 引擎那张表**只有 256 槽**（`0x454adc ecx = 0x100` / `0x454ae6 rep stosd`），槽号 =
    // **装载文件对象自带的号** = `[对象+114h]` = 统一文件 id 的高 8 位（区号，见 `25-fs`）。
    // ★ 本模型装到的都是基础索引的文件（统一 id = 区号 0 + 条目下标，`read_install_file` 用 TOC 下标）
    //   ⇒ 区号恒 0 ⇒ 写格 0；`0x143` 不检查格 0。扩展索引（区号 1..255）见 `load_extension_index()`。
    const auto zone = file_id >> 24;
    file_library_slots_[zone] = 1;  // zone ∈ 0..255（高 8 位）⇒ 恒在表内
}

void Machine::load_extension_index(std::int32_t zone) {
    // 文件库装载一个扩展索引（AAI）：按它自带的区号登记（`0x455948`）。
    // ★ 区号字段**没有范围校验**（`25-fs`：越界区号的行为不规定）⇒ 本版不写表外，只记诊断。
    if (zone < 0 || zone >= static_cast<std::int32_t>(kFileLibrarySlotCount)) {
        report_error("file-library-zone-out-of-range:" + std::to_string(zone));
        return;
    }
    register_loaded_file(static_cast<std::uint32_t>(zone) << 24);
}

void Machine::pump_command_queue() {
    // 入口两判、按此顺序（`70-vm.md:306`）：区间忙标记 ≠ 0 ⇒ 一条也不取；队列头 ≥ 队列尾（空）⇒ 返回。
    if (driven_elsewhere_) return;
    if (!queue_occupied()) return;
    const std::int32_t command = command_queue_[queue_head_];
    ++queue_head_;  // 取一条 = 头 +1（三种命令都先走这一步）
    if (!queue_occupied()) queue_head_ = queue_tail_ = 0;  // 取空 ⇒ 收拢；判空口径仍是「头 ≥ 尾」
    // 0 = 空命令：头已前进，此外什么都不做（`70-vm.md:306`）。
    if (command == 0) return;

    // ── 非 0 命令先做**挂起**（两支相同、顺序固定，`70-vm.md:308`）──
    suspended_frame_ = static_cast<std::int32_t>(current_frame_);  // ① 记住当前帧号
    status_restore_ = state_flags_;                                // ② 状态选择字整字存进待恢复副本
    state_flags_ = 0;                                              //    再把它清 0（门随即打开）
    pump_busy_ = true;                                             // ③ 置队列泵占用标记
    // 泵帧父帧号 = 哨兵 −10：**不是装载器自己决定的** —— 泵先写「父帧候选」格，装载器只照抄；
    // 负支不装帧，由泵自己把目标帧的父帧号写成 −10（`70-vm.md:308`）。
    parent_candidate_ = kPumpParentSentinel;

    if (command > 0) {
        // 正命令（`70-vm.md:309`）：先让当前帧号 ← 37，再按「装载帧」的合同把统一文件 id 装进帧 37。
        // 装载失败沿用失败分档，**但当前帧号停在 37、不恢复**。
        current_frame_ = kPumpFrameSlot;
        try {
            install_script_into(kPumpFrameSlot, static_cast<std::uint32_t>(command));
        } catch (const CoreError& error) {
            const std::string what = error.what();
            if (is_load_open_failure(what)) throw CoreError("display-message:" + what);
            // 头坏 / 装载器返回 0 ⇒ 退出型异常：执行就此结束，队列与待恢复副本不再被读
            //（泵帧清理不在本条范围 —— 残差 `RES-VM-PUMP-FAIL-CLEAN`）。
            throw;
        }
        return;
    }

    // 负命令（`70-vm.md:310`）：**先**让当前帧号 ← 目标帧号（−命令）。
    const auto target = -static_cast<std::int64_t>(command);
    if (target >= static_cast<std::int64_t>(kFrameCount)) {
        // 帧号范围在规范里是**未定义行为**（`60-ops-scalar.md:181`：格号 ≥ 128 的投递值量级 2^24 以上，
        // 落在 0..39 之外）⇒ 本模型不许越界读帧表：记诊断并按同一失败点抛显示消息型异常。
        report_error("pump-target-frame-out-of-range:" + std::to_string(target));
        throw CoreError("display-message:pump-target-frame-out-of-range:" + std::to_string(target));
    }
    current_frame_ = static_cast<std::size_t>(target);
    Frame& slot = frame_mut();
    if (!slot.loaded) {
        // 目标帧没有装载脚本（代码区起点 = 0）⇒ 显示消息型异常（「この階層には…Depth=%d」那一句；
        // 文本本身属机器细节）。**当前帧号停在目标帧、不恢复** —— 与「换帧」的失败支相反。
        throw CoreError("display-message:pump-frame-not-loaded:" + std::to_string(target));
    }
    slot.parent = kPumpParentSentinel;  // 负支由泵直接写，不经过装载器
    slot.ip = 0;                        // 指令指针 ← 该帧代码区起点
    slot.length_slot = 0;               // 长度槽 ← 0 ⇒ 分派器不推进，下一迭代执行该帧第一条
}

void Machine::advance_host_pump() {
    // 迭代头的宿主消息泵节流（`CORE-VM-LOOP`：「回消息的节流是引擎侧的固定周期」）：
    // 计数器初值 1；每迭代看它与常量 **4** 的按位与 —— 非 0（计数 = 4）⇒ 本迭代泵一次、泵后回 1，
    // **同一迭代再判一次**（1 & 4 = 0 ⇒ 不再泵）并 +1 到 2；为 0 ⇒ 计数器 +1、不泵。
    // ⇒ 泵一次之后隔 3 个迭代再泵，与宿主无关。这里只落计数序列与 `should_pump_host()`。
    const bool pump = (pump_counter_ & 4u) != 0;
    if (pump) {
        pump_counter_ = 1;
        pump_counter_ += 1;  // 同一迭代再判 + 加到 2
    } else {
        pump_counter_ += 1;
    }
    host_pump_due_ = pump;
}

Frame& Machine::frame_mut() {
    return frames_[current_frame_];
}

void Machine::note_missing_opcode(std::uint32_t opcode) {
    for (auto& entry : missing_opcodes_) {
        if (entry.first == opcode) {
            entry.second += 1;
            return;
        }
    }
    missing_opcodes_.emplace_back(opcode, 1);
}

void Machine::write_length(std::uint32_t words) {
    frame_mut().length_slot = words;
}

void Machine::jump_to(std::uint32_t position) {
    if (position > frame_mut().code.size() / 4) {
        throw CoreError("jump-out-of-code:" + std::to_string(position));
    }
    frame_mut().ip = static_cast<std::size_t>(position) * 4;
    write_length(0);
}

void Machine::push_return(std::uint32_t position) {
    Frame& f = frame_mut();
    // 每帧容量 **256**（`CORE-OPS-FRAME-CALL`；引擎的帧步长 = 256，压栈前**不检查容量**）。
    // 第 257 次压栈在引擎里落到帧区之外 = 未定义行为 ⇒ 实现必须**拒绝或记诊断**；
    // ⛔ 不许覆盖最旧一项、也不许静默截断（两者都会静默丢掉一个返回点）。
    if (f.ret_stack.size() >= kRetStackCapacity) {
        report_error("ret-stack-overflow:frame=" + std::to_string(current_frame_) +
                     ",position=" + std::to_string(position));
        return;
    }
    // 先记"在哪条指令上登记的返回点"（诊断用），再压返回位置 —— 两条栈必须同深。
    f.call_sites.push_back(static_cast<std::int32_t>(f.ip / 4));
    f.ret_stack.push_back(static_cast<std::int32_t>(position));
}

void Machine::push_engine_state_word(std::uint32_t word) {
    // `CORE-OPS-FRAME-RET`：引擎重入脚本前压入当时的状态选择字，并把它第 31 / 20 位清 0
    // （`0x4116eb` 取实参、`0x4116f2` 是那条 push；清位是脚本返回后门要重新关上的前提）。
    engine_state_word_stack_.push_back(word & ~((1u << 31) | (1u << 20)));
}

std::string Machine::stop_stack_report() const {
    std::string out;
    const auto bin_name = [this](std::uint32_t script_id) -> std::string {
        if (install_ == nullptr) return {};
        if (script_id >= install_->index.files.size()) return {};
        return cp932_to_utf8(install_->index.files[script_id].name);
    };
    std::size_t depth = 0;
    for (std::int32_t index = static_cast<std::int32_t>(current_frame_); index >= 0;
         index = frames_[static_cast<std::size_t>(index)].parent) {
        const Frame& frame = frames_[static_cast<std::size_t>(index)];
        if (!frame.loaded) break;
        ++depth;
        std::uint32_t opcode = 0;
        if (frame.ip + 4 <= frame.code.size()) opcode = read_u32(frame.code, frame.ip);
        const std::string name = bin_name(frame.script_id);
        char line[512];
        std::snprintf(line, sizeof(line),
                      "[stack] #%zu frame=%d/%zu script=%u bin=%s ip=%zu file=%zu opcode=0x%X",
                      depth, index, kFrameCount, frame.script_id, name.empty() ? "?" : name.c_str(),
                      frame.ip, static_cast<std::size_t>(frame.body_offset) + frame.ip, opcode);
        out += line;
        if (!frame.call_sites.empty()) {
            out += " call_at=" + std::to_string(frame.call_sites.back());
        }
        out += "\n";
    }
    if (out.empty()) out = "[stack] (no frame)\n";
    return out;
}

void Machine::execute(const Instruction& instruction) {
    switch (instruction.opcode) {
        case 0x01: op_01(instruction); return;
        case 0x02: op_02(instruction); return;
        case 0x88: op_88(instruction); return;
        case 0xC0: op_c0(instruction); return;
        case 0x142: op_142(instruction); return;
        case 0x12F: op_12f(instruction); return;
        case 0x2CC: op_2cc(instruction); return;
        case 0x2CD: op_2cd(instruction); return;
        case 0x2CE: op_2ce(instruction); return;
        case 0x2CF: op_2cf(instruction); return;
        case 0x307: op_307(instruction); return;
        case 0x2E6: op_2e6(instruction); return;
        case 0x2E7: op_2e7(instruction); return;
        case 0x2E8: op_2e8(instruction); return;
        case 0x2EA: op_2ea(instruction); return;
        case 0xC5: op_c5(instruction); return;
        case 0xC6: op_c6(instruction); return;
        case 0xC7: op_c7(instruction); return;
        case 0x306: op_306(instruction); return;
        case 0x10D: op_10d(instruction); return;
        case 0x1B5: op_1b5(instruction); return;
        case 0x1B8: op_1b8(instruction); return;
        case 0x1B9: op_1b9(instruction); return;
        case 0x1BA: op_1ba(instruction); return;
        case 0xB6: op_b6(instruction); return;
        case 0xB7: op_b7(instruction); return;
        case 0xB8: op_b8(instruction); return;
        case 0xBA: op_ba(instruction); return;
        case 0x1BB: op_1bb(instruction); return;
        case 0x1A6: op_1a6(instruction); return;
        case 0x300: op_300(instruction); return;
        case 0x301: op_301(instruction); return;
        case 0x1D2: op_1d2(instruction); return;
        case 0x1D3: op_1d3(instruction); return;
        case 0x6E: op_6e(instruction); return;
        case 0x6F: op_6f(instruction); return;
        case 0x196: op_196(instruction); return;
        case 0x03: op_03(instruction); return;
        case 0x09: op_09(instruction); return;
        case 0x06: op_06(instruction); return;
        case 0x08: op_08(instruction); return;
        case 0x6C: op_6c(instruction); return;
        case 0x2D8: op_2d8(instruction); return;
        case 0x61: op_61(instruction); return;
        case 0x60: op_60(instruction); return;
        case 0x63: op_63(instruction); return;
        case 0x6D: op_6d(instruction); return;
        case 0x74: op_74(instruction); return;
        case 0x7B: op_7b(instruction); return;
        case 0x7C: op_7c(instruction); return;
        case 0x7F: op_7f(instruction); return;
        case 0xD0: op_d0(instruction); return;
        case 0x191: op_191(instruction); return;
        case 0x199: op_199(instruction); return;
        case 0x19A: op_19a(instruction); return;
        case 0x1AD: op_1ad(instruction); return;
        case 0x19E: op_19e(instruction); return;
        case 0x1A0: op_1a0(instruction); return;
        case 0x1A1: op_1a1(instruction); return;
        case 0x1AB: op_1ab(instruction); return;
        case 0x1AC: op_1ac(instruction); return;
        case 0x1AE: op_1ae(instruction); return;
        case 0x1AF: op_1af(instruction); return;
        case 0x1B0: op_1b0(instruction); return;
        case 0x1B6: op_1b6(instruction); return;
        case 0x1B7: op_1b7(instruction); return;
        case 0x1C7: op_1c7(instruction); return;
        case 0x1CC: op_1cc(instruction); return;
        case 0x218: op_218(instruction); return;
        case 0x217: op_217(instruction); return;
        case 0x21A: op_21a(instruction); return;
        case 0x21D: op_21d(instruction); return;
        case 0x21E: op_21e(instruction); return;
        case 0x21F: op_21f(instruction); return;
        case 0x228: op_228(instruction); return;
        case 0x229: op_229(instruction); return;
        case 0x2D0: op_2d0(instruction); return;
        case 0x2D1: op_2d1(instruction); return;
        case 0x2D3: op_2d3(instruction); return;
        case 0x12C: op_12c(instruction); return;
        case 0x64: op_64(instruction); return;
        case 0x10C: op_10c(instruction); return;
        case 0x308: op_308(instruction); return;
        case 0xFE: op_fe(instruction); return;
        case 0xBF: op_bf(instruction); return;
        case 0xB4: op_b4(instruction); return;
        case 0xB5: op_b5(instruction); return;
        case 0x2BF: op_2bf(instruction); return;
        case 0x2C7: op_2c7(instruction); return;
        case 0x341: case 0x345: case 0x34E: case 0x352: op_341(instruction); return;
        case 0x2EC: op_2ec(instruction); return;
        case 0x2EB: op_2eb(instruction); return;
        case 0x23B: op_23b(instruction); return;
        case 0x109: op_109(instruction); return;
        case 0x108: op_108(instruction); return;
        case 0xC8: op_c8(instruction); return;
        case 0xCC: op_cc(instruction); return;
        case 0xCD: op_cd(instruction); return;
        case 0xFB: op_fb(instruction); return;
        case 0xFC: op_fc(instruction); return;
        case 0xFF: op_ff(instruction); return;
        case 0x102: op_102(instruction); return;
        case 0x103: op_103(instruction); return;
        case 0x104: op_104(instruction); return;
        case 0x105: op_105(instruction); return;
        case 0x100: op_100(instruction); return;
        case 0x12E: op_12e(instruction); return;
        case 0x2FC: op_2fc(instruction); return;
        case 0x24E: op_24e(instruction); return;
        case 0x2DA: op_2da(instruction); return;
        case 0x344: op_344(instruction); return;
        case 0x1FF: op_1ff(instruction); return;
        case 0x1FD: op_1fd(instruction); return;
        case 0x1FE: op_1fe(instruction); return;
        case 0x20B: op_20b(instruction); return;
        case 0x216: op_216(instruction); return;
        case 0x208: op_208(instruction); return;
        case 0x20C: op_20c(instruction); return;
        case 0x23C: op_23c(instruction); return;
        case 0x20D: op_20d(instruction); return;
        case 0x33F: op_33f(instruction); return;
        case 0x204: op_204(instruction); return;
        case 0x205: op_205(instruction); return;
        case 0x20E: op_20e(instruction); return;
        case 0x1F6: op_1f6(instruction); return;
        case 0x1F4: op_1f4(instruction); return;
        case 0x1F5: op_1f5(instruction); return;
        case 0x20F: op_20f(instruction); return;
        case 0x21C: op_21c(instruction); return;
        case 0x101: op_101(instruction); return;
        case 0xAE: op_ae(instruction); return;
        case 0x2EE: op_2ee(instruction); return;
        case 0x10F: op_10f(instruction); return;
        case 0x248: op_248(instruction); return;
        case 0x8B: op_8b(instruction); return;
        case 0x90: op_90(instruction); return;
        case 0x91: op_91(instruction); return;
        case 0x92: op_92(instruction); return;
        case 0x93: op_93(instruction); return;
        case 0x94: op_94(instruction); return;
        case 0x97: op_97(instruction); return;
        case 0x261: op_261(instruction); return;
        case 0x260: op_260(instruction); return;
        case 0x107: op_107(instruction); return;
        case 0x10B: op_10b(instruction); return;
        case 0xFD: op_fd(instruction); return;
        case 0x30A: op_30a(instruction); return;
        case 0x25B: op_25b(instruction); return;
        case 0x143: op_143(instruction); return;
        case 0x130: op_130(instruction); return;
        case 0x1A4: op_1a4(instruction); return;
        case 0x76: case 0x77: op_76(instruction); return;
        case 0x2BD: op_2bd(instruction); return;
        case 0x2DE: op_2de(instruction); return;
        case 0x194: op_194(instruction); return;
        case 0x195: op_195(instruction); return;
        case 0x192: op_192(instruction); return;
        case 0x19B: op_19b(instruction); return;
        case 0x19C: op_19c(instruction); return;
        case 0x1BF: op_1bf(instruction); return;
        case 0x1B1: op_1b1(instruction); return;
        case 0x238: op_238(instruction); return;
        case 0x219: op_219(instruction); return;
        case 0x220: op_220(instruction); return;
        case 0x224: op_224(instruction); return;
        case 0x7A: op_7a(instruction); return;
        case 0x1CB: op_1cb(instruction); return;
        case 0x256: op_256(instruction); return;
        case 0x32: op_32(instruction); return;
        case 0x72: op_72(instruction); return;
        case 0x10A: op_10a(instruction); return;
        case 0x193: op_193(instruction); return;
        case 0x20A: op_20a(instruction); return;
        case 0x243: op_243(instruction); return;
        case 0x2F7: op_2f7(instruction); return;
        case 0x304: op_304(instruction); return;
        case 0x305: op_305(instruction); return;
        case 0x2E9: op_2e9(instruction); return;
        case 0x214: op_214(instruction); return;
        case 0x215: op_215(instruction); return;
        case 0x242: op_242(instruction); return;
        case 0x1BC: op_1bc(instruction); return;
        case 0x1CF: op_1cf(instruction); return;
        case 0x1A9: op_1a9(instruction); return;
        case 0x1AA: op_1aa(instruction); return;
        case 0x1A3: op_1a3(instruction); return;
        case 0x1A2: op_1a2(instruction); return;
        case 0x223: op_223(instruction); return;
        case 0x24D: op_24d(instruction); return;
        case 0x24F: op_24f(instruction); return;
        case 0x250: op_250(instruction); return;
        case 0x251: op_251(instruction); return;
        case 0x320: op_320(instruction); return;
        case 0x23D: op_23d(instruction); return;
        case 0x32B: op_32b(instruction); return;
        case 0x322: op_322(instruction); return;
        case 0x32a: op_32a(instruction); return;
        case 0x32d: op_32d(instruction); return;
        case 0x323: op_323(instruction); return;
        case 0x1FB: op_1fb(instruction); return;
        case 0x239: op_239(instruction); return;
        case 0x258: op_258(instruction); return;
        case 0x259: op_259(instruction); return;
        case 0x202: op_202(instruction); return;
        case 0x203: op_203(instruction); return;
        case 0x1F7: op_1f7(instruction); return;
        case 0x1F8: op_1f8(instruction); return;
        case 0x1FA: op_1fa(instruction); return;
        case 0x1F9: op_1f9(instruction); return;
        case 0x303: op_303(instruction); return;
        case 0x212: op_212(instruction); return;
        case 0x213: op_213(instruction); return;
        case 0x25D: op_25d(instruction); return;
        case 0x1C1: op_1c1(instruction); return;
        case 0x79: op_79(instruction); return;
        case 0xA1: op_a1(instruction); return;
        case 0xA2: op_a2(instruction); return;
        case 0xA3: op_a3(instruction); return;
        case 0x2DB: op_2db(instruction); return;
        case 0x78: op_78(instruction); return;
        case 0x73: op_73(instruction); return;
        case 0x71: op_71(instruction); return;
        case 0x80: op_80(instruction); return;
        case 0x70: op_70(instruction); return;
        case 0x32F: op_32f(instruction); return;
        case 0x324: op_324(instruction); return;
        case 0x1CA: op_1ca(instruction); return;
        case 0x149: op_149(instruction); return;
        case 0x21B: op_21b(instruction); return;
        case 0x252: op_252(instruction); return;
        case 0xD9: op_d9(instruction); return;
        case 0x1A7: op_1a7(instruction); return;
        case 0x1A8: op_1a8(instruction); return;
        case 0x2F8: op_2f8(instruction); return;
        case 0x2F6: op_2f6(instruction); return;
        case 0x05: op_05(instruction); return;
        case 0x50: op_50(instruction); return;
        case 0x51: op_51(instruction); return;
        case 0x52: op_52(instruction); return;
        case 0x53: op_53(instruction); return;
        case 0x54: op_54(instruction); return;
        case 0x1A5: op_1a5(instruction); return;
        case 0x2FE: op_2fe(instruction); return;
        case 0x197: op_197(instruction); return;
        case 0x198: op_198(instruction); return;
        case 0x75: op_75(instruction); return;
        case 0x2D5: op_2d5(instruction); return;
        case 0x56: op_56(instruction); return;
        case 0x57: op_57(instruction); return;
        case 0x58: op_58(instruction); return;
        case 0x59: op_59(instruction); return;
        case 0x5A: op_5a(instruction); return;
        case 0x5B: op_5b(instruction); return;
        case 0x5C: op_5c(instruction); return;
        case 0x5D: op_5d(instruction); return;
        case 0x5E: op_5e(instruction); return;
        case 0x5F: op_5f(instruction); return;
        case 0x135: op_135(instruction); return;
        case 0x1CE: op_1ce(instruction); return;
        case 0x136: op_136(instruction); return;
        case 0x13F: op_13f(instruction); return;
        case 0x55: op_55(instruction); return;
        case 0x8c: op_8c(instruction); return;
        case 0x8f: op_8f(instruction); return;
        case 0xC2: op_c2(instruction); return;
        case 0xC4: op_c4(instruction); return;
        case 0x82: op_82(instruction); return;
        case 0x2F5: op_2f5(instruction); return;
        case 0x2FF: op_2ff(instruction); return;
        case 0xa0: op_a0(instruction); return;
        default:
            throw CoreError(
                "opcode-not-implemented:0x" + [&] {
                    char buffer[16];
                    std::snprintf(buffer, sizeof(buffer), "%X", instruction.opcode);
                    return std::string(buffer);
                }());
    }
}

// 门之前的影片对象表服务（`CORE-VM-MOVIE`）：每次迭代、过门之前，若「有活动对象」为 1 就扫一遍。
// ★ 它只做轮询 / 收尾 / 循环 / 填纹理；影片位（门上那一位）只管把门关住。

// `0x21c` 谓词 A（`sub_407E20` 的合同）：消息子系统"还有事"。
// 定时器就是 `0x238` 写的那一对格：起点（0 = 未盖章，首次检查时盖章 = 现在）+ 时长；
// 判据用**无符号比较、相等仍算窗内**（引擎 `ja`）。到期 / 中止（`0x243`/`0x305` 的 A=1）
// 就把那一对清 0，退回看"待处理"闩。
// ★ "待处理"闩 = 场景侧的 `+0B5B4`（`sub_407E20` 的最后一个 `mov eax,[ecx+0B5B4h]` 就是它）：
//   · 排窗指令（`0x202` 的 `0x4AD155`、`0x323`、`0x239`、`0x220` / `0x21e` / `0x21f` 那一族 setter
//     里的 `[this+0B5B4h] = 1`）置 1；
//   · 窗还在走时每帧再置 1（`sub_4B06D0` 内的 `0x4B326A` / `0x4B3CCB`）；
//   · **渲染步入**（`0x4B40AA` / `0x4B40B0`）把它与脏位（`+0B5AC`）一起清 0 ⇒ 本模型的对应物是
//     `advance_scene()` 的开头（每步都跑，不依赖宿主是否呈现；见 vm_timing.cpp）。
//   ⇒ 窗没走完之前本条不放行；这就是"颜色窗会挡住 0x21c"的机制（残差 `RES-WAIT-PENDING` 里那条
//     "写者全集未取证"因此收窄到"已取证这些"）。
bool Machine::message_subsystem_busy() {
    if (msg_wait_c_ != 0) {
        if (msg_wait_b_ == 0) msg_wait_b_ = static_cast<std::int32_t>(scene_clock_ms_);
        const auto start = static_cast<std::uint32_t>(msg_wait_b_);
        const auto duration = static_cast<std::uint32_t>(msg_wait_c_);
        if (msg_wait_a_ != 1 && scene_clock_ms_ <= start + duration) return true;  // 窗内（相等算窗内）
        msg_wait_b_ = 0;
        msg_wait_c_ = 0;
    }
    // 待处理闩（`+0B5B4`）：排过窗、或窗还没走完 ⇒ 消息子系统不算完成。
    if (scene_pending_) return true;
    // ★ 不再另加"已排窗但还没盖章"的兜底：引擎在**第一次推进时无条件盖章**（`0x4B087C`，
    //   只判起点为 0、不看持续时间），所以"排过窗"那一帧的忙态由 setter 置起的闩本身表达
    //   （`0x4AD155` 等），下一次推进就把起点盖上。那个兜底在引擎里没有对应物。
    // ★ **打字位 / 待画格不是谓词 A 的一部分**（`CORE-OPS-WAIT` 的成立条件：谓词 A = 它自己的定时器
    //   在窗内 **或** 等待谓词的待处理闩）。把它们算进来会死锁：位序表里第 10 位在第 14 项、
    //   第 29 位在第 17 项 ⇒ 等待位先被服务；若谓词 A 因为"还在打字"而恒为真，就永远轮不到
    //   第 29 位去推进，打字也永远不结束（实测：`emulator-core-adv-click-rate` 卡在
    //   `gate=0xE0000400` 上只推进 18 句）。⇒ 打字 / 演示闸期间点击照常放行等待位。
    return false;
}

// `0x21c` 谓词 B（`0x41322A` 的 var_768 ← `0x412648` 扫 1000 槽 + `sub_4050E0`）：
// 影片对象表里还有对象处于"等它播完"的状态 ⇒ 不放行。
// 判据 = 生效参数 `& 5 == 4`（第 0 位 = 循环、第 2 位 = 未取证，残差 `RES-WAIT-MOVIE-BIT2`）；
// 本模型没有"延时启动待办"那一支，直接用最近一次启动的参数。
bool Machine::movie_gate_busy() const {
    for (const auto& [slot, record] : movies_) {
        (void)slot;
        if (!record.present || !record.started) continue;
        if ((record.options & 5u) == 4u) return true;
    }
    return false;
}

// 第 27 位（位序表第 16 行）的服务体：进入**一次消息处理**（读一条消息并派发）→ 让出一拍 →
// 窥视消息（不移除）→ 交回迭代头（本迭代不派发脚本指令）。
// ★ 它与迭代头按节流周期触发的宿主泵**不是同一处**、也不受那个计数器约束（`60-ops-scalar`「成对写」）。
// ★ **不自己清第 27 位**：清位归消息处理（未建模）与脚本指令（`0x88` 为 0 支 / `0x19b` / `0x101` …）。
void Machine::service_message_frame() {
    ++host_message_steps_;
    if (host_message_io_.process_one) host_message_io_.process_one();
    if (host_message_io_.yield) host_message_io_.yield();
    if (host_message_io_.peek) (void)host_message_io_.peek();
}

// ── 门服务链（`CORE-VM-GATE` / `CORE-VM-STATE`）──
// `CORE-VM-STATE` 的**位序表**（spec：「检查序」= 多个位同时置起时的优先级；位号从 0 起）。
// ⛔ **不是位号升序**（列表只有一份 = 下面的数组，⛔ 别在注释里再抄一遍）。
// ★ 相邻两项的接续就是表里的「服务后 = 继续扫描」：14 落到 9、9 落到 12、12 落到 26、
//   30 落到 13、13（被暂停挡下时）落到 3。
constexpr std::int32_t kGateServiceOrder[] = {
    22, 24, 14, 9, 12, 26, 20, 30, 13, 3, 4, 7, 8, 10, 6, 27, 29, 31, 0, 5, 28, 23,
};

Machine::GateAction Machine::service_gate_bit(std::int32_t bit) {
    // ★ 位序表（`kGateServiceOrder`）决定**先服务谁**；这里只决定"这一位被服务完之后控制交给谁"。
    //   `CORE-VM-STATE` 的「服务后」列：多数位交回迭代头；14 / 9 / 30 一律继续扫描；
    //   12 / 13 看区间忙；29 是**唯一**"服务体自己派发"的位。
    switch (bit) {
        case 0:
            // 位序表第 19 行（一个计时器的到期位；服务后 = 交回迭代头）。
            // ★ 本实现没有这个计时器的服务体：按"服务一次就清位"处理（沿用本片之前的模型）。
            state_flags_ &= ~1u;
            return GateAction::ToHead;
        case 9:
            // 位序表第 4 行：音乐音量渐变进行中（`0xc2` 的淡变 / 压低后的恢复共用同一生命周期）。
            // 按计时器步进渐变（进度满）后清第 9 位；服务后 = **继续扫描（落到第 12 位）**。
            service_bgm_fade();
            return GateAction::Continue;
        case 10:
            // 位序表第 14 行：等待位（`0x21c`）。谓词 A（消息子系统"还有事"）与谓词 B（影片对象扫描）
            // **都为假**时清位；服务后 = 交回迭代头（见 `message_subsystem_busy()` 与 `60-ops-wait`）。
            if (!message_subsystem_busy() && !movie_gate_busy()) state_flags_ &= ~(1u << 10);
            return GateAction::ToHead;
        case 12:
            // 位序表第 5 行：**仅当区间忙 = 0** 才做"一个等待 / 放行步（与第 11 位配对）"。
            // ★ 那个步的内部三支判据**未取证**（残差 `RES-CLEAR-D9-BIT12` / `60-ops-scalar.md:297`）
            //   ⇒ 本实现只落"区间忙"这道前置：
            //   · 区间忙 ≠ 0 ⇒ 服务体**不执行**（`CORE-OPS-RUN-BRACKET` 的前置）⇒ 不算交回迭代头 ⇒
            //     **继续扫描（落到第 26 位）** —— 这正是"暂停不挡表尾派发"（`70-vm.md:25`）；
            //   · 区间忙 = 0 ⇒ 体执行；体内三支未取证 ⇒ 按保守选择**交回迭代头**。
            if (driven_elsewhere_) return GateAction::Continue;
            return GateAction::ToHead;
        case 13:
            // 位序表第 9 行：影片位（门上 `0x2000`）——只清位；对象表仍有已启动影片槽 ⇒ 再置位。
            // **被暂停挡下时落到第 3 位**（`70-vm.md:84`）；这一位的判据只测区间忙，等待位**不**为它开闸
            // （`CORE-VM-MOVIE`）。其余情形服务后 = 交回迭代头。
            if (driven_elsewhere_) return GateAction::Continue;
            state_flags_ &= ~(1u << 13);
            if (movie_active_) state_flags_ |= (1u << 13);
            return GateAction::ToHead;
        case 14:
            // 位序表第 3 行：音乐压低待执行。前置 = 语音子系统空闲 ∧「该子系统仍有一件未完成的工作」
            // 标记 ≠ 0 ∧ 第 9 位未置（不成立 ⇒ 第 14 位**保留**、下个迭代再判）；成立 ⇒ 清 14、置 9
            // 并起计时。服务后 = **继续扫描（落到第 9 位）**。
            service_bgm_duck();
            return GateAction::Continue;
        case 26:
            // 位序表第 6 行：本地块内的输入等待（读输入字，非 0 则清位并按副进入目标重入）；
            // 服务后 = 交回迭代头（`CORE-OPS-FRAME-LOCAL-RET-7C` 的输入服务分支）。
            service_local_input_wait();
            return GateAction::ToHead;
        case 27:
            // 位序表第 16 行：消息逐帧推进门位（又称「跳过位」）——进入一次消息处理、让出一拍、
            // 窥视一次，然后**交回迭代头**（本迭代不派发脚本指令）。
            service_message_frame();
            return GateAction::ToHead;
        case 29:
            // 位序表第 17 行：文本推进器；推进器跑完之后按**文本槽在用标志**（`[77984h]`）决定收场 ——
            // **有槽在用 ⇒ 服务体自己派发一条**（表里唯一的直派位）；**无槽在用 ⇒ 交回迭代头**。
            // spec 句：`70-vm.md` 服务链位序表第 17 行 +「门」条第三收场（`0x41342D` / `0x413433`）。
            // ★ 这一格的判据是「槽 1..9 任一状态字非 0」（`0x409421` / `0x409426`），**不是**待处理
            //   命令队列的占用标记：队列判空是「头 ≥ 尾」，且投递口就在同一次调用里顺手泵掉 ⇒ 门上
            //   那一刻它几乎恒 0。拿队列当判据 = 演示闸永远派发不出去（取证 `report-text-gate.md` §5.3）。
            if (service_text_layers()) return GateAction::Dispatch;
            return GateAction::ToHead;
        case 30:
            // 位序表第 8 行：逐格揭示——按节拍请求画一格并推进格号；**不负责清位**；
            // 服务后 = **继续扫描（落到第 13 位）**（`70-vm.md:117` / `:65`）。
            service_text_cell_reveal();
            return GateAction::Continue;
        case 31:
            // 位序表第 18 行：等推进（抑制态）——等待一步；服务后 = 交回迭代头。
            service_advance_wait();
            return GateAction::ToHead;
        default:
            // 表里其余位（22 / 24 / 20 / 3 / 4 / 7 / 8 / 6 / 5 / 28 / 23）在这个模型里**没有服务体**：
            // 一律按"交回迭代头"（保守选择，也是本片之前的表现）。它们的领域语义见残差
            // `RES-VM-BIT-CLEARERS` / `RES-VM-OTHER-BITS-SUBSYSTEMS` —— ⛔ 不猜名字、⛔ 不发明行为。
            return GateAction::ToHead;
    }
}

bool Machine::step() {
    if (halted_) return false;
    // ★ 诊断足迹：先记"当前帧脚本成为过执行脚本"（即使这一步只走门、没派发），
    //   静态足迹据此解码"这次运行装载到的脚本"。
    footprint_.note_script(frame().script_id);
    // 迭代头第一件事：宿主消息泵的**节流**（`CORE-VM-LOOP`；四迭代的固定周期，泵完隔 3 个迭代）。
    // ★ 只落计数序列与 `should_pump_host()`；**宿主的实际泵仍由现有 SDL 路径承担**。
    advance_host_pump();
    // 执行区间的暂停 = 区间忙标记 ≠ 0 **且**等待位 = 0（`70-vm.md:24`）。它挡的是**时间交付**、
    // 影片逐槽计时与门前那一次**请求** —— ⛔ 不挡盖钟（盖钟在 `advance_render_step` 里、忙判定之前）。
    const bool paused = driven_elsewhere_ && (state_flags_ & (1u << 10)) == 0;
    // 迭代头：宿主毫秒时钟的一次采样（`CORE-TIMING-CLOCK`）。没有外部时钟时，虚拟宿主时钟
    // 每迭代 +16ms（固定步长是仿真参数，不是引擎常量）；★ 它是**采样**，与"场景时钟有没有被盖"
    // 无关 —— 门前的盖钟可能被 (a)(b)(c) 跳过，但宿主时刻照走，门后 `0x1f4` / `0x20c` / `0x23c`
    // 的再采样读到的是这个值。
    if (!external_clock_) host_clock_ms_ += 16;
    const std::uint32_t now_ms = host_clock_ms_;
    // 时间交付（`CORE-VM-LOOP` 的「一次迭代做哪些事」）：把这次读数交给**两个声音子系统各一次**。
    // ★ 它是**两套到期计时器**、不是混音：只推进引擎自己的通道记录、不发宿主请求。
    // ★ 跳过条件只有一条：区间忙标记 = 1 **且**等待位 = 0 ⇒ 两个接收者本迭代都不被调用（不盖章、不计时）；
    //   等待位 = 1 时**照常交付**（`70-vm.md:22`）。
    if (!paused) {
        deliver_sound_time(now_ms);
        deliver_voice_time(now_ms);
    }
    // ★ 门之前那个渲染步的"更早子步"（`CORE-TIMING-WINDOW`）：三个"整段跳过" → 盖钟 → 五个"只挡推进"。
    //   `Skipped` ⇒ 本迭代不盖钟、不判据、不发请求、不清帧标志、不推窗；`ClockOnly` ⇒ 钟走了、窗没走；
    //   `Windows` ⇒ 走到请求那一行。
    const SceneAdvance scene_advance = advance_render_step(now_ms);
    // 门之前的影片对象表服务（`CORE-VM-MOVIE`）：新帧会置脏，所以必须在门前那次渲染判定之前。
    if (!paused) service_movies();
    // ★ **位上的一切服务都在门的服务链里**（`CORE-VM-GATE` / `CORE-VM-STATE`）：BGM 淡变（第 9 位）、
    //   逐格揭示（第 30 位）、文本泵（第 29 位）、等推进（第 31 位）原先在门之前无条件跑，现在由
    //   位序表决定"这一迭代轮到谁"。这里只留**不属于门**的引擎路径。
    // 自动消息的到期服务（`0x1b7` 开启时由 `0x72` 武装）：到期就放行同一对位。
    service_auto_message();
    // 平台输入路径：把宿主按钮源的上沿锁存一次（门上的第 31 位服务体只消费这个锁存）。
    sample_advance_input();
    // 门之前：**选项消费**（点中某条 ⇒ 执行位置 ← 该条的决定入口）。
    service_choice_click();
    // 门前渲染请求（`0x4127E2`）：**这条渲染步完整通过**三个"整段跳过"与五个"只挡推进"之后才发出
    // （`Windows`）。⇒ (a)(b)(c) 的跳过、第 24 位 / 影片持有 / 不需要重绘 / 未就绪 / (d) 暂停
    // 都**不发请求** —— 判据与请求同在那一块里（`0x412777`–`0x4127E7`）。
    // ★ 紧随请求之后清帧标志（`0x4127E7`）；(c) 跳过时**一并跳过**（spec 的 (c) 句）。
    if (scene_advance == SceneAdvance::Windows) {
        frame_step_requested_ = true;
        frame_flag_ = false;
    }
    // ── 门（`CORE-VM-GATE` / `CORE-VM-STATE`）──
    // 整字为 0 ⇒ 本迭代直接派发一条。整字非 0 ⇒ 先按**位序表**逐位扫服务链：
    //   · 服务体交回迭代头 ⇒ 本迭代到此为止、**不派发**（更晚的位本迭代不再看）；
    //   · 服务体继续扫描 ⇒ 落到表里的下一位；
    //   · 扫到表尾（一路没有任何服务体交回迭代头）⇒ **照常派发一条**；
    //   · 第 29 位命中且**有文本槽在用**（`[77984h]`：槽 1..9 任一状态字非 0，由推进器那一趟
    //     算出）⇒ 由**它的服务体自己**派发（表里唯一的"服务体直派"位）。
    // ★ 「门是忙的」与「本迭代仍派发一条」**不互斥**：spec 明文 ⛔ 不许写成"任一位置起就挡住派发"。
    // ★ 表外的位不挡派发；表里未建模的体按"交回迭代头"处理（见 `service_gate_bit`）。
    if (state_flags_ != 0) {
        for (const std::int32_t bit : kGateServiceOrder) {
            if ((state_flags_ & (1u << bit)) == 0) continue;  // 未置起 ⇒ 未命中
            // 诊断足迹：门服务链**命中并交给服务体**的位（位序 / 收场都不受影响）。
            footprint_.note_gate(bit);
            const GateAction action = service_gate_bit(bit);
            if (action == GateAction::ToHead) {
                // 合法等待（⛔ 不是停因）：位没被服务体清掉就一直关着，下一迭代继续服务同一位。
                return true;
            }
            if (action == GateAction::Dispatch) break;  // 第 29 位：服务体自己派发一条
            // Continue：按位序表里的**下一位**继续扫。
        }
        // 扫到表尾 ⇒ 派发一条（与第 29 位的直派共用下面这条出口）。
    }
    Frame& f = frame_mut();
    if (f.ip + 4 > f.code.size()) {
        halted_ = true;
        stop_.reason = StopReason::Error;
        stop_.offset = f.ip;
        stop_.message = "ip-past-code";
        return false;
    }

    const auto opcode = read_u32(f.code, f.ip);
    stop_.offset = f.ip;
    stop_.opcode = opcode;
    if (stop_at_opcode_ != 0 && opcode == stop_at_opcode_) {
        halted_ = true;
        stop_.reason = StopReason::StoppedAtOpcode;
        stop_.message = "stop-opcode";
        return false;
    }
    if (opcode > 0x3FF) {
        halted_ = true;
        stop_.reason = StopReason::BadOpcode;
        stop_.message = "opcode-out-of-domain:" + std::to_string(opcode);
        return false;
    }
    const OpcodeRow* row = table_.find(opcode);
    if (row == nullptr || !row->has_handler) {
        halted_ = true;
        stop_.reason = StopReason::UnsupportedOpcode;
        stop_.message = "opcode-without-handler:" + std::to_string(opcode);
        return false;
    }

    const std::size_t size = 4 + static_cast<std::size_t>(row->argc) * 8;
    if (f.ip + size > f.code.size()) {
        halted_ = true;
        stop_.reason = StopReason::Error;
        stop_.message = "instruction-past-code:" + std::to_string(size);
        return false;
    }
    // 诊断足迹：只记**真的要派发**的那一条（校验全部通过之后、执行之前）。
    footprint_.note_op(opcode, f.script_id);

    // ── 两个**写**的统计（`--dump-footprint` 的 `writer` 记录）──
    // ① 位写：本指令执行前后 `state_flags_` 的净变化 ⇒ 按（位号, opcode）记 set / clear。
    //    用 RAII 保证**异常 / 提前 return** 的路径也记（catch 里是 `return false`）。
    // ② 格写：`current_opcode` 在执行期间非 0 ⇒ `note_cell_write` 按（地址, opcode）归因。
    // ★ 门服务发生在快照之前 ⇒ 服务体的位 / 格写不会被算到上一条指令头上。
    const std::uint32_t state_before = state_flags_;
    struct StateWriteScope {
        Footprint& footprint;
        const std::uint32_t before;
        const std::uint32_t opcode;
        const std::uint32_t* after;
        ~StateWriteScope() {
            footprint.note_state_delta(before, *after, opcode);
            footprint.current_opcode = 0;
        }
    } state_write_scope{footprint_, state_before, opcode, &state_flags_};
    footprint_.current_opcode = opcode;

    Instruction instruction;
    instruction.offset = f.ip;
    instruction.opcode = opcode;
    instruction.operands.reserve(row->argc);
    for (std::uint32_t i = 0; i < row->argc; ++i) {
        instruction.operands.push_back(Operand{
            read_u32(f.code, f.ip + 4 + i * 8),
            read_u32(f.code, f.ip + 4 + i * 8 + 4),
        });
    }

    try {
        write_length(2 * row->argc + 1);
        execute(instruction);
    } catch (const DisplayMessageError& error) {
        // 显示消息型异常（「三类异常」）：错**不**静默 —— 记下错误码与要给玩家看的文本，
        // 主循环的"显示消息 + 重试 / 跳过 / 结束"三支本版未成篇 ⇒ 这里只把观察口填上并停机。
        // ★ 选择显示请求会在引擎的错误日志里写一行「致命错误 + 文本」（`05-boot.md:191`）⇒
        //   走**输出对象**的「报告」那一项（`45-objects`：日志落点写 `\r\n致命的エラー[...]\r\n`，
        //   再把文本交给根对象的错误处理）。⛔ 这不是错误通道（`report_error`），别混。
        last_display_code_ = error.code();
        last_display_message_ = error.text();
        output_report(error.text());
        halted_ = true;
        stop_.reason = StopReason::Error;
        stop_.message = error.what();
        return false;
    } catch (const std::exception& e) {
        // 诊断扫场（`--sweep-missing`）：缺 handler 不中止，记账后当空操作继续 —— 一次跑遍界面
        // 就能把所有缺口按首次出现顺序列出来。⛔ 这不是引擎语义（跳过会让脚本失真）。
        const std::string what = e.what();
        const std::string prefix = "opcode-not-implemented:";
        if (sweep_missing_ && what.compare(0, prefix.size(), prefix) == 0) {
            note_missing_opcode(opcode);
        } else {
            halted_ = true;
            stop_.reason = StopReason::Error;
            stop_.message = what;
            return false;
        }
    }
    if (is_scene_mutating(opcode)) scene_dirty_ = true;

    if (!halted_) {
        const auto advance = 4u * frame_mut().length_slot;
        if (advance == 0) {
            // 控制转移已自行设置 ip；不额外推进。
        } else {
            frame_mut().ip += advance;
        }
    }
    return !halted_;
}

RunResult Machine::run_until_script(std::uint32_t script_id, std::size_t max_steps) {
    std::size_t steps = 0;
    while (!halted_ && steps < max_steps) {
        if (frame().script_id == script_id) {
            stop_.reason = StopReason::ScriptReached;
            stop_.message = "script-reached";
            break;
        }
        if (!step()) {
            if (stop_.reason == StopReason::Halted) ++steps;
            break;
        }
        ++steps;
    }
    if (!halted_ && steps == max_steps && stop_.reason != StopReason::ScriptReached) {
        stop_.reason = StopReason::StepLimit;
        stop_.message = "step-limit";
    }
    stop_.steps = steps;
    return stop_;
}

RunResult Machine::run(std::size_t max_steps) {
    std::size_t steps = 0;
    while (!halted_ && steps < max_steps) {
        if (!step()) {
            if (stop_.reason == StopReason::Halted) ++steps;
            break;
        }
        ++steps;
    }
    if (!halted_ && steps == max_steps) {
        stop_.reason = StopReason::StepLimit;
        stop_.message = "step-limit";
    }
    stop_.steps = steps;
    return stop_;
}

}  // namespace core
