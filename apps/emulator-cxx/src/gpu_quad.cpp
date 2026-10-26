// 四边形管线：一张纹理铺满目标（冒烟测试 / 场景批次不可用时的兜底）。
//
// ★ 场景批次在 gpu_scene.cpp；这里只保留最简单的"整幅贴图 + 混合模式"。
//
// ── 着色器：一套管线、两个语言的源 ────────────────────────────────────────────
// SDL_GPU 的后端各认各的格式，本仓的搭配是：
//   · Windows / D3D12 → **DXIL**：HLSL 在运行时由 SDL_shadercross 编出来；
//   · Linux  / Vulkan → **SPIR-V**：同一份 HLSL，同样由 shadercross 编；
//   · macOS  / Metal  → **MSL**：SDL 的 `SDL_GPU_SHADERFORMAT_MSL` 收的是**源码文本**
//     （Metal 后端直接 `newLibraryWithSource:` 编译），所以 **macOS 不需要 shadercross**。
//     这一点很关键：shadercross 依赖 directx-dxc，而那个 vcpkg port 只支持 windows / linux-x64
//     ⇒ 想让 macOS 也走 GPU，只能喂 MSL。这也是 SDL 生态的标准做法（SDL_shadercross 的
//     README / ImGui 的 SDL_GPU3 后端都是这么分派的：设备支持 MSL 就发 MSL 源码）。
// ⇒ `compile_shader()` 按 `SDL_GetGPUShaderFormats(device)` 现场挑一套源。
// ★ 两份源描述**同一对管线**，改一个必须改另一个（否则两个平台画出不一样的东西）。
//   骨架照 SDL_shadercross 的输出写（入口 `main0`、varying `[[user(locnN)]]`、
//   纹理 `[[texture(0)]]` / 采样器 `[[sampler(0)]]`、顶点属性 `[[attribute(n)]]`），
//   这样将来若改用 shadercross 生成的 MSL，调用点一个字都不用动。
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "app.h"

#include <cstring>

namespace app {

namespace {

// ── 四边形：HLSL（shadercross 平台） ──
constexpr const char* kQuadHlslVertex = R"(
struct VSInput {
    float2 position : TEXCOORD0;
    float4 color : TEXCOORD1;
};
struct VSOutput {
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};
VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position.x / 640.0 - 1.0,
                             1.0 - input.position.y / 360.0,
                             0.0, 1.0);
    output.color = input.color;
    return output;
}
)";

constexpr const char* kQuadHlslFragment = R"(
Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);
float4 PSMain(float4 position : SV_Position, float4 uv : TEXCOORD0) : SV_Target {
    return tex.Sample(samp, uv.xy);
}
)";

// ── 四边形：MSL（Metal） ──
// 顶点属性 0/1 与 `create_quad_pipeline()` 的 vertex attribute location 一一对应；
// 目标分辨率 1280×720 是**逻辑分辨率**（与 HLSL 那份的 640.0 / 360.0 一致）。
// ★ vertex / fragment 分成两份源：一个 MSL 编译单元里放两个同名 entry 是不必要的风险。
constexpr const char* kQuadMslVertex = R"(
#include <metal_stdlib>
using namespace metal;

struct VSInput {
    float2 position [[attribute(0)]];
    float4 color    [[attribute(1)]];
};

struct VSOutput {
    float4 position [[position]];
    float4 color    [[user(locn0)]];
};

vertex VSOutput main0(VSInput input [[stage_in]]) {
    VSOutput output;
    output.position = float4(input.position.x / 640.0 - 1.0,
                             1.0 - input.position.y / 360.0,
                             0.0, 1.0);
    output.color = input.color;
    return output;
}
)";

constexpr const char* kQuadMslFragment = R"(
#include <metal_stdlib>
using namespace metal;

struct PSInput {
    float4 color [[user(locn0)]];
};

fragment float4 main0(PSInput input [[stage_in]],
                      texture2d<float> tex [[texture(0)]],
                      sampler samp [[sampler(0)]]) {
    return tex.sample(samp, input.color.xy);
}
)";

}  // namespace

