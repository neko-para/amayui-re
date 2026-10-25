// 场景批次：绘制项 / 4 顶点网格 → 顶点缓冲 + 每段绑一张纹理。
//
// ★ 绘制顺序（含同键平键序）是引擎语义，见 scene_raster 与 CORE-SCENE-DRAW-ORDER；
//   这里按同一顺序产出 GPU 顶点，纹理按 `TextureRecord::revision` 缓存。
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "app.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/scene_raster.h"
namespace app {

#ifdef EMULATOR_CXX_HAVE_SHADERCROSS

constexpr const char* kSceneVertexShader = R"(
struct VSInput {
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};
struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};
VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
)";

constexpr const char* kSceneFragmentShader = R"(
Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);
float4 PSMain(float2 uv : TEXCOORD0, float4 color : TEXCOORD1) : SV_Target {
    return tex.Sample(samp, uv) * color;
}
)";

bool create_scene_pipeline(SDL_GPUDevice* device, SDL_GPUTextureFormat format, ScenePipeline& out) {
    SDL_GPUShader* vertex = compile_shader(
        device, kSceneVertexShader, "VSMain", SDL_SHADERCROSS_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fragment = compile_shader(
        device, kSceneFragmentShader, "PSMain", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, 1);
    if (vertex == nullptr || fragment == nullptr) {
        if (vertex != nullptr) SDL_ReleaseGPUShader(device, vertex);
        if (fragment != nullptr) SDL_ReleaseGPUShader(device, fragment);
        return false;
    }

    SDL_GPUVertexBufferDescription buffer_description{};
    buffer_description.slot = 0;
    buffer_description.pitch = sizeof(SceneVertex);
    buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attributes[3]{};
    attributes[0].location = 0;
    attributes[0].buffer_slot = 0;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].buffer_slot = 0;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[1].offset = sizeof(float) * 2;
    attributes[2].location = 2;
    attributes[2].buffer_slot = 0;
    attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[2].offset = sizeof(float) * 4;

    SDL_GPUVertexInputState vertex_input{};
    vertex_input.vertex_buffer_descriptions = &buffer_description;
    vertex_input.num_vertex_buffers = 1;
    vertex_input.vertex_attributes = attributes;
    vertex_input.num_vertex_attributes = 3;

    SDL_GPUColorTargetBlendState blend{};
    blend.enable_blend = true;
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUColorTargetDescription target{};
    target.format = format;
    target.blend_state = blend;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.vertex_input_state = vertex_input;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    out.pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, vertex);
    SDL_ReleaseGPUShader(device, fragment);
    if (out.pipeline == nullptr) {
        log_sdl_error("SDL_CreateGPUGraphicsPipeline(scene)");
        return false;
    }

    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    out.sampler = SDL_CreateGPUSampler(device, &sampler_info);
    return out.sampler != nullptr;
}

void destroy_scene_pipeline(SDL_GPUDevice* device, ScenePipeline& pipeline) {
    if (pipeline.sampler != nullptr) SDL_ReleaseGPUSampler(device, pipeline.sampler);
    if (pipeline.pipeline != nullptr) SDL_ReleaseGPUGraphicsPipeline(device, pipeline.pipeline);
    pipeline = {};
}

void release_scene_textures(SDL_GPUDevice* device, SceneTextureCache& cache) {
    for (auto& [id, texture] : cache.textures) {
        (void)id;
        if (texture != nullptr) SDL_ReleaseGPUTexture(device, texture);
    }
    if (cache.white != nullptr) SDL_ReleaseGPUTexture(device, cache.white);
    cache.textures.clear();
    cache.revisions.clear();
    cache.white = nullptr;
}

SDL_GPUTexture* scene_texture(SDL_GPUDevice* device, SceneTextureCache& cache, const core::Machine& machine,
                              std::uint32_t id) {
    if (id == 0) {
        if (cache.white == nullptr) {
            const core::Bytes white{255, 255, 255, 255};
            cache.white = upload_texture(device, white, 1, 1);
        }
        return cache.white;
    }
    const core::TextureRecord* record = machine.texture(id);
    if (record == nullptr || record->rgba.empty()) return nullptr;
    const auto revision_it = cache.revisions.find(id);
    const auto texture_it = cache.textures.find(id);
    if (texture_it != cache.textures.end() && revision_it != cache.revisions.end() &&
        revision_it->second == record->revision) {
        return texture_it->second;
    }
    SDL_GPUTexture* uploaded = upload_texture(device, record->rgba, static_cast<int>(record->width),
                                              static_cast<int>(record->height));
    if (uploaded == nullptr) return nullptr;
    if (texture_it != cache.textures.end() && texture_it->second != nullptr) {
        SDL_ReleaseGPUTexture(device, texture_it->second);
    }
    cache.textures[id] = uploaded;
    cache.revisions[id] = record->revision;
    return uploaded;
}

