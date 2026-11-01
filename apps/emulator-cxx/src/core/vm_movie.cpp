// 影片 / 媒体对象表：`0x20F` 处理体 + **门之前**的对象表服务 + "填纹理"，都在本文件。
//
// ★ 分工（`CORE-VM-MOVIE`）：影片位只管把门关住；轮询 / 收尾 / 循环 / 填纹理都在
//   `service_movies()` 里，每次迭代在过门之前跑一遍。解码器是平台边界（core/movie.h）。
// ★ 这张表就是 `60-ops-render.md`「媒体对象表（合同）」的**同一张表**，也是「纹理族」的
//   **共享对象槽**（`60-ops-wait.md:94` / `60-ops-texture.md:16-17`）：定长 1000 格、空槽为空。
//   填 = 五条指令（`0x20f` / `0x236` / `0x240` / `0x241` / `0x24d`，**全部按第 2 个操作数选格**，
//   同槽已有对象 ⇒ 不重建、只改配置 / 启动）；清 = ① 纹理族三条写指令 ② `0x210` / `0x249`
//   ③ 每拍的扫描 ⑤ `0x23d` 的 `42..999`（④ 那条「本拍不计入」路径的判定身份未取证 ⇒ 未实现）。
#include "core/vm.h"
#include "core/install.h"

namespace core {

// ── 表的三条原语（见 vm.h 的声明）──

MovieRecord& Machine::media_cell(std::uint32_t slot) {
    // ★ 判据只有"格空不空"这一条：⛔ 不许在这里加"槽号 < 1000"之类的范围检查
    //   （正文：「上限一千」是**每拍扫描的循环边界**，不是任何一条指令选格时的判据）。
    if (slot < kMediaSlotCount) {
        auto& cell = movies_[slot];
        if (!cell) {
            cell.emplace();
            cell->present = true;
            cell->identity = next_movie_identity_++;  // 只有**真的新建**才取号
        }
        return *cell;
    }
    // 表外落点：本版不规定它会怎样；模型不检查、不越界，也不让它进任何整表遍历。
    auto [it, inserted] = movies_out_of_range_.try_emplace(slot);
    if (inserted) {
        it->second.present = true;
        it->second.identity = next_movie_identity_++;
    }
    return it->second;
}

void Machine::release_media_cell(std::uint32_t slot) {
    // 「把该格的对象释放并置空」：格空 ⇒ 静默跳过，不报错（正文：扫描**不报错**）。
    MovieRecord* record = nullptr;
    if (slot < kMediaSlotCount) {
        auto& cell = movies_[slot];
        if (!cell) return;
        record = &*cell;
        if (record->backend) {
            record->backend->close();
            record->backend.reset();
        }
        cell.reset();  // 先释放对象、再置空（逐槽）
    } else {
        const auto it = movies_out_of_range_.find(slot);
        if (it == movies_out_of_range_.end()) return;
        if (it->second.backend) {
            it->second.backend->close();
            it->second.backend.reset();
        }
        movies_out_of_range_.erase(it);
    }
    recompute_movie_active();
}

void Machine::recompute_movie_active() {
    // 「有活动对象」= 表里还有**已启动**的对象（失败残留 / 只建未启动的格子不参与，
    // 见 `CORE-OPS-MOVIE` 的「失败残留对象没有"已启动"」那条）。
    for (const auto& cell : movies_) {
        if (cell && cell->present && cell->started) {
            movie_active_ = true;
            return;
        }
    }
    movie_active_ = false;
}

// ── 播影片：`0x20f` ──

void Machine::op_20f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 播影片：建对象 → 初始化后端 → 绑定门 → 取文件配置 → 读选项 → 启动 → 置影片位
    // 读取顺序 2 → 1 → 1 → 3（`CORE-OPS-MOVIE`）：槽号 → 资源 id → 资源 id → 选项字。
    const auto slot = static_cast<std::uint32_t>(read_int(ops[1]));
    // ★ 同槽已有对象 ⇒ **不重建**：格上仍是原来那个对象（不释放、不换新），只改配置与启动。
    MovieRecord& movie = media_cell(slot);
    movie.present = true;
    // 只改配置与启动：先落成「未启动」，失败时正是规范要求的残留形态。
    movie.started = false;
    if (movie.backend) {
        movie.backend->close();
        movie.backend.reset();
    }
    movie.file_id = static_cast<std::uint32_t>(read_int(ops[0]));
    // 初始化播放后端（外部请求）。失败留在槽里，不启动、不置位。
    auto backend = movie_factory_ ? movie_factory_() : make_mpeg_backend();
    if (!backend) throw CoreError("movie-backend-init-failed");
    movie.backend = std::move(backend);
    (void)read_int(ops[0]);  // 绑定步骤再读一次资源 id
    // 绑定门：纹理槽必须有绑定；对象表空只是建对象，纹理槽空则抛「显示消息型」异常。
    if (textures_.find(slot) == textures_.end()) {
        throw CoreError("movie-texture-slot-empty:" + std::to_string(slot));
    }
    // 取文件并配置播放后端。打开失败同样留下未启动的残留对象。
    Bytes bytes;
    try {
        if (movie_bytes_loader_) {
            bytes = movie_bytes_loader_(movie.file_id);
        } else if (install_ != nullptr) {
            bytes = read_install_file(*install_, movie.file_id);
            register_loaded_file(movie.file_id);  // 文件库装载登记（按区号；`0x143` 的投递源；`0x455948`）
        } else {
            throw CoreError("movie-install-missing");
        }
    } catch (const std::exception& e) {
        throw CoreError(std::string("movie-open-failed:") + e.what());
    }
    std::string error;
    if (!movie.backend->open(std::move(bytes), error)) {
        throw CoreError("movie-open-failed:" + error);
    }
    movie.options = static_cast<std::uint32_t>(read_int(ops[2]));
    movie.started_ms = scene_clock_ms_;
    movie.backend->play();
    movie.started = true;
    movie_active_ = true;
    state_flags_ |= (1u << 13);
    return;

}

