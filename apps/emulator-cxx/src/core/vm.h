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
    std::vector<std::int32_t> ret_stack;
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
    float scale[3]{1.0f, 1.0f, 1.0f};
    float translate[3]{};
    float rotate_axis[3]{};
    float rotate_degrees = 0.0f;
    std::uint32_t color = 0xFFFFFFFFu;
    // 两个三元向量（`0x218` / `0x21a` 读；`0x217` / `0x219` 写，后者本版未实现）。
    float vec1[3]{};
    float vec2[3]{};
};

// 影片对象（对象表的一格）：引擎状态 + 播放后端句柄（平台边界，核心不解释其形状）。
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

struct TextureRecord {
    std::int32_t file = -1;
    std::uint32_t color_key = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool blank = false;
    bool present = false;
    // 「内容非文件装载」标志（`0x1f8` 空白创建 / `0x20b` 填充共用；全语料无读点）。
    bool non_file = false;
    // 像素内容版本：宿主用它判断是否需要重传 GPU 纹理（不是引擎字段）。
    std::uint32_t revision = 0;
    Bytes rgba;  // top-down RGBA8；空白纹理为透明
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
    std::int32_t scalar_252() const { return scalar_252_; }
    std::int32_t scalar_88a() const { return scalar_88a_; }
    std::int32_t scalar_88b() const { return scalar_88b_; }
    std::int32_t scalar_88c() const { return scalar_88c_; }
    std::int32_t named_message_read_text_skip() const { return named_message_read_text_skip_; }
    const ListRecord& list_record(std::size_t group) const { return list_records_[group]; }
    std::int32_t default_group() const { return default_group_; }
    std::int32_t scalar_d107656() const { return scalar_d107656_; }
    std::int32_t font_draw_mode() const { return font_draw_mode_; }
    std::int32_t font_align_mode() const { return font_align_mode_; }
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
    std::int32_t font_size() const { return font_size_; }
    const std::string& font_string() const { return font_string_; }
    std::int32_t engine5(std::size_t i) const { return engine5_[i]; }
    std::int32_t key_total() const { return key_total_; }
    // 引擎配置库（保存 / 脚本配置读写共用）。
    const ConfigStore& config() const { return config_; }
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
    const std::unordered_map<std::uint32_t, TextureRecord>& textures() const { return textures_; }
    // 测试 / 诊断注入点：直接布置纹理记录的来源文件；⛔ 产品路径不要靠它伪造状态。
    void set_texture_file(std::uint32_t slot, std::int32_t file) {
        textures_[slot].file = file;
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
    const std::unordered_map<std::uint32_t, MeshRecord>& meshes() const { return meshes_; }
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
    TextureRecord* texture_mut(std::uint32_t id) {
        const auto it = textures_.find(id);
        return it == textures_.end() ? nullptr : &it->second;
    }
    void report_error(std::string text) { errors_.push_back(std::move(text)); }
    const std::vector<std::string>& errors() const { return errors_; }
    void clear_errors() { errors_.clear(); }
    bool command_animating() const { return command_animating_; }
    void set_command_animating(bool value) { command_animating_ = value; }
    std::uint32_t render_now_ms() const { return render_now_ms_; }
    void begin_render() { render_now_ms_ = scene_clock_ms_; command_animating_ = false; }
    void end_render() {
        scene_dirty_ = false;  // 引擎在渲染步入口清脏（0x4B40AA）
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
    const TextureRecord* texture(std::uint32_t id) const {
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
    void op_23c(const Instruction&);
    // 纹理 / 绘制项 / 网格（vm_ops_texture.cpp）
    void op_344(const Instruction&); void op_1ff(const Instruction&); void op_1fd(const Instruction&); void op_1fe(const Instruction&);
    void op_1f6(const Instruction&); void op_216(const Instruction&); void op_320(const Instruction&); void op_322(const Instruction&); void op_323(const Instruction&); void op_23d(const Instruction&); void op_32b(const Instruction&);
    void op_1fb(const Instruction&); void op_239(const Instruction&); void op_202(const Instruction&); void op_203(const Instruction&);
    void op_1f7(const Instruction&); void op_1f8(const Instruction&); void op_1fa(const Instruction&); void op_1f9(const Instruction&);
    // 纹理尺寸查询（vm_ops_texture.cpp）
    void op_208(const Instruction&);
    // 3D 记录取回（vm_ops_texture.cpp）：两个三元向量 + 平移分量
    void op_218(const Instruction&); void op_21a(const Instruction&); void op_228(const Instruction&);
    // 记录复制（vm_ops_texture.cpp）
    void op_21d(const Instruction&);
    // 3D setter 族（vm_ops_texture.cpp）
    void op_21e(const Instruction&); void op_21f(const Instruction&);
    // 影片（vm_movie.cpp）
    void op_20f(const Instruction&);
    // 场景命令记录（vm_ops_scene.cpp）
    void op_223(const Instruction&); void op_24d(const Instruction&); void op_24f(const Instruction&); void op_250(const Instruction&);
    void op_251(const Instruction&);
    // 引擎级状态与宿主请求（vm_ops_engine.cpp）
    void op_88(const Instruction&); void op_308(const Instruction&); void op_bf(const Instruction&); void op_b4(const Instruction&);
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
    void op_7f(const Instruction&); void op_60(const Instruction&); void op_63(const Instruction&);
    // 字体与文本（vm_ops_text.cpp）
    void op_2ec(const Instruction&); void op_2eb(const Instruction&); void op_1a4(const Instruction&); void op_76(const Instruction&);
    void op_2bd(const Instruction&); void op_2de(const Instruction&); void op_192(const Instruction&); void op_1a9(const Instruction&);
    void op_1aa(const Instruction&); void op_1a3(const Instruction&); void op_1a2(const Instruction&); void op_2db(const Instruction&);
    void op_78(const Instruction&); void op_71(const Instruction&); void op_1a5(const Instruction&); void op_197(const Instruction&);
    void op_75(const Instruction&);
    // 输入 / 等待 / 空操作（vm_ops_io.cpp）
    void op_109(const Instruction&); void op_108(const Instruction&); void op_c8(const Instruction&); void op_cc(const Instruction&);
    void op_cd(const Instruction&); void op_fb(const Instruction&); void op_24e(const Instruction&); void op_21c(const Instruction&);
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
    std::int32_t named_message_read_text_skip_ = 0;
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
    std::int32_t scalar_10f_ = 0;
    std::int32_t scalar_248_ = 0;
    std::int32_t font_8b_ = 0;
    std::int32_t font_size_ = 24;
    std::string font_string_;
    std::array<std::int32_t, 5> engine5_{};
    std::int32_t key_total_ = 7;  // 引擎构造（Input[259]=7）建立的初值；0xfe 可覆盖
    std::int32_t save_version_flag_ = 0;
    std::int32_t input_a_ = 0;
    std::int32_t input_b_ = 0;
    std::uint32_t input_word_ = 0;
    // 平台输入采样的注入点（`0xff`）；空 = 没有设备。
    KeySampler key_sampler_;
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
    std::unordered_map<std::uint32_t, TextureRecord> textures_;
    // 1000 槽的释放守卫（默认 0；没有脚本写者）。
    std::array<std::int32_t, 1000> surface_release_gate_{};
    std::unordered_map<std::uint32_t, DrawItem> draw_items_;
    std::unordered_map<std::uint32_t, MeshRecord> meshes_;
    std::unordered_map<std::uint32_t, MovieRecord> movies_;
    MovieBackendFactory movie_factory_;
    MovieBytesLoader movie_bytes_loader_;
    std::unordered_map<std::uint32_t, SceneRecord> scene_records_;
    std::map<std::uint32_t, SceneCommandRecord> scene_commands_;
    std::uint32_t render_now_ms_ = 0;
    bool command_animating_ = false;
    std::vector<std::string> errors_;
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
