// 影片：`0x20F` 处理体 + **门之前**的对象表服务 + "填纹理"，都在本文件。
//
// ★ 分工（`CORE-VM-MOVIE`）：影片位只管把门关住；轮询 / 收尾 / 循环 / 填纹理都在
//   `service_movies()` 里，每次迭代在过门之前跑一遍。解码器是平台边界（core/movie.h）。
#include "core/vm.h"
#include "core/install.h"

namespace core {

void Machine::op_20f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 播影片：建对象 → 初始化后端 → 绑定门 → 取文件配置 → 读选项 → 启动 → 置影片位
    // 读取顺序 2 → 1 → 1 → 3（`CORE-OPS-MOVIE`）：槽号 → 资源 id → 资源 id → 选项字。
    const auto slot = static_cast<std::uint32_t>(read_int(ops[1]));
    MovieRecord& movie = movies_[slot];  // 槽为空 ⇒ 新建；同槽已有 ⇒ 不重建
    if (!movie.present) movie.present = true;
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

void Machine::service_movies() {
    if (!movie_active_) return;
    bool any = false;
    for (auto it = movies_.begin(); it != movies_.end();) {
        MovieRecord& movie = it->second;
        const std::uint32_t slot = it->first;
        // 槽为空 / 未启动（打开或绑定失败留下的残留）⇒ 跳过：不进播完判据、不维持影片位。
        if (!movie.present || !movie.started) {
            ++it;
            continue;
        }
        if (!movie.backend) {
            // 已启动却没有后端（正常路径不会出现）：⛔ 不伪造"播完"，门继续关着（与参考实现同口径）。
            any = true;
            ++it;
            continue;
        }
        MovieBackend& backend = *movie.backend;
        // 「当前位置」按启播后的引擎时钟换算（32 位回绕用有符号差）。
        const std::int32_t elapsed = static_cast<std::int32_t>(scene_clock_ms_ - movie.started_ms);
        const double target = elapsed > 0 ? static_cast<double>(elapsed) / 1000.0 : 0.0;
        if (backend.advance_to(target)) {
            // 新帧：写进同槽纹理；它同时是一次「场景需要重绘」的输入（影片新帧）。
            if (fill_movie_texture(slot, backend.frame())) scene_dirty_ = true;
        }
        const double stop = backend.stop_time();
        const bool done = backend.ended() || (stop > 0.0 && backend.position() >= stop);
        if (!done) {
            any = true;
            ++it;
            continue;
        }
        if ((movie.options & 1u) != 0u) {
            // 循环位：回到开头重新启动；保留对象与槽，不销毁、不填纹理。
            backend.rewind();
            backend.play();
            movie.started_ms = scene_clock_ms_;
            any = true;
            ++it;
            continue;
        }
        // 正常收尾：`(选项字 & 3) != 2` ⇒ 一次「填纹理」请求；随后停止 / 关闭 / 移除槽。
        if ((movie.options & 3u) != 2u) fill_movie_texture(slot, backend.frame());
        backend.close();
        it = movies_.erase(it);
    }
    movie_active_ = any;
}

// 「填纹理」的像素效果不在规范正文里（`RES-MOVIE-FILL`）：本实现写最新解码帧；
// 尺寸不一致时按最近邻缩放（只为不越界，不是取证结论）。

bool Machine::fill_movie_texture(std::uint32_t slot, const MovieFrame& frame) {
    if (frame.width == 0 || frame.height == 0 || frame.rgba.empty()) return false;
    const auto it = textures_.find(slot);
    if (it == textures_.end()) return false;  // 绑定已被换掉：没有最后一帧可留（残差）
    TextureRecord& texture = it->second;
    if (texture.width == 0 || texture.height == 0) return false;
    const std::size_t expected =
        static_cast<std::size_t>(texture.width) * texture.height * 4u;
    if (texture.width == frame.width && texture.height == frame.height &&
        frame.rgba.size() == expected) {
        texture.rgba = frame.rgba;
    } else {
        texture.rgba.resize(expected);
        for (std::uint32_t y = 0; y < texture.height; ++y) {
            const std::uint32_t sy = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(y) * frame.height / texture.height);
            for (std::uint32_t x = 0; x < texture.width; ++x) {
                const std::uint32_t sx = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(x) * frame.width / texture.width);
                const std::size_t src =
                    (static_cast<std::size_t>(sy) * frame.width + sx) * 4u;
                const std::size_t dst =
                    (static_cast<std::size_t>(y) * texture.width + x) * 4u;
                if (src + 4u > frame.rgba.size()) continue;
                texture.rgba[dst + 0] = frame.rgba[src + 0];
                texture.rgba[dst + 1] = frame.rgba[src + 1];
                texture.rgba[dst + 2] = frame.rgba[src + 2];
                texture.rgba[dst + 3] = frame.rgba[src + 3];
            }
        }
    }
    texture.blank = false;
    texture.revision++;  // 让宿主（GPU 纹理缓存）重传
    return true;
}

}  // namespace core