// ── 填格：`0x236` / `0x240` / `0x241`（三条**整条尚未成篇**）──
//
// 正文只登记它们**与本表有关的那一项副作用**（`60-ops-render.md:351`）：
// 按**第 2 个操作数**选一格、**格空时**建一条播放类对象（格非空 ⇒ 原对象留着，不重建、不释放）。
// ⇒ 本版**不读**它们其余的操作数（那些操作数的语义未取证；读了就等于替引擎编读序与类型检查）。

void Machine::op_236(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    (void)media_cell(static_cast<std::uint32_t>(read_int(ops[1])));
}

void Machine::op_240(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    (void)media_cell(static_cast<std::uint32_t>(read_int(ops[1])));
}

void Machine::op_241(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    (void)media_cell(static_cast<std::uint32_t>(read_int(ops[1])));
}

// ── 显式清格：`0x210`（第 1 个操作数）/ `0x249`（第 2 个操作数）──
//
// 同样只登记这一项副作用（`60-ops-render.md:354`）：把该格的对象**释放并置空**。

void Machine::op_210(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    release_media_cell(static_cast<std::uint32_t>(read_int(ops[0])));
}

void Machine::op_249(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    release_media_cell(static_cast<std::uint32_t>(read_int(ops[1])));
}

// ── 每拍的扫描（清 ③）──

