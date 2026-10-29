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

#include <cstdio>

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

}  // namespace core
