// 存档族（`0x19e` / `0x1a0` / `0x1a1` / `0x1ab` / `0x1ac` / `0x1ae` / `0x1af`）。
//
// ★ 三条硬口径（照 spec 正文，不照参考实现）：
//   ① `0x19e` **成功 = 0**（不是 1）；确认只在「目标已存在且头读不回来」时发生。
//   ② `0x1a1` 打开成功后**无条件重编码**全局整型池（计数格 + 1 格），且**成功路径不写 op1**。
//   ③ `0x1af` 的判定顺序是**打开先于表面门**（打不开 ⇒ 1；0/1/2 被拒 ⇒ 2）。
// ★ 宿主 IO 全走 `SaveHost`（核心不碰文件系统）。写侧内容构造与整档读回分别属另外几张单，
//   这里只留 `slot_builder_` / `slot_loader_` 两个 seam；没注入就**响亮**失败。
#include "core/vm.h"
#include "core/save_bmp.h"
#include "core/save_host.h"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace core {

std::string Machine::save_directory() const {
    // `FMT-SAVE-SLOT`：根 [+ 分隔符] + `set:SavePath`；两个分隔符各自只在左侧非空时补。
    std::string root;
    if (config_.get_int("set:UseAppDataFolder") == 1 && save_host_ != nullptr) {
        root = save_host_->app_data_root();  // ★ 本地应用数据目录（不是漫游）
    }
    auto sub = config_.get_string("set:SavePath");
    if (sub.empty()) sub = "SAVE";  // 缺键默认
    if (root.empty()) return sub;
    return root + "\\" + sub;  // ⛔ 不折叠连续分隔符
}

std::string Machine::slot_path(const std::string& dir, std::int32_t slot, bool thumbnail) {
    // `%2.2d`：宽度 2、精度 2 ⇒ 至少两位、零填充、负数带 `-`（0→00、7→07、-1→-01）。
    char name[64];
    std::snprintf(name, sizeof name, "SAVE%2.2d.%s", static_cast<int>(slot), thumbnail ? "STH" : "DAT");
    if (dir.empty()) return name;
    return dir + "\\" + name;
}

void Machine::require_save_host(const char* opcode) const {
    if (save_host_ == nullptr) throw CoreError(std::string("save-host-missing:") + opcode);
}

bool Machine::parse_save_header(ByteSpan bytes, SaveHeader& header) const {
    if (bytes.size() < 292) return false;  // 读不满 292 ⇒ 头不合法
    const std::string expected_magic = save_family_ == "S3" ? "S3SD" : "S4SD";
    const std::string magic(reinterpret_cast<const char*>(bytes.data()), 4);
    if (magic != expected_magic) return false;  // ★ 只接受本族，不学参考实现的宽进
    if (!save_title_.empty() && read_field(bytes, 8, 128) != save_title_) return false;
    const auto u16 = [bytes](std::size_t offset) {
        return static_cast<std::uint32_t>(bytes[offset]) |
               (static_cast<std::uint32_t>(bytes[offset + 1]) << 8);
    };
    header.year = u16(264);
    header.month = u16(266);
    header.day = u16(270);   // +268 是星期（跳过）
    header.hour = u16(272);
    header.minute = u16(274);
    header.second = u16(276);  // +278 是毫秒（跳过）
    header.game_time = read_i32(bytes, 280);
    return true;
}

void Machine::note_global_int_write(std::uint32_t index) {
    // 计数格 = max(index + 1)；bank 键的高位不属于下标。
    const auto raw = index & (kBankKeyStride - 1u);
    if (raw + 1u > global_int_count_) global_int_count_ = raw + 1u;
}

void Machine::recode_global_int_pool() {
    // ★ 对**存储位**再 ENC 一遍（不是 DEC∘ENC）；范围 = 计数格 + 1 格。
    for (std::uint32_t i = 0; i <= global_int_count_; ++i) {
        const auto it = global_int_.find(bank_key(i));
        if (it == global_int_.end()) continue;  // 缺槽读出来就是 0，等价
        it->second = encode_int(static_cast<std::int32_t>(it->second));
    }
    return;
}

