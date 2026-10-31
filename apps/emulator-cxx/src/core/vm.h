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
    // ★ **新门模型不再产生这个停因**（`CORE-VM-GATE` / `CORE-VM-STATE`：22 位位序表里的每一位都有
    //   服务体，未建模的体按「交回迭代头」处理 ⇒ 门关着是**合法等待**，不是停因）。
    //   枚举值保留：`StopReason` 是观测面的编号（日志 / 报告按数值读），删一个值会挪动其它值的编号。
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

// ── 字体对象的一份字体描述（`LOGFONTA` 的最小投影）──
// `height` = lfHeight（**负值**：字号取相反数）、`width` = lfWidth（"下一格"，`0x1a5` / `0x197` 写它）。
// 每支一对、每对两份变体：一份字面名 = 操作数原样，一份 = `'@'` + 操作数（`60-ops-forward` 的
// 「两条字符串入口不许合并」）。⛔ 不要把它写成"同一份的两个拷贝"：`'@'` 那份是独立的变体。
struct FontDescription {
    std::string face;
    std::int32_t height = 0;
    std::int32_t width = 0;
};
struct FontDescriptionPair {
    std::array<FontDescription, 2> variants{};
};

// ── 显示消息型异常（`60-ops-numeric` 的「三类异常」之一种）──
// 携带**错误码**（诊断 / 日志用）与**要显示给玩家的文本**（逐字）。主循环捕获它之后显示消息，
// 再按返回值 / 用户选择决定重试 / 跳过 / 结束 —— 那三支本版未成篇。
// ⛔ 三类异常可区分、不许合并：本类型就是"显示消息"那一支；`CoreError` 裸用 = 操作数类型异常；
//    退出型异常另有一支（本模型里走 `StopReason::Halted`）。
class DisplayMessageError : public CoreError {
public:
    DisplayMessageError(std::string code, std::string text)
        : CoreError("display-message:" + code), code_(std::move(code)), text_(std::move(text)) {}
    const std::string& code() const { return code_; }
    const std::string& text() const { return text_; }

private:
    std::string code_;
    std::string text_;
};

// 最小 CORE-VM：一次 step 至多执行一条指令。状态选择字（门字）**不是常量**：写者有 `0x88` 的 0 支、
// `0x19b` / `0x19c` / `0x71`、追加前置支与第 27 位服务体等；位的服务链见 `service_gate_bit`，
// 位序表与「服务后」三件事见 `CORE-VM-GATE` / `CORE-VM-STATE`。
class Machine {
public:
    static constexpr std::size_t kFrameCount = 40;
    // 每帧返回栈容量（`CORE-OPS-FRAME-CALL`：**每帧 256 个位置**，压栈**不检查容量**）。
    // 第 257 次压栈在引擎里落到帧区之外（未定义行为）⇒ 实现必须**拒绝或记诊断**；
    // ⛔ 不许把它当成"覆盖最旧一项"，也不许静默截断。
    static constexpr std::size_t kRetStackCapacity = 256;
    // 泵的专用帧槽（`CORE-OPS-RUN-BRACKET`：**每条正命令都装进帧 37**，装载前无条件释放它）；
    // 泵帧父帧号的哨兵 **−10**（`0x2` 的 −10 支认它；负支由泵直接写、正支靠「父帧候选」格照抄）。
    static constexpr std::size_t kPumpFrameSlot = 37;
    static constexpr std::int32_t kPumpParentSentinel = -10;