SDL_GPUShader* compile_shader(SDL_GPUDevice* device, const ShaderPair& shaders,
                              SDL_GPUShaderStage stage, Uint32 num_samplers) {
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);

#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
    const SDL_ShaderCross_ShaderStage cross_stage = stage == SDL_GPU_SHADERSTAGE_VERTEX
        ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX
        : SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
    // 入口名是 HLSL 里的函数名；MSL 那套用 kShaderEntrypoint。
    const char* hlsl_entrypoint = stage == SDL_GPU_SHADERSTAGE_VERTEX ? "VSMain" : "PSMain";

    SDL_ShaderCross_HLSL_Info hlsl_info{};
    hlsl_info.source = shaders.hlsl;
    hlsl_info.entrypoint = hlsl_entrypoint;
    hlsl_info.include_dir = nullptr;
    hlsl_info.defines = nullptr;
    hlsl_info.shader_stage = cross_stage;
    hlsl_info.props = 0;

    // D3D12：直接 DXIL，避免 SPIRV 中转后的 root signature 绑定差异。
    if ((formats & SDL_GPU_SHADERFORMAT_DXIL) != 0) {
        size_t dxil_size = 0;
        void* dxil = SDL_ShaderCross_CompileDXILFromHLSL(&hlsl_info, &dxil_size);
        if (dxil == nullptr) {
            std::fprintf(stderr, "[emulator-cxx] shadercross: DXIL 编译失败: %s\n", SDL_GetError());
            return nullptr;
        }
        SDL_GPUShaderCreateInfo create_info{};
        create_info.code_size = dxil_size;
        create_info.code = static_cast<const Uint8*>(dxil);
        create_info.entrypoint = hlsl_entrypoint;
        create_info.format = SDL_GPU_SHADERFORMAT_DXIL;
        create_info.stage = stage;
        create_info.num_samplers = num_samplers;
        SDL_GPUShader* shader = SDL_CreateGPUShader(device, &create_info);
        SDL_free(dxil);
        if (shader == nullptr) {
            std::fprintf(stderr, "[emulator-cxx] shadercross: DXIL shader 创建失败: %s\n", SDL_GetError());
        }
        return shader;
    }

    if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
        size_t spirv_size = 0;
        void* spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl_info, &spirv_size);
        if (spirv == nullptr) {
            std::fprintf(stderr, "[emulator-cxx] shadercross: SPIRV 编译失败: %s\n", SDL_GetError());
            return nullptr;
        }

        auto* metadata = SDL_ShaderCross_ReflectGraphicsSPIRV(
            static_cast<const Uint8*>(spirv), spirv_size, 0);
        if (metadata == nullptr) {
            std::fprintf(stderr, "[emulator-cxx] shadercross: 反射失败: %s\n", SDL_GetError());
            SDL_free(spirv);
            return nullptr;
        }

        SDL_ShaderCross_SPIRV_Info info{
            static_cast<const Uint8*>(spirv),
            spirv_size,
            hlsl_entrypoint,
            cross_stage,
            0,
        };
        SDL_GPUShader* shader = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(
            device, &info, &metadata->resource_info, 0);
        SDL_free(metadata);
        SDL_free(spirv);
        if (shader == nullptr) {
            std::fprintf(stderr, "[emulator-cxx] shadercross: GPU shader 创建失败: %s\n", SDL_GetError());
        }
        return shader;
    }
#else
    (void)shaders.hlsl;  // 没有 shadercross ⇒ HLSL 这条路不存在（只剩 MSL）
#endif

    // Metal：MSL 是**源码**，Metal 自己在运行时编译（macOS 10.14+ 就能用；
    // METALLIB 那条要 macOS 14+，所以这里优先 MSL）。
    if ((formats & SDL_GPU_SHADERFORMAT_MSL) != 0) {
        SDL_GPUShaderCreateInfo create_info{};
        create_info.code_size = std::strlen(shaders.msl);
        create_info.code = reinterpret_cast<const Uint8*>(shaders.msl);
        create_info.entrypoint = kShaderEntrypoint;
        create_info.format = SDL_GPU_SHADERFORMAT_MSL;
        create_info.stage = stage;
        create_info.num_samplers = num_samplers;
        SDL_GPUShader* shader = SDL_CreateGPUShader(device, &create_info);
        if (shader == nullptr) {
            std::fprintf(stderr, "[emulator-cxx] MSL shader 创建失败: %s\n", SDL_GetError());
        }
        return shader;
    }

    std::fprintf(stderr,
                 "[emulator-cxx] 这台设备的着色器格式无法满足（formats=0x%X）："
                 "需要 DXIL / SPIR-V（配 shadercross）或 MSL（Metal）。\n",
                 static_cast<unsigned>(formats));
    return nullptr;
}