void Machine::op_1a0(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    require_save_host("0x1a0");
    const auto slot = read_int(ops[1]);  // 读序：op2
    const auto dir = save_directory();
    save_host_->ensure_directory(dir);
    const ReadResult result = save_host_->read(slot_path(dir, slot, false));
    if (result.outcome == ReadOutcome::OpenFailed) {
        write_int(ops[0], 1);  // 打不开：只写 op1，op3..9 保持原值
        return;
    }
    SaveHeader header{};
    if (result.outcome != ReadOutcome::Ok || !parse_save_header(result.bytes, header)) {
        write_int(ops[0], 2);  // 头不合法：只写 op1
        return;
    }
    write_int(ops[2], static_cast<std::int32_t>(header.year));
    write_int(ops[3], static_cast<std::int32_t>(header.month));
    write_int(ops[4], static_cast<std::int32_t>(header.day));
    write_int(ops[5], static_cast<std::int32_t>(header.hour));
    write_int(ops[6], static_cast<std::int32_t>(header.minute));
    write_int(ops[7], static_cast<std::int32_t>(header.second));
    write_int(ops[8], header.game_time);
    write_int(ops[0], 0);  // op1 最后写
    return;
}

void Machine::op_19e(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    require_save_host("0x19e");
    if (!slot_builder_) throw CoreError("save-codec-missing:0x19e");
    const auto slot = read_int(ops[1]);
    const auto dir = save_directory();
    save_host_->ensure_directory(dir);
    const auto path = slot_path(dir, slot, false);
    const ReadResult probe = save_host_->read(path);
    bool need_confirm = false;
    if (probe.outcome != ReadOutcome::OpenFailed) {
        SaveHeader header{};
        need_confirm = !(probe.outcome == ReadOutcome::Ok && parse_save_header(probe.bytes, header));
    }
    if (need_confirm && !save_host_->confirm_overwrite(slot)) {
        write_int(ops[0], 1);  // 用户不同意覆写
        return;
    }
    switch (save_host_->write_create_always(path, slot_builder_(*this))) {
        case WriteOutcome::Ok:
            write_int(ops[0], 0);  // ★ 成功 = 0
            return;
        case WriteOutcome::OpenFailed:
        case WriteOutcome::WriteFailed:
            report_error("save-write-failed:" + std::to_string(slot));
            write_int(ops[0], 1);
            return;
    }
}

void Machine::op_1a1(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    require_save_host("0x1a1");
    const auto slot = read_int(ops[1]);
    const auto dir = save_directory();
    save_host_->ensure_directory(dir);
    const ReadResult result = save_host_->read(slot_path(dir, slot, false));
    if (result.outcome == ReadOutcome::OpenFailed) {
        write_int(ops[0], 1);  // ★ 唯一写 op1 的支路
        return;
    }
    // 打开成功之后：**成功路径不写 op1**，头坏 / 读失败也不写。
    SaveHeader header{};
    if (result.outcome != ReadOutcome::Ok || !parse_save_header(result.bytes, header)) {
        report_error("save-read-header-invalid:" + std::to_string(slot));
    } else if (slot_loader_) {
        try {
            slot_loader_(*this, result.bytes);
        } catch (const std::exception& error) {
            report_error(std::string("save-load-") + error.what());
        }
    } else {
        report_error("save-restore-unmodeled");  // 响亮：不假装做到了
    }
    recode_global_int_pool();  // ★ 无条件
    return;
}

void Machine::op_1ab(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    require_save_host("0x1ab");
    const auto slot = read_int(ops[1]);
    const auto dir = save_directory();
    save_host_->ensure_directory(dir);
    std::int32_t status = 0;  // 0 两个都成功 / 1 第二个成功而第一个失败 / 2 第二个失败
    if (!save_host_->remove(slot_path(dir, slot, false))) status = 1;
    if (!save_host_->remove(slot_path(dir, slot, true))) status = 2;  // 失败不返回，后一步覆盖前一步
    write_int(ops[0], status);
    return;
}