void push_quad(std::vector<SceneVertex>& vertices, std::vector<SceneRange>& ranges,
               float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
               std::uint32_t argb, std::uint32_t texture_id, float width, float height) {
    const float a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
    const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
    const auto px = [&](float x) { return x / width * 2.0f - 1.0f; };
    const auto py = [&](float y) { return 1.0f - y / height * 2.0f; };
    const auto push = [&](float x, float y, float u, float v) {
        vertices.push_back({px(x), py(y), u, v, r, g, b, a});
    };
    const std::uint32_t first = static_cast<std::uint32_t>(vertices.size());
    push(x0, y0, u0, v0);
    push(x1, y0, u1, v0);
    push(x1, y1, u1, v1);
    push(x0, y0, u0, v0);
    push(x1, y1, u1, v1);
    push(x0, y1, u0, v1);
    ranges.push_back({first, 6, texture_id});
}

void build_scene_vertices(const core::Machine& machine, float width, float height,
                          std::vector<SceneVertex>& vertices, std::vector<SceneRange>& ranges) {
    std::vector<std::uint32_t> keys;
    for (const auto& [key, item] : machine.draw_items()) { (void)item; keys.push_back(key); }
    for (const auto& [key, mesh] : machine.meshes()) {
        if (static_cast<std::int32_t>(key) < 0) continue;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), [](std::uint32_t a, std::uint32_t b) {
        return static_cast<std::int32_t>(a) < static_cast<std::int32_t>(b);
    });
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    const std::uint32_t now = machine.scene_clock_ms();
    const bool forced = machine.forced_complete();
    for (const auto key : keys) {
        const auto mesh_it = machine.meshes().find(key);
        if (mesh_it != machine.meshes().end()) {
            const core::MeshRecord& mesh = mesh_it->second;
            if ((mesh.flags & 1u) != 0 && mesh.count == 4 && mesh.copies[0].size() >= 4 &&
                mesh.copies[1].size() >= 4) {
                const auto color = core::derive_color(mesh, now, forced);
                if ((color >> 24) != 0) {
                    const auto fx = [&](std::size_t i) { return std::bit_cast<float>(mesh.copies[0][i]); };
                    const auto fy = [&](std::size_t i) { return std::bit_cast<float>(mesh.copies[1][i]); };
                    // 两个三角形：0-1-2 / 1-3-2（与软件光栅同一拓扑）。
                    const std::uint32_t first = static_cast<std::uint32_t>(vertices.size());
                    const auto push = [&](std::size_t i) {
                        vertices.push_back({fx(i) / width * 2.0f - 1.0f, 1.0f - fy(i) / height * 2.0f,
                                            0.0f, 0.0f,
                                            static_cast<float>((color >> 16) & 0xFFu) / 255.0f,
                                            static_cast<float>((color >> 8) & 0xFFu) / 255.0f,
                                            static_cast<float>(color & 0xFFu) / 255.0f,
                                            static_cast<float>((color >> 24) & 0xFFu) / 255.0f});
                    };
                    push(0); push(1); push(2); push(1); push(3); push(2);
                    ranges.push_back({first, 6, 0});
                }
            }
        }

        const auto item_it = machine.draw_items().find(key);
        if (item_it == machine.draw_items().end()) continue;
        const core::DrawItem& item = item_it->second;
        if ((item.flags & 1u) == 0) continue;
        const core::TextureRecord* texture = machine.texture(item.texture_id);
        if (texture == nullptr || texture->rgba.empty()) continue;
        const int sx0 = std::max(0, item.left);
        const int sy0 = std::max(0, item.top);
        const int sx1 = std::min(static_cast<int>(texture->width), item.right);
        const int sy1 = std::min(static_cast<int>(texture->height), item.bottom);
        if (sx1 <= sx0 || sy1 <= sy0) continue;
        const float dx = item.aux[0] + static_cast<float>(sx0 - item.left);
        const float dy = item.aux[1] + static_cast<float>(sy0 - item.top);
        const auto color = core::derive_color(item, now, forced);
        if ((color >> 24) == 0) continue;
        push_quad(vertices, ranges, dx, dy, dx + static_cast<float>(sx1 - sx0),
                  dy + static_cast<float>(sy1 - sy0),
                  static_cast<float>(sx0) / static_cast<float>(texture->width),
                  static_cast<float>(sy0) / static_cast<float>(texture->height),
                  static_cast<float>(sx1) / static_cast<float>(texture->width),
                  static_cast<float>(sy1) / static_cast<float>(texture->height),
                  color, item.texture_id, width, height);
    }
}