bool init_shader_toolchain() {
#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
    if (!SDL_ShaderCross_Init()) {
        log_sdl_error("SDL_ShaderCross_Init");
        return false;
    }
#endif
    return true;
}

void quit_shader_toolchain() {
#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
    SDL_ShaderCross_Quit();
#endif
}

bool create_quad_pipeline(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat target_format,
    SDL_GPUBlendOp color_op,
    SDL_GPUBlendFactor src_factor,
    SDL_GPUBlendFactor dst_factor,
    QuadPipeline& out) {
    const ShaderPair vertex_source{kQuadHlslVertex, kQuadMslVertex};
    const ShaderPair fragment_source{kQuadHlslFragment, kQuadMslFragment};
    SDL_GPUShader* vertex = compile_shader(device, vertex_source, SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fragment = compile_shader(device, fragment_source, SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    if (vertex == nullptr || fragment == nullptr) {
        if (vertex != nullptr) SDL_ReleaseGPUShader(device, vertex);
        if (fragment != nullptr) SDL_ReleaseGPUShader(device, fragment);
        return false;
    }

    SDL_GPUVertexBufferDescription buffer_description{};
    buffer_description.slot = 0;
    buffer_description.pitch = sizeof(float) * 6;
    buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attributes[2]{};
    attributes[0].location = 0;
    attributes[0].buffer_slot = 0;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].buffer_slot = 0;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[1].offset = sizeof(float) * 2;

    SDL_GPUVertexInputState vertex_input{};
    vertex_input.vertex_buffer_descriptions = &buffer_description;
    vertex_input.num_vertex_buffers = 1;
    vertex_input.vertex_attributes = attributes;
    vertex_input.num_vertex_attributes = 2;

    SDL_GPUColorTargetBlendState blend{};
    blend.enable_blend = true;
    blend.src_color_blendfactor = src_factor;
    blend.dst_color_blendfactor = dst_factor;
    blend.color_blend_op = color_op;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUColorTargetDescription target{};
    target.format = target_format;
    target.blend_state = blend;

    SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.vertex_shader = vertex;
    pipeline_info.fragment_shader = fragment;
    pipeline_info.vertex_input_state = vertex_input;
    pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipeline_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pipeline_info.target_info.color_target_descriptions = &target;
    pipeline_info.target_info.num_color_targets = 1;

    out.pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeline_info);
    SDL_ReleaseGPUShader(device, vertex);
    SDL_ReleaseGPUShader(device, fragment);
    if (out.pipeline == nullptr) {
        log_sdl_error("SDL_CreateGPUGraphicsPipeline");
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
    if (out.sampler == nullptr) {
        log_sdl_error("SDL_CreateGPUSampler");
        return false;
    }

    const std::array<float, 36> vertices{
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
        static_cast<float>(kWidth), 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
        0.0f, static_cast<float>(kHeight), 0.0f, 1.0f, 0.0f, 1.0f,
        static_cast<float>(kWidth), 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
        static_cast<float>(kWidth), static_cast<float>(kHeight), 1.0f, 1.0f, 0.0f, 1.0f,
        0.0f, static_cast<float>(kHeight), 0.0f, 1.0f, 0.0f, 1.0f,
    };

    SDL_GPUBufferCreateInfo buffer_info{};
    buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buffer_info.size = static_cast<Uint32>(vertices.size() * sizeof(float));
    out.vertex_buffer = SDL_CreateGPUBuffer(device, &buffer_info);
    if (out.vertex_buffer == nullptr) {
        log_sdl_error("SDL_CreateGPUBuffer");
        return false;
    }

    SDL_GPUTransferBufferCreateInfo upload_info{};
    upload_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    upload_info.size = buffer_info.size;
    SDL_GPUTransferBuffer* upload = SDL_CreateGPUTransferBuffer(device, &upload_info);
    if (upload == nullptr) {
        log_sdl_error("SDL_CreateGPUTransferBuffer(upload)");
        return false;
    }

    void* mapped = SDL_MapGPUTransferBuffer(device, upload, false);
    std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(float));
    SDL_UnmapGPUTransferBuffer(device, upload);

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTransferBufferLocation location{upload, 0};
    SDL_GPUBufferRegion region{out.vertex_buffer, 0, buffer_info.size};
    SDL_UploadToGPUBuffer(copy, &location, &region, true);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, upload);

    SDL_GPUTextureCreateInfo texture_info{};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = 2;
    texture_info.height = 2;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    out.texture = SDL_CreateGPUTexture(device, &texture_info);
    if (out.texture == nullptr) {
        log_sdl_error("SDL_CreateGPUTexture(2x2)");
        return false;
    }
    const std::array<Uint8, 16> texels{
        kTexR, kTexG, kTexB, kTexA, kTexR, kTexG, kTexB, kTexA,
        kTexR, kTexG, kTexB, kTexA, kTexR, kTexG, kTexB, kTexA,
    };
    SDL_GPUTransferBufferCreateInfo tex_upload_info{};
    tex_upload_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tex_upload_info.size = static_cast<Uint32>(texels.size());
    SDL_GPUTransferBuffer* tex_upload = SDL_CreateGPUTransferBuffer(device, &tex_upload_info);
    if (tex_upload == nullptr) {
        log_sdl_error("SDL_CreateGPUTransferBuffer(texture)");
        return false;
    }
    void* tex_mapped = SDL_MapGPUTransferBuffer(device, tex_upload, false);
    std::memcpy(tex_mapped, texels.data(), texels.size());
    SDL_UnmapGPUTransferBuffer(device, tex_upload);
    command = SDL_AcquireGPUCommandBuffer(device);
    copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo tex_source{};
    tex_source.transfer_buffer = tex_upload;
    tex_source.pixels_per_row = 2;
    tex_source.rows_per_layer = 2;
    SDL_GPUTextureRegion tex_region{};
    tex_region.texture = out.texture;
    tex_region.w = 2;
    tex_region.h = 2;
    tex_region.d = 1;
    SDL_UploadToGPUTexture(copy, &tex_source, &tex_region, true);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, tex_upload);

    return true;
}

