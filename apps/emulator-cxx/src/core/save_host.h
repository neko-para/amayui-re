// 存档族的**宿主边界**：核心只定义接口，平台实现文件系统与表面捕捉。
//
// ★ 为什么 `read` 必须分三态：引擎把「打不开」与「打开成功但读失败」当成两个不同的状态码
//   （`0x1a0`：1 / 2；`0x1af`：1 / 2），而参考实现的 `read(): Uint8Array|null` 把两者合并了 ——
//   合并会让「文件不存在 + 表面选择 0」这类用例给出 2 而不是 1（与引擎相反）。
#pragma once

#include "core/bytes.h"

#include <cstdint>
#include <optional>
#include <string>

namespace core {

// 存档头（292 字节）里的七个整数：年 / 月 / 日 / 时 / 分 / 秒 / 游戏时间。
// ★ 前六项各是 16 位无符号；游戏时间是 32 位**原样**（可能为负）。星期（+268）与毫秒（+278）跳过。
struct SaveHeader {
    std::uint32_t year = 0;
    std::uint32_t month = 0;
    std::uint32_t day = 0;
    std::uint32_t hour = 0;
    std::uint32_t minute = 0;
    std::uint32_t second = 0;
    std::int32_t game_time = 0;
};

enum class SaveFileKind { Dat, Sth };

enum class ReadOutcome { Ok, OpenFailed, ReadFailed };
struct ReadResult {
    ReadOutcome outcome = ReadOutcome::OpenFailed;
    Bytes bytes;
};

enum class WriteOutcome { Ok, OpenFailed, WriteFailed };

// 一次表面捕捉：平台只提供「像素 + 行宽 + 通道掩码」。
// ★ 行序约定：**bottom-up**（与 BMP 同向）⇒ 核心的编码器不翻转；行距与通道掩码由平台给。
struct SurfacePixels {
    Bytes pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride_bytes = 0;
    std::uint32_t bytes_per_pixel = 0;
    std::uint32_t r_mask = 0;
    std::uint32_t g_mask = 0;
    std::uint32_t b_mask = 0;
};

class SaveHost {
public:
    virtual ~SaveHost() = default;
    // ① 用户的**本地**应用数据目录（不是漫游；`65-save.md` 的存档目录规则）。
    virtual std::string app_data_root() = 0;
    // ② 递归建目录（每次槽操作都先探一次）。
    virtual bool ensure_directory(const std::string& dir) = 0;
    // ③ 三态读。
    virtual ReadResult read(const std::string& path) = 0;
    // ④ CREATE_ALWAYS 语义（存在即截断）。
    virtual WriteOutcome write_create_always(const std::string& path, ByteSpan bytes) = 0;
    // ⑤ DeleteFileA 语义：**文件不存在也算失败**。
    virtual bool remove(const std::string& path) = 0;
    // ⑥ CopyFileA(..., bFailIfExists = 0)：**覆盖**目标，不因已存在而失败。
    virtual bool copy_overwrite(const std::string& from, const std::string& to) = 0;
    // ⑦ `0x19e` 的覆写确认；无 UI 的宿主返回 false（= 用户不同意）。
    virtual bool confirm_overwrite(std::int32_t slot) = 0;
    // ⑧ `0x1ae` 的表面捕捉；没有该对象 ⇒ nullopt。
    virtual std::optional<SurfacePixels> capture_surface(std::int32_t object_id) = 0;
};

}  // namespace core
