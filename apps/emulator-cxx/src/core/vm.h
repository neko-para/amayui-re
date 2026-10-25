#pragma once

#include "core/bytes.h"
#include "core/config.h"
#include "core/opcode_table.h"
#include "core/script.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

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
};

enum class StopReason {
    Running,
    Halted,
    UnsupportedOpcode,
    BadOpcode,
    StepLimit,
    StoppedAtOpcode,
    ScriptReached,
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
};

struct SceneRecord {
    float scale[3]{1.0f, 1.0f, 1.0f};
    float translate[3]{};
    float rotate_axis[3]{};
    float rotate_degrees = 0.0f;
    std::uint32_t color = 0xFFFFFFFFu;
};

struct MovieRecord {
    bool present = false;
    bool started = false;
    std::uint32_t file_id = 0;
    std::uint32_t options = 0;
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
    std::int32_t input_a() const { return input_a_; }
    std::int32_t input_b() const { return input_b_; }
    std::uint32_t input_mode() const { return input_mode_; }
    std::uint32_t scene_clock_ms() const { return scene_clock_ms_; }
    std::uint32_t scene_previous_ms() const { return scene_previous_ms_; }
    std::uint32_t engine_now_ms() const { return engine_now_ms_; }
    std::uint32_t engine_previous_ms() const { return engine_previous_ms_; }
    void set_driven_elsewhere(bool value) { driven_elsewhere_ = value; }
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
    void advance_scene(std::uint32_t now_ms);
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
    const MeshRecord* mesh(std::uint32_t key) const { const auto it = meshes_.find(key); return it == meshes_.end() ? nullptr : &it->second; }
    const TextureRecord* texture(std::uint32_t id) const {
        const auto it = textures_.find(id);
        return it == textures_.end() ? nullptr : &it->second;
    }
    std::int32_t forward_container_at(std::uint32_t index) const {
        const auto it = forward_container_.find(index);
        return it == forward_container_.end() ? 0 : it->second;
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
    std::uint32_t address_of(const Operand& operand) const;
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
    std::uint32_t script_int_key_index(const Operand& operand);
    void jump_to(std::uint32_t position);
    void push_return(std::uint32_t position);
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
    std::int32_t key_total_ = 0;
    std::int32_t save_version_flag_ = 0;
    std::int32_t input_a_ = 0;
    std::int32_t input_b_ = 0;
    std::uint32_t input_word_ = 0;
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
    std::int32_t mouse_interval_ = 0;
    std::int32_t mouse_position_ = -1;
    std::unordered_map<std::int32_t, std::int32_t> indirect_;
    std::unordered_map<std::uint32_t, TextureRecord> textures_;
    std::unordered_map<std::uint32_t, DrawItem> draw_items_;
    std::unordered_map<std::uint32_t, MeshRecord> meshes_;
    std::unordered_map<std::uint32_t, MovieRecord> movies_;
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