bool draw_scene_gpu(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command, SDL_GPUTexture* target,
                    const core::Machine& machine, const ScenePipeline& pipeline,
                    SceneTextureCache& cache, int width, int height, bool clear) {
    std::vector<SceneVertex> vertices;
    std::vector<SceneRange> ranges;
    build_scene_vertices(machine, static_cast<float>(width), static_cast<float>(height), vertices,
                         ranges);

    SDL_GPUBuffer* vertex_buffer = nullptr;
    SDL_GPUTransferBuffer* transfer = nullptr;
    if (!vertices.empty()) {
        SDL_GPUBufferCreateInfo buffer_info{};
        buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        buffer_info.size = static_cast<Uint32>(vertices.size() * sizeof(SceneVertex));
        vertex_buffer = SDL_CreateGPUBuffer(device, &buffer_info);
        SDL_GPUTransferBufferCreateInfo transfer_info{};
        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size = static_cast<Uint32>(vertices.size() * sizeof(SceneVertex));
        transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
        if (vertex_buffer == nullptr || transfer == nullptr) {
            if (vertex_buffer != nullptr) SDL_ReleaseGPUBuffer(device, vertex_buffer);
            if (transfer != nullptr) SDL_ReleaseGPUTransferBuffer(device, transfer);
            return false;
        }
        void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
        if (mapped == nullptr) {
            SDL_ReleaseGPUBuffer(device, vertex_buffer);
            SDL_ReleaseGPUTransferBuffer(device, transfer);
            return false;
        }
        std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(SceneVertex));
        SDL_UnmapGPUTransferBuffer(device, transfer);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
        SDL_GPUTransferBufferLocation source{transfer, 0};
        SDL_GPUBufferRegion destination{vertex_buffer, 0, static_cast<Uint32>(vertices.size() * sizeof(SceneVertex))};
        SDL_UploadToGPUBuffer(copy, &source, &destination, false);
        SDL_EndGPUCopyPass(copy);
    }

    SDL_GPUColorTargetInfo target_info{};
    target_info.texture = target;
    target_info.clear_color = kClearColor;
    target_info.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    target_info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target_info, 1, nullptr);
    if (pass == nullptr) {
        if (vertex_buffer != nullptr) SDL_ReleaseGPUBuffer(device, vertex_buffer);
        if (transfer != nullptr) SDL_ReleaseGPUTransferBuffer(device, transfer);
        return false;
    }
    SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    SDL_Rect scissor{0, 0, width, height};
    SDL_SetGPUScissor(pass, &scissor);
    SDL_BindGPUGraphicsPipeline(pass, pipeline.pipeline);
    if (vertex_buffer != nullptr) {
        SDL_GPUBufferBinding binding{vertex_buffer, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        for (const SceneRange& range : ranges) {
            SDL_GPUTexture* texture = scene_texture(device, cache, machine, range.texture);
            if (texture == nullptr) continue;
            SDL_GPUTextureSamplerBinding sampler_binding{texture, pipeline.sampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &sampler_binding, 1);
            SDL_DrawGPUPrimitives(pass, range.count, 1, range.first, 0);
        }
    }
    SDL_EndGPURenderPass(pass);
    if (vertex_buffer != nullptr) SDL_ReleaseGPUBuffer(device, vertex_buffer);
    if (transfer != nullptr) SDL_ReleaseGPUTransferBuffer(device, transfer);
    return true;
}

void preload_scene_textures(SDL_GPUDevice* device, SceneTextureCache& cache,
                            const core::Machine& machine) {
    for (const auto& [key, item] : machine.draw_items()) {
        (void)key;
        if ((item.flags & 1u) != 0) scene_texture(device, cache, machine, item.texture_id);
    }
}

bool render_scene_gpu_to_bmp(SDL_GPUDevice* device, const core::Machine& machine,
                             const std::string& path, int width, int height) {
    ScenePipeline pipeline;
    if (!create_scene_pipeline(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, pipeline)) return false;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(width);
    info.height = static_cast<Uint32>(height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(device, &info);
    if (target == nullptr) {
        destroy_scene_pipeline(device, pipeline);
        return false;
    }
    SceneTextureCache cache;
    preload_scene_textures(device, cache, machine);
    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
    if (command == nullptr) {
        release_scene_textures(device, cache);
        SDL_ReleaseGPUTexture(device, target);
        destroy_scene_pipeline(device, pipeline);
        return false;
    }
    if (!draw_scene_gpu(device, command, target, machine, pipeline, cache, width, height, true)) {
        SDL_CancelGPUCommandBuffer(command);
        release_scene_textures(device, cache);
        SDL_ReleaseGPUTexture(device, target);
        destroy_scene_pipeline(device, pipeline);
        return false;
    }
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (fence == nullptr) {
        release_scene_textures(device, cache);
        SDL_ReleaseGPUTexture(device, target);
        destroy_scene_pipeline(device, pipeline);
        return false;
    }
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);
    Readback readback = readback_texture(device, target, width, height);
    bool ok = false;
    if (readback.mapped != nullptr) {
        core::Bytes canvas(static_cast<std::size_t>(width) * height * 4);
        std::memcpy(canvas.data(), readback.mapped, canvas.size());
        core::save_bmp_rgba(path, canvas, static_cast<std::uint32_t>(width),
                            static_cast<std::uint32_t>(height));
        ok = true;
    }
    release_readback(device, readback);
    release_scene_textures(device, cache);
    SDL_ReleaseGPUTexture(device, target);
    destroy_scene_pipeline(device, pipeline);
    return ok;
}


#endif

}  // namespace app