void destroy_quad_pipeline(SDL_GPUDevice* device, QuadPipeline& pipeline) {
    if (pipeline.vertex_buffer != nullptr) SDL_ReleaseGPUBuffer(device, pipeline.vertex_buffer);
    if (pipeline.texture != nullptr) SDL_ReleaseGPUTexture(device, pipeline.texture);
    if (pipeline.sampler != nullptr) SDL_ReleaseGPUSampler(device, pipeline.sampler);
    if (pipeline.pipeline != nullptr) SDL_ReleaseGPUGraphicsPipeline(device, pipeline.pipeline);
    pipeline = {};
}

bool draw_quad(SDL_GPUCommandBuffer* command, SDL_GPUTexture* target, const QuadPipeline& pipeline) {
    SDL_GPUColorTargetInfo target_info{};
    target_info.texture = target;
    target_info.clear_color = kClearColor;
    target_info.load_op = SDL_GPU_LOADOP_CLEAR;
    target_info.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target_info, 1, nullptr);
    if (pass == nullptr) {
        log_sdl_error("SDL_BeginGPURenderPass");
        return false;
    }

    SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    SDL_Rect scissor{0, 0, kWidth, kHeight};
    SDL_SetGPUScissor(pass, &scissor);

    SDL_GPUTextureSamplerBinding sampler_binding{pipeline.texture, pipeline.sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &sampler_binding, 1);
    SDL_GPUBufferBinding binding{pipeline.vertex_buffer, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    SDL_BindGPUGraphicsPipeline(pass, pipeline.pipeline);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return true;
}

}  // namespace app