void Machine::op_1ac(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    require_save_host("0x1ac");
    const auto from = read_int(ops[1]);
    const auto to = read_int(ops[2]);
    const auto dir = save_directory();
    save_host_->ensure_directory(dir);
    std::int32_t status = 0;
    if (!save_host_->copy_overwrite(slot_path(dir, from, false), slot_path(dir, to, false))) status = 1;
    if (!save_host_->copy_overwrite(slot_path(dir, from, true), slot_path(dir, to, true))) status = 2;
    write_int(ops[0], status);
    return;
}

void Machine::op_1ae(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    require_save_host("0x1ae");
    const auto slot = read_int(ops[1]);
    const auto dir = save_directory();
    save_host_->ensure_directory(dir);
    const auto path = slot_path(dir, slot, true);
    // ★ 文件在**捕捉之前**就以截断方式建立（捕捉失败也不删 ⇒ 槽里留 0 字节）。
    if (save_host_->write_create_always(path, ByteSpan{}) != WriteOutcome::Ok) {
        report_error("thumb-create-failed:" + std::to_string(slot));
        write_int(ops[0], 1);
        return;
    }
    const auto object = read_int(ops[2]);  // 读序：建立成功之后
    const auto surface = save_host_->capture_surface(object);
    if (!surface) {
        report_error("thumb-capture-failed:" + std::to_string(object));
        write_int(ops[0], 2);
        return;
    }
    const Bytes bmp = encode_thumb_bmp(*surface);
    write_int(ops[0], save_host_->write_create_always(path, bmp) == WriteOutcome::Ok ? 0 : 2);
    return;
}

void Machine::op_1af(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    require_save_host("0x1af");
    const auto slot = read_int(ops[1]);
    const auto dir = save_directory();
    save_host_->ensure_directory(dir);
    const ReadResult result = save_host_->read(slot_path(dir, slot, true));
    if (result.outcome == ReadOutcome::OpenFailed) {
        write_int(ops[0], 1);  // ★ 打不开优先于表面门
        return;
    }
    const auto target = read_int(ops[2]);
    if (target >= 0 && target <= 2) {  // 0/1/2 被拒；负数按无符号位模式落在接受侧
        report_error("surface-selection-invalid:" + std::to_string(target));
        write_int(ops[0], 2);
        return;
    }
    ThumbImage image{};
    if (result.outcome != ReadOutcome::Ok || !decode_thumb_bmp(result.bytes, image)) {
        report_error("thumb-decode-failed:" + std::to_string(slot));
        write_int(ops[0], 2);
        return;
    }
    TextureEntry entry;
    entry.record.file = -1;
    entry.slot.width = image.width;
    entry.slot.height = image.height;
    entry.slot.rgba = std::move(image.rgba);
    entry.slot.present = true;
    entry.record.blank = false;
    entry.record.non_file = false;
    entry.slot.revision = next_texture_revision(static_cast<std::uint32_t>(target));
    textures_[static_cast<std::uint32_t>(target)] = std::move(entry);
    write_int(ops[0], 0);
    return;
}

// ══ 存档**布局 1**（主版本 1 / 次版本 20）的暂存块与位置还原 ══
// 规则 `CORE-OPS-SAVE-VERSION-AE`（`60-ops-scalar.md:306-321`）。★ 本条只落**正文写下来的那几段**：
// 记录头 / 格式标记三档 / 长度前缀 / 三对 / 帧管理器整块回填 / 两族位置还原 / 帧号分叉。
// ⛔ **没有**落：容器（`0x19f` 那一层）里这些段各自的字节偏移 —— 正文没写、残差里也没有。