void Machine::service_movies() {
    if (!movie_active_) return;
    bool any = false;
    // ★ 逐槽（0..999 = **上限一千 / 每拍扫描的循环边界**）、空槽静默跳过、扫描**不报错**。
    for (std::uint32_t slot = 0; slot < kMediaSlotCount; ++slot) {
        auto& cell = movies_[slot];
        if (!cell) continue;  // 空槽：静默跳过
        MovieRecord& movie = *cell;
        // 槽为空 / 未启动（打开或绑定失败留下的残留）⇒ 跳过：不进播完判据、不维持影片位。
        if (!movie.present || !movie.started) continue;
        if (!movie.backend) {
            // 已启动却没有后端（正常路径不会出现）：⛔ 不伪造"播完"，门继续关着（与参考实现同口径）。
            any = true;
            continue;
        }
        MovieBackend& backend = *movie.backend;
        // 「当前位置」按启播后的引擎时钟换算（32 位回绕用有符号差）。
        const std::int32_t elapsed = static_cast<std::int32_t>(scene_clock_ms_ - movie.started_ms);
        const double target = elapsed > 0 ? static_cast<double>(elapsed) / 1000.0 : 0.0;
        if (backend.advance_to(target)) {
            // 新帧：写进同槽纹理；它同时是一次「场景需要重绘」的输入（影片新帧）。
            // ★ 「有待消费的新帧」是**渲染步门槛**的输入之一（`CORE-TIMING-WINDOW` 的"只挡推进"
            //   第 3 条，**读一次即消费**）：不管填纹理成不成功，解出新帧就置它。
            movie_new_frame_ = true;
            if (fill_movie_texture(slot, backend.frame())) scene_dirty_ = true;
        }
        const double stop = backend.stop_time();
        // **结束条件** = 对象自己的两个时刻比较成立（"已经过去 / 播完"那一对）。
        // ★ 这两个取值器的**领域名**本版未取证（残差 `RES-RENDER-STEP-COMMAND-OBJECTS` ①）
        //   ⇒ 合同只能写成「已到结束条件」，这里**不**给它们编名字。
        const bool done = backend.ended() || (stop > 0.0 && backend.position() >= stop);
        if (!done) {
            any = true;
            continue;
        }
        if ((movie.options & 1u) != 0u) {
            // 循环位（选项字第 0 位）= 1 ⇒ **不销毁**：回到开头重新启动；保留对象与槽，不填纹理。
            backend.rewind();
            backend.play();
            movie.started_ms = scene_clock_ms_;
            any = true;
            continue;
        }
        // 正常收尾：`(选项字 & 3) != 2` ⇒ 一次「填纹理」请求；随后停止 / 关闭 / **释放并置空**。
        if ((movie.options & 3u) != 2u) fill_movie_texture(slot, backend.frame());
        backend.close();
        cell.reset();
    }
    movie_active_ = any;
}

// 「填纹理」的像素效果不在规范正文里（`RES-MOVIE-FILL`）：本实现写最新解码帧；
// 尺寸不一致时按最近邻缩放（只为不越界，不是取证结论）。

bool Machine::fill_movie_texture(std::uint32_t slot, const MovieFrame& frame) {
    if (frame.width == 0 || frame.height == 0 || frame.rgba.empty()) return false;
    const auto it = textures_.find(slot);
    if (it == textures_.end()) return false;  // 绑定已被换掉：没有最后一帧可留（残差）
    TextureEntry& entry = it->second;
    if (!entry.slot.present || entry.slot.width == 0 || entry.slot.height == 0) return false;
    const std::size_t expected = static_cast<std::size_t>(entry.slot.width) * entry.slot.height * 4u;
    if (entry.slot.width == frame.width && entry.slot.height == frame.height &&
        frame.rgba.size() == expected) {
        entry.slot.rgba = frame.rgba;
    } else {
        entry.slot.rgba.resize(expected);
        for (std::uint32_t y = 0; y < entry.slot.height; ++y) {
            const std::uint32_t sy = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(y) * frame.height / entry.slot.height);
            for (std::uint32_t x = 0; x < entry.slot.width; ++x) {
                const std::uint32_t sx = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(x) * frame.width / entry.slot.width);
                const std::size_t src =
                    (static_cast<std::size_t>(sy) * frame.width + sx) * 4u;
                const std::size_t dst =
                    (static_cast<std::size_t>(y) * entry.slot.width + x) * 4u;
                if (src + 4u > frame.rgba.size()) continue;
                entry.slot.rgba[dst + 0] = frame.rgba[src + 0];
                entry.slot.rgba[dst + 1] = frame.rgba[src + 1];
                entry.slot.rgba[dst + 2] = frame.rgba[src + 2];
                entry.slot.rgba[dst + 3] = frame.rgba[src + 3];
            }
        }
    }
    entry.record.blank = false;
    entry.slot.revision++;  // 让宿主（GPU 纹理缓存）重传
    return true;
}

}  // namespace core