    Machine(std::uint32_t int_key, const OpcodeTable& table, const Install* install = nullptr,
            ConfigStore config = ConfigStore{});
    /** 析构 = 文件库析构的模型入口（`0x454cd0`：槽位表逐格清 0）。 */
    ~Machine();

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
    // 按槽取帧记录（观察口；⛔ 不是产品接口）：`0x6` 的目标槽 / 泵帧都**不在当前帧上**。
    const Frame& frame_at(std::size_t index) const { return frames_[index]; }
    std::size_t current_frame() const { return current_frame_; }
    std::uint32_t state_flags() const { return state_flags_; }
    // 引擎级状态字保存栈（`CORE-OPS-FRAME-RET`）：**只**由引擎自己重入脚本的路径压入
    // （不是 opcode 处理体；脚本自己的调用与返回不碰它）；空栈时 `0x5` 不写状态选择字。
    std::size_t engine_state_word_stack_size() const { return engine_state_word_stack_.size(); }
    // 「父帧候选」格（`CORE-OPS-FRAME-LOAD-FRAME`）：**装载器只照抄它**，不自己判断父帧号；
    // 写者是 `0x3` / `0x6` / `0x9` 重置 / 泵（泵写 −10）。
    std::int32_t parent_candidate() const { return parent_candidate_; }
    // ── 待处理命令队列 / 泵（`CORE-OPS-RUN-BRACKET`；观察口 + 测试接缝，⛔ 不是产品接口）──
    // 泵挂起帧号（−1 = 没有挂起帧）与队列泵占用标记。
    std::int32_t suspended_frame() const { return suspended_frame_; }
    bool pump_busy() const { return pump_busy_; }
    // 状态选择字的**待恢复副本**（挂起时整字存、泵帧收场时并回；`0xd9` 的第 2 步也写它）。
    std::uint32_t status_restore() const { return status_restore_; }
    // 队列本体：头 / 尾 / 占用数 / 历史最高占用。判空口径 = **头 ≥ 尾**（`CORE-OPS-RUN-BRACKET`），
    // 占用标记与它是**同一状态**（非 0 ⟺ 头 < 尾），所以不另存一个可漂移的标记格。
    // ★ 门上的第 29 位**不读这一格**：它的判据是「文本槽在用标志」（见 `text_slot_in_use()`）。
    std::size_t queue_head() const { return queue_head_; }
    std::size_t queue_tail() const { return queue_tail_; }
    std::size_t pending_command_count() const { return queue_tail_ - queue_head_; }
    bool queue_occupied() const { return queue_head_ < queue_tail_; }
    std::size_t queue_high_water() const { return queue_high_water_; }
    std::int32_t queue_command(std::size_t index) const { return command_queue_[index]; }
    // ── 文件库的 **256 格槽位表**（`CORE-OPS-EVENT-143`）──
    // ★ 它就是 `0x143` 第 3 步逐槽检查的那张表（不是 `0x143` 私有的登记表）：按**槽号 = 统一文件 id
    //   的高 8 位（区号）**索引「已装载文件对象」（一张**索引对象**，不是单个文件），格值 0 = 该区号
    //   没装载。整表在文件库建立时清 0（`0x454adc ecx = 0x100` / `0x454ae6 rep stosd`），此后只在
    //   **文件装载**（`0x455948`：`ecx = [对象+114h]` = 该索引对象自带的区号）与**文件库析构**
    //   （`0x454cd0`：逐槽销毁非空格并清 0）时变。
    // ★ 本模型没有独立的文件库对象 ⇒ 表挂在 `Machine` 上。产品侧的登记点 = 走 `read_install_file`
    //   的四个装载点（脚本 `install_script_into` / 影片 `vm_movie` / 纹理 `vm_ops_texture` /
    //   `0x9` 的根重装）—— 它们都调 `register_loaded_file()`（按统一 id 取区号：本模型装到的都是
    //   基础索引的文件 ⇒ 区号 0 ⇒ 写格 0，而 `0x143` 不检查格 0）。★ `src/boot.cpp` 的**启动期根装载**
    //   不登记：它拿到的字节来自已经打开的 ALF 索引（本模型没有把"打开索引"建成一次
    //   `read_install_file`），而 `register_loaded_file()` 是私有的 ⇒ 启动路径没有产品接缝可调；
    //   它同样只会写格 0，`0x143` 不看格 0。
    //   **扩展索引（AAI，区号 1..255）的装载本模型不解析** ⇒ 只留文件库的装载入口
    //   `load_extension_index()`（引擎在启动期枚举 `*.AAI`，其解析属残差）。
    //   `~Machine()` 走 `release_file_library()`（析构口径）。
    static constexpr std::size_t kFileLibrarySlotCount = 256;
    std::int32_t file_library_slot(std::size_t index) const {
        return index < file_library_slots_.size() ? file_library_slots_[index] : 0;
    }
    /** 文件库装载一个**扩展索引**（AAI）：按它自带的**区号**登记槽位（`0x455948`）。
     *  ★ 本模型不解析 AAI 文件（残差）⇒ 这是"装载一个扩展索引"的模型入口，不是 `0x143` 私有的表。
     *  区号 0 = 基础索引那一格（`0x143` 不检查）；≥ 256 是表外写 ⇒ 不写、只记诊断。 */
    void load_extension_index(std::int32_t zone);
    /** 文件库析构（`0x454cd0`）：逐槽清 0。`~Machine()` 也调它 —— 测试用它模拟"卸载"，
     *  因为本模型没有独立的文件库对象。 */
    void release_file_library() { file_library_slots_.fill(0); }
    // 宿主消息泵的节流（`CORE-VM-LOOP`）：**本迭代**是否该泵一次（迭代头算出的结果），与计数器现值。
    bool should_pump_host() const { return host_pump_due_; }
    std::uint32_t pump_counter() const { return pump_counter_; }
    // ★ 测试接缝（⛔ 不是产品接口）：
    /** 直接往队列尾放一条命令（只模拟「投递」那一下，**不**带投递口的顺手泵尾巴）。 */
    void enqueue_command_for_test(std::int32_t command);
    void seed_status_restore_for_test(std::uint32_t value) { status_restore_ = value; }
    // 全局引用族槽里存的**地址**（观察口；⛔ 不是产品接口）：`0x9` 复位后必须读 0。
    std::uint32_t pointer_slot(std::uint32_t tag, std::uint32_t index) const {
        return read_pointer(Operand{tag, index});
    }
    bool voice_playing() const { return voice_playing_; }
    std::int32_t voice_volume(std::size_t group) const { return voice_volumes_[group]; }
    std::int32_t scalar_149() const { return scalar_149_; }
    std::int32_t scalar_21b() const { return scalar_21b_; }
    // 夹具注入：门前渲染步的 (c)（`CORE-TIMING-WINDOW`）读这一格。⛔ 产品路径只由 `0x21b` 写它
    // （引擎初值 0）；这里的 setter 只为「要窗 / 节拍真的走」的用例省掉一条 `0x21b 1` 指令。
    void set_scalar_21b(std::int32_t value) { scalar_21b_ = value; }
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
    // ── **文本槽在用标志**（`[77984h]`；观察口 + 第 29 位的判据，⛔ 不是产品接口）──
    // 槽 1..9 **任一状态字非 0** ⇒ 有槽在用（`0x409421 cmp dword ptr [edi],0` / `0x409426` 置 1；
    // 入口 `0x409413` 先清 0；槽 0 不参与 —— 服务体的循环从槽 1 起）。
    // ★ 与 `pending_glyph_count()`（"有待画字"）**不是一回事**：这是 `0x300` 写的那张槽状态字表，
    //   逐字推进队列是另一件东西；两条判据分开，⛔ 别用一个顶另一个。
    // ★ 也⛔ **不是**待处理命令队列的占用标记：队列判空是「头 ≥ 尾」（`0x40FB73` / `0x40FB79`），
    //   全语料没有"队列非空"的单格 ⇒ 拿队列当这一位的判据，演示闸会永远派发不出去
    //   （取证 `report-text-gate.md` §1.3 / §5.3）。
    bool text_slot_in_use() const;
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
    // ★ 字号是**未缩放**的量：缩放乘在**建字体**那一步（`font_build_height()`，比例见 `font_scale_*`）。
    //   ★ 本宿主**不建模建字体缩放**：`font_build_height()` 只是**实现侧观察口**（无产品消费者，
    //     产品绘制走 `text_gdi.cpp` 的未缩放 `lfHeight`）。比例 = 1.0 也**不**等于"未缩放字号" ——
    //     公式是 `trunc(高 × 比例 + 0.5)`，−40 × 1.0 + 0.5 截断是 **−39** 而不是 −40（见
    //     `font_build_height()` 与自检 `spec_font_build_scale`）。
    std::int32_t font_size() const { return glyph_height_; }
    std::int32_t glyph_height() const { return glyph_height_; }
    std::int32_t font_b_height() const { return font_b_height_; }
    // ── 字体对象的两对字体描述（`0x75` / `0x1a5` 与 `0x197` / `0x2fe` 各一对；观察口，⛔ 不是产品接口）──
    // 每对两份变体：`[0]` 字面名 = 操作数原样、`[1]` = `'@'` + 操作数；两份的高 / 下一格完全相同。
    const FontDescription& font_description(bool branch_b, std::size_t variant) const {
        const auto& pair = branch_b ? font_b_pair_ : font_a_pair_;
        return pair.variants[variant];
    }
    // 建字体高度 = `trunc(该支描述的高 × 缩放比例 + 0.5)`（向零截断；高是负值 ⇒ 等价于"向上取整"）。
    // ★ 引擎在**建字体**那一步乘缩放（`0x45A3A9` A 支 / `0x45A7F8` B 支）；重建的内部过程属实现侧，
    //   本观察口只回答"若现在建字体，lfHeight 会是多少"。
    std::int32_t font_build_height(bool branch_b = false) const;
    // 缩放两个分量（高分量乘进 lfHeight；宽分量只抄进十个文本槽的两格）。
    double font_scale_width() const { return font_scale_width_; }
    double font_scale_height() const { return font_scale_height_; }
    // 整趟重算**写没写** / 发了几次重建请求（守卫用来钉"两个分量都相等 ⇒ 整趟什么都不写"）。
    std::size_t font_scale_write_count() const { return font_scale_write_count_; }
    std::size_t font_rebuild_requests() const { return font_rebuild_requests_; }
    // 十个**文本槽**：各自的字形高度（`槽+0xC8`）与绘制原点偏移（`槽+0x0C` / `槽+0x10`）。
    std::int32_t text_slot_glyph_height(std::size_t i) const { return text_slots_[i].glyph_height; }
    std::int32_t text_slot_x(std::size_t i) const { return text_slots_[i].x_offset; }
    std::int32_t text_slot_y(std::size_t i) const { return text_slots_[i].y_offset; }
    // 文本槽的**缩放两格**（`槽+0x74` = 宽分量、`槽+0x78` = 高分量；与绘制原点偏移不是同一对格）。
    double text_slot_scale_width(std::size_t i) const { return text_slots_[i].scale_width; }
    double text_slot_scale_height(std::size_t i) const { return text_slots_[i].scale_height; }
    static constexpr std::size_t text_slot_count() { return 10; }
    // 当前**A 支描述的字面名**（原样那一份）。消费方只有 GDI 对照档（`EMULATOR_CXX_GLYPH_GDI_FACE=engine`）；
    // 缺省为空串（引擎的缺省字体名本版未取证）。
    const std::string& font_string() const { return font_a_pair_.variants[0].face; }
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
    // ── 字体缩放的平台输入接缝（`60-ops-forward`「字号是"未缩放"的量」）──
    // 视口 / 客户区尺寸是**平台输入**（`0x46586F` / `0x46587E` 用引擎缓存的视口与配置客户区）。
    // 缺省 = 按配置的客户区尺寸（⇒ 比例 1.0）；宿主 / 守卫可注入真实视口。⛔ handler 里不许直接读 Win32。
    struct Viewport {
        std::int32_t width = 0;
        std::int32_t height = 0;
    };
    using ViewportSource = std::function<Viewport()>;
    void set_viewport_source(ViewportSource source) { viewport_source_ = std::move(source); }
    // 引擎「重新应用字体设置」的路径（重算缩放的**第 ② 个时机**；`sub_465840` 的调用点 0x41806E / 0x4692D7）。
    // 本模型没有与之对应的 opcode ⇒ 用这个接缝表示那一次重算（⛔ 产品脚本路径不调用它）。
    void reapply_font_settings();
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
    // 「本次读数」= 宿主毫秒时钟的一次采样（`CORE-TIMING-CLOCK`）。外部时钟 = 宿主给的值
    // （`set_host_clock_ms`）；否则 `step()` 每迭代把虚拟宿主时钟 +16ms —— 固定步长是**仿真参数**，
    // 不是引擎常量（spec 的「成立条件」）。
    std::uint32_t host_now_ms() const { return host_clock_ms_; }
    // 时钟对的**唯一写法**：先搬旧、再写新（`CORE-TIMING-CLOCK` 登记的四个写者同序）。
    // ★ 「引擎时钟对」与「场景时钟对」是**同一对**（`70-vm.md:190` / `60-ops-render.md:290-291`）：
    //   ⇒ (c) 取消门前那一次盖钟之后，场景时钟仍由 `0x1f4` / `0x20c` / `0x23c` 推进。
    //   ⛔ 别再为 `0x1f4` / `0x20c` 另建一对「引擎时刻」—— 那样 (c) 一落，时间就真的停走了
    //   （实测：门前的钟被取消 + 其余三个写者写的是另一对 ⇒ TITLE 输入泵死锁）。
    void stamp_scene_clock(std::uint32_t sample_ms) {
        scene_previous_ms_ = scene_clock_ms_;
        scene_clock_ms_ = sample_ms;
    }
    void set_driven_elsewhere(bool value) { driven_elsewhere_ = value; }
    // 启动时是否跑 LOGO（`0x130` 读的格）：`--skip-logo` 在跑根脚本前置 0。
    void set_show_logo(bool value) { show_logo_ = value ? 1 : 0; }
    // show-logo 标志的当前值（观察口；`0x130` 读的就是这一格）。`0x9` 必须把它复位成 0。
    bool show_logo() const { return show_logo_ != 0; }
    // 启动时登记的重置钩子被调用了几次（守卫用；⛔ 不是产品接口）。
    // ★ 钩子的两处写入（当前帧推进格数 ← 1 / 清状态字第 9 位）在 `0x9` 后续的
    //   「帧槽全释放 → 状态字整字清 0 → 根帧装载写长度槽 0」之后**不可观察**，所以这里
    //   只能观察"钩子被调用过"；两处写入本身的红判据不存在（见 `core_selftest.cpp` 的说明）。
    std::size_t engine_reset_hook_calls() const { return engine_reset_hook_calls_; }
    bool scene_animating() const { return scene_animating_; }
    // 场景侧的"待处理"标记（引擎 `+0B5B4`）：排窗 / 窗还在走 / 命令进度还在走 ⇒ 1；
    // 渲染步的推进子步清 0 再重算。读者 = `message_subsystem_busy()`（`0x21c` 谓词 A）
    // 与**渲染步收尾**（`end_render`：= 0 ⇒ 清空命令表）。
    bool scene_pending() const { return scene_pending_; }
    // 测试注入口（`0x202` / `0x323` 等排窗写点之外，用来单独布「收尾判据」的两种取值）。
    void set_scene_pending(bool value) { scene_pending_ = value; }
    bool forced_complete() const { return forced_complete_; }
    void force_complete() { forced_complete_ = true; }
    // ── 渲染步门槛的四个输入（`CORE-TIMING-WINDOW`）──
    // (b) 画面暂停闩：非 0 ⇒ **连盖钟一起跳过**（与执行区间的区间忙标记不是同一格）。
    //     ★ 消息号未取证（`RES-VM-SCENE-HOLD-LATCH`）：这里只暴露"宿主 / 测试怎么置它"。
    std::int32_t scene_hold_latch() const { return scene_hold_latch_; }
    void set_scene_hold_latch(std::int32_t value) { scene_hold_latch_ = value; }
    // 「只挡推进」第 2 条：影片对象的缓冲被另一处持有（`RES-WINDOW-MOVIE-HELD`）。
    bool movie_held() const { return movie_held_; }
    void set_movie_held(bool value) { movie_held_ = value; }
    // 「只挡推进」第 3 条的影片半边：有待消费的新帧（**读一次即消费**，由渲染步门槛消费）。
    bool movie_new_frame() const { return movie_new_frame_; }
    // 测试注入口：模拟"后端刚解出一帧"（真实生产者在 `service_movies()`）。
    void set_movie_new_frame(bool value) { movie_new_frame_ = value; }
    // 「只挡推进」第 4 条：渲染步内部的就绪判据（缺省就绪；判据体未取证）。
    bool render_ready() const { return render_ready_; }
    void set_render_ready(bool value) { render_ready_ = value; }
    // 宿主提供时钟（毫秒）；不设置时每次 step 自动 +16ms（仿真的固定步长）。
    void set_host_clock_ms(std::uint32_t now_ms) { host_clock_ms_ = now_ms; external_clock_ = true; }
    // 回到自动步长时钟（每次 step +16ms）；用于无窗口的确定性逐帧导出。
    void reset_scene_clock(std::uint32_t now_ms = 0) {
        external_clock_ = false;
        scene_clock_ms_ = now_ms;
        scene_previous_ms_ = now_ms;
        host_clock_ms_ = now_ms;  // 虚拟宿主时钟也从这里起步（之后每迭代 +16）
    }
    // 场景推进：盖章 / 插值 / 到期（`CORE-TIMING-WINDOW`）。step() 在门之前调用它。
    // ★ `advance_windows = false` = 只更新场景时钟（迭代头读数），**不推进窗** —— 这是
    //   测试 / 诊断的直接入口；主循环走下面的 `advance_render_step()`（带门槛）。
    void advance_scene(std::uint32_t now_ms, bool advance_windows = true);
    // 渲染步的**推进子步**（同一渲染步的更早子步）：三个"连盖钟一起跳过" → **盖钟** →
    // 五个"只挡推进"（`CORE-TIMING-WINDOW` 的「暂停时」那一段）。返回值供主循环决定
    // "这一迭代有没有跑渲染步"（跳过 ⇒ 不判据、不发请求、不清帧标志、不推窗）。
    enum class SceneAdvance {
        Skipped,    // 三个"整段跳过"之一成立（(a) 绘制模式 / (b) 画面暂停闩 / (c) 0x21b 空转）：连盖钟一起跳过
        ClockOnly,  // 盖了钟，但五个"只挡推进"之一成立（含 (d) 执行区间暂停）：窗不前进、两个标记不重算
        Windows,    // 盖钟 + 推进（⇒ 门前那一次请求在此之后发出）
    };
    SceneAdvance advance_render_step(std::uint32_t now_ms);
    // 0x20C 的帧步进请求：由渲染步消费。
    bool frame_step_requested() const { return frame_step_requested_; }
    void request_render() { frame_step_requested_ = true; }
    bool consume_frame_step() {
        const bool value = frame_step_requested_;
        frame_step_requested_ = false;
        // ★ 「强制完成」**不在这里清**：它的清点是**渲染步收尾**（`end_render()`，在
        //   "待处理 = 0 ⇒ 清空命令表"之后）**无条件**清 —— `CORE-TIMING-WINDOW` 的「不粘住」。
        //   ⛔ 别把它写成"读一次请求就清"：宿主可能只消费请求、不跑渲染步收尾。
        return value;
    }
    bool scene_dirty() const { return scene_dirty_; }
    void mark_dirty() { scene_dirty_ = true; }
    std::int32_t draw_mode() const { return draw_mode_; }
    bool frame_flag() const { return frame_flag_; }
    void clear_frame_flag() { frame_flag_ = false; }
    // 夹具注入「帧标志」被置起（引擎里它的写者未取证，见 `RES-RENDER-STEP-FLAG`）：
    // 用来观察 `0x4127E7` 的清点 —— (c) 跳过时**不清**，门前那一次请求之后才清。
    void set_frame_flag(bool value) { frame_flag_ = value; }
    bool render_target_present() const { return render_target_ != -1; }
    std::int32_t render_target() const { return render_target_; }
    std::int32_t blend_mode() const { return blend_mode_; }
    std::uint32_t blend_color() const { return blend_color_; }
    // ── 键绑定三张表（`CORE-OPS-SCALAR-TABLE-10C`；观察口，⛔ 不是产品接口）──
    // ① 键号 → 动作号：与 0x10b（单键绑定）**共用同一张 32 格表**（`array_d1383_`）；0x10c 本条**不碰**它。
    // ② 动作号 → 槽号：0x10c **只读**（第 2 个操作数是动作号）。
    // ③ 槽号 → 键号：0x10c **写**它（`-1` = 未绑定）。
    std::int32_t key_action(std::size_t key) const { return array_d1383_[key]; }
    std::int32_t action_slot(std::size_t action) const { return action_slot_table_[action]; }
    std::int32_t slot_key(std::size_t slot) const { return slot_key_table_[slot]; }
    // 测试播种（⛔ 不是产品接口）：预置"动作 → 槽"那一格。
    void set_action_slot_for_test(std::size_t action, std::int32_t slot) {
        action_slot_table_[action] = slot;
    }
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
    // ── 错误通道（`60-ops-numeric` 的「错误通道」）──
    // 引擎为**一次**触发发**一个**平台写请求：文本 + 它的长度。两个抑制条件由引擎自己判定：
    //   ① 输出句柄无效（宿主没打开那个文件）⇒ 整条丢弃（不写、不抛、不中止）；
    //   ② 宿主装了过滤器、过滤器返回非 0 ⇒ 同样丢弃。
    // ★ 诊断本身**无条件**保留在 `errors_`（测试 / 诊断读它）；抑制只影响那次平台写请求。
    // ★ 平台写请求长什么样（哪个文件、显示给谁）属平台侧，不在规范正文：这里只留一条接缝。
    //   ⇒ 一次触发 = 一次调用；⛔ 不要在 50+ 个 handler 里各拼一次请求（改在 `report_error` 内部）。
    using ErrorOutputWriter = std::function<void(const std::string& text, std::size_t length)>;
    using ErrorOutputFilter = std::function<std::int32_t()>;
    void set_error_output_writer(ErrorOutputWriter writer) { error_writer_ = std::move(writer); }
    void set_error_output_handle_valid(bool valid) { error_handle_valid_ = valid; }
    void set_error_output_filter(ErrorOutputFilter filter) { error_filter_ = std::move(filter); }
    // 平台写请求**实际发出**的次数（观察口；⛔ 不是产品接口）。
    std::size_t error_write_requests() const { return error_write_requests_; }
    // 触发条件判定在各 handler；这里只做「判定抑制 → 发请求 → 保留诊断」。
    void report_error(std::string text);
    const std::vector<std::string>& errors() const { return errors_; }
    void clear_errors() { errors_.clear(); }
    // ── 输出对象（`45-objects`「对象前缀：类行为表与输出对象」）──
    // ★ 它是进程级静态对象、**不是**错误通道（`report_error` 是另一路：场景错误缓冲 + 平台写请求）。
    //   本条只规定三项操作的外部效果：日志落点的文本组成 + 报告再把文本交给根对象的错误处理。
    //   落盘时机 / 缓冲 / 平台是否受理属平台侧（写失败不改变引擎状态）⇒ 这里只留接缝。
    // ★ 「按脚本位置追加」的前缀 = `(<脚本名>：<行号>行目) `（全角冒号 + 「行目」+ 一个半角空格）。
    //   脚本名与行号的**来源未取证**（残差 `RES-OBJ-LOG-PREFIX`）⇒ 由调用方按参数给，⛔ 不发明来源。
    // ★ 行为表是三项：删除式析构 + 两个致命错误报告入口。基类操作只用到「窗口句柄 + 文本」那一项
    //   （`45-objects`：`基类操作只用到"窗口句柄"那一项`）⇒ 「两个整数 + 文本」那个入口本模型
    //   **未接线**（没有任何调用者，对象模型属子单）⇒ 不建字段：grep 到的表面积不许大于真实能力。
    struct OutputObjectIo {
        std::function<void(const std::string& text)> log;              // 日志落点：写一段文本
        std::function<void(const std::string& text)> root_error_window; // 根错误处理（窗口句柄 + 文本）
    };
    void set_output_object_io(OutputObjectIo io) { output_object_io_ = std::move(io); }
    /** 追加一段文本：原样交给日志落点。 */
    void output_append(const std::string& text);
    /** 按脚本位置追加：`(<脚本名>：<行号>行目) ` + 文本（两参数的来源由调用方给）。 */
    void output_append_with_position(const std::string& script_name, std::int32_t line,
                                     const std::string& text);
    /** 致命错误报告：先写 `\r\n致命的エラー[` + 文本 + `]\r\n`，再把文本交给根对象的错误处理
     *  （「窗口句柄 + 文本」那一项）。★ 与错误通道无关。 */
    void output_report(const std::string& text);
    /** 走过「报告」这一项的次数（观察口；⛔ 不是产品接口）。 */
    std::size_t output_reports() const { return output_reports_; }
    // ── 第 27 位（消息逐帧推进门位）的宿主 / 子系统接缝 ──
    // ① 门服务体的一次消息处理（`60-ops-scalar`「成对写：opcode 0x88」）：读一条消息并派发 →
    //    让出一拍 → 窥视一次。★ 它与迭代头按节流触发的宿主泵**不是同一处**，也不受那个计数器约束。
    //    缺省三件都是 no-op（没有宿主消息源）；`host_message_steps()` 数"进入过几次消息处理"。
    struct HostMessageIo {
        std::function<void()> process_one;  // 读一条消息并派发
        std::function<void()> yield;        // 睡眠 / 让出一拍
        std::function<bool()> peek;         // 窥视消息（不移除）
    };
    void set_host_message_io(HostMessageIo io) { host_message_io_ = std::move(io); }
    std::size_t host_message_steps() const { return host_message_steps_; }
    // ② 文本子系统一次调用的返回值（`0x71`；返回值非 0 ⇒ 置第 27 位，领域含义未定）。
    using TextRequestResult = std::function<std::int32_t(std::int32_t request)>;
    void set_text_request_result(TextRequestResult result) {
        text_request_result_ = std::move(result);
    }
    // ③ 「读文本跳过」的**已读命中**（残差 `RES-MSG-BLOCK-READ-HIT`：已读表未建模）。
    //    缺省 = 无命中；`0x305` 的 ② 与 `0x6e` / `0x196` 的追加前置支共用同一判据。
    using ReadTextSkipHit = std::function<bool()>;
    void set_read_text_skip_hit(ReadTextSkipHit hit) { read_text_skip_hit_ = std::move(hit); }
    // 最近一次被主循环捕获的**显示消息型异常**的错误码 / 文本（观察口；⛔ 不是产品接口）。
    const std::string& last_display_code() const { return last_display_code_; }
    const std::string& last_display_message() const { return last_display_message_; }
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
    // 门字（`state_flags_`）：**只给自检 / 夹具**摆前置位用（`0x88` / `0x71` / `0x305` 与追加前置支
    // 已是第 27 位的产品写者，但 `0xc2` 的两条支路仍需要能单独摆位来测）。⛔ 不要在产品代码里用它绕开指令。
    std::uint32_t& state_flags_mut() { return state_flags_; }
    // ── **门外直派**：⛔ 只给自检 ──
    // 门（`state_flags_`）一旦非 0 就不取指；第 27 位现在有服务体（交回迭代头），而它的「清」由
    // 消息处理 / 脚本指令负责 ⇒ 置起后脚本本身派发不到。`0xc2` 的 A 支与 `0xc4` 的影片门支、
    // 以及 `0x19c` 置位之后的读回，都要靠这个接缝在**不取指**的情况下跑 handler。
    // ★ 它只做**一次派发**：不取指、不推进 ip、不服务门位。⛔ 生产路径只有 `step()`。
    void dispatch_for_test(const Instruction& instruction) { execute(instruction); }
    // ── 门服务链的收场（`CORE-VM-STATE` 位序表的「服务后」列）──
    // `CORE-VM-GATE`：一个位的服务体执行完之后只可能做三件事之一；扫描的收场由这三件事拼出来。
    enum class GateAction {
        ToHead,    // 交回迭代头：本迭代到此为止、**不派发**（更晚的位本迭代不再看）
        Continue,  // 继续扫描：落到位序表里的**下一位**
        Dispatch,  // 由服务体自己派发一条（位序表里只有第 29 位这一种）
    };
    // ── 门服务链的测试接缝（S2；⛔ 不是门本身）──
    /** 只跑**一个位的服务体**并返回它的收场：不扫表、不派发。
     *  `check_text_cell_reveal` 用它把"一拍"压成"时钟推进 + 第 30 位服务体"（那一格的观察口是逐拍节奏）。 */
    GateAction service_gate_bit_for_test(std::int32_t bit) { return service_gate_bit(bit); }
    // ── 帧族的测试脚手架（S1；⛔ 不是平台边界）──
    // 「泵」已落地（S3），但引擎自己重入脚本的那两处入口还没实现 ⇒
    // 「状态字保存栈非空」这个前置只能由测试摆出来。⛔ 不要在产品代码里用它们。
    void set_parent_candidate_for_test(std::int32_t value) { parent_candidate_ = value; }
    /** 直接调用**装载器**（`0x3` / `0x6` / `0x9` / 泵正命令共用的那条）：它只照抄「父帧候选」格。 */
    void load_frame_for_test(std::uint32_t file_id, std::int32_t target);
    void set_pump_state_for_test(std::int32_t suspended_frame, bool pump_busy) {
        suspended_frame_ = suspended_frame;
        pump_busy_ = pump_busy;
    }
    void seed_frame_length_for_test(std::size_t slot, std::uint32_t words) {
        frames_[slot].length_slot = words;
    }
    void push_engine_state_word_for_test(std::uint32_t word) { push_engine_state_word(word); }
    bool bgm_fade_active() const { return (state_flags_ & (1u << 9)) != 0; }
    std::int32_t voice_resource() const { return voice_resource_; }
    // 三个语音槽的到期计时记录（`CORE-VM-LOOP` 的**语音通道**）：状态（低两位 = 完成 / 跳过）/ 起点 /
    // 时长 / 该槽的待办标记。★ 起点的 **0 是"未盖章"哨兵**（与真实时刻 0 撞车）⇒ 只由时间交付盖一次。
    std::int32_t voice_delay(std::size_t i) const { return voice_slot_timers_[i].duration; }
    std::int32_t voice_state(std::size_t i) const { return voice_slot_timers_[i].state; }
    bool voice_pending() const {
        for (const auto& slot : voice_slot_timers_) {
            if (slot.pending) return true;
        }
        return false;
    }
    std::int32_t voice_group_enabled(std::size_t i) const { return voice_group_enabled_[i]; }
    std::int32_t voice_group_value(std::size_t i) const { return voice_group_value_[i]; }
    static constexpr std::size_t voice_slot_count() { return 3; }
    // ── `0xc4`（配音播放）/ `0x82`（画一段文本记录）的观察口 ──
    std::size_t voice_load_requests() const { return voice_load_requests_; }
    std::size_t voice_play_requests() const { return voice_play_requests_; }
    std::int32_t voice_mirror_resource(std::size_t i) const { return voice_mirror_resource_[i]; }
    std::int32_t voice_mirror_a4(std::size_t i) const { return voice_mirror_a4_[i]; }
    // ── 时间交付：两套到期计时器（`CORE-VM-LOOP` 的「一次迭代做哪些事」）──
    // ★ 交付**不是混音**：它只读写下面这些引擎记录，并把「到期」交给该子系统自己的释放 / 启动入口；
    //   混音 / 输出 / 音量在音频子系统与平台（`70-vm.md:23` 的范围声明）。交付路径**不发宿主请求**。
    // ★ 音效通道的 10 格与 `kSeSlotTableSize`（`T_SE+0x4BC` 的「槽 → 资源号」）**不是同一张表**：
    //   那条记录是「活动 / 起点 / 时长 / 待交值」，这条是「槽 → 当前资源号」；条数相同只是巧合。
    //   10 这个数取自同一条 spec 句（「音效通道：10 个通道」，`70-vm.md:19`）+ 绑定里的
    //   `0x4b52a4 音效通道：10 个通道`。
    static constexpr int kSeChannelCount = 10;
    /** 一个音效通道的到期计时记录（`70-vm.md:19`）。★ `start` 的 0 = **未盖章哨兵**。
     *  ★ 四格逐字对应 `T_SE` 的四组：活动 `+0x418` / 起点 `+0x440` / 时长 `+0x468` / 交值 `+0x490`。
     *    第五组（**播放名**，`+0x4BC`）不在本条记录里 —— 它是 `se_slot_resource_`（交付不读它）。
     *    「待办」**不是每通道一格**：它是 `T_SE+0x4B8` 的**单格**（登记 `0x4b51d0` ← 1 / 交付入口
     *    `0x4b5239` 读它并在进入时清 / 未到期 `0x4b5296` 再置 / 整组清零 `0x4b521e` ← 0）
     *    ⇒ 由 `se_work_pending_` 承担，⛔ 不要给每个通道再发一个同义字段。 */
    struct SeChannel {
        bool active = false;        // 活动标记
        std::uint32_t start = 0;    // 起点（0 = 未盖章）
        std::int32_t duration = 0;  // 时长（毫秒）
        std::int32_t value = 0;     // 到期时随通道交给释放入口的值
    };
    /** 一个语音槽的到期计时记录（`70-vm.md:20`）。★ `start` 的 0 = **未盖章哨兵**。 */
    struct VoiceSlotTimer {
        std::int32_t state = 0;     // 状态字：低两位 = 完成 / 跳过（3）
        std::uint32_t start = 0;    // 起点（0 = 未盖章）
        std::int32_t duration = 0;  // 时长（毫秒；`0x2f5` 写的延时）
        bool pending = false;       // 该槽的待办标记（`0x2f5` 置 1）
    };
    const SeChannel& se_channel(std::size_t i) const { return se_channels_[i]; }
    bool se_work_pending() const { return se_work_pending_; }
    std::size_t se_release_steps() const { return se_release_steps_; }
    std::int32_t se_last_released_value() const { return se_last_released_value_; }
    const VoiceSlotTimer& voice_slot_timer(std::size_t i) const { return voice_slot_timers_[i]; }
    // 「该子系统仍有一件未完成的工作」（语音那一份；第 14 位服务体的三个前置之一，见 `70-vm.md:21` / `:112`）。
    bool voice_work_pending() const { return voice_work_pending_; }
    std::size_t voice_advance_steps() const { return voice_advance_steps_; }
    /** 时间交付的**音效通道**（`70-vm.md:19`）：逐通道懒盖章 → **有符号**到期 → 清活动 / 交值给释放入口。
     *  未到期 ⇒ 重新置「仍有一件未完成的工作」；入口处该标记为 0 ⇒ 直接返回（`0x4b5239`）。 **不发宿主请求**。 */
    void deliver_sound_time(std::uint32_t now_ms);
    /** 时间交付的**语音通道**（`70-vm.md:20`）：逐槽懒盖章 → **有符号**到期 → 清槽状态。
     *  返回 true = 「有推进」（`0x4bbc01`）；主循环据此判音乐压低。 **不发宿主请求**。 */
    bool deliver_voice_time(std::uint32_t now_ms);
    /** 音乐压低请求（`sub_407120`）：四条件成立 ⇒ 状态选择字**第 14 位 ← 1**（`0x40716c` 写回门字）
     *  并置语音的「仍有一件未完成的工作」。语音推进与 `0xc4` 的第 2 步都走这里。 */
    void request_bgm_duck();
    // ── 时间交付的测试注入口（⛔ 不是产品接口）──
    // ★ 产品路径的登记写者已落地：`0x2bf` → `op_2bf`（活动 ← 1 / 起点 ← 0 / 时长 / 交值 / 待办 ← 1，
    //   通道 ≥ 10 一格都不写）。这里的 setter 只给守卫摆"交付本身"的前置（如符号性用例的未来起点）。
    void set_se_channel_for_test(std::size_t i, bool active, std::uint32_t start, std::int32_t duration,
                                 std::int32_t value) {
        if (i >= se_channels_.size()) return;
        se_channels_[i] = SeChannel{active, start, duration, value};
    }
    void set_se_work_pending_for_test(bool value) { se_work_pending_ = value; }
    void set_voice_slot_for_test(std::size_t i, std::int32_t state, std::uint32_t start,
                                 std::int32_t duration, bool pending) {
        if (i >= voice_slot_timers_.size()) return;
        voice_slot_timers_[i] = VoiceSlotTimer{state, start, duration, pending};
    }
    void set_voice_work_pending_for_test(bool value) { voice_work_pending_ = value; }
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
    // ── 「声音后端存在」的接缝（`60-ops-voice` 的 `0xb4` / `0xb6` / `0xba` 共用的同一个判断）──
    // ★ 后端不存在（`T_SE+0x408` 为空）时 `0xb6` **一格都不清**、`0xb4` 整段跳过（`0x4124bc` 一族）。
    //   本模型没有真的声音后端 ⇒ 缺省 = **不存在**；接上它才走"发请求 + 清格"那一支。
    void set_se_backend_attached(bool value) { se_backend_attached_ = value; }
    bool se_backend_attached() const { return se_backend_attached_; }
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
    void set_command_animating(bool value) {
        command_animating_ = value;
        // ★ 命令自己的计时没到终点（延迟内 / 推进中 / 进度没走完）⇒ **同一步**也置
        //   「场景待处理标记」（`60-ops-scene` 的「命令档的前置与出档」：同一步里这些推进点
        //   同时把两个标记置起）。少了这一句，收尾判据换到待处理标记之后，还在推进的命令
        //   会在同一帧被清表（命令档就再也走不到第二帧）。
        if (value) scene_pending_ = true;
    }
    std::uint32_t render_now_ms() const { return render_now_ms_; }
    void begin_render() { render_now_ms_ = scene_clock_ms_; command_animating_ = false; }
    void end_render() {
        scene_dirty_ = false;  // 引擎在渲染步入口清脏（0x4B40AA）
        // ★ "待处理"闩（`+0B5B4`）与脏位是**同一条指令对**（0x4B40AA / 0x4B40B0），但它必须在
        //   "推进窗"之前清、由推进重新置起 —— 那一段在 `advance_scene_windows` 里（它在渲染步的
        //   更早子步、不管宿主呈不呈现都跑）。这里不重复清。
        // ★ 收尾判据是**场景待处理标记**（`60-ops-scene` 的「命令档的前置与出档」：为 0 ⇒ 把
        //   **整张**命令表清空）—— ⛔ 不是「有动画」，也不是命令档自己的动画格。那是两个不同的
        //   标记：同处清 0，但"有没有东西在动"只有待处理标记回答。
        if (!scene_pending_) clear_scene_commands();
        // ★ **随后无条件**把「强制完成」清 0（不管本步有没有人读过它）：它只活一个渲染步；
        //   命令档每帧都会重新置位。⛔ 不许写成跨步粘住、也不许写成"读一次就清"。
        forced_complete_ = false;
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
    // `0x0d9` 清的第二个字（泵期间对外有效的那份状态字）见上面的 `status_restore()` ——
    // ★ 它**不是**本条私有的格：泵在挂起时整字存、收场时并回（`CORE-OPS-RUN-BRACKET`）。
    // 模式族的七个格（`CORE-OPS-MODE-BIT`；代号见规范，地址对应未取证）。
    // ★ 身份（本批统一）：`mode_c_` = 规范字母表的 **C**（也是 `0x19a` 原样读出的那一格 /「格 X」）、
    //   `mode_e_` = **E**（**就是 `0x1cc` 读的「状态格」**，⛔ 不是另一格）、`mode_d_` / `mode_f_` /
    //   `mode_g_` = D / F / G。`0x88` 一次写 C 与 D。
    std::int32_t mode_a() const { return mode_a_; }
    std::int32_t mode_b() const { return mode_b_; }
    std::int32_t mode_c() const { return mode_c_; }
    std::int32_t mode_d() const { return mode_d_; }
    std::int32_t mode_e() const { return mode_e_; }
    std::int32_t mode_f() const { return mode_f_; }
    std::int32_t mode_g() const { return mode_g_; }
    // 逐格揭示当前文本族（`CORE-OPS-TEXT-CELL-REVEAL-1CE`）：开关格（只写不读）/ 当前格号 / 格数 /
    // 节拍器相位起点·周期·计数 / 两个请求计数（画一格 / 释放排版；请求的参数与像素不在本条）。
    std::int32_t reveal_switch() const { return reveal_switch_; }
    std::int32_t reveal_cursor() const { return reveal_cursor_; }
    std::int32_t reveal_count() const { return reveal_count_; }
    std::uint32_t reveal_tick_start_ms() const { return reveal_tick_start_ms_; }
    std::uint32_t reveal_tick_period_ms() const { return reveal_tick_period_ms_; }
    std::uint32_t reveal_tick_counter() const { return reveal_tick_counter_; }
    // ★ 「最近一次画格请求的格号」（推进**之前**的值；−1 = 还没画过）。守卫观察口，⛔ 不是产品接口。
    std::int32_t text_cell_last_draw_cursor() const { return text_cell_last_draw_cursor_; }
    std::size_t text_cell_draw_requests() const { return text_cell_draw_requests_; }
    std::size_t text_layout_release_requests() const { return text_layout_release_requests_; }
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
    // 两个互不相同的格（⛔ 不要合并）：`0x1b6`+`0x1b7` 的 **Y** 格 / `0x19a` 读的那一格（= 模式族的 **C**）。
    // ★ `0x1cc` 的「状态格」不是第三个独立格 —— 它就是模式族的 **E**（`mode_e()`）。
    std::int32_t flag_y() const { return flag_y_; }
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
    // ★ 测试脚手架（⛔ 不是平台边界）：E / F 两格由引擎里本版**未建模的消息处理**写，
    //   没有脚本可达的写者 ⇒ 想让 `0x19c` 的 E / F 支被测到，只能由测试播种。
    //   C **不再播种**：它是 `0x88` 的产品落点，守卫走 `0x88 ⇒ 0x19c` 的真实路径。
    void seed_mode_cells(std::int32_t mode_e, std::int32_t mode_f) {
        mode_e_ = mode_e;
        mode_f_ = mode_f;
    }
    // ★ 测试脚手架（⛔ 不是平台边界）：格数由排版侧写入，而本模型的排版侧没有"格数快照"这条写路径的
    //   全部前置（`0x72` 尾段能写它，纯 selftest 里要先把字符格追加出来）⇒ 想让取模推进被测到，只能播种。
    void seed_reveal_cells(std::int32_t cursor, std::int32_t count) {
        reveal_cursor_ = cursor;
        reveal_count_ = count;
    }
    // ★ 测试脚手架（⛔ 不是平台边界）：节拍长度**不是 `0x1ce` 的写点**（本条规定"本条不复位它"）。
    //   要让"不复位"这条成为**会红的断言**（而不是读一次初值 500 的恒真断言），就得先把它播种成别的值。
    void seed_reveal_tick_period_ms(std::uint32_t period) { reveal_tick_period_ms_ = period; }
    // ★ 测试脚手架（测试后门，⛔ 不是平台边界）：只**置位**、**不清位** —— 调用方给的是"要拉起来的位"，
    //   返回值是不可观察的内部字，所以这里不许做整字覆盖（那会让测试能伪造门字的任意取值）。
    //   调用点（改签名时逐处核对）：`tests/core_selftest.cpp` 的 `check_text_cell_reveal`（置第 20 位）。
    void set_state_flags_for_test(std::uint32_t bits) { state_flags_ |= bits; }
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
    // **门服务链的一个位**（`CORE-VM-GATE` / `CORE-VM-STATE`）：执行该位的服务体，返回它的收场。
    // ★ 位序由 `kGateServiceOrder`（vm.cpp，spec 的位序表）决定，⛔ 不是位号升序。
    GateAction service_gate_bit(std::int32_t bit);
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
    // 裸 32 位字访问。★ 字符串族（5 / 11）按「记录内 4 字节裸写」处理：记录步长 28 字节，
    //   内部字段布局属机器本体 ⇒ 只登记"哪些 4 字节字被写进了哪些地址"（`string_record_words_`），
    //   与文本池互不影响；未写过的字**响亮失败**（⛔ 不伪造 0）。
    void store_raw(std::uint32_t address, std::uint32_t bits);
    std::uint32_t read_raw(std::uint32_t address);
    // 地址是不是字符串族池里的一个 4 字节字（记录内偏移 0 / 4 / … / 24）。
    bool is_string_record_word(std::uint32_t address) const;
    // 把 `string_record_words_` 里落在 [base, base + span) 的字一起清掉（池释放 / 帧重装用）：
    // 裸字叠层与文本池**同寿命**，⛔ 不让释放过的记录里的位模式活到下一次装载。
    void clear_string_record_words(std::uint32_t base, std::uint32_t span);
    // 数组写区间的**实现侧边界**（`RES-ARRAY-FILL-ZERO-OOB` / `RES-ARRAY-SET-TO-OOB` /
    // `RES-ARRAY-COPY-OOB` 的 impact：「实现必须拒绝或先记诊断再拒绝（不许静默写坏相邻池）」）：
    // 目标首必须在某个池窗口里，且 count 个 4 字节格必须**整段留在同一个窗口**内。
    // ★ 每条数组自己的容量在本模型里没有数据（指令也只给地址、不给容量；引擎结构上同样无从比较）
    //   ⇒ 能机械判的只有"跨出池窗口"这一档；跨窗即写出相邻族，本模型选择**响亮失败 + 诊断**。
    //   ⛔ 不夹紧、不截断 count（spec 的行为条款是"引擎照样写满 count 格"；这里拒的是 UB 落点）。
    //   ★ `count` 按**无符号格数**传入（0 = 一格都不写）：0x64 的 count 来自块首的 u32，
    //     转成有符号会在大值上回绕成负数、把"越界"误判成"不写"。
    void check_array_write_range(std::uint32_t target, std::uint32_t count) const;
    // 地址所在的池窗口（起点 + 跨度）。字符串族记录内的 4 字节偏移也算（对齐只要求 4 字节）。
    bool pool_window_of(std::uint32_t address, std::uint32_t& base, std::uint32_t& span) const;
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
    // 启动时登记的引擎重置钩子（`CORE-OPS-FRAME-EXIT-SCRIPT`）：当前帧的推进格数 ← 1、
    // 清状态选择字的第 9 位；此外它复位**它自己的一个子系统**（子系统身份与内部效果不在本条，
    // ⛔ 本模型不造空函数冒充，见 `op_09` 的注释与 S7 报告）。
    void engine_reset_hook();
    // `0x1fa` 与 `0x9` 的 1000 槽循环共用：逐槽释放**一个**纹理槽 —— 记录保留、
    // 只清来源文件与槽对象；释放守卫非 0 ⇒ 什么都不做（`sub_49E980` 的 `[this+id*4+0B670h]` 判据）。
    void release_texture_slot(std::uint32_t id);
    std::uint32_t script_int_key_index(const Operand& operand);
    // 选项窗口（`0x91`…`0x94`）的内部原语。
    void reset_choice_table();
    std::int32_t choice_hit_test() const;
    void jump_to(std::uint32_t position);
    void push_return(std::uint32_t position);
    // 帧装载器（`CORE-OPS-FRAME-CALL-SCRIPT` / `-LOAD-FRAME` / `-EXIT-SCRIPT` / 泵正命令**共用**
    // 同一条装载路径）：返回栈栈顶 ← 0（数组内容不写）、两个本地块进入目标 ← −1、
    // 指令指针 ← 代码区起点、长度槽 ← 0、父帧号 ← 「父帧候选」格。⛔ 各条不许再自己拼一份形状。
    void apply_frame_record(std::size_t slot, const Bytes& script, const ScriptHeader& header,
                            std::uint32_t file_id);
    void install_script_into(std::size_t slot, std::uint32_t file_id);
    /** 文件库装载登记（`0x455948`）：按**装载文件对象自带的号**（= 统一文件 id 的高 8 位=区号）填槽。
     *  每个 `read_install_file` 装载点都要调它 —— 文件库析构见 `release_file_library()`。 */
    void register_loaded_file(std::uint32_t file_id);
    // 引擎级状态字保存栈的压入（`0x4116f2`）：压入时把第 31 / 20 位清 0（`CORE-OPS-FRAME-RET`）。
    void push_engine_state_word(std::uint32_t word);
    // ── 待处理命令队列 / 泵（`CORE-OPS-RUN-BRACKET`）──
    /** 只写队列尾（不带"顺手泵"尾巴）：产品投递见 `enqueue_command`，测试接缝见下一个入口。 */
    void push_command(std::int32_t command);
    /** 投递动作：把一条命令放进队列尾；**每次投递还带一个"顺手泵一次"的尾巴**（`0x143` 第 3 步）——
     *  泵被**队列泵占用标记**挡下时一次也不泵（那也是 `0x143` 借用这个标记的用处）。 */
    void enqueue_command(std::int32_t command);
    /** 泵入口：两判按序（区间忙 ≠ 0 ⇒ 返回；队列头 ≥ 队列尾 ⇒ 返回），否则**只取一条**。
     *  取走后按命令的三态处置（`0` / 正 = 统一文件 id 装帧 37 / 负 = 目标帧号取负）。
     *  ⛔ 一趟只取一条：链式再泵只发生在泵帧收场那一步（`0x2` 的 −10 支第 ⑤ 步）。 */
    void pump_command_queue();
    /** 迭代头的宿主消息泵节流（`CORE-VM-LOOP`）：计数器与常量 4 按位与 ⇒ 本迭代泵一次 / 不泵。
     *  只落计数序列与 `should_pump_host()`；宿主实际怎么泵仍由平台路径承担。 */
    void advance_host_pump();
    /** 渲染步推进的本体（`CORE-TIMING-WINDOW`）：清「有动画」/「场景待处理标记」→ 逐项盖章 / 插值 /
     *  到期。只有 `advance_render_step()`（门前的渲染步）与 `advance_scene()`（测试直接入口）会调它；
     *  "跳过这一迭代"时两个标记**不重算**（停在上一迭代的值）。 */
    void advance_scene_windows();
    // 状态字第 26 位的服务体（`CORE-OPS-FRAME-LOCAL-RET-7C`）：本地块内的输入等待。
    void service_local_input_wait();

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
    // 音效通道登记（`70-vm`「音效通道」的登记段；0x2bf）
    void op_2bf(const Instruction&);
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
    // 平台输入路径：把宿主按钮源的上升沿**锁存**一次（事件锁存，不是电平读）。每迭代采样，
    // 与门无关（门上的第 31 位服务体只**消费**这个锁存）。
    void sample_advance_input();
    // 第 31 位的服务体（服务后 = 交回迭代头）：等推进位置起时，消费锁存 / 读跳过 ⇒ 清第 31 + 30 位。
    void service_advance_wait();
    // 第 27 位的服务体（位序表第 16 行；服务后 = 交回迭代头）：一次消息处理（读一条并派发）→
    // 让出一拍 → 窥视一次。**不自己清第 27 位**（清位归消息处理 / 脚本指令）。
    void service_message_frame();
    // 「读文本跳过」的判据（`0x305` ② 与 `0x6e` / `0x196` 的追加前置支共用）：
    // `message:ReadTextSkip` 非 0 ∧ 已读命中。★ 已读表未建模（残差 `RES-MSG-BLOCK-READ-HIT`）。
    bool read_text_skip_requested() const;
    void service_auto_message();
    // `0x21c` 谓词 A：消息子系统"还有事"（定时器在窗内 **或** 待处理）。谓词 B（影片对象扫描）另算。
    bool message_subsystem_busy();
    bool movie_gate_busy() const;
    void service_choice_click();
    void op_2f7(const Instruction&); void op_304(const Instruction&); void op_305(const Instruction&);
    // 第 9 位的服务体（位序表第 4 行）：BGM 淡变 / 压低恢复的逐拍推进；服务后 = 继续扫描（落到第 12 位）。
    // 规范没规定这一位的内部动作 ⇒ 见实现里的残差说明（`RES-AUDIO-BGM-FADE-SERVICE`）。
    void service_bgm_fade();
    // 第 14 位的服务体（位序表第 3 行）：前置齐 ⇒ 清第 14 位、置第 9 位并起计时；
    // 不成立 ⇒ 第 14 位**保留**。服务后 = 继续扫描（落到第 9 位）。
    void service_bgm_duck();
    // 第 30 位的服务体（位序表第 8 行；`CORE-OPS-TEXT-CELL-REVEAL-1CE`）：活动位置起才走；
    // 够一拍 ⇒ 只请求画一格，然后格号 ← (格号+1) mod 格数。服务后 = 继续扫描（落到第 13 位）。
    void service_text_cell_reveal();
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
    // 逐格揭示当前文本的启停（`0x1ce`；vm_ops_numeric.cpp。⛔ 不是"乱数启用"）
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
    void op_78(const Instruction&); void op_71(const Instruction&); void op_1a5(const Instruction&); void op_2fe(const Instruction&); void op_197(const Instruction&); void op_198(const Instruction&);
    void op_75(const Instruction&);
    // 字体对象的两对描述 + 建字体缩放（vm_ops_text.cpp）
    void write_font_pair(FontDescriptionPair& pair, const std::string& face, std::int32_t size);
    void refresh_font_scale();
    Viewport viewport_input() const;
    void request_font_rebuild() { ++font_rebuild_requests_; }
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
    // ★ 返回值 = 这一趟的**文本槽在用标志**（`[77984h]` 的入口快照）：门上的第 29 位服务体拿它决定
    //   「随即派发一条」还是「交回迭代头」（`CORE-VM-GATE`；取证 `report-text-gate.md` §3.2）。
    bool service_text_layers();
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
    // 「父帧候选」格（装载器照抄）与泵的挂起帧 / 队列泵占用标记（`CORE-OPS-FRAME-LOAD-FRAME` /
    // `CORE-OPS-RUN-BRACKET`）。泵本身是 S3 的单 ⇒ 本片只建这三格与 −10 的收场支。
    std::int32_t parent_candidate_ = -1;
    std::int32_t suspended_frame_ = -1;
    bool pump_busy_ = false;
    // 状态选择字的**待恢复副本**：泵挂起时整字存、泵帧收场时并回（`CORE-OPS-RUN-BRACKET`；
    // `0xd9` 的第 2 步读-改-写它）。★ 泵期间"对外有效"的状态字就是这份副本（读者按标记选）。
    std::uint32_t status_restore_ = 0;
    // 引擎级状态字保存栈（`CORE-OPS-FRAME-RET`）：只由引擎重入脚本的路径压入。
    std::vector<std::uint32_t> engine_state_word_stack_;
    std::uint32_t state_flags_ = 0;
    // 第 27 位服务体的宿主消息接缝 + 文本子系统的两个接缝（缺省全 no-op）。
    HostMessageIo host_message_io_;
    std::size_t host_message_steps_ = 0;
    TextRequestResult text_request_result_;
    ReadTextSkipHit read_text_skip_hit_;
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
    // （`a1`/`a2`/`a3`）「文本键 → 整数」命名表；`a1` 清空、`a2` 写、`a3` 按键分派。
    std::unordered_map<std::string, std::int32_t> named_table_;
    // （`1cf` / `1bf`）状态字与它的相邻格 —— ⛔ 与门字 `state_flags_` 是**不同**的引擎格。
    std::int32_t status_word_ = 0;
    std::int32_t status_neighbor_ = 0;
    // （`0d9`）第 2 步清的字是**待恢复副本**（`status_restore_`，泵的挂起件），不是本条私有格。
    // （`19b` / `19c`）模式族的七个格。
    std::int32_t mode_a_ = 0;
    std::int32_t mode_b_ = 0;
    std::int32_t mode_c_ = 0;
    std::int32_t mode_d_ = 0;
    std::int32_t mode_e_ = 0;
    std::int32_t mode_f_ = 0;
    std::int32_t mode_g_ = 0;
    // （`1ce`）逐格揭示当前文本：开关格（只写不读）/ 当前格号 / 格数 /
    // 节拍器（相位起点 / 周期 / 计数，1 起算）/ 两个请求计数 / **最近一次画格请求的格号**。
    std::int32_t reveal_switch_ = 0;
    std::int32_t reveal_cursor_ = 0;
    std::int32_t reveal_count_ = 0;
    std::uint32_t reveal_tick_start_ms_ = 0;
    std::uint32_t reveal_tick_period_ms_ = 500;  // 初始化写死的默认周期（`CORE-OPS-TEXT-CELL-REVEAL-1CE`）
    std::uint32_t reveal_tick_counter_ = 1;
    std::size_t text_cell_draw_requests_ = 0;
    // ★ 「最近一次画格请求的格号」= 请求发出**那一刻**的当前格号（推进**之前**的值；−1 = 还没画过）。
    //   它把"画的是推进前的格号"这条合同变成**可观察量**：⛔ 它不是产品字段，只是守卫的观察口。
    std::int32_t text_cell_last_draw_cursor_ = -1;
    std::size_t text_layout_release_requests_ = 0;
    // 本地块的存档（`0x199` 整字搬 + 置第 25/26 位；`0x7c` 读/清）。
    std::uint32_t local_save_word_ = 0;
    std::uint32_t local_save_pos_ = 0;
    std::int32_t local_save_depth_ = 0;
    // ★ `0x7c` 尾段还清四对"从未写入有效登记"的格与一个回调派发闩 —— 那些在本仓的
    //   `audit` 里已判 **discarded**（`RES-LOCAL-RET-SLOTS` / `RES-LOCAL-RET-PENDING`：
    //   语料里没有有效写者，清 0 不可达）⇒ **不建模**，也不再在 `0x7c` 上写它们。
    // 两个互不相同的格（`0x19a` 读的 C / `0x1b6`+`0x1b7` 的 Y）；`0x1cc` 的状态格 = `mode_e_`。
    std::int32_t flag_y_ = 0;
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
    // 两对字体描述（每组两份变体：原样 / '@' + 原样）。`0x75` / `0x1a5` 写 A 对、`0x197` / `0x2fe` 写 B 对。
    FontDescriptionPair font_a_pair_{};
    FontDescriptionPair font_b_pair_{};
    // 建字体缩放的两个分量（`Font+0x355E0` 宽 / `Font+0x355E4` 高；构造器把两者写成 1.0）。
    // 高分量乘进 lfHeight；宽分量**不参与**字号缩放，只与高分量一起抄进十个文本槽的两格。
    double font_scale_width_ = 1.0;
    double font_scale_height_ = 1.0;
    // 守卫观察口（⛔ 不是产品状态）：整趟重算写了几次 / 发了几次重建请求。
    std::size_t font_scale_write_count_ = 0;
    std::size_t font_rebuild_requests_ = 0;
    // 平台输入接缝：空 = 视口按配置的客户区尺寸（比例 1.0）。
    ViewportSource viewport_source_;
    struct TextSlot {
        std::int32_t glyph_height = 0;  // `槽+0xC8`：`0x197` 把**十槽全写**成同一个值
        std::int32_t x_offset = 0;      // `槽+0x0C`：`0x198` 的 op2
        std::int32_t y_offset = 0;      // `槽+0x10`：`0x198` 的 op3
        // 缩放两格（`槽+0x74` = 宽分量 / `槽+0x78` = 高分量；`sub_465840` 每次重算整表抄一遍）。
        // 初值取 1.0（与两个缩放分量的初值同口径；引擎构造器里十槽的这两格初值本版未单独取证）。
        double scale_width = 1.0;
        double scale_height = 1.0;
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
    // 启动时登记的引擎重置钩子（`CORE-OPS-FRAME-EXIT-SCRIPT`）：登记发生在构造期，
    // `0x9` 只调**已登记**的钩子。两类计数只给守卫（⛔ 不是产品状态）。
    bool engine_reset_hook_registered_ = false;
    std::size_t engine_reset_hook_calls_ = 0;
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
    // 声音后端存在与否（`0xb6` 的「后端不存在 ⇒ 一格都不清」）。缺省 false = 没有后端。
    bool se_backend_attached_ = false;
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
    // `0x2f5`（配音排队）写的那五格：延时 ms / 资源号（**全局一格**）/ 排队标记 / 起算 / 该槽状态字。
    // ★ 其中三格是**该槽**的（时间交付按槽推进）⇒ 记录形状见 `VoiceSlotTimer`。
    std::int32_t voice_resource_ = 0;
    std::array<VoiceSlotTimer, 3> voice_slot_timers_{};
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
    // ── 时间交付的引擎状态（`CORE-VM-LOOP`）──
    // 音效通道表 10 格 + 两个子系统各自的「仍有一件未完成的工作」标记。
    std::array<SeChannel, kSeChannelCount> se_channels_{};
    bool se_work_pending_ = false;
    std::size_t se_release_steps_ = 0;          // 交给释放入口的次数（子系统内部步骤，⛔ 不是宿主请求）
    std::int32_t se_last_released_value_ = 0;   // 最近一次交给释放入口的值（观察口）
    bool voice_work_pending_ = false;
    std::size_t voice_advance_steps_ = 0;       // 「有推进」的次数（= 交付返回值 true 的次数）
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
    // 引擎 `+0B5B4`（"场景还有待处理的东西"闩 = 场景待处理标记）：排窗时置、窗推进时置、
    // 命令进度还在走时置；**渲染步的推进子步**（`advance_scene_windows`）清 0 再重算。
    // ★ 读者两个：`message_subsystem_busy()`（`0x21c` 谓词 A）与本模型的**渲染步收尾**
    //   （`end_render`：`+0B5B4` = 0 ⇒ 清空命令表）—— 收尾判的是它，⛔ 不是「有动画」。
    bool scene_pending_ = false;
    bool forced_complete_ = false;
    bool frame_step_requested_ = false;
    bool frame_flag_ = false;
    bool scene_dirty_ = true;
    std::int32_t draw_mode_ = 1;
    // ── 渲染步门槛的四个输入（`CORE-TIMING-WINDOW` 的跳过点分类）──
    // ★ 注：现在是**五个**输入 —— (d) 执行区间的暂停复用 `driven_elsewhere_`（区间忙标记），
    //   不另存一格。
    // (b) **画面暂停闩**：由**窗口消息**置 / 清的引擎格，与执行区间的区间忙标记**不是同一格**。
    //     消息号与初值未取证（残差 `RES-VM-SCENE-HOLD-LATCH`）⇒ 这里只留一格 + 宿主 / 测试注入口，
    //     缺省 0（不挡）。⛔ 不伪造消息号、不接平台消息。
    std::int32_t scene_hold_latch_ = 0;
    // 「只挡推进」第 2 条：**有影片对象正被另一处持有**（引擎级布尔量，来源不属本条 ——
    // 残差 `RES-WINDOW-MOVIE-HELD`）⇒ 缺省 false + 注入口。
    bool movie_held_ = false;
    // 「只挡推进」第 3 条的影片半边：**有待消费的新帧**（**读一次即消费**）。生产者在门前的
    // 影片对象表服务（后端解出新帧 ⇒ 置），消费者是渲染步的门槛。
    bool movie_new_frame_ = false;
    // 「只挡推进」第 4 条：渲染步内部的**就绪判据**（返回 0 = 未就绪）。判据体未成篇、无 EA
    // ⇒ 缺省 true（就绪）+ 注入口；⛔ 不实现内部判据。
    bool render_ready_ = true;
    bool driven_elsewhere_ = false;
    // （`1f4` / `1f5`）执行区间的嵌套计数；忙标记复用 `driven_elsewhere_`（同一格）。
    std::int32_t bracket_depth_ = 0;
    // ── 待处理命令队列 + 泵（`CORE-OPS-RUN-BRACKET` / `CORE-OPS-EVENT-143`）──
    // 队列表 256 格（与 `T_Command` 里嵌入的那条队列同形；引擎级）。`queue_head_` / `queue_tail_`
    // 是**线性下标**（不是回绕计数器）：判空口径照 spec 是「队列头 ≥ 队列尾」，取一条只前进头。
    // 占用标记不另存一格：`queue_occupied()`（头 < 尾）就是泵读的**同一状态**。
    // ★ 门上的第 29 位**不读它**：那一位的判据是「文本槽在用标志」（`text_slot_in_use()`），
    //   与队列无关（取证 `report-text-gate.md` §1.3 / §5.3）。⛔ 别把两者当成同一格。
    static constexpr std::size_t kCommandQueueCapacity = 256;
    std::array<std::int32_t, kCommandQueueCapacity> command_queue_{};
    std::size_t queue_head_ = 0;
    std::size_t queue_tail_ = 0;
    std::size_t queue_high_water_ = 0;
    // 文件库的 256 格槽位表（`CORE-OPS-EVENT-143`）：格 0 存在但 `0x143` 不检查它，只是让下标对上格号。
    // 登记 / 析构见 public 的 `kFileLibrarySlotCount` / `release_file_library()`。
    std::array<std::int32_t, kFileLibrarySlotCount> file_library_slots_{};
    // 宿主的消息泵节流（`CORE-VM-LOOP`）：计数器初值 1；本迭代是否该泵一次 = 迭代头算出的结果。
    std::uint32_t pump_counter_ = 1;
    bool host_pump_due_ = false;
    std::int32_t mouse_interval_ = 1;
    // 鼠标回调的登记状态（引擎级一格；`0xcc` 写、`0xcd` 读）。
    std::int32_t mouse_target_ = -1;   // 登记的脚本位置（dword 下标）；−1 = 取消
    std::int32_t mouse_owner_ = -1;    // 登记时的当前帧脚本身份（frame.local_depth；见 CORE-INPUT-CALLBACK-REGISTER）
    std::uint32_t mouse_last_ms_ = 0;  // 上一次派发时刻（登记时 = 当前时刻）
    // 键绑定三张表（`CORE-OPS-SCALAR-TABLE-10C`）：
    //   ① 与 0x10b 共用 `array_d1383_`（键号 → 动作号；0x10c 不碰它）。
    //   ② / ③ 是本模型新增的两张表：动作号 → 槽号（0x10c 只读）、槽号 → 键号（0x10c 写；−1 = 未绑定）。
    // ★ ② / ③ 各 **256 格**：引擎里两张表在同一对象里首尾相接，基址差 `0x1A68 − 0x1668 = 0x400`
    //   = 256 × 4（EA 0x4220eb / 0x4220f3）。⚠ 早先按 32 格建 ⇒ 真实脚本的动作号 ≥ 32 时
    //   MSVC Debug STL 的 `std::array::operator[]` 会断言（模态对话框、CTest 挂死）。
    // ★ 索引**完全不检查**（越界是引擎的未定义行为，spec 不作语义断言）⇒ ⛔ 不要给下标加夹取。
    static constexpr std::size_t kKeyBindingTableSize = 256;
    std::array<std::int32_t, kKeyBindingTableSize> action_slot_table_{};
    std::array<std::int32_t, kKeyBindingTableSize> slot_key_table_{};
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
    // 错误通道的宿主侧接缝：缺省 = 没有输出句柄（所有平台写请求都被丢弃）。
    ErrorOutputWriter error_writer_;
    ErrorOutputFilter error_filter_;
    bool error_handle_valid_ = false;
    std::size_t error_write_requests_ = 0;
    // 输出对象（`CORE-OBJECTS-BASE`）的宿主侧接缝与「报告」计数。缺省 = 没有日志落点 / 根错误处理
    // （平台是否受理属平台侧 ⇒ 缺省 no-op 不改变引擎状态）。
    OutputObjectIo output_object_io_;
    std::size_t output_reports_ = 0;     // 走过「报告」这一项的次数
    // 显示消息型异常的最近一次捕获（主循环写；观察口只读）。
    std::string last_display_code_;
    std::string last_display_message_;
    // 字符串记录的**裸字叠层**（`store_raw` / `read_raw` 的 5 / 11 支）：键 = 绝对地址。
    // ★ 记录内部布局属机器本体（40-pools「字符串记录」明文：字段布局不在本版）⇒ 这里只登记
    //   "哪些 4 字节字被写进了哪些地址"，与文本池 `global_string_` / `local_string_` 互不影响。
    std::unordered_map<std::uint32_t, std::uint32_t> string_record_words_;
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
