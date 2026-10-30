#pragma once

#include "core/bytes.h"
#include "core/config.h"
#include "core/movie.h"
#include "core/opcode_table.h"
#include "core/script.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <vector>

namespace core {

// 存档族的宿主边界与头部结构（定义见 `core/save_host.h`）。
class SaveHost;
struct SaveHeader;

// 「无颜色」哨兵（机器细节；与 `CORE-TIMING-WINDOW` / 场景族共用这一口径）。
inline constexpr std::uint32_t kNoColor = 0xFFFFFFFFu;

struct Install;

// 一个脚本帧：代码区（body）、指令指针（字节偏移）、长度槽、返回栈、局部池。
// 局部整型池按规范保存**编码后的位模式**；缺省槽是 ENC(0)，不是 0。
struct Frame {
    bool loaded = false;
    Bytes code;
    std::size_t ip = 0;
    std::uint32_t length_slot = 0;
    std::uint32_t position = 0;
    std::int32_t parent = -1;
    std::uint32_t script_id = 0;
    // 脚本头里的 body 起址：`ip` 是 **body 内偏移**，文件内偏移 = `body_offset + ip`。
    // ★ 诊断用（halt 时报告"哪个 BIN 的哪个偏移"）；⛔ 不是脚本可见量。
    std::uint32_t body_offset = 0;
    std::vector<std::int32_t> ret_stack;
    // 压返回栈的**位置**（dword 下标；与 `ret_stack` 同深）。`0x8d` 的 call 与几条回调登记
    // （`vm_ops_io`）共用这条栈 ⇒ 报告里叫 `call_at`，含义是"这一层最近一次登记返回点的地方"。
    std::vector<std::int32_t> call_sites;
    std::unordered_map<std::uint32_t, std::uint32_t> local_int;
    std::unordered_map<std::uint32_t, float> local_float;
    std::unordered_map<std::uint32_t, std::string> local_string;
    std::unordered_map<std::int32_t, std::int32_t> key_jumps;
    std::int32_t key_cursor = 0;
    // 按键扫描上限（`0xff` 写、`0x100` / `0x104` 读）：采样时的键总数快照；`0x104` 还会把它夹到被掩码命中的键号。
    // ★ 与 `Machine::key_total_` **不是同一格**：`0x100` 的扫描上界是键总数本身（`CORE-INPUT-KEY-DISPATCH`）。
    std::int32_t key_limit = 0;
    // 进入本地块的两个目标位置（`0x7b` 写、`0x199` 读；初值 −1 = 未登记）。
    std::int32_t local_main = -1;
    std::int32_t local_sub = -1;
    // 本帧记录里的**身份编号**（= 该帧所跑脚本的统一文件 id）；`0x199` / `0x7c` 的深度判据比它。
    // ★ 不是帧号、不是调用层数。
    std::int32_t local_depth = -1;
    // `0x102` 的**按住派发表**：键号 × 10 + 槽号 → 目标位置（dword 下标；−1 = 未登记）。
    // ★ 按帧分层、每（帧 × 键）固定 10 槽；⛔ 不与 key_jumps 合表。
    // ★ 键用 64 位算：槽号 = 按住游标，而模数**不做范围检查** ⇒ 32 位算会溢出（UB）。
    std::unordered_map<std::int64_t, std::int32_t> hold_slots;
};

enum class StopReason {
    Running,
    Halted,
    UnsupportedOpcode,
    BadOpcode,
    StepLimit,
    StoppedAtOpcode,
    ScriptReached,
    // 门一直关着，且挡门的位没有任何服务分支（规范：其余位只登记存在；第 30 位启用后门恒关）。
    // ★ 这是**观测**结论，不是引擎语义：引擎表现为"停在门上"，外面看不出与"等输入"的区别。
    GateStalled,
    Error,
};

struct ListRecord {
    std::int32_t f0c = 0;
    std::int32_t f10 = 0;
    std::int32_t f14 = 0;
    std::int32_t f18 = 0;
    std::int32_t f1c = 0;
    std::int32_t f20 = 0;
    std::int32_t f24 = 0;
    std::int32_t f28 = 0;
    std::int32_t f7c = 0;
    std::int32_t f80 = 0;
    std::int32_t f3c = 0;
    std::int32_t f40 = 0;
    std::int32_t f44 = 0;
    std::int32_t f48 = 0;
    std::int32_t f4c = 0;
    std::int32_t f50 = 0;
    std::int32_t f54 = 0;
    std::int32_t f58 = 0;
    std::int32_t f5c = 0;
    std::int32_t f60 = 0;
    std::int32_t f64 = 0;
    std::int32_t f68 = 0;
    std::int32_t f6c = 0;
    // `0x213` 是否写过键基数：0 是**合法值**，不能用 `f68 <= 0` 当"没写过"。
    bool key_base_set = false;
    // 宿主兜底基数是否已写进 `f68`（`0x70` 在该层**没有** `0x213` 时补一个，见残差
    // `ADV-TEXT-LAYER-DRAW-KEY`）。★ 它与 `key_base_set` **不是**一回事：前者 = "脚本写过"，
    // 后者 = "f68 现在可用"。⛔ 把"脚本没写过"当成"没有基数"会让 ADV 消息层一个字都画不出来
    // （本作 ADV 的消息层**不调** `0x213`）。
    bool key_base_fallback = false;
    // 逐字符绘制条目的键基数是否可用（脚本写过 **或** 兜底写过）。
    bool key_base_ready() const { return key_base_set || key_base_fallback; }
    std::int32_t f114 = 0;
    std::int32_t f118 = 0;
    std::int32_t f120 = 0;
    std::int32_t f124 = 0;
    std::int32_t registered = 0;
};

struct DrawItem {
    std::uint32_t flags = 0;
    std::uint32_t texture_id = 0;
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    float aux[3]{};
    std::uint32_t color = 0xFFFFFFFFu;
    std::uint32_t target_color = 0xFFFFFFFFu;
    // `0x203` 第 2 个操作数（记录里的一个整数位；本版没有读者）。
    std::int32_t set_color_int = 0;
    std::int32_t delay = 0;
    std::int32_t duration = 0;
    // 窗起点（毫秒）：0 = 未盖章哨兵（`CORE-TIMING-WINDOW`）。
    std::uint32_t window_start = 0;
    // 源矩形窗（`0x239`；与颜色窗共用「有窗」位与窗起点）。
    std::int32_t rect_delay = 0;
    std::int32_t rect_duration = 0;
    std::int32_t rect_step_total = 0;
    std::int32_t rect_row_steps = 0;
    std::int32_t rect_switch = 0;
    // 3D setter 族（`0x21e` / `0x21f`）**自己的**格与矩阵。
    // ★ 与 `0x1fd/1fe/1ff` 写的那组（`SceneRecord::scale/rotate_*`）不是同一组（规范明文不同矩阵）。
    std::int32_t config_g3 = 0;
    std::int32_t set21e_int0 = 0;
    std::int32_t set21e_int1 = 0;
    std::array<float, 16> set21e_scale{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::int32_t set21f_int0 = 0;
    std::int32_t set21f_int1 = 0;
    float set21f_axis[3]{};
    float set21f_degrees = 0.0f;
    std::array<float, 16> set21f_rot{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

struct SceneRecord {
    // 记录首字的两个位：位 0 = 「已存在」（`0x1fb` 置）、位 1 = 「已配置」（`0x220` 置）。
    std::int32_t flags = 0;
    // `0x1fb` / `0x23b` 在**建记录时**写的那一个整数标签（引擎 payload+4；`0x215` 读它）。
    std::int32_t tag = 0;
    // `0x242` 写的第一处（引擎容器 `+0x408` 的记录里 `+0x2D0`）；与 `tag`（`+0x4`）**不是同一格**。
    std::int32_t field_2d0 = 0;
    // `0x256` 给一段键区间内的记录写的：一个标志字 + 一个平移矩阵的三个分量。
    std::int32_t flag_256 = 0;
    float matrix_translate[3]{};
    // `0x220` 写的四个互异整数格：G0（窗起点，清 0）/ G1 / G2（两个属性）/ G3（写 1）。
    std::int32_t g[4] = {0, 0, 0, 0};
    // `0x220` 写的**第四个平移矩阵**（与 `0x1ff` 的那个不是同一个）。
    float translate4[3]{};
    float scale[3]{1.0f, 1.0f, 1.0f};
    float translate[3]{};
    float rotate_axis[3]{};
    float rotate_degrees = 0.0f;
    std::uint32_t color = 0xFFFFFFFFu;
    // 两个三元向量（`0x218` / `0x21a` 读；`0x217` / `0x219` 写，后者本版未实现）。
    float vec1[3]{};
    float vec2[3]{};
};

// **场景自身变换**（`CORE-OPS-SCENE-SELF-TRANSFORM-229`，`0x229` 写）：
// 作用键区间 + 三元平移。绘制项的消费者对**键落在区间内**的条目额外加这一次平移
// （命令档则按「区间 A 的起点」跳过它）。默认 `active=false` ⇒ 不加。
struct SceneSelfTransform {
    bool active = false;
    std::uint32_t range_start = 0;
    std::uint32_t range_count = 0;
    float translate[3]{};
    bool covers(std::uint32_t key) const {
        return active && key >= range_start && key < range_start + range_count;
    }
};

// 1000 项表里每项的**标志两格**（`0x258` 写 / `0x259` 全清；观察口用）。
// 「场景颜色」请求（`0x32d`）：按 0..1 归一化的四通道换算成 ARGB 后交给渲染模块（命令 0x8B）。
// ★ 本模型只记**请求**（引擎侧不保存任何可观察状态），渲染模块是否用它不在本版。
struct SceneColorRequest {
    std::uint32_t argb = 0;
    std::size_t count = 0;
};struct TextureFlags {
    std::int32_t flag0 = 0;  // 位 0：恢复时的纹理重建门
    std::int32_t flag1 = 0;  // 位 1：本作没有读者
};

// **选项窗口的状态格**（`0x91` / `0x92` / `0x93` / `0x94` 写；`ADV-CHOICE-WINDOW`）：
// 它们只开关"窗口 / 闩锁"，真正的输入消费在帧循环里（本模型尚未建消费侧，见残差）。
struct ChoiceWindowState {
    std::int32_t step = 0;         // 上下键步长（`0x91` / `0x92` 的第 1 个操作数；`0x94` 写 10000）
    std::int32_t hover = -1;       // 悬停下标（-1 = 无）
    std::int32_t fallback = -1;    // 兜底跳转目标（`0x92` 的第 2 个操作数；清表复位 -1）
    bool latch = false;            // 完成闩锁：`0x93` 的 else 支置、`0x91` / `0x92` 消费
    bool input_gate = false;       // 输入总闸：`0x94` 置、`0x93` 清
    bool arm = false;              // arm-once 闸（命中测试只跑一次）
    bool move_hit = false;         // 鼠标移动时做命中测试的开关
    bool pending = false;          // 选择变更挂起
};

// **选项表的一条**（`ADV-CHOICE-ADD-90`，`0x90` 追加）：矩形四边 + 三个脚本入口（body 内字下标，-1 = 无）。
// 第 7 个操作数是「决定」入口、第 5 / 6 个是「进入 / 离开」入口（消费侧才有读者）。
inline constexpr std::int32_t kTextLayerFallbackBase = 200000;
inline constexpr std::size_t kChoiceCapacity = 100;  // 选项表容量（不是错误文本里写的 20）
struct ChoiceEntry {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    std::int32_t enter = -1;   // 第 5 个操作数
    std::int32_t leave = -1;   // 第 6 个操作数
    std::int32_t decide = -1;  // 第 7 个操作数
    std::int32_t extra = -1;   // `0x97` 按矩形匹配后写的附加整数（初值 -1）
    std::int32_t depth = 0;    // 追加时当前帧的深度档（本模型没有深度 ⇒ 恒 0，见残差）
};

// 影片对象（对象表的一格）：引擎状态 + 播放后端句柄（平台边界，核心不解释其形状）。
// Font 的**条目登记表**里的一条（`0x1D2` 入队 / `0x1D3` 按 key 查）。
// ★ 这是引擎状态（Font 侧），不是渲染缓冲：消费方未建模（残差）。域名词多半未取证 ⇒ 名字保持中性。
struct FontEntry {
    std::int32_t list = 0;    // +0x00 列表号
    std::int32_t value = 0;   // +0x14 值
    std::int32_t key = 0;     // +0x18 key
    std::uint32_t flags = 0;  // +0x28 flags（bit0 = 脏槽曾非 0 / bit29 = 可被 0x1D3 命中）
    std::string text;         // +0x2C 起的 std::string（本版没有写者）
};

// 消息窗口记录（`Engine+0x14D30` 的 Font 侧）里**本版建模的那一部分**：追加的正文 / 振り仮名 / 行数。
// ★ 窗口记录的其余字段（行结束三格、按键区间、`[vtbl+8]` 载荷）未建模 ⇒ 残差。
// ★ 键是**窗口号**（0 = 引擎默认槽）：`0x6E` / `0x196` 追加、`0x6F` 行结束、`0x301` 释放。
struct TextWindowRecord {
    std::string text;   // 追加的正文（`0x6E` 的 op2 / `0x196` 的 op2）
    std::string rubi;   // 最近一次的振り仮名（`0x196` 的 op3，截断到 1023 字节）
    std::int32_t lines = 0;
};

// 逐字符显现（typewriter）的**待画格**。
// ★ 引擎：`0x6e` / `0x196` 在追加时一次性写好布局格与文本记录，**绘制侧每帧只推进一格**
//   （推进器 `sub_409400` 每帧 → `0x4096a9` 序号 +1，帧间按 `message:MessageSpeed` 睡眠，
//   有按键 / 跳过时把剩余格一次抽干）。我们的 `step()` 就是"一帧"（`advance_scene` 每步 +16ms），
//   所以这里排的是一个**跨帧队列**：追加时入队，每步画出一个。
//
// ★ 队里存的是**追加当时的字形样式快照**（字号 / 粗细 / 绘制模式 / 主色 / 副色），不是"画的时候再读字体对象"：
//   引擎的 `sub_46BE30` 在**追加时**就 SelectObject + 逐字写出（量度与光栅化都在那一刻完成），
//   之后的推进只是把已排好的格逐格呈现。画的时候再读字体对象会让"追加之后、这一格画出来之前"
//   的 `0x75` / `0x2bd` / `0x76` / `0x77` 写到**已经排好版的行**上 —— 实测症状两例：
//   ① 设置界面示例文本里恰好被跨帧夹住的那一个字按别的行的字号（30 → 20）画出来，字宽仍按 30；
//   ② 同一个字按**常规体**画出来（其余是粗体），因为那一帧脚本刚把 `0x2bd` 写成 0。
struct PendingGlyph {
    std::int32_t slot = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t x1 = 0;  // 这一格的右 / 下边 = 逐字符绘制条目的源矩形（`0x300` 重放时要用）
    std::int32_t y1 = 0;
    std::int32_t size = 0;    // 追加当时的字号（A 支或振り仮名的 B 支）；0 ⇒ 回退到"画的时候再读"
    std::int32_t weight = 0;  // 追加当时的粗细（`0x2bd` 的两格）；0 ⇒ 回退
    std::int32_t draw_mode = 0;
    std::int32_t primary = 0;
    std::int32_t secondary = 0;
    std::string unit;       // 一个字符的 SJIS 字节串（1~2 字节；振り仮名可能是一整串）
};

// 文本层（`0x70` / `0x79` / `0x213` / `0x71` 一起描述的那张离屏画布）的**宿主侧游标**。
// ★ 层矩形 / 文本原点 / 绘制项键基数仍落在 `ListRecord`（`0x70` / `0x79` / `0x213` 的既有格）；
//   这里只存"下一个字符画在哪"，它是引擎在一帧内推进的状态，不是独立领域量。
struct TextLayerState {
    std::int32_t cursor_x = 0;
    std::int32_t cursor_y = 0;
    std::int32_t draw_index = 0;  // 本层已排的逐字符绘制条目数（键 = 键基数 + draw_index）
    bool texture_ready = false;
    // ── `0x300` 的**循环演示闸**（规则 `CORE-OPS-TEXT-SLOT-TIMER`）──
    // 本层已追加的字符格（样式快照同 `PendingGlyph`）+ 揭示游标：闸开时泵每 `MessageSpeed` 毫秒
    // 揭示一格；整段揭示完等「期限」毫秒后清掉本层绘制条目、游标归 0（闸位保持）⇒ 从第一格重来。
    std::vector<PendingGlyph> cells;
    std::size_t reveal_cursor = 0;
    std::uint32_t reveal_due_ms = 0;   // 下一格的到期时刻（0 = 立刻）
    std::uint32_t reveal_done_ms = 0;  // 最后一格揭示的时刻（0 = 还没揭示完）
    std::int32_t gate_cycles = 0;      // 已完成的重放轮数（守卫用）
};

struct MovieRecord {
    bool present = false;
    bool started = false;  // 只有成功启动过才进「播完」判据 / 维持影片位
    std::uint32_t file_id = 0;
    std::uint32_t options = 0;
    // 启播时的引擎时钟（毫秒）：后端报告的「当前位置」由它换算。
    std::uint32_t started_ms = 0;
    std::shared_ptr<MovieBackend> backend;
};

struct MeshRecord {
    std::uint32_t flags = 0;
    std::int32_t count = 0;
    std::int32_t op10 = 0;
    std::vector<std::uint32_t> copies[2];
    std::vector<std::uint32_t> colors;
    std::uint32_t color = 0xFFFFFFFFu;
    std::uint32_t target_color = 0xFFFFFFFFu;
    std::int32_t delay = 0;
    std::int32_t duration = 0;
    std::uint32_t window_start = 0;
};

// ── 纹理的两层：**记录**与**槽里的对象**（⛔ 不要再合成一张表）──
// 2026-10-29 的跨界面纹理污染就是"合成一张表 + `0x1fa` 删整条"造成的：删记录把版本号打回 1，
// 窗口路径的 GPU 缓存按 (id, revision) 命中，于是后进的那个界面拿到对方的纹理。
// spec 的「释放纹理」（`0x1fa`）本来就分两步：把**记录的来源文件**写成「无」、清**槽里的对象**；
// 记录本身保留（颜色键等字段不动）⇒「槽空」≠「空白纹理」是两种状态。

// **纹理记录**：由 `0x1f8`（空白创建）/ `0x1f9`（设纹理）建或替换，`0x1fa` / `0x23d` / `0x1af` 不删它。
struct TextureRecord {
    std::int32_t file = -1;
    std::uint32_t color_key = 0;
    bool blank = false;
    // 「内容非文件装载」标志（`0x1f8` 空白创建 / `0x20b` 填充共用；全语料无读点）。
    bool non_file = false;
    // 1000 项表里**每项的标志两格**（`0x258` 写 / `0x259` 全清；`CORE-OPS-SCENE-TABLE-FLAGS`）：
    //   flag0 = 位 0 = **恢复时的纹理重建门**；flag1 = 位 1 = 本作**没有读者**。
    // ★ 引擎把它存在纹理记录的同一项里（两张平行表 = 活动表 + 快照，本模型只有活动表）。
    std::int32_t flag0 = 0;
    std::int32_t flag1 = 0;
};

// **纹理槽里的对象**：像素 + 尺寸 + 版本。`0x1fa` / `0x23d` 只清它（`present = false`）。
struct TextureSlotObject {
    bool present = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // 像素内容版本：宿主用它判断是否需要重传 GPU 纹理（不是引擎字段）。
    std::uint32_t revision = 0;
    Bytes rgba;  // top-down RGBA8；空白纹理为透明
};

struct TextureEntry {
    TextureRecord record;
    TextureSlotObject slot;
};

struct SceneCommandRecord {
    std::int32_t kind = 0;
    std::uint32_t key = 0;
    std::uint32_t texture_id = 0;
    std::int32_t a_start = 0;
    std::int32_t a_count = 0;
    std::int32_t b_start = 0;
    std::int32_t b_count = 0;
    std::int32_t delay = 0;
    std::int32_t duration = 0;
    std::uint32_t timestamp = 0;
    // 种类 1：影片目标矩形（右 / 下由宽高加出来）与遮罩取值。
    std::int32_t rect[4]{};
    std::int32_t mask_flag = 0;
    bool mask_filled = false;
    // 种类 2：抹除图案 / 分割幅。
    std::int32_t wipe_type = 0;
    std::int32_t split = 1;
    // 种类 3：变体格（0 = SlideBlur / 1 = ZoomBlur）与「起值 → 终值」。
    std::int32_t variant = 0;
    std::int32_t from[3]{};
    std::int32_t to[3]{};
};

struct RunResult {
    StopReason reason = StopReason::Running;
    std::size_t steps = 0;
    std::uint32_t opcode = 0;
    std::size_t offset = 0;
    std::string message;
};

// 最小 CORE-VM：一次 step 至多执行一条指令；状态选择字恒 0（状态位服务尚未实现）。
class Machine {
public:
    static constexpr std::size_t kFrameCount = 40;

    Machine(std::uint32_t int_key, const OpcodeTable& table, const Install* install = nullptr,
            ConfigStore config = ConfigStore{});

    // 解析脚本头并把 body 装进当前帧（根脚本 id 0）。
    void load_root(const Bytes& script, std::uint32_t script_id = 0);
    void stop_at_opcode(std::uint32_t opcode) { stop_at_opcode_ = opcode; }

    // 执行至多 max_steps 条；未实现 / 非法 opcode 停在它**前面**。
    RunResult run(std::size_t max_steps);

    // 执行到某脚本成为当前帧为止（不执行该脚本的第一条指令）；用于“到 TITLE 就开窗”。
    RunResult run_until_script(std::uint32_t script_id, std::size_t max_steps);

    // 执行一条；返回 false 表示执行已结束。
    bool step();

    const Frame& frame() const { return frames_[current_frame_]; }
    std::size_t current_frame() const { return current_frame_; }
    std::uint32_t state_flags() const { return state_flags_; }
    bool voice_playing() const { return voice_playing_; }
    std::int32_t voice_volume(std::size_t group) const { return voice_volumes_[group]; }
    std::int32_t scalar_149() const { return scalar_149_; }
    std::int32_t scalar_21b() const { return scalar_21b_; }
    // `0xC0` 读的引擎标量（观察口 + 测试注入；写者 `0xC3` 未实现）。
    std::int32_t engine_aa9e4() const { return engine_aa9e4_; }
    void seed_engine_aa9e4(std::int32_t value) { engine_aa9e4_ = value; }
    // `0x142` / `0x1BB` 写的引擎格（观察口）。
    std::int32_t engine_aab70() const { return engine_aab70_; }
    std::int32_t font_text_gate() const { return font_text_gate_5ec7c_; }
    // 字体槽表的观察口（`0x300` / `0x301`）。
    std::int32_t font_slot_state(std::int32_t index) const {
        const auto it = font_slot_state_.find(index);
        return it == font_slot_state_.end() ? 0 : it->second;
    }
    std::int32_t font_slot_deadline(std::int32_t index) const {
        const auto it = font_slot_deadline_.find(index);
        return it == font_slot_deadline_.end() ? 0 : it->second;
    }
    std::int32_t font_slot_start(std::int32_t index) const {
        const auto it = font_slot_start_.find(index);
        return it == font_slot_start_.end() ? 0 : it->second;
    }
    const std::vector<FontEntry>& font_entries() const { return font_entries_; }
    // 消息窗口的观察口（`0x6E` / `0x6F` / `0x196`）。
    std::int32_t current_text_slot() const { return current_text_slot_; }
    const TextWindowRecord* text_window(std::int32_t index) const {
        const auto it = text_windows_.find(index);
        return it == text_windows_.end() ? nullptr : &it->second;
    }
    // 文本层观察口（守卫用；⛔ 不是产品接口）：画布纹理编号 = 20 + 层号；游标是纹理内坐标。
    static constexpr std::uint32_t text_layer_texture_id(std::size_t slot) {
        return static_cast<std::uint32_t>(20 + slot);
    }
    std::int32_t text_layer_cursor_x(std::size_t slot) const { return text_layer_state_[slot].cursor_x; }
    std::int32_t text_layer_cursor_y(std::size_t slot) const { return text_layer_state_[slot].cursor_y; }
    bool text_layer_texture_ready(std::size_t slot) const { return text_layer_state_[slot].texture_ready; }
    std::int32_t text_layer_draw_key(std::size_t slot) const { return list_records_[slot].f68; }
    // `0x300` 循环演示闸（守卫用；⛔ 不是产品接口）：已揭示的格数 / 已完成的重放轮数。
    std::size_t text_layer_revealed(std::size_t slot) const {
        return text_layer_state_[slot].reveal_cursor;
    }
    std::int32_t text_gate_cycles(std::size_t slot) const { return text_layer_state_[slot].gate_cycles; }
    std::size_t text_layer_cell_count(std::size_t slot) const {
        return text_layer_state_[slot].cells.size();
    }
    // typewriter：还没画出来的待画格数（守卫用；⛔ 不是产品接口）。
    std::size_t pending_glyph_count() const { return pending_glyphs_.size(); }
    // 打字中（状态字第 29 位）与计时器间隔（守卫用；⛔ 不是产品接口）。
    bool text_typing() const { return (state_flags_ & (1u << 29)) != 0; }
    std::int32_t text_timer_interval_ms() const { return text_timer_interval_ms_; }
    // 文本记录的**字号支**：位 2（0x4）置起 ⇒ B 支（`0x197`），否则 A 支（`0x75`）。
    // 返回值是"画这条记录时给绘制入口的字号覆盖"（0 = 沿用 A 支，与 `0x204` 同口径）。
    std::int32_t record_font_size(std::int32_t flags) const {
        return (flags & 4) != 0 ? (font_b_height_ > 0 ? font_b_height_ : 10) : 0;
    }
    // 待画格的字号覆盖（守卫用：ruby run 的格应当是 B 支）。
    std::int32_t pending_glyph_size(std::size_t i) const { return pending_glyphs_[i].size; }
    std::int32_t scalar_252() const { return scalar_252_; }
    std::int32_t scalar_88a() const { return scalar_88a_; }
    std::int32_t scalar_88b() const { return scalar_88b_; }
    std::int32_t scalar_88c() const { return scalar_88c_; }
    std::int32_t named_message_read_text_skip() const { return config_.get_int("message:ReadTextSkip"); }
    const ListRecord& list_record(std::size_t group) const { return list_records_[group]; }
    std::int32_t default_group() const { return default_group_; }
    std::int32_t scalar_d107656() const { return scalar_d107656_; }
    std::int32_t font_draw_mode() const { return font_draw_mode_; }
    std::int32_t font_align_mode() const { return font_align_mode_; }
    // ★ `0x204` 的「缓存 ascent」（`Font+0x313F0` = 重建入口那张 TEXTMETRICA 的 tmAscent）**不单独建模**：
    //   它由 `GetTextMetricsA` 的出参写入，量的是**重建入口的固定参考字体**（常量 "ＭＳ ゴシック"），
    //   不是对象当前字面名那份渲染字体。绘制侧按 `reference_ascent()` 单独取（见 `text_raster.cpp`）。
    std::int32_t font_primary_color() const { return font_primary_color_; }
    std::int32_t font_secondary_color() const { return font_secondary_color_; }
    std::int32_t font_metric_a() const { return font_metric_a_; }
    std::int32_t font_metric_b() const { return font_metric_b_; }
    std::int32_t font_pair_a() const { return font_pair_a_; }
    std::int32_t font_pair_b() const { return font_pair_b_; }
    std::int32_t array107(std::size_t i) const { return array_d551_[i]; }
    std::int32_t array10b(std::size_t i) const { return array_d1383_[i]; }
    std::int32_t array30a(std::size_t i) const { return array_d1969_[i]; }
    std::int32_t scalar_25b_a() const { return scalar_25b_a_; }
    std::int32_t scalar_25b_b() const { return scalar_25b_b_; }
    std::int32_t scalar_2ee() const { return scalar_2ee_; }
    std::int32_t scalar_10f() const { return scalar_10f_; }
    std::int32_t scalar_248() const { return scalar_248_; }
    std::int32_t font_8b() const { return font_8b_; }
    // ★ 字号与行高是**两个量**：字号 = 字形高度（`Font+0x355D8`，缺省 8，`0x197` 写它），
    //   行高 = 行距（`Font+0x313D4`，缺省 24，`0x75` 写它）。合并成一个格会让「设字号」把行高也改掉。
    //   ⚠ 未建模：引擎绘制时把字号乘**显示缩放**（`Font+0x355E4`，初值 1.0，随渲染器高度比例刷新）。
    //   本宿主按**逻辑分辨率**渲染 ⇒ 该缩放恒为 1.0，乘法是恒等；换成分辨率无关的呈现时要补上。
    std::int32_t font_size() const { return glyph_height_; }
    std::int32_t glyph_height() const { return glyph_height_; }
    std::int32_t font_b_height() const { return font_b_height_; }
    // 十个**文本槽**：各自的字形高度（`槽+0xC8`）与绘制原点偏移（`槽+0x0C` / `槽+0x10`）。
    std::int32_t text_slot_glyph_height(std::size_t i) const { return text_slots_[i].glyph_height; }
    std::int32_t text_slot_x(std::size_t i) const { return text_slots_[i].x_offset; }
    std::int32_t text_slot_y(std::size_t i) const { return text_slots_[i].y_offset; }
    static constexpr std::size_t text_slot_count() { return 10; }
    const std::string& font_string() const { return font_string_; }
    std::int32_t engine5(std::size_t i) const { return engine5_[i]; }
    std::int32_t key_total() const { return key_total_; }
    // 引擎配置库（保存 / 脚本配置读写共用）。`config_mut()` 只给**自检 / 夹具**注入键值用
    // （引擎自己写配置走 0x1a9 / 0x1a2 那些入口，⛔ 不是这条旁路）。
    const ConfigStore& config() const { return config_; }
    ConfigStore& config_mut() { return config_; }
    std::int32_t input_a() const { return input_a_; }
    std::int32_t input_b() const { return input_b_; }
    std::uint32_t input_mode() const { return input_mode_; }
    // 输入字（`CORE-INPUT-WORD`）：`0xff` 清 / 采样、`0x100` 读、`0x101` 清。
    std::uint32_t input_word() const { return input_word_; }
    // ★ 平台边界注入点（测试 / 脚本化输入用）：⛔ 产品路径不要靠它伪造输入。
    void set_input_word(std::uint32_t value) { input_word_ = value; }
    // 平台输入采样（`0xff` 的外部请求）；缺省 = 没有设备 ⇒ 输入字保持 0。
    using KeySampler = std::function<std::uint32_t()>;
    void set_key_sampler(KeySampler sampler) { key_sampler_ = std::move(sampler); }
    // 宿主每帧喂一次"当前有没有输入被按下"（键盘 / 鼠标任一）。它只被**文本层的抽干**读：
    // 引擎在推进器里"有按键 ⇒ 把剩下的待画格一次画完"（`0x4096e0`–`0x4096f4` 的 do-while），
    // 那是主循环**被动观察**输入状态，⛔ 与脚本主动取键（`0xff` / `0x109`）不是一回事。
    void set_input_pressed(bool pressed) { input_pressed_ = pressed; }
    bool input_pressed() const { return input_pressed_; }
    // 滚轮增量（`0x10d` 读并清 `Engine+0x1E74`）：**宿主推**（窗口过程把 WM_MOUSEWHEEL 加进那一格）。
    // ★ 符号与刻度属平台边界（0x10d 只规定"读出后清 0"）⇒ 本版按宿主给的原值累加。
    void add_wheel_delta(std::int32_t delta) { wheel_accum_ += delta; }
    std::int32_t wheel_delta() const { return wheel_accum_; }
    // 一个当前触点（`0x2fc` 的外部请求）；坐标是**客户区**坐标，由平台负责映射。
    struct TouchPoint {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t flags = 0;
        std::int32_t extra = 0;
    };
    using TouchSource = std::function<bool(TouchPoint&)>;
    void set_touch_source(TouchSource source) { touch_source_ = std::move(source); }
    // 平台光标位置（`0x109` 的 `CORE-INPUT-MOUSE-POS`）：返回 false = 设备不可用（写 −100000）。
    // 坐标是**客户区**坐标，映射属平台边界。
    using CursorSource = std::function<bool(std::int32_t& x, std::int32_t& y)>;
    void set_cursor_source(CursorSource source) { cursor_source_ = std::move(source); }
    // 平台鼠标按钮位图（`0x108` 的 `CORE-INPUT-MOUSE-BUTTON`）：返回 false = 设备不可用（写 0）。
    // 位 0 左 / 位 1 右（左右交换属平台）。
    using MouseButtonSource = std::function<bool(std::int32_t& mask)>;
    void set_mouse_button_source(MouseButtonSource source) { mouse_button_source_ = std::move(source); }
    // 宿主随机源（`0x60` 的 `CORE-OPS-RANDOM-60`）：缺省是确定性 LCG（同种子同序列）。
    // ★ 返回值按**非负**使用（规范：被除数非负 ⇒ 余数非负）。
    using RandomSource = std::function<std::uint32_t()>;
    void set_random_source(RandomSource source) { random_source_ = std::move(source); }
    std::uint32_t scene_clock_ms() const { return scene_clock_ms_; }
    std::uint32_t scene_previous_ms() const { return scene_previous_ms_; }
    std::uint32_t engine_now_ms() const { return engine_now_ms_; }
    std::uint32_t engine_previous_ms() const { return engine_previous_ms_; }
    void set_driven_elsewhere(bool value) { driven_elsewhere_ = value; }
    // 启动时是否跑 LOGO（`0x130` 读的格）：`--skip-logo` 在跑根脚本前置 0。
    void set_show_logo(bool value) { show_logo_ = value ? 1 : 0; }
    bool scene_animating() const { return scene_animating_; }
    // 场景侧的"待处理"闩（引擎 `+0B5B4`）：排窗 / 窗还在走 ⇒ 1；渲染步入清 0。
    // 读者只有 `message_subsystem_busy()`（`0x21c` 谓词 A）；`0x4B3F9E` 还用它把脏位置起。
    bool scene_pending() const { return scene_pending_; }
    void set_scene_pending(bool value) { scene_pending_ = value; }
    bool forced_complete() const { return forced_complete_; }
    void force_complete() { forced_complete_ = true; }
    // 宿主提供时钟（毫秒）；不设置时每次 step 自动 +16ms（仿真的固定步长）。
    void set_host_clock_ms(std::uint32_t now_ms) { host_clock_ms_ = now_ms; external_clock_ = true; }
    // 回到自动步长时钟（每次 step +16ms）；用于无窗口的确定性逐帧导出。
    void reset_scene_clock(std::uint32_t now_ms = 0) {
        external_clock_ = false;
        scene_clock_ms_ = now_ms;
        scene_previous_ms_ = now_ms;
    }
    // 场景推进：盖章 / 插值 / 到期（`CORE-TIMING-WINDOW`）。step() 在门之前调用它。
    // ★ `advance_windows = false` = 只更新场景时钟（迭代头读数），**不推进窗** —— 这是
    //   「暂停」时主循环的行为（`CORE-VM-LOOP`：区间忙标记 = 1 且等待位 = 0 ⇒ 场景推进整段跳过）。
    void advance_scene(std::uint32_t now_ms, bool advance_windows = true);
    // 0x20C 的帧步进请求：由渲染步消费。
    bool frame_step_requested() const { return frame_step_requested_; }
    void request_render() { frame_step_requested_ = true; }
    bool consume_frame_step() {
        const bool value = frame_step_requested_;
        frame_step_requested_ = false;
        forced_complete_ = false;  // 渲染步收尾清「强制完成」（`CORE-OPS-SCENE-COMMAND-RECORDS`）
        return value;
    }
    bool scene_dirty() const { return scene_dirty_; }
    void mark_dirty() { scene_dirty_ = true; }
    std::int32_t draw_mode() const { return draw_mode_; }
    bool frame_flag() const { return frame_flag_; }
    void clear_frame_flag() { frame_flag_ = false; }
    bool render_target_present() const { return render_target_ != -1; }
    std::int32_t render_target() const { return render_target_; }
    std::int32_t blend_mode() const { return blend_mode_; }
    std::uint32_t blend_color() const { return blend_color_; }
    std::int32_t indirect_value(std::int32_t slot) const { const auto it = indirect_.find(slot); return it == indirect_.end() ? 0 : it->second; }
    std::size_t draw_item_count() const { return draw_items_.size(); }
    std::size_t texture_count() const { return textures_.size(); }
    std::size_t mesh_count() const { return meshes_.size(); }
    const std::unordered_map<std::uint32_t, DrawItem>& draw_items() const { return draw_items_; }
    const std::unordered_map<std::uint32_t, TextureEntry>& textures() const { return textures_; }
    // 测试 / 诊断注入点：直接写一个配置键（自动消息时长这类"配置驱动"的行为用它布场）。
    void set_config_int(const std::string& key, std::int32_t value) { config_.set_int(key, value); }
    // 测试 / 诊断注入点：直接布置纹理记录的来源文件；⛔ 产品路径不要靠它伪造状态。
    void set_texture_file(std::uint32_t slot, std::int32_t file) {
        textures_[slot].record.file = file;
    }
    // 释放守卫（`0x1fa` / `0x23d` 读；本语料没有写者，观察上恒 0）。
    std::int32_t surface_release_gate(std::uint32_t slot) const {
        return slot < surface_release_gate_.size() ? surface_release_gate_[slot] : 0;
    }
    void set_surface_release_gate(std::uint32_t slot, std::int32_t value) {
        if (slot < surface_release_gate_.size()) surface_release_gate_[slot] = value;
    }
    // 测试用：读一个全局整型槽（解码后）。缺槽 = 0。
    std::int32_t global_int(std::uint32_t index) const {
        const auto it = global_int_.find(index);
        return it == global_int_.end() ? 0 : decode_int(it->second);
    }
    // 诊断用：读**当前帧**的一个局部整型格（解码后）。缺格 = 0。
    // ★ 这是宿主的**观察口**（`--watch-locals`），⛔ 不是脚本可见量，也不改任何状态。
    std::int32_t local_int(std::uint32_t index) const;
    const std::unordered_map<std::uint32_t, MeshRecord>& meshes() const { return meshes_; }
    // `0x229` 写的场景自身变换（观察口：绘制项的消费者按它给区间内的条目加平移）。
    const SceneSelfTransform& scene_self_transform() const { return scene_self_transform_; }
    // `0x258` / `0x259` 写的两项标志（观察口；`0x258` 按位掩码写、`0x259` 全清）。
    TextureFlags texture_flags(std::uint32_t id) const {
        const auto it = textures_.find(id);
        if (it == textures_.end()) return TextureFlags{};
        return TextureFlags{it->second.record.flag0, it->second.record.flag1};
    }
    // 选项表（`0x90` 追加；观察口）。
    const std::vector<ChoiceEntry>& choices() const { return choices_; }
    std::int32_t choice_index() const { return choice_index_; }
    const ChoiceWindowState& choice_window() const { return choice_window_; }
    // 守卫用：摆一个非 -1 的兜底目标，验证 `0x93` 清表**会**把它复位成 -1
    // （`0x403F07 [ecx+74ACh] = -1`；`[+74ACh]` = engine `0CA84h`，就是 `0x92` 写的那一格）。
    void set_choice_fallback(std::int32_t value) { choice_window_.fallback = value; }
    // `0x32d` 发过的「场景颜色」请求（命令 0x8B）；`0x32a` 销毁过的子对象槽。
    const SceneColorRequest& scene_color_request() const { return scene_color_request_; }
    // `0x1b1` 写的那格（领域名未取证；ADV 脚本开头用它设一个整数档）。
    std::int32_t cell_1b1() const { return cell_1b1_; }
    std::int32_t cell_2e9() const { return cell_2e9_; }
    std::int32_t msg_arm() const { return msg_arm_; }
    std::int32_t msg_wait(std::size_t i) const {
        return i == 0 ? msg_wait_a_ : (i == 1 ? msg_wait_b_ : msg_wait_c_);
    }
    void set_msg_wait_gate(std::int32_t value) { msg_wait_gate_ = value; }
    // 自动消息（auto-message）：`0x1b7` 写的那个引擎格（`0x5EC70`）就是开关；`0x72` 在它非 0 时
    // 会按"时长公式"武装一个计时器，到期后**放行**等推进位（与点击同一对操作，不换行、不取下一行）。
    std::uint32_t auto_interval_ms() const { return auto_interval_ms_; }
    bool auto_armed() const { return auto_armed_; }
    // 文本追加次数（`0x6e` / `0x196` 成功追加一条就 +1）：宿主拿它当"这一轮真的推进了几句"的观测口。
    std::size_t text_appends() const { return text_appends_; }
    std::int32_t line_record_count(std::size_t slot) const {
        return slot < line_records_.size() ? line_records_[slot] : 0;
    }
    // 测试 / 诊断注入点：语音是否在播（`0x77A14`；本模型没有音频后端 ⇒ 产品里恒 0）。
    void set_voice_playing(bool value) { voice_playing_ = value; }
    std::int32_t voice_option_2f7(std::size_t i) const { return i < 3 ? voice_option_2f7_[i] : 0; }
    std::int32_t cursor_request_x() const { return cursor_request_x_; }
    std::int32_t cursor_request_y() const { return cursor_request_y_; }
    std::size_t cursor_requests() const { return cursor_requests_; }
    bool advance_waiting() const { return (state_flags_ & (1u << 31)) != 0; }
    void set_voice_option_2f7(std::size_t i, std::int32_t value) { if (i < 3) voice_option_2f7_[i] = value; }
    // `0x238` 写的两格就是**消息等待三格**的 B / C（见 `msg_wait`）；A 不动。
    std::int32_t cell_238_left() const { return msg_wait_b_; }
    std::int32_t cell_238_value() const { return msg_wait_c_; }
    // `0x215` 的观察口 / 测试钩子。
    bool scene_keyed_record(std::int32_t key, std::int32_t& flags, std::int32_t& value) const {
        const auto it = scene_records_.find(static_cast<std::uint32_t>(key));
        if (it == scene_records_.end()) return false;
        flags = it->second.flags;
        value = it->second.tag;
        return true;
    }
    std::int32_t scene_record_2d0(std::int32_t key) const {
        const auto it = scene_records_.find(static_cast<std::uint32_t>(key));
        return it == scene_records_.end() ? 0 : it->second.field_2d0;
    }
    bool scene_record_translate(std::int32_t key, float out[3]) const {
        const auto it = scene_records_.find(static_cast<std::uint32_t>(key));
        if (it == scene_records_.end()) return false;
        out[0] = it->second.matrix_translate[0];
        out[1] = it->second.matrix_translate[1];
        out[2] = it->second.matrix_translate[2];
        return true;
    }
    void set_scene_keyed_record(std::int32_t key, std::int32_t flags, std::int32_t value) {
        SceneRecord& record = scene_records_[static_cast<std::uint32_t>(key)];
        record.flags = flags;
        record.tag = value;
    }
    bool scene_keyed_pair(std::int32_t key, std::int32_t& a, std::int32_t& b) const {
        const auto it = scene_keyed_pair_.find(key);
        if (it == scene_keyed_pair_.end()) return false;
        a = it->second.field_a;
        b = it->second.field_b;
        return true;
    }
    void set_scene_keyed_pair(std::int32_t key, std::int32_t a, std::int32_t b) {
        scene_keyed_pair_[key] = KeyedPairRecord{a, b};
    }
    std::size_t container_224_items() const { return container_224_items_; }
    std::pair<std::int32_t, std::int32_t> run_header(std::size_t slot) const {
        return slot < run_header_.size() ? std::make_pair(run_header_[slot].a, run_header_[slot].b)
                                          : std::make_pair(0, 0);
    }
    void set_container_224_items(std::size_t value) { container_224_items_ = value; }
    bool scene_effect_present(std::size_t i) const { return i < 3 ? scene_effect_present_[i] : false; }
    void set_scene_effect_present(std::size_t i, bool value) {
        if (i < 3) scene_effect_present_[i] = value;
    }
    const std::map<std::uint32_t, SceneCommandRecord>& scene_commands() const { return scene_commands_; }
    std::map<std::uint32_t, SceneCommandRecord>& scene_commands_mut() { return scene_commands_; }
    void clear_scene_commands() { scene_commands_.clear(); }
    DrawItem* draw_item_mut(std::uint32_t key) {
        const auto it = draw_items_.find(key);
        return it == draw_items_.end() ? nullptr : &it->second;
    }
    MeshRecord* mesh_mut(std::uint32_t key) {
        const auto it = meshes_.find(key);
        return it == meshes_.end() ? nullptr : &it->second;
    }
    // 写：**整条**（记录 + 槽对象）。只有「建 / 换纹理 / 某条指令顺手建画布」该用它；
    // ⛔ 读完像素前先看 `slot.present` —— 记录在、槽空是合法状态。
    TextureEntry* texture_mut(std::uint32_t id) {
        const auto it = textures_.find(id);
        return it == textures_.end() ? nullptr : &it->second;
    }
    // 建 / 取条目（没有就建一条**空记录 + 空槽**）。⛔ 不要用它"顺手"造一个可见表面。
    TextureEntry& texture_entry(std::uint32_t id) { return textures_[id]; }
    // 只读两半（绝大多数读点只要一半）。
    const TextureRecord* texture_record(std::uint32_t id) const {
        const auto it = textures_.find(id);
        return it == textures_.end() ? nullptr : &it->second.record;
    }
    // 没有记录、或记录在但**槽对象已释放** ⇒ nullptr（`0x1fa` 之后就是这一档）。
    const TextureSlotObject* texture_slot(std::uint32_t id) const {
        const auto it = textures_.find(id);
        return it == textures_.end() || !it->second.slot.present ? nullptr : &it->second.slot;
    }
    // 纹理版本号的**全局下限**（每个 id 一条单调计数，跨 `0x1fa` 释放槽 / 重建也不回退）。
    // 窗口路径的 GPU 纹理缓存按 `(id, revision)` 命中：槽被释放再建、版本号若回到 1，缓存会把
    // **上一条记录**的纹理当成当前纹理。实测（2026-10-29，用户口径 + 本机窗口复现）：设置界面
    // （CONFIG1.BIN）↔ 初始配置界面（Game Start）来回一次，后进的那个界面整张美术图**不画**
    // （背景透出来）或画成**对方的内容** —— 两个界面用的是同一批纹理编号。
    // ★ 它自己就把"条目当前的 slot.revision"算进下限（不需要调用方先记一次），⛔ 也不要为了
    //   "省一次上传"把版本号变小。
    std::uint32_t next_texture_revision(std::uint32_t id);
    void report_error(std::string text) { errors_.push_back(std::move(text)); }
    const std::vector<std::string>& errors() const { return errors_; }
    void clear_errors() { errors_.clear(); }
    // ── 观察口（守卫用；⛔ 不是产品接口）──
    std::int32_t display_mode_cache() const { return display_mode_cache_; }
    // 音频观察口（守卫用；⛔ 不是产品接口）。
    std::int32_t se_slot_resource(std::size_t slot) const { return se_slot_resource_[slot]; }
    std::int32_t bgm_track() const { return bgm_track_; }
    std::int32_t bgm_param() const { return bgm_param_; }
    std::int32_t bgm_progress() const { return bgm_progress_; }
    std::size_t bgm_play_requests() const { return bgm_play_requests_; }
    std::size_t bgm_stop_requests() const { return bgm_stop_requests_; }
    // ── `0xc2` 淡变 / `0x2f5` 配音排队 / `0x2ff` 配音组（观察口，给自检用）──
    std::int32_t bgm_fade_target() const { return bgm_fade_target_; }
    std::int32_t bgm_fade_step() const { return bgm_fade_step_; }
    std::int32_t bgm_fade_progress() const { return bgm_fade_progress_; }
    std::int32_t bgm_fade_interval_ms() const { return bgm_fade_interval_ms_; }
    std::int32_t bgm_fade_mix_mode() const { return bgm_fade_mix_mode_; }
    std::size_t bgm_fade_requests() const { return bgm_fade_requests_; }
    // 门字（`state_flags_`）：**只给自检 / 夹具**摆前置位用（当前没有哪条已实现的指令能可靠置第 27 位，
    // 而 `0xc2` 的两条支路正是按那一位分的）。⛔ 不要在产品代码里用它绕开指令。
    std::uint32_t& state_flags_mut() { return state_flags_; }
    // ── **门外直派**：⛔ 只给自检 ──
    // 门（`state_flags_`）一旦非 0 就不取指，而本模型的主循环只有第 0 / 10 / 13 位的服务分支：
    // 第 27 位（"影片 / 立即"）没有服务者 ⇒ `0xc2` 的 A 支与 `0xc4` 的影片门支**用合成脚本永远走不到**
    // （只会得到 `gate-bit-unserviced` 诊断）。这两条支路按取证实现了，却因此长期没有行为级守卫。
    // ⇒ 给一个最小接缝，口径与 `emulator-new` 那边"测试直接取 `HANDLERS.get(op)` 调"一致。
    // ★ 它只做**一次派发**：不取指、不推进 ip、不服务门位。⛔ 生产路径只有 `step()`。
    void dispatch_for_test(const Instruction& instruction) { execute(instruction); }
    bool bgm_fade_active() const { return (state_flags_ & (1u << 9)) != 0; }
    std::int32_t voice_resource() const { return voice_resource_; }
    bool voice_pending() const { return voice_pending_; }
    std::int32_t voice_start_ms() const { return voice_start_ms_; }
    std::int32_t voice_delay(std::size_t i) const { return voice_delay_[i]; }
    std::int32_t voice_state(std::size_t i) const { return voice_state_[i]; }
    std::int32_t voice_group_enabled(std::size_t i) const { return voice_group_enabled_[i]; }
    std::int32_t voice_group_value(std::size_t i) const { return voice_group_value_[i]; }
    static constexpr std::size_t voice_slot_count() { return 3; }
    // ── `0xc4`（配音播放）/ `0x82`（画一段文本记录）的观察口 ──
    std::size_t voice_load_requests() const { return voice_load_requests_; }
    std::size_t voice_play_requests() const { return voice_play_requests_; }
    std::int32_t voice_mirror_resource(std::size_t i) const { return voice_mirror_resource_[i]; }
    std::int32_t voice_mirror_a4(std::size_t i) const { return voice_mirror_a4_[i]; }
    bool bgm_ducked() const { return bgm_ducked_; }
    std::int32_t voice_switch(std::size_t i) const { return voice_switch_46c_[i]; }
    std::int32_t voice_switch_478(std::size_t i) const { return voice_switch_478_[i]; }
    std::int32_t voice_busy() const { return voice_busy_; }
    std::size_t voice_stop_requests() const { return voice_stop_requests_; }
    // 测试钩子：`0x1bc` 的后端分支（挂上才发停止请求）与要清的格需要能预置。
    void set_voice_backend_attached(bool value) { voice_backend_attached_ = value; }
    void set_voice_cells(std::size_t i, std::int32_t voice46c, std::int32_t voice478,
                         std::int32_t mirror_resource, std::int32_t mirror_a4) {
        if (i >= 3) return;
        voice_switch_46c_[i] = voice46c;
        voice_switch_478_[i] = voice478;
        voice_mirror_resource_[i] = mirror_resource;
        voice_mirror_a4_[i] = mirror_a4;
    }
    void set_voice_busy(std::int32_t value) { voice_busy_ = value; }
    std::size_t text_span_count() const { return text_spans_.size(); }
    std::int32_t text_span_resource(std::size_t i) const { return text_spans_[i].resource; }
    std::int32_t text_span_voice_slot(std::size_t i) const { return text_spans_[i].voice_slot; }
    std::int32_t text_span_flags(std::size_t i) const { return text_spans_[i].flags; }
    const std::string& text_span_text(std::size_t i) const { return text_spans_[i].text; }
    std::int32_t text_span_slot(std::size_t i) const { return text_spans_[i].slot; }
    std::size_t text_span_draw_requests() const { return text_span_draw_requests_; }
    std::size_t text_span_alt_requests() const { return text_span_alt_requests_; }
    // ── 音效的两个**不同**槽量（⛔ 别再合成一个 15 —— 那曾让实现假造 5 个引擎格） ──
    //   · `kSoundSlotCount` = **15**：声音后端（`T_Sound`）的槽数组条数。
    //     语料：`0x4b603e cmp edi,0Eh / ja`（播放受理边界）、`0x4b65c5 cmp edi,0Fh / jl`（装载受理边界）、
    //     `0x4b639a cmp edi,0Fh / jl`（释放受理边界）；shape 里 `T_Sound` 的 +0x51c / +0x564 / +0x5a0 /
    //     +0x5dc / +0x618 / +0x690 / +0x7f8 七张表都是 count 15。
    //   · `kSeSlotTableSize` = **10**：引擎自己那张「槽 → 当前资源号」表（`T_SE+0x4BC`）。
    //     语料：构造器 `sub_4B4E60` 逐格清 10 个（`0x4b4eab`..`0x4b4ee1`：0x4BC / 0x4C0 / … / 0x4E0）；
    //     重装全表的循环 `0x4b50db cmp edi,0Ah / jge` 也是 0..9；对象边界印证同一件事 ——
    //     `Engine+0x143BC` 是 T_SE、下一块嵌入对象在 `+0x148A0`（`0x4156ea` / `0x4156f9`）⇒ 大小 0x4E4，
    //     而 0x4BC + 10*4 = 0x4E4 正好是表尾。
    //   ⇒ 装载成功路径 `0x4b5004 [T_SE+0x4BC+slot*4] ← 资源` 与停止路径 `0x4b506a` / `0x4b5075` 的清格
    //     **都没有边界检查**：槽号 10..14 落在 T_SE 表外（越界写，写进下一个嵌入对象），本版不复刻。
    // ★ 建模前提（不是取证结论）：后端存在 + 装载成功（两条都有残差登记）。
    static constexpr int kSeSlotTableSize = 10;
    static constexpr int kSoundSlotCount = 15;
    std::size_t se_play_requests() const { return se_play_requests_; }
    std::size_t se_stop_requests() const { return se_stop_requests_; }
    std::size_t se_load_requests() const { return se_load_requests_; }
    bool sound_se_enabled() const { return sound_se_enabled_; }
    bool sound_voice_enabled() const { return sound_voice_enabled_; }
    // ── 诊断（⛔ 不是引擎语义）：`--sweep-missing` 时把"没有 handler 的 opcode"" 记账后跳过 ──
    void set_sweep_missing(bool value) { sweep_missing_ = value; }
    bool sweep_missing() const { return sweep_missing_; }
    /** 按首次出现顺序记一次「这个 opcode 还没 handler」；已记过只加计数。 */
    void note_missing_opcode(std::uint32_t opcode);
    /** 首次出现顺序的 (opcode, 次数)。 */
    const std::vector<std::pair<std::uint32_t, std::size_t>>& missing_opcodes() const {
        return missing_opcodes_;
    }
    bool command_animating() const { return command_animating_; }
    void set_command_animating(bool value) { command_animating_ = value; }
    std::uint32_t render_now_ms() const { return render_now_ms_; }
    void begin_render() { render_now_ms_ = scene_clock_ms_; command_animating_ = false; }
    void end_render() {
        scene_dirty_ = false;  // 引擎在渲染步入口清脏（0x4B40AA）
        // ★ "待处理"闩（`+0B5B4`）与脏位是**同一条指令对**（0x4B40AA / 0x4B40B0），但它必须在
        //   "推进窗"之前清、由推进重新置起 —— 那一段在 `advance_scene` 里（它每步都跑，
        //   不依赖宿主是否呈现）。这里不再重复清：否则影片 / 不呈现的宿主路径语义会分叉。
        if (!command_animating_ && !scene_animating_) clear_scene_commands();
    }
    const DrawItem* draw_item(std::uint32_t key) const { const auto it = draw_items_.find(key); return it == draw_items_.end() ? nullptr : &it->second; }
    const SceneRecord* scene_record(std::uint32_t key) const { const auto it = scene_records_.find(key); return it == scene_records_.end() ? nullptr : &it->second; }
    const MovieRecord* movie(std::uint32_t slot) const { const auto it = movies_.find(slot); return it == movies_.end() ? nullptr : &it->second; }
    // 「有活动对象」标记：对象表里是否还有已启动的影片槽（`CORE-VM-MOVIE`）。
    bool movie_active() const { return movie_active_; }
    // 影片后端工厂（平台边界注入点）：缺省 = MPEG-1 program stream（pl_mpeg）。
    // 测试用它换一个确定性的假后端；⛔ 生产代码不要改。
    using MovieBackendFactory = std::function<std::unique_ptr<MovieBackend>()>;
    void set_movie_backend_factory(MovieBackendFactory factory) { movie_factory_ = std::move(factory); }
    // 影片字节的来源（统一文件 id → 字节）。缺省 = 安装目录 / ALF 切片；测试用它喂固定字节。
    using MovieBytesLoader = std::function<Bytes(std::uint32_t file_id)>;
    void set_movie_bytes_loader(MovieBytesLoader loader) { movie_bytes_loader_ = std::move(loader); }
    std::size_t aux_a_count() const { return aux_a_.size(); }
    std::size_t aux_b_count() const { return aux_b_.size(); }
    const MeshRecord* mesh(std::uint32_t key) const { const auto it = meshes_.find(key); return it == meshes_.end() ? nullptr : &it->second; }
    const TextureEntry* texture(std::uint32_t id) const {
        const auto it = textures_.find(id);
        return it == textures_.end() ? nullptr : &it->second;
    }
    std::int32_t forward_container_at(std::uint32_t index) const {
        const auto it = forward_container_.find(index);
        return it == forward_container_.end() ? 0 : it->second;
    }
    // 「文本键 → 整数」命名表（`CORE-OPS-TABLE-RESET-A1` / `-BIND-A2` / `-DISPATCH-A3`）。
    // ★ 与配置的命名键表**不是同一张**；键相等 = 完整文本相等。
    const std::unordered_map<std::string, std::int32_t>& named_table() const { return named_table_; }
    // 状态字（`CORE-OPS-STATUS-WORD`；`0x1cf` 写、`0x1bf` 读-清）—— ⛔ 不是门字 `state_flags_`。
    std::int32_t status_word() const { return status_word_; }
    std::int32_t status_neighbor() const { return status_neighbor_; }
    // `0x0d9` 的两格（身份在规范里未规定；本版按无读者格建模）。
    std::int32_t d9_cell() const { return d9_cell_; }
    std::int32_t d9_word() const { return d9_word_; }
    // 模式族的七个格（`CORE-OPS-MODE-BIT`；代号见规范，地址对应未取证）。
    std::int32_t mode_a() const { return mode_a_; }
    std::int32_t mode_b() const { return mode_b_; }
    std::int32_t mode_c() const { return mode_c_; }
    std::int32_t mode_d() const { return mode_d_; }
    std::int32_t mode_e() const { return mode_e_; }
    std::int32_t mode_f() const { return mode_f_; }
    std::int32_t mode_g() const { return mode_g_; }
    // 乱数启用族（`CORE-OPS-RNG-ENABLE`）。
    std::int32_t rng_switch() const { return rng_switch_; }
    std::int32_t rng_enabled() const { return rng_enabled_; }
    std::uint32_t rng_seed() const { return rng_seed_; }
    // 执行区间（`CORE-OPS-RUN-BRACKET`）：忙标记与 `0x20c` 的「别处驱动」是同一格。
    bool bracket_busy() const { return driven_elsewhere_; }
    std::int32_t bracket_depth() const { return bracket_depth_; }
    // 鼠标回调（`0xcc` / `0xcd` 的 `CORE-INPUT-MOUSE-CALLBACK-PUMP`）。
    std::int32_t mouse_interval() const { return mouse_interval_; }
    std::int32_t mouse_target() const { return mouse_target_; }
    std::int32_t mouse_owner() const { return mouse_owner_; }
    std::uint32_t mouse_last_ms() const { return mouse_last_ms_; }
    // 本地块（`0x7b` / `0x199` / `0x7c`）：存档字 / 存档位置 / 入块深度。
    std::uint32_t local_save_word() const { return local_save_word_; }
    std::uint32_t local_save_pos() const { return local_save_pos_; }
    std::int32_t local_save_depth() const { return local_save_depth_; }
    // 三个互不相同的格（⛔ 不要合并）：`0x19a` 的 X 格 / `0x1b6`+`0x1b7` 的 Y 格 / `0x1cc` 的状态格。
    std::int32_t flag_x() const { return flag_x_; }
    std::int32_t flag_y() const { return flag_y_; }
    std::int32_t status_cell() const { return status_cell_; }
    // 当前层号（本版没有观察到的写者 ⇒ 恒 0）与 `0x1ad` 写的那一格（复位值 −1）。
    std::int32_t current_layer() const { return current_layer_; }
    std::int32_t layer_cell() const { return layer_cell_; }
    // 参数格（`0x74` 写 / `0x7f` 读）：没写过 ⇒ nullopt（规范：初值没有观察）。
    const std::optional<std::int32_t>& param_cell() const { return param_cell_; }
    // 随机族（`0x60`）：计数器 0..12 / 上一次随机数 / 上一次范围（后两个无读者，规范明说）。
    std::int32_t random_counter() const { return random_counter_; }
    std::int32_t random_last() const { return random_last_; }
    std::int32_t random_range() const { return random_range_; }
    // 按住派发族（`0x102`..`0x105` / `0xfc`）：掩码 / 模数 / 上下文编号 / 按住游标（按帧槽号）。
    std::uint32_t hold_mask() const { return hold_mask_; }
    std::int32_t hold_modulus() const { return hold_modulus_; }
    std::int32_t hold_context_key() const { return hold_context_key_; }
    const std::optional<std::int32_t>& hold_cursor() const { return hold_cursor_by_slot_[current_frame_]; }
    const std::optional<std::int32_t>& hold_cursor(std::size_t slot) const {
        return hold_cursor_by_slot_[slot];
    }
    // ★ 测试脚手架（⛔ 不是平台边界，同 seed_mode_cells 的口径）：`0x103` 只清**当前帧槽**，
    //   纯 selftest 没有切帧路径 ⇒ 「只清当前帧 / 0xfc 不清游标」只能靠播种别的槽来判。
    void seed_hold_cursor(std::size_t slot, std::int32_t value) {
        hold_cursor_by_slot_[slot] = value;
    }
    // 存档族的宿主注入（核心只定义接口，平台实现）。
    void set_save_host(SaveHost* host) { save_host_ = host; }
    // 头部校验用的两个引擎串（标题 / 族标识；族标识是数据 ⇒ 本作 `S4`）。
    void set_save_identity(std::string title, std::string family) {
        save_title_ = std::move(title);
        save_family_ = std::move(family);
    }
    // `0x19e` 的写侧与 `0x1a1` 的 stage-2 都是**另几张单**的 seam；没注入就响亮失败。
    void set_save_codec(std::function<Bytes(const Machine&)> builder,
                        std::function<void(Machine&, const Bytes&)> loader) {
        slot_builder_ = std::move(builder);
        slot_loader_ = std::move(loader);
    }
    // 测试脚手架：全局整型池的「计数格」（`0x1a1` 的重编码范围 = 计数格 + 1 格）。
    void seed_global_int_count(std::uint32_t count) { global_int_count_ = count; }
    // 槽位 → 完整路径（宿主与测试共用同一套拼法；`thumbnail` 选 `.STH`）。
    std::string save_path_for(std::int32_t slot, bool thumbnail) const {
        return slot_path(save_directory(), slot, thumbnail);
    }
    // ★ 测试脚手架（⛔ 不是平台边界）：这几格由引擎里本版未建模的路径写，
    //   没有脚本可达的写者 ⇒ 想让对应分支被测到，只能由测试播种。
    void seed_mode_cells(std::int32_t mode_c, std::int32_t mode_e, std::int32_t mode_f) {
        mode_c_ = mode_c;
        mode_e_ = mode_e;
        mode_f_ = mode_f;
    }
    void seed_d9_cells(std::int32_t cell, std::int32_t word) {
        d9_cell_ = cell;
        d9_word_ = word;
    }
    bool halted() const { return halted_; }
    const RunResult& last_stop() const { return stop_; }
    // ★ halt 诊断（`[stack]` 行）：当前帧链**最内层在前**，每层给
    // `script=<id> bin=<名> ip=<body 内> file=<文件内偏移> opcode=<该位置的指令> calls=<返回栈深>`。
    // 目的：halt 只说 `opcode=0x192` 时能直接定位"哪个 BIN 的哪个偏移"，不用再猜。
    // ★ 只读；⛔ 不是引擎字段，也不是脚本可见量。
    std::string stop_stack_report() const;

    std::int32_t read_int(const Operand& operand);
    double read_float(const Operand& operand);
    void write_int(const Operand& operand, std::int32_t value);
    void write_float(const Operand& operand, double value);
    std::string read_text(const Operand& operand);
    void write_text(const Operand& operand, std::string value);

    std::uint32_t encode_int(std::int32_t value) const;
    std::int32_t decode_int(std::uint32_t bits) const;

private:
    Frame& frame_mut();
    void write_length(std::uint32_t words);
    // 门之前的影片对象表服务（`CORE-VM-MOVIE`）：轮询 / 收尾 / 循环 / 填纹理。
    void service_movies();
    // 把一帧写进同槽纹理（呈现层的像素效果，不在规范正文里）；写成功 ⇒ true。
    bool fill_movie_texture(std::uint32_t slot, const MovieFrame& frame);
    // 场景命令族（`0x223` / `0x24f` / `0x250` / `0x251`）的「槽里没有对象 ⇒ 建一张 1280×720 透明画布」。
    // ★ 判据是**槽对象**是否为空（不是"记录是否存在"）：`0x1fa` 之后记录还在、槽已释放 ⇒ 这里照建。
    void ensure_scene_texture_slot(std::uint32_t id);
    std::uint32_t address_of(const Operand& operand) const;
    // 「取址」原语（`0x63` / `0x1b0` 共用）：值族 ⇒ 槽地址；引用族 6/7/8/12/13/14 ⇒ **它指向的地址**。
    std::uint32_t address_of_operand(const Operand& operand) const;
    static bool is_pointer_tag(std::uint32_t tag);
    // 一次随机数（`0x60`）：注入源优先，否则确定性 LCG；返回**非负**值。
    std::uint32_t next_random();
    // 按编号取记录；**查不到就先建一条默认记录**（建记录那一步标脏，见 `CORE-OPS-SCENE3D-RECORD-LOOKUP`）。
    DrawItem& draw_item_or_create(std::uint32_t key);
    // `0x21e` 的缩放矩阵（百分数 ÷100 后进对角）与 `0x21f` 的旋转矩阵（度 → 弧度）。
    // ★ 旋转矩阵的元素布局 / 轴归一化方式规范**未成篇** ⇒ 这里是一个**实现选择**（行主序 + Rodrigues）。
    static std::array<float, 16> scale_matrix(double x, double y, double z);
    static std::array<float, 16> rotate_matrix_deg(double ax, double ay, double az, double degrees);
    bool resolve_address(std::uint32_t address, std::uint32_t& tag, std::uint32_t& index) const;
    std::uint32_t read_pointer(const Operand& operand) const;
    void write_pointer(const Operand& operand, std::uint32_t address);
    void store_raw(std::uint32_t address, std::uint32_t bits);
    std::uint32_t read_raw(std::uint32_t address);
    std::int32_t read_int_at(std::uint32_t address);
    double read_float_at(std::uint32_t address);
    void write_int_at(std::uint32_t address, std::int32_t value);
    void write_float_at(std::uint32_t address, double value);
    std::uint32_t pool_base(std::uint32_t tag) const;
    // 带 bank 的池基点：六个全局族的 bank 1 整体上移 0x18000000（一组 = 6 × 0x04000000）。
    std::uint32_t pool_base(std::uint32_t tag, std::uint32_t bank) const;
    // 全局槽的**存储键**：bank 1 的槽统一加 kBankKeyStride（两张 bank 共用同一张表）。
    std::uint32_t bank_key(std::uint32_t index) const;
    static constexpr std::uint32_t kBankKeyStride = 0x40000000u;
    // `0x09` 只清**当前选中 bank** 的三个全局池（另一组保留）。
    void clear_global_pool_bank();
    std::uint32_t script_int_key_index(const Operand& operand);
    // 选项窗口（`0x91`…`0x94`）的内部原语。
    void reset_choice_table();
    std::int32_t choice_hit_test() const;
    void jump_to(std::uint32_t position);
    void push_return(std::uint32_t position);

    // ── 指令实现：一个 opcode 一个函数；分派仍在 vm.cpp 的 execute()，实现按规范章节分文件 ──
    // 控制流 / 帧（vm_ops_flow.cpp）
    void op_01(const Instruction&); void op_02(const Instruction&); void op_03(const Instruction&); void op_06(const Instruction&);
    void op_143(const Instruction&); void op_05(const Instruction&); void op_8c(const Instruction&); void op_8f(const Instruction&);
    void op_a0(const Instruction&);
    // 全局重置 / 执行区间（vm_ops_flow.cpp）
    void op_09(const Instruction&); void op_1f4(const Instruction&); void op_1f5(const Instruction&);
    // 帧切换 / 本地块（vm_ops_flow.cpp）
    void op_08(const Instruction&); void op_7b(const Instruction&); void op_7c(const Instruction&);
    void op_199(const Instruction&);
    // 渲染入口（vm_ops_render.cpp）
    void op_23b(const Instruction&); void op_2da(const Instruction&); void op_20b(const Instruction&); void op_20c(const Instruction&);
    void op_20d(const Instruction&); void op_33f(const Instruction&); void op_204(const Instruction&);
    // 数值文本（0x205）/ 清屏（0x20e）
    void op_205(const Instruction&); void op_20e(const Instruction&);
    void op_23c(const Instruction&);
    // 纹理 / 绘制项 / 网格（vm_ops_texture.cpp）
    void op_344(const Instruction&); void op_1ff(const Instruction&); void op_1fd(const Instruction&); void op_1fe(const Instruction&);
    void op_1f6(const Instruction&); void op_216(const Instruction&); void op_320(const Instruction&); void op_322(const Instruction&); void op_323(const Instruction&); void op_23d(const Instruction&); void op_32b(const Instruction&);
    void op_1fb(const Instruction&); void op_239(const Instruction&); void op_202(const Instruction&); void op_203(const Instruction&);
    void op_1f7(const Instruction&); void op_1f8(const Instruction&); void op_1fa(const Instruction&); void op_1f9(const Instruction&);
    // 纹理尺寸查询（vm_ops_texture.cpp）
    void op_208(const Instruction&);
    // 3D 记录取回（vm_ops_texture.cpp）：两个三元向量 + 平移分量
    void op_218(const Instruction&); void op_219(const Instruction&); void op_21a(const Instruction&); void op_228(const Instruction&);
    void op_258(const Instruction&); void op_259(const Instruction&);
    void op_90(const Instruction&);
    void op_97(const Instruction&);
    void op_91(const Instruction&); void op_92(const Instruction&);
    void op_93(const Instruction&); void op_94(const Instruction&);
    // 写第一三元向量（vm_ops_texture.cpp）
    void op_217(const Instruction&);
    // 记录复制（vm_ops_texture.cpp）
    void op_21d(const Instruction&);
    // 3D setter 族（vm_ops_texture.cpp）
    void op_21e(const Instruction&); void op_21f(const Instruction&);
    // 影片（vm_movie.cpp）
    void op_20f(const Instruction&);
    // 场景命令记录（vm_ops_scene.cpp）
    void op_223(const Instruction&); void op_24d(const Instruction&); void op_24f(const Instruction&); void op_250(const Instruction&);
    void op_229(const Instruction&);
    void op_251(const Instruction&);
    // 引擎级状态与宿主请求（vm_ops_engine.cpp）
    void op_88(const Instruction&); void op_308(const Instruction&); void op_bf(const Instruction&); void op_b4(const Instruction&);
    // 把引擎标量写进第 1 个操作数（vm_ops_engine.cpp）
    void op_c0(const Instruction&);
    // 引擎标量写入 / 文本显示门（vm_ops_engine.cpp）
    void op_142(const Instruction&); void op_1bb(const Instruction&);
    // 配置 getter / setter · 画面模式 getter（vm_ops_engine.cpp）
    void op_2cc(const Instruction&); void op_2cd(const Instruction&); void op_2ce(const Instruction&);
    void op_2cf(const Instruction&); void op_307(const Instruction&);
    // 配置命名键 / 声音模式（`55-config` 与 `60-ops-voice`）：本批按取证补的 4 条。
    void op_1b5(const Instruction&); void op_1b8(const Instruction&);
    void op_1b9(const Instruction&); void op_1ba(const Instruction&);
    // 音频请求族（0xb6 停止一个音效槽 / 0xb7 BGM 播放 / 0xb8 BGM 停止 / 0xba 循环变体）。
    void op_b6(const Instruction&); void op_b7(const Instruction&); void op_b8(const Instruction&);
    void op_ba(const Instruction&);
    // `60-ops-voice`：BGM 淡变 / 配音排队 / 配音组
    void op_c2(const Instruction&); void op_2f5(const Instruction&); void op_2ff(const Instruction&);
    // 文本记录（`0x82` 消费者 / `0xc4` 生产者）
    void op_82(const Instruction&); void op_c4(const Instruction&);
    /** 显示模式请求的实现（`0x2cf` 转发到的入口；`0x2ce` 读的是同一个缓存格）。 */
    void apply_display_mode(std::int32_t target);
    void op_2e6(const Instruction&); void op_2e7(const Instruction&); void op_2e8(const Instruction&);
    void op_2ea(const Instruction&);
    // 设置表 getter：声道音量（0xC5）/ 点击跳过效果（0x306）
    void op_c5(const Instruction&); void op_306(const Instruction&);
    // 声道音量 setter（0xC6）/ 声道开关 getter（0xC7）
    void op_c6(const Instruction&); void op_c7(const Instruction&);
    void op_b5(const Instruction&); void op_2c7(const Instruction&); void op_341(const Instruction&); void op_ae(const Instruction&);
    void op_2ee(const Instruction&); void op_10f(const Instruction&); void op_248(const Instruction&); void op_8b(const Instruction&);
    void op_261(const Instruction&); void op_260(const Instruction&); void op_25b(const Instruction&); void op_130(const Instruction&);
    void op_303(const Instruction&); void op_212(const Instruction&); void op_213(const Instruction&); void op_25d(const Instruction&);
    void op_1c1(const Instruction&); void op_79(const Instruction&); void op_73(const Instruction&); void op_80(const Instruction&);
    void op_70(const Instruction&); void op_1ca(const Instruction&); void op_149(const Instruction&); void op_21b(const Instruction&);
    void op_252(const Instruction&); void op_2f8(const Instruction&); void op_2f6(const Instruction&);
    // 命名表 / 状态字 / 模式族 / 清位（vm_ops_engine.cpp）
    void op_a1(const Instruction&); void op_a2(const Instruction&); void op_a3(const Instruction&);
    void op_d9(const Instruction&); void op_19b(const Instruction&); void op_19c(const Instruction&);
    void op_1bf(const Instruction&); void op_1cf(const Instruction&);
    void op_1bc(const Instruction&);
    void op_32d(const Instruction&); void op_32a(const Instruction&);
    void op_1b1(const Instruction&);
    void op_238(const Instruction&);
    void op_215(const Instruction&);
    void op_242(const Instruction&);
    void op_220(const Instruction&);
    void op_224(const Instruction&);
    void op_7a(const Instruction&);
    void op_1cb(const Instruction&);
    void op_256(const Instruction&);
    void op_193(const Instruction&); void op_20a(const Instruction&); void op_243(const Instruction&);
    void op_32(const Instruction&); void op_72(const Instruction&); void op_10a(const Instruction&);
    void op_2e9(const Instruction&);
    void op_214(const Instruction&);
    // `0x72` 的放行（帧服务）：等推进位置起时，输入一到就清位。
    void service_advance_wait();
    void service_auto_message();
    // `0x21c` 谓词 A：消息子系统"还有事"（定时器在窗内 **或** 待处理）。谓词 B（影片对象扫描）另算。
    bool message_subsystem_busy();
    // 已排窗但还没盖章（`window_start == 0`）且**真会盖章**（时长 > 0）的条目 ⇒ 也算忙。
    bool scene_window_unqueued() const;
    bool movie_gate_busy() const;
    void service_choice_click();
    void op_2f7(const Instruction&); void op_304(const Instruction&); void op_305(const Instruction&);
    // 门之前的 BGM 淡变推进（第 9 位的服务分支；规范未规定 ⇒ 见实现里的残差说明）。
    void service_bgm_fade();
    // 标志格 / 状态格 / 模式位读 / 层号（vm_ops_engine.cpp）
    void op_19a(const Instruction&); void op_1b6(const Instruction&); void op_1b7(const Instruction&);
    void op_1ad(const Instruction&); void op_1c7(const Instruction&); void op_1cc(const Instruction&);
    // 数值 / 数组（vm_ops_numeric.cpp）
    void op_6c(const Instruction&); void op_2d8(const Instruction&); void op_61(const Instruction&); void op_12c(const Instruction&);
    void op_64(const Instruction&); void op_10c(const Instruction&); void op_fe(const Instruction&); void op_107(const Instruction&);
    void op_10b(const Instruction&); void op_30a(const Instruction&); void op_32f(const Instruction&); void op_324(const Instruction&);
    void op_50(const Instruction&); void op_51(const Instruction&); void op_52(const Instruction&); void op_53(const Instruction&);
    void op_54(const Instruction&); void op_2d5(const Instruction&); void op_56(const Instruction&); void op_57(const Instruction&);
    void op_58(const Instruction&); void op_59(const Instruction&); void op_5a(const Instruction&); void op_5b(const Instruction&);
    void op_5c(const Instruction&); void op_5d(const Instruction&); void op_5e(const Instruction&); void op_5f(const Instruction&);
    void op_135(const Instruction&); void op_136(const Instruction&); void op_13f(const Instruction&); void op_55(const Instruction&);
    // 乱数启用（vm_ops_numeric.cpp）
    void op_1ce(const Instruction&);
    // 池拷贝（vm_ops_numeric.cpp）
    void op_1b0(const Instruction&);
    // 池翻页（vm_ops_numeric.cpp）
    void op_6d(const Instruction&);
    // 浮点族 / 时钟 / 参数 / 随机 / lea（vm_ops_numeric.cpp）
    void op_191(const Instruction&); void op_2d0(const Instruction&); void op_2d1(const Instruction&);
    void op_2d3(const Instruction&); void op_d0(const Instruction&); void op_74(const Instruction&);
    // 排序置换（vm_ops_numeric.cpp）：三个数组基址 + 元素个数 → A 里写下标的升序置换
    void op_12f(const Instruction&);
    void op_7f(const Instruction&); void op_60(const Instruction&); void op_63(const Instruction&);
    // 字体与文本（vm_ops_text.cpp）
    void op_2ec(const Instruction&); void op_2eb(const Instruction&); void op_1a4(const Instruction&); void op_76(const Instruction&);
    void op_2bd(const Instruction&); void op_2de(const Instruction&); void op_192(const Instruction&); void op_1a9(const Instruction&);
    void op_194(const Instruction&); void op_195(const Instruction&);
    void op_1aa(const Instruction&); void op_1a3(const Instruction&); void op_1a2(const Instruction&); void op_2db(const Instruction&);
    void op_78(const Instruction&); void op_71(const Instruction&); void op_1a5(const Instruction&); void op_197(const Instruction&); void op_198(const Instruction&);
    void op_75(const Instruction&);
    // 文本长度 / 字体槽表 / Font 条目登记表（vm_ops_text.cpp）
    void op_1a6(const Instruction&); void op_300(const Instruction&); void op_301(const Instruction&);
    void op_1d2(const Instruction&); void op_1d3(const Instruction&);
    // 消息窗口的文本追加 / 行结束（vm_ops_text.cpp）
    void op_6e(const Instruction&); void op_6f(const Instruction&); void op_196(const Instruction&);
    // 文本层画布（0x70 / 0x71 / 0x79 / 0x213 的宿主侧）：纹理 = 20 + 层号；rect / 原点 / 键基数在 ListRecord。
    int resolve_text_layer(int slot) const;
    bool ensure_text_layer_texture(int slot);
    void clear_text_layer(int slot);
    void draw_text_layer_text(int slot, const std::string& text, const std::string& rubi);
    // 排版层追加一条 72 字节文本记录（`sub_45F090` 的最小投影；文本 / 位置 / 标志）。
    void append_text_span_record(int slot, const std::string& text, int x, int y, int x1, int y1,
                                 std::int32_t flags);
    void end_text_layer_line(int slot);
    void create_text_layer_item(int slot, int left, int top, int right, int bottom);
    void erase_text_layer_draw_items(int slot);
    // 逐字符显现（typewriter）：按每字间隔**每 D 毫秒画一格、每帧最多一格**（`0x409400` 的计时器闸门）；
    // 追加成功时由 `begin_text_typing_or_drain()` 两档决定"置第 29 位 + 起计时器"还是"同帧抽干"。
    void service_text_layers();
    void begin_text_typing_or_drain();
    void draw_front_pending_glyph();
    void drain_pending_glyphs();
    void erase_pending_glyphs(int slot);
    // `0x300` 循环演示闸（规则 `CORE-OPS-TEXT-SLOT-TIMER`）：揭示下一格 / 揭示全部（关闸时抽干）。
    void reveal_text_layer_cell(int slot);
    void reveal_text_layer_to(std::size_t slot, std::size_t revealed);
    void reveal_all_text_layer_cells(int slot);
    void service_text_gate(int slot);
    // 输入 / 等待 / 空操作（vm_ops_io.cpp）
    void op_109(const Instruction&); void op_108(const Instruction&); void op_c8(const Instruction&); void op_cc(const Instruction&);
    // 滚轮（读并清 `Engine+0x1E74`）
    void op_10d(const Instruction&);    void op_cd(const Instruction&); void op_fb(const Instruction&); void op_24e(const Instruction&); void op_21c(const Instruction&);
    void op_101(const Instruction&); void op_1a7(const Instruction&); void op_1a8(const Instruction&);
    // 输入采样 / 按键派发 / 命中查找 / 触点查询（vm_ops_io.cpp）
    void op_ff(const Instruction&); void op_100(const Instruction&); void op_12e(const Instruction&); void op_2fc(const Instruction&);
    // 按住派发族（vm_ops_io.cpp）
    void op_fc(const Instruction&); void op_102(const Instruction&); void op_103(const Instruction&);
    void op_104(const Instruction&); void op_105(const Instruction&);
    void advance_hold_cursor();  // `0x104` 的游标推进（三条出口共用）
    // 存档族（vm_ops_save.cpp）
    void op_19e(const Instruction&); void op_1a0(const Instruction&); void op_1a1(const Instruction&);
    void op_1ab(const Instruction&); void op_1ac(const Instruction&); void op_1ae(const Instruction&);
    void op_1af(const Instruction&);
    std::string save_directory() const;
    static std::string slot_path(const std::string& dir, std::int32_t slot, bool thumbnail);
    bool parse_save_header(ByteSpan bytes, SaveHeader& header) const;
    void require_save_host(const char* opcode) const;
    void recode_global_int_pool();
    void note_global_int_write(std::uint32_t index);

    void execute(const Instruction& instruction);

    std::uint32_t key_;
    const OpcodeTable& table_;
    const Install* install_ = nullptr;
    ConfigStore config_;
    std::array<Frame, kFrameCount> frames_{};
    std::size_t current_frame_ = 0;
    std::uint32_t state_flags_ = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> global_int_;
    std::unordered_map<std::uint32_t, float> global_float_;
    std::unordered_map<std::uint32_t, std::string> global_string_;
    // 六个全局族的 bank 选择（`0x6d` 整体对调；0 = 主组、1 = 备组）。存储键由 `bank_key()` 平移。
    std::uint32_t global_flip_ = 0;
    // 启动期配置 `set:EnableMemFlip` 是否为非 0（决定备组是否"已登记"）。
    bool memflip_available_ = false;
    std::unordered_map<std::uint32_t, std::uint32_t> ref_slots_;
    std::array<std::array<std::int32_t, 4>, 3> voice_engine_cells_{};
    std::array<std::int32_t, 3> voice_volumes_{};
    std::int32_t scalar_149_ = 0;
    std::int32_t scalar_21b_ = 0;
    std::int32_t scalar_252_ = 0;
    std::int32_t scalar_88a_ = 0;
    std::int32_t scalar_88b_ = 0;
    std::int32_t scalar_88c_ = 0;
    // （`a1`/`a2`/`a3`）「文本键 → 整数」命名表；`a1` 清空、`a2` 写、`a3` 按键分派。
    std::unordered_map<std::string, std::int32_t> named_table_;
    // （`1cf` / `1bf`）状态字与它的相邻格 —— ⛔ 与门字 `state_flags_` 是**不同**的引擎格。
    std::int32_t status_word_ = 0;
    std::int32_t status_neighbor_ = 0;
    // （`0d9`）清位判据的两格；身份在规范里未规定。
    std::int32_t d9_cell_ = 0;
    std::int32_t d9_word_ = 0;
    // （`19b` / `19c`）模式族的七个格。
    std::int32_t mode_a_ = 0;
    std::int32_t mode_b_ = 0;
    std::int32_t mode_c_ = 0;
    std::int32_t mode_d_ = 0;
    std::int32_t mode_e_ = 0;
    std::int32_t mode_f_ = 0;
    std::int32_t mode_g_ = 0;
    // （`1ce`）乱数启用族：开关值 / 已启用 / 种子（种子 = 本迭代宿主毫秒时钟）。
    std::int32_t rng_switch_ = 0;
    std::int32_t rng_enabled_ = 0;
    std::uint32_t rng_seed_ = 0;
    // 本地块的存档（`0x199` 整字搬 + 置第 25/26 位；`0x7c` 读/清）。
    std::uint32_t local_save_word_ = 0;
    std::uint32_t local_save_pos_ = 0;
    std::int32_t local_save_depth_ = 0;
    // 本地块的四格与"待处理"标记：领域身份未取证、也没有脚本可达的写者 ⇒ 只有"清 0"这条合同。
    std::int32_t local_cell_a_ = 0;
    std::int32_t local_cell_b_ = 0;
    std::int32_t local_cell_c_ = 0;
    std::int32_t local_cell_d_ = 0;
    std::int32_t local_pending_ = 0;
    // 三个互不相同的格（`0x19a` / `0x1b6`+`0x1b7` / `0x1cc`）。
    std::int32_t flag_x_ = 0;
    std::int32_t flag_y_ = 0;
    std::int32_t status_cell_ = 0;
    // 按住派发族：掩码 / 模数 / 上下文编号（引擎级）+ 按住游标（引擎级数组、按**帧槽号**索引）。
    // ★ 前三个是**合同初值**（启动路径写 0 / 0 / −1）；游标 nullopt = 引擎不建立初值（读到是 UB）。
    std::uint32_t hold_mask_ = 0;
    std::int32_t hold_modulus_ = 0;
    std::int32_t hold_context_key_ = -1;
    std::array<std::optional<std::int32_t>, kFrameCount> hold_cursor_by_slot_{};
    // 层号：`current_layer_` 无观察到的写者（本版恒 0）；`layer_cell_` 的复位值是 **−1**。
    std::int32_t current_layer_ = 0;
    std::int32_t layer_cell_ = -1;
    // 参数格（`0x74` / `0x7f`）：没写过 ⇒ nullopt（初值没有观察）。
    std::optional<std::int32_t> param_cell_;
    // 随机族（`0x60`）：计数器 / 上一次随机数 / 上一次范围 / 确定性 LCG 状态 / 注入源。
    std::int32_t random_counter_ = 0;
    std::int32_t random_last_ = 0;
    std::int32_t random_range_ = 0;
    std::uint32_t random_state_ = 0x12345678u;
    RandomSource random_source_;
    // ── 存档族（`0x19e`..`0x1af`） ──
    SaveHost* save_host_ = nullptr;
    std::function<Bytes(const Machine&)> slot_builder_;
    std::function<void(Machine&, const Bytes&)> slot_loader_;
    std::string save_title_;            // 头 +8 的标题串（来源未取证 ⇒ 空 = 不校验）
    std::string save_family_ = "S4";    // 族标识 ⇒ 魔数 "S4SD"
    // 全局整型池的「计数格」：`0x1a1` 的重编码范围 = 它 + 1 格（写入时维护成 max(index + 1)）。
    std::uint32_t global_int_count_ = 0;
    std::uint32_t save_file_version1_ = 0;  // 版本门通过后回填（`FMT-SAVE-ENVELOPE`，本批不建模）
    std::uint32_t save_file_version2_ = 0;
    std::unordered_map<std::uint32_t, std::int32_t> forward_container_;
    std::array<ListRecord, 10> list_records_{};
    std::int32_t default_group_ = 1;
    std::int32_t scalar_d107656_ = 0;
    std::int32_t font_draw_mode_ = 1;
    // 字形高度（`Font+0x355D8`）缺省 **8**、行高（`Font+0x313D4`）缺省 **24** —— 这是引擎的两个不同缺省，
    // 不是同一个量的两个名字（实测：`0x75` 只写行高那一组，`0x197` 只写字号那一组）。
    // 字体对象有**两套字号**（不是"行高 / 字形高度"——那是本模型以前的误读）：
    //   · A 支：`Font+0x313D4`（缺省 0x18=24），由 `0x75` 写；**0x204 把文字画进纹理时用的就是它**
    //     （`0x459F63 mov eax,[ebx+313D4h]` → `0x459F6B neg eax` → 逐字字体的 lfHeight；HFONT 在 `Font+0x43C`）。
    //   · B 支：`Font+0x355D8`（缺省 8），由 `0x197` 写（`0x418687`）；只有 `0x82` 跨度那条路径在
    //     `sub_455ED0` 的第 9 实参 ≠ 0 时才选它（`0x45607C cmp [ebp+arg_1C],0` / `0x456082 mov eax,[esi+448h]`），
    //     HFONT 在 `Font+0x448`（`0x45A792`）。
    //   ⇒ 把 `0x75` 当成"行高"会让界面里"设行高"顺手改字号（历史上正是这个错）。
    std::int32_t glyph_height_ = 24;      // A 支（0x75）：0x204 出字形的那个高度
    std::int32_t font_b_height_ = 8;      // B 支（0x197）：跨度绘制那条路径的字号
    struct TextSlot {
        std::int32_t glyph_height = 0;  // `槽+0xC8`：`0x197` 把**十槽全写**成同一个值
        std::int32_t x_offset = 0;      // `槽+0x0C`：`0x198` 的 op2
        std::int32_t y_offset = 0;      // `槽+0x10`：`0x198` 的 op3
    };
    std::array<TextSlot, 10> text_slots_{};
    std::int32_t font_align_mode_ = 0;
    std::int32_t font_primary_color_ = 0xFFFFFF;
    std::int32_t font_secondary_color_ = 0x000000;
    std::int32_t font_metric_a_ = 0;
    std::int32_t font_metric_b_ = 0;
    std::int32_t font_pair_a_ = 0;
    std::int32_t font_pair_b_ = 0;
    std::array<std::int32_t, 32> array_d551_{};
    std::array<std::int32_t, 32> array_d1383_{};
    std::array<std::int32_t, 8> array_d1969_{};
    std::int32_t scalar_25b_a_ = 0;
    std::int32_t scalar_25b_b_ = 0;
    std::int32_t show_logo_ = 1;
    std::int32_t scalar_2ee_ = 0;
    // `0xC0` 读的引擎标量（复位为 0；写者是 `0xC3`，本版未实现）。领域名未取证。
    std::int32_t engine_aa9e4_ = 0;
    // `0x142` 写的引擎标量（初值 1；读者 `sub_4765C0` = 判非 0）。领域名未取证。
    std::int32_t engine_aab70_ = 1;
    // `0x1BB` 写的文本门：0 = 正常显示，bit31 = 抑制（`0x1D2` 的守卫读它）。
    std::int32_t font_text_gate_5ec7c_ = 0;
    // 画面模式缓存（`0x2CE` 读 / `0x2CF` apply 时写回配置；初值 = 配置 `display:ScreenMode`）。
    std::int32_t display_mode_cache_ = 0;
    // ── 音频（`60-ops-voice`）：只建模**引擎侧可见的格**与**向声音后端发出的请求计数**。 ──
    // ★ 音效的两个槽量见 `public:` 的 `kSeSlotTableSize` / `kSoundSlotCount`（那里有取证）。
    // 引擎的可观察状态（取证：0xb4/0xb6/0xb7/0xb8/0xba 的 handler）：
    //   · 每个音效槽"当前资源号"（0xb4 成功写、0xb6 清 0）；
    //   · BGM 的 曲目 / 参数 / 淡变进度 三格（0xb7 写前两格、0xb8 清前两格）；
    //   · "音乐淡变进行中"就是门字第 9 位：**`0xc2` 置它**（`0x420E55 or …0x200`），
    //     `0xb7` / `0xb8` / `0xbf` / 主循环都是**清**它（本文件以前把这条注释放反过）。
    // ⛔ 声音本身不建模 —— 请求只计数（守卫靠它区分"发了/没发"）。
    // ★ 表量由 `T_SE+0x4BC` 定（10），⛔ 不是后端的 15：见上面两条常量的取证。
    std::array<std::int32_t, kSeSlotTableSize> se_slot_resource_{};
    std::int32_t bgm_track_ = 0;
    std::int32_t bgm_param_ = 0;
    std::int32_t bgm_progress_ = 0;
    std::size_t bgm_play_requests_ = 0;
    std::size_t bgm_stop_requests_ = 0;
    std::size_t se_play_requests_ = 0;
    std::size_t se_stop_requests_ = 0;
    std::size_t se_load_requests_ = 0;
    // `0xc2`（BGM 淡变）：目标值 / 步长 / 进度 / 间隔（ms）/ 混音方式，外加一笔"发了淡变请求"。
    // ★ `op1` **不是曲目号**（曲目另有格）；刻度满值 = 10000（唯一观察），本条不解释它的单位。
    std::int32_t bgm_fade_target_ = 0;
    std::int32_t bgm_fade_step_ = 0;
    std::int32_t bgm_fade_progress_ = 0;
    std::int32_t bgm_fade_interval_ms_ = 0;
    std::int32_t bgm_fade_mix_mode_ = 0;
    // 淡变推进的记账：上一次推进的时刻（毫秒）。`0` = 还没起算。
    std::uint32_t bgm_fade_last_ms_ = 0;
    std::size_t bgm_fade_requests_ = 0;
    // `0x2f5`（配音排队）写的那五格：延时 ms / 资源号 / pending / 起算 / 状态字（每槽一格）。
    // ★ 真正"响"是帧更新里的计时（本版不建模时钟 ⇒ 只落状态与请求）。
    std::int32_t voice_resource_ = 0;
    bool voice_pending_ = false;
    std::int32_t voice_start_ms_ = 0;
    std::array<std::int32_t, 3> voice_delay_{};
    std::array<std::int32_t, 3> voice_state_{};
    // `0x2ff`（配音组）：三个槽各一对「组开关位」与「组值」。★ 组值的领域含义未取证
    //（只证到播放时会抄进声音后端的一列里，而那一列在本语料里没有读者）。
    std::array<std::int32_t, 3> voice_group_enabled_{};
    std::array<std::int32_t, 3> voice_group_value_{};
    // `0xc4` 消费的**另一个**槽 0 开关格（`T_Voice+0x46C`）。本模型里没有写者 ⇒ 保持缺省 0。
    // ★ 两个开关格的消费写法与排队路径**不同**：这里是"位16 ⇒ 整格 0；否则位0 ⇒ 置位16（**保留位0**）"，
    //   而排队播放那一路是 `and 0x10000`（**清位0**）。照抄，不要统一。
    std::array<std::int32_t, 3> voice_switch_46c_{};
    // `0x1bc` 清的**第二组**每槽选项位（`T_Voice+0x478`）；`0x2f6` 也清它。本模型里没有写者。
    std::array<std::int32_t, 3> voice_switch_478_{};
    // `Engine+0x77A14`：**voice 正在播**标志（`0x1bc` 清 0；谓词在别处）。
    std::int32_t voice_busy_ = 0;
    // voice 后端的挂接状态（平台边界）：没挂上时 `0x1bc` 不发停止请求（引擎同一支）。
    bool voice_backend_attached_ = false;
    std::size_t voice_stop_requests_ = 0;
    // `0xc4` 写的两个"镜像"格（`Engine+0x77A24` / `+0x77A30`，每槽一份）。
    std::array<std::int32_t, 3> voice_mirror_resource_{};
    std::array<std::int32_t, 3> voice_mirror_a4_{};
    std::size_t voice_load_requests_ = 0;
    std::size_t voice_play_requests_ = 0;
    bool bgm_ducked_ = false;  // `0xAAB44` 第 14 位的投影（BGM 压低，`sub_407120` 那一步）
    // `0x82` 消费的那条**文本记录向量**（`T_Font+0xD24`，72 字节/条）的**最小可观察投影**：
    // 只建已取证的 6 个字段（+0x00 / +0x14 / +0x18 / +0x1C / +0x20 / +0x28），其余字段不建（残差）。
    struct TextSpanRecord {
        std::int32_t slot = 0;        // +0x00 当前文本槽（排版记录里是层号）
        std::int32_t x = 0;           // +0x04 绘制起点 x（排版记录）
        std::int32_t y = 0;           // +0x08 绘制起点 y（排版记录）
        std::int32_t x1 = 0;          // +0x0C 段末 x（排版记录）
        std::int32_t y1 = 0;          // +0x10 段末 y（排版记录）
        std::int32_t resource = 0;    // +0x14 语音资源
        std::int32_t a18 = 0;         // +0x18
        std::int32_t a1c = 0;         // +0x1C
        std::int32_t voice_slot = 0;  // +0x20
        std::int32_t flags = 0;       // +0x28（bit0 = 新段；bit3 = 行结束空串记录；bit30 = 可触发语音）
        std::string text;             // +0x2C 起的 28 字节字符串对象（本模型只存内容）
    };
    std::vector<TextSpanRecord> text_spans_;
    std::size_t text_span_draw_requests_ = 0;  // 追加绘制命令（度量与命令本身未建模）
    std::size_t text_span_alt_requests_ = 0;   // `node[0x70]==1` 时转交的另一条绘制路径
    // 文本节点槽的"种类"格（`T_Font+0x414` 十项数组各自的 +0x70）。本模型里没有写者 ⇒ 缺省 0（= 走主路径）。
    std::unordered_map<std::int32_t, std::int32_t> font_node_kind_;
    // `0x1ba` 的两个引擎开关格（构造期默认 **1 = 开**，取证 `0x491932` / `0x49194f`）。
    bool sound_se_enabled_ = true;
    bool sound_voice_enabled_ = true;
    // 字体槽表的三个各 10 条的 dword 数组（`0x300` 写 A/B、`0x301` 清 C）。
    // ★ 引擎**不做范围检查** ⇒ 这里用 map 表达"任何下标都落得下"，⛔ 不用定长数组冒充。
    std::unordered_map<std::int32_t, std::int32_t> font_slot_state_;   // A（状态位 + 粘住位）
    std::unordered_map<std::int32_t, std::int32_t> font_slot_deadline_; // B（期限 ms）
    std::unordered_map<std::int32_t, std::int32_t> font_slot_start_;   // C（起点时刻）
    // Font 的**条目登记表**（`0x1D2` 入队 / `0x1D3` 按 key 查；`0x1D0` 走对表，本版未实现）。
    // ★ 这是引擎状态（Font 侧），不是渲染缓冲：消费方未建模（残差）。
    std::vector<FontEntry> font_entries_;
    std::vector<std::uint32_t> font_entry_dirty_;  // 每记录一个"脏槽"（`0x1D2` 读并清）
    std::vector<std::pair<std::int32_t, std::int32_t>> font_pairs_;  // 8 字节对表（无写者）
    std::int32_t font_pair_cursor_ = 0;            // 对表光标
    // 消息窗口记录（`0x6E` / `0x196` 追加、`0x6F` 行结束、`0x301` 释放）。
    std::unordered_map<std::int32_t, TextWindowRecord> text_windows_;
    std::int32_t current_text_slot_ = 0;  // `Engine+0x7780C`（`0x6E` / `0x196` 写）
    // 消息块状态（`0x304` / `0x305` / `0x6e`）：`msg_arm_` 的第 0 位 = 块已开始、
    // 第 16 位 = 本块真的追加过文本；`msg_saved_cursor_` = 块开头该层的揭示游标。
    std::int32_t msg_arm_ = 0;
    std::int32_t msg_saved_cursor_ = 0;
    // `0x243` 写的三格（消息等待状态）与它前面那道位（`0x5A2D0` 的第 1 位）。
    std::int32_t msg_wait_a_ = 0;
    std::int32_t msg_wait_b_ = 0;
    std::int32_t msg_wait_c_ = 0;
    std::int32_t msg_wait_gate_ = 0;
    // 自动消息：时长 / 到期时刻（0 = 未武装）。`line_records_` 是每槽"行记录数"（≈行号 + 1）。
    std::uint32_t auto_interval_ms_ = 0;
    std::uint32_t auto_due_ms_ = 0;
    bool auto_armed_ = false;
    // 输入是**事件锁存**（引擎：窗口过程把按下位或进输入字，消费者读一次就清）：
    // 只在"上一拍没按、这一拍按"时置锁存；放行/选中各消费一次。⛔ 按电平每帧读会让长按连发。
    bool advance_input_prev_ = false;
    bool advance_input_latch_ = false;
    bool choice_input_prev_ = false;
    std::array<std::int32_t, 10> line_records_{};
    std::size_t text_appends_ = 0;
    // `0x2f7` 写的语音选项格（每槽一格，整格写 1）。
    std::array<std::int32_t, 3> voice_option_2f7_{};
    // `0x10a` 的光标瞬移请求（平台边界：本模型只记请求，不改任何引擎状态）。
    std::int32_t cursor_request_x_ = 0;
    std::int32_t cursor_request_y_ = 0;
    std::size_t cursor_requests_ = 0;
    // 十个文本层的宿主游标（纹理 = 20 + 层号；矩形 / 原点 / 键基数在 `list_records_`）。
    std::array<TextLayerState, 10> text_layer_state_{};
    // typewriter 的跨帧待画格（追加时入队；全局一个队列 = 引擎的推进器是全局的）。
    std::vector<PendingGlyph> pending_glyphs_;
    // 文本计时器（`Engine+0x691EC` 的模型对应物）：起点 / 间隔（= `Font+0x560`，即 `param_cell_`）/ 步号。
    // 语义 = `sub_453A60` 启动、`sub_453B60` 非阻塞步进：未超过 n×间隔 ⇒ 本帧一格都不画。
    std::uint32_t text_timer_start_ms_ = 0;
    std::int32_t text_timer_interval_ms_ = 0;
    std::int32_t text_timer_step_ = 1;
    std::int32_t scalar_10f_ = 0;
    std::int32_t scalar_248_ = 0;
    std::int32_t font_8b_ = 0;
    std::string font_string_;
    std::array<std::int32_t, 5> engine5_{};
    std::int32_t key_total_ = 7;  // 引擎构造（Input[259]=7）建立的初值；0xfe 可覆盖
    std::int32_t save_version_flag_ = 0;
    std::int32_t input_a_ = 0;
    // 滚轮累加格（`Engine+0x1E74`；`0x10d` 读并清 0）。
    std::int32_t wheel_accum_ = 0;
    std::int32_t input_b_ = 0;
    std::uint32_t input_word_ = 0;
    // 平台输入采样的注入点（`0xff`）；空 = 没有设备。
    KeySampler key_sampler_;
    // 宿主每帧喂的"有输入在按下"（见 set_input_pressed）：只驱动文本层的抽干。
    bool input_pressed_ = false;
    // 平台触点的注入点（`0x2fc`）；空 = 永远没有触点。
    TouchSource touch_source_;
    // 平台光标 / 按钮（`0x109` / `0x108`）；空 = 设备不可用。
    CursorSource cursor_source_;
    MouseButtonSource mouse_button_source_;
    std::uint32_t input_mode_ = 0;
    std::int32_t render_target_ = -1;
    std::int32_t blend_mode_ = 0;
    std::uint32_t blend_color_ = 0xFFFFFFFFu;
    std::uint32_t scene_clock_ms_ = 0;
    std::uint32_t scene_previous_ms_ = 0;
    std::uint32_t host_clock_ms_ = 0;
    bool external_clock_ = false;
    bool scene_animating_ = false;
    // 引擎 `+0B5B4`（"场景还有待处理的东西"闩）：排窗时置、窗推进时置、渲染步入清（见 `end_render`）。
    bool scene_pending_ = false;
    bool forced_complete_ = false;
    bool frame_step_requested_ = false;
    bool frame_flag_ = false;
    bool scene_dirty_ = true;
    std::int32_t draw_mode_ = 1;
    std::uint32_t engine_now_ms_ = 0;
    std::uint32_t engine_previous_ms_ = 0;
    bool driven_elsewhere_ = false;
    // （`1f4` / `1f5`）执行区间的嵌套计数；忙标记复用 `driven_elsewhere_`（同一格）。
    std::int32_t bracket_depth_ = 0;
    // 「门一直关着且没有服务分支」的连续迭代计数（只用于诊断，不是引擎状态）。
    std::size_t gate_stall_steps_ = 0;
    bool gate_stall_reported_ = false;
    std::int32_t mouse_interval_ = 1;
    // 鼠标回调的登记状态（引擎级一格；`0xcc` 写、`0xcd` 读）。
    std::int32_t mouse_target_ = -1;   // 登记的脚本位置（dword 下标）；−1 = 取消
    std::int32_t mouse_owner_ = -1;    // 登记时的当前帧脚本身份（frame.local_depth；见 CORE-INPUT-CALLBACK-REGISTER）
    std::uint32_t mouse_last_ms_ = 0;  // 上一次派发时刻（登记时 = 当前时刻）
    std::unordered_map<std::int32_t, std::int32_t> indirect_;
    std::unordered_map<std::uint32_t, TextureEntry> textures_;
    // 每个纹理编号"已经用过的最高版本号"（见 `next_texture_revision`）。释放槽对象 / 重建都不清它。
    std::unordered_map<std::uint32_t, std::uint32_t> texture_revision_floor_;
    // 1000 槽的释放守卫（默认 0；没有脚本写者）。
    std::array<std::int32_t, 1000> surface_release_gate_{};
    std::unordered_map<std::uint32_t, DrawItem> draw_items_;
    std::unordered_map<std::uint32_t, MeshRecord> meshes_;
    std::unordered_map<std::uint32_t, MovieRecord> movies_;
    MovieBackendFactory movie_factory_;
    MovieBytesLoader movie_bytes_loader_;
    std::unordered_map<std::uint32_t, SceneRecord> scene_records_;
    // `0x215` 查的那张**按整数键的记录表**（引擎 Scene 侧；每项 = 一个标志字 + 一个整数值）。
    // ★ 写者还没找到（残差）⇒ 本模型里通常为空，查不到就返回 -1（引擎的同一支）。
    struct KeyedRecord { std::int32_t flags = 0; std::int32_t value = 0; };
        struct KeyedPairRecord { std::int32_t field_a = 0; std::int32_t field_b = 0; };
    std::unordered_map<std::int32_t, KeyedPairRecord> scene_keyed_pair_;
    // `0x224` 清空的那个按 key 的容器（引擎 Scene 侧）：本模型只记条目数（填充者未取证）。
    std::size_t container_224_items_ = 0;
    // `0x7a` 往每个文本槽的 run 头写的两个整数（引擎侧；消费者未建模）。
    struct RunHeader { std::int32_t a = 0; std::int32_t b = 0; };
    std::array<RunHeader, 10> run_header_{};
    // 场景自身变换（`0x229`）：作用键区间 + 三元平移；绘制项的消费者读它。
    SceneSelfTransform scene_self_transform_;
    // 选项表（`0x90`）：按追加顺序；`choice_index_` = 当前选中项（-1 = 无）。
    std::vector<ChoiceEntry> choices_;
    std::int32_t choice_index_ = -1;
    ChoiceWindowState choice_window_;
    // `0x32d` / `0x32a`：场景颜色的请求记录 + 三个天气子对象槽的"存在"标志。
    SceneColorRequest scene_color_request_;
    std::array<bool, 3> scene_effect_present_{};
    // `0x1b1` 写的那格（Engine 侧一个整数档；领域名未取证）。
    std::int32_t cell_1b1_ = 0;
    std::int32_t cell_2e9_ = 0;
    // `0x238` 写的两格（第 1 格清 0、第 2 格 ← 操作数）。
    std::map<std::uint32_t, SceneCommandRecord> scene_commands_;
    std::uint32_t render_now_ms_ = 0;
    bool command_animating_ = false;
    std::vector<std::string> errors_;
    bool sweep_missing_ = false;
    std::vector<std::pair<std::uint32_t, std::size_t>> missing_opcodes_;
    std::unordered_map<std::int32_t, std::pair<std::int32_t, std::int32_t>> aux_a_;
    std::unordered_map<std::int32_t, std::pair<std::int32_t, std::int32_t>> aux_b_;
    std::array<std::array<std::int32_t, 7>, 11> cg_params_{};
    bool movie_active_ = false;
    bool voice_playing_ = false;
    bool halted_ = false;
    std::uint32_t stop_at_opcode_ = 0;
    RunResult stop_;
};

}  // namespace core
