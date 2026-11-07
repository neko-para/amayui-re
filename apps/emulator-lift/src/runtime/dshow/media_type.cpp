// emulator-lift · `AM_MEDIA_TYPE` + `VIDEOINFOHEADER` 在 guest 内存的构造 / 释放
//
// ★ 为什么要自己造：引擎的 `CMovieTextureRenderer::SetMediaType`（`sub_488A80`）只看四样东西
//   （doc §3-2，逐字读的）：
//     ① `pbFormat`（`0x488AB3 mov eax,[ecx+44h]`）非 0；
//     ② `biWidth`（`[+0x34]`）与 `biHeight`（`[+0x38]`，取绝对值 ⇒ 允许负高）；
//     ③ **subtype 是否等于 `MEDIASUBTYPE_ARGB32`** ⇒ 4 字节/像素；否则 3 字节 + `(w*3+3)&~3`；
//     ④ 纹理格式（`D3DFMT_A8R8G8B8` / `D3DFMT_A4R4G4B4`，不是本文件的事）。
//   `biBitCount` / `biCompression` / `biSizeImage` / `dwBitRate` **没有任何读取**
//   ⇒ 它们只为形态正确（以及"别让下游误会"）而填，⛔ 不要拿它们当判据。
// ★ 两块都在**guest 堆**上（guest 代码会解引用它们）；释放必须走 `freeMediaType`。
#include "runtime/dshow/dshow.h"

#include <cstdio>

#include "runtime/heap.h"
#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift::dshow {
namespace {

void writeGuid(X86& cpu, std::uint32_t ea, const Guid& g) {
    cpu.mem->wr32(ea, g.data1);
    cpu.mem->wr16(ea + 4u, g.data2);
    cpu.mem->wr16(ea + 6u, g.data3);
    for (std::uint32_t i = 0; i < 8u; ++i) cpu.mem->wr8(ea + 8u + i, g.data4[i]);
}

}  // namespace

std::uint32_t strideForRgb24(std::uint32_t width) { return (width * 3u + 3u) & ~3u; }

bool buildVideoMediaType(X86& cpu, std::uint32_t width, std::uint32_t height, std::int64_t avg_time_100ns,
                         MediaTypeBlock* out, std::string* err) {
    if (out == nullptr) {
        if (err) *err = "buildVideoMediaType: out 为空";
        return false;
    }
    if (cpu.heap == nullptr || !cpu.heap->ready()) {
        if (err) *err = "buildVideoMediaType: guest 堆没就绪（夹具要先 init heap）";
        return false;
    }
    if (width == 0 || height == 0) {
        if (err) *err = "buildVideoMediaType: 宽/高为 0（帧源的 info() 没给？）";
        return false;
    }
    const std::uint32_t stride = strideForRgb24(width);
    const std::uint32_t size_image = stride * height;

    const std::uint32_t mt = cpu.heap->alloc(amt::kBytes, /*zero=*/true);
    if (mt == 0) {
        if (err) *err = std::string("buildVideoMediaType: guest 堆分配 AM_MEDIA_TYPE 失败：") + cpu.heap->lastError();
        return false;
    }
    const std::uint32_t vi = cpu.heap->alloc(vih::kBytes, /*zero=*/true);
    if (vi == 0) {
        if (err) *err = std::string("buildVideoMediaType: guest 堆分配 VIDEOINFOHEADER 失败：") + cpu.heap->lastError();
        cpu.heap->free(mt);
        return false;
    }

    // ---- AM_MEDIA_TYPE ----
    writeGuid(cpu, mt + amt::kMajorType, kMajorTypeVideo);
    writeGuid(cpu, mt + amt::kSubType, kSubTypeRgb24);
    cpu.mem->wr32(mt + amt::kFixedSizeSamples, 1u);      // 每帧定长
    cpu.mem->wr32(mt + amt::kTemporalCompression, 0u);   // 帧内不再压缩
    cpu.mem->wr32(mt + amt::kSampleSize, size_image);    // RGB24 一帧的字节数
    writeGuid(cpu, mt + amt::kFormatType, kFormatVideoInfo);
    cpu.mem->wr32(mt + amt::kUnk, 0u);
    cpu.mem->wr32(mt + amt::kCbFormat, vih::kBytes);     // = sizeof(VIDEOINFOHEADER)
    cpu.mem->wr32(mt + amt::kPbFormat, vi);              // ★ 渲染器第一件事就查这个非 0

    // ---- VIDEOINFOHEADER ----
    // rcSource / rcTarget 全 0 = "整帧"（DirectShow 的惯例）。
    cpu.mem->wr32(vi + vih::kBitRate, 0u);
    cpu.mem->wr32(vi + vih::kBitErrorRate, 0u);
    cpu.mem->wr64(vi + vih::kAvgTimePerFrame, static_cast<std::uint64_t>(avg_time_100ns < 0 ? 0 : avg_time_100ns));

    const std::uint32_t bmi = vi + vih::kBmiHeader;
    cpu.mem->wr32(bmi + bmi::kSize, bmi::kBytes);  // sizeof(BITMAPINFOHEADER) = 40
    cpu.mem->wr32(bmi + bmi::kWidth, width);
    // ★ **biHeight 传正数**（doc §3-2 的 `0x488AC5 xor/sbb` 取绝对值 ⇒ 允许负高；
    //   但 DIB 的行序由缓冲内容决定，渲染器自己按 `base + (h-1-y)*stride` 读）。
    cpu.mem->wr32(bmi + bmi::kHeight, height);
    cpu.mem->wr16(bmi + bmi::kPlanes, 1u);
    cpu.mem->wr16(bmi + bmi::kBitCount, 24u);
    cpu.mem->wr32(bmi + bmi::kCompression, 0u);  // BI_RGB —— ⛔ 不是判据（doc §3-2 末行）
    cpu.mem->wr32(bmi + bmi::kSizeImage, size_image);

    out->media_type_ea = mt;
    out->video_info_ea = vi;
    out->bytes = amt::kBytes + vih::kBytes;
    return true;
}

void freeMediaType(X86& cpu, MediaTypeBlock* mt) {
    if (mt == nullptr) return;
    if (cpu.heap == nullptr) {
        mt->media_type_ea = 0;
        mt->video_info_ea = 0;
        mt->bytes = 0;
        return;
    }
    if (mt->video_info_ea != 0) {
        cpu.heap->free(mt->video_info_ea);
        mt->video_info_ea = 0;
    }
    if (mt->media_type_ea != 0) {
        cpu.heap->free(mt->media_type_ea);
        mt->media_type_ea = 0;
    }
    mt->bytes = 0;
}

}  // namespace lift::dshow