namespace {

// `_strncmp` 语义的比较：**无符号字节逐比**，最多 4 字节、**遇 NUL 即止**。返回 −1 / 0 / 1。
// ★ 拿 4 字节整数比会把 `"/zzz"`（0x7A7A7A2F 小端）判成「比 "080 " 大」⇒ 分档方向翻转。
int compare_format_marker(ByteSpan left, ByteSpan right) {
    constexpr std::size_t limit = 4;  // 正文：最多 4 字节
    for (std::size_t i = 0; i < limit; ++i) {
        const std::uint32_t a = i < left.size() ? left[i] : 0;
        const std::uint32_t b = i < right.size() ? right[i] : 0;
        if (a != b) return a < b ? -1 : 1;
        if (a == 0) return 0;  // 遇 NUL 即止
    }
    return 0;
}

}  // namespace

SaveStagedFrame parse_save_staged_frame(ByteSpan bytes, ByteSpan engine_marker) {
    SaveStagedFrame staged;
    // ① 定长记录头（32 字节）。读不满 ⇒ 头不合法（调用方据此记诊断）。
    if (bytes.size() < kSaveRecordHeaderBytes) return staged;
    staged.header_ok = true;
    const ByteSpan marker = bytes.subspan(kSaveRecordMarkerOffset, 4);
    // ② 格式标记：**字符串口径**与常量 `"080 "` 比（不小于 ⇒ 按三对解释；小于 ⇒ 旧格式支）。
    staged.marker = compare_format_marker(marker, ByteSpan(kSaveFormatMarker080, 4)) < 0
                        ? SaveFormatMarker::Legacy
                        : SaveFormatMarker::Current;
    // 同一格 > **引擎自己那份**标记 ⇒ 报「引擎版本太旧」（错误处置，不改变上面的分档）。
    if (!engine_marker.empty()) staged.engine_outdated = compare_format_marker(marker, engine_marker) > 0;
    if (staged.marker == SaveFormatMarker::Legacy) return staged;  // 旧格式支：**负载不读**
    // ③ 长度前缀（4 字节，长度**含前缀自身**）→ ④ 负载（六个 u32）。
    const std::size_t prefix_offset = kSaveRecordHeaderBytes;
    if (bytes.size() < prefix_offset + kSaveRecordLengthPrefixBytes) return staged;
    const std::uint32_t prefix = read_u32(bytes, prefix_offset);
    if (prefix < kSaveRecordLengthPrefixBytes) return staged;  // 长度含自身 ⇒ 至少 4
    const std::size_t payload_declared = prefix - kSaveRecordLengthPrefixBytes;  // ★ N − 4，不是 N
    const std::size_t payload_offset = prefix_offset + kSaveRecordLengthPrefixBytes;
    const std::size_t payload_available = bytes.size() - payload_offset;
    // 声明的负载要装得下六个 u32，且**真的在**（短读 ⇒ 不按三对解释）。
    if (payload_declared < kSaveRecordPayloadBytes || payload_declared > payload_available) return staged;
    for (std::size_t pair = 0; pair < kSavePositionPairCount; ++pair) {
        staged.pairs[pair].integer = read_i32(bytes, payload_offset + pair * 8);
        staged.pairs[pair].word_offset = read_i32(bytes, payload_offset + pair * 8 + 4);
    }
    staged.payload_read = true;
    return staged;
}

bool Machine::restore_save_frame_manager(ByteSpan block) {
    // 长度 = 定长头 + 整数条记录（每条记录的跨度固定）。长度不合这条形状 ⇒ 不是一份能整块回填的
    // 帧管理器 ⇒ 记诊断、**一格不写**（⛔ 不按"能拷多少拷多少"糊过去）。
    if (block.size() < kSaveFrameManagerHeaderBytes) {
        report_error("save-frame-manager-short:" + std::to_string(block.size()));
        return false;
    }
    const std::size_t records = block.size() - kSaveFrameManagerHeaderBytes;
    if (records % kSaveFrameRecordStride != 0) {
        report_error("save-frame-manager-length-invalid:" + std::to_string(records));
        return false;
    }
    const std::size_t frames = records / kSaveFrameRecordStride;  // 存档记的帧数
    const std::size_t covered = std::min(frames, kFrameCount);
    for (std::size_t i = 0; i < covered; ++i) {
        // ★ 两个数组的**起点在块内 +0x8F8**（定长头里），**步长 = 帧记录跨度**（不是「记录起点 + 8F8」）：
        //   第 i 帧的两格落在 `0x8F8 + 0x41C × i` / `0x8FC + 0x41C × i` —— 与该帧的记录同段。
        const std::size_t stride_offset = i * kSaveFrameRecordStride;
        // ★ 下标值**来自存档**（两个数组落在这段覆盖范围内）—— ⛔ 不是"恒 0"、也不是"有 / 无"布尔。
        save_priority_index_[i] = read_i32(block, kSaveFrameIndexPriorityOffset + stride_offset);
        save_fallback_index_[i] = read_i32(block, kSaveFrameIndexFallbackOffset + stride_offset);
    }
    // 覆盖不到的帧槽（`frames < kFrameCount` 的尾巴）本版**不规定** ⇒ 一格不动（残差）。
    if (frames > kFrameCount) {
        report_error("save-frame-manager-more-frames-than-engine:" + std::to_string(frames));
    }
    return true;
}

std::int32_t Machine::save_frame_index(std::size_t frame, bool priority) const {
    if (frame >= kFrameCount) return -1;
    return priority ? save_priority_index_[frame] : save_fallback_index_[frame];
}

void Machine::seed_save_frame_index_for_test(std::size_t frame, std::int32_t priority,
                                             std::int32_t fallback) {
    if (frame >= kFrameCount) {
        report_error("save-frame-index-out-of-range:" + std::to_string(frame));
        return;
    }
    save_priority_index_[frame] = priority;
    save_fallback_index_[frame] = fallback;
}

void Machine::rebuild_save_frame_layout1(std::size_t frame_index, ByteSpan code_body,
                                         const SaveStagedFrame& staged) {
    if (frame_index >= kFrameCount) {
        report_error("save-frame-index-out-of-range:" + std::to_string(frame_index));
        return;
    }
    if (!staged.header_ok) {
        report_error("save-record-header-short");
        return;
    }
    if (staged.engine_outdated) {
        // 正文：这一条属**错误处置**、不改变上面的分档 ⇒ 记一条诊断，档位照常处置。
        report_error("save-format-engine-outdated");
    }
    if (staged.marker == SaveFormatMarker::Legacy) {
        // 标记 < `"080 "` ⇒ **负载不读**：旧格式支的完整复原语义本版未读（残差）⇒ 不重建。
        report_error("save-record-legacy-format-unmodeled");
        return;
    }
    if (!staged.payload_read) {
        report_error("save-record-payload-short");
        return;
    }
    Frame& frame = frames_[frame_index];
    // ① 为这一帧的**代码块**分配内存、把代码体**整块**读进去（⛔ 不是按脚本身份重新装载）。
    frame.code.assign(code_body.begin(), code_body.end());
    // ② 三对写进帧记录的三对槽（表基址 = 代码块基址 + 4 × 偏移；模型里代码块基址 = 帧内 0）。
    frame.save_pairs = staged.pairs;
    frame.save_pairs_ready = true;
    // ③ 指令指针初值 = **代码块基址**（⛔ 不是"从存档读回指令指针"）。
    frame.ip = 0;
}

bool Machine::resolve_save_layout1_position(std::size_t frame_index) {
    if (frame_index >= kFrameCount) {
        report_error("save-frame-index-out-of-range:" + std::to_string(frame_index));
        return false;
    }
    Frame& frame = frames_[frame_index];
    // 三对不可用（旧格式支 / 没重建过）⇒ 本版对那种帧的位置还原不规定。
    if (!frame.save_pairs_ready) return false;
    // 表元素 = **代码块内**的 32 位**字**偏移 ⇒ 字节偏移 = 4 × (表偏移 + 下标)；元素值是目标位置的字偏移。
    const auto position_at = [this, &frame](const SavePositionPair& pair,
                                            std::int32_t index) -> std::optional<std::size_t> {
        if (index < 0) return std::nullopt;  // 该族不命中（⛔ 不是"位置不变"）
        const std::int64_t word = static_cast<std::int64_t>(pair.word_offset) + index;
        if (word < 0) return std::nullopt;
        const std::size_t element = static_cast<std::size_t>(word) * 4;
        if (element + 4 > frame.code.size()) {
            // 两张位置表**装在代码块内部** ⇒ 越出代码块 = 读不到（记诊断、按"该族不命中"处置）。
            report_error("save-position-table-out-of-code:" + std::to_string(index));
            return std::nullopt;
        }
        const std::int32_t target = read_i32(frame.code, element);
        if (target < 0 || static_cast<std::size_t>(target) * 4 > frame.code.size()) {
            report_error("save-position-out-of-code:" + std::to_string(target));
            return std::nullopt;
        }
        return static_cast<std::size_t>(target) * 4;  // 代码块基址 + 4 × 表[下标]（基址 = 帧内 0）
    };
    // ① 优先族 = **换脚本**那一族，读**第二对**的表；命中 ⇒ 推进格数 ← **3** 个字。
    if (const auto target = position_at(frame.save_pairs[1], save_priority_index_[frame_index])) {
        frame.ip = *target;
        frame.length_slot = 3;
        return true;
    }
    // ② 该族不命中 ⇒ **回退另一族**（第一对表）；命中 ⇒ 推进格数 ← **0**。
    if (const auto target = position_at(frame.save_pairs[0], save_fallback_index_[frame_index])) {
        frame.ip = *target;
        frame.length_slot = 0;
        return true;
    }
    // ③ 两族都不命中 ⇒ **指令指针与推进格数一格不改**（⛔ 不是"写成表首 / 0"）。
    return false;
}

void Machine::apply_save_layout1() {
    // 引擎暂存区由读档路径（`0x19f`，本版未成篇）填；没有它 ⇒ 响亮留痕，⛔ 不假装恢复过。
    if (!staged_save_.present) {
        report_error("save-container-unmodeled");
        return;
    }
    // ① 帧管理器**整块**回填：两个「每帧当前位置下标」的值由此**来自存档**。
    if (!restore_save_frame_manager(staged_save_.frame_manager_block)) return;
    // ② 按帧重建（暂存块 → 代码块 / 三对 / 指令指针 = 代码块基址）。
    for (std::size_t i = 0; i < staged_save_.frames.size() && i < kFrameCount; ++i) {
        const SaveStagedFrame staged =
            parse_save_staged_frame(staged_save_.frames[i].staged_block, save_engine_marker_bytes());
        rebuild_save_frame_layout1(i, staged_save_.frames[i].code_body, staged);
    }
    // ③ 两族位置还原（当前帧）：优先族推进 3 / 回退族推进 0 / 两族都不命中 ⇒ 不改指令指针。
    resolve_save_layout1_position(current_frame_);
    // ④ 应用之后按**当前帧号**分叉。
    if (static_cast<std::int32_t>(current_frame_) == staged_save_.frame_number) {
        // 等于存档帧号 ⇒ 恢复帧号（同值）并**清标志**。
        current_frame_ = static_cast<std::size_t>(staged_save_.frame_number);
        save_version_flag_ = 0;
        return;
    }
    // 不等 ⇒ 帧号 + 1 并**重装**。★ 重装要脚本标识，而布局 1 的容器里它的位置本版未读
    //   （残差 `RES-OPS-SAVE-VERSION-AE-LAYOUT1`）⇒ 只落「帧号 + 1」，重装**留痕不假装**。
    if (current_frame_ + 1 >= kFrameCount) {
        report_error("save-frame-number-out-of-range:" + std::to_string(current_frame_));
        return;
    }
    current_frame_ += 1;
    report_error("save-frame-reload-unmodeled:" + std::to_string(current_frame_));
}

}  // namespace core
