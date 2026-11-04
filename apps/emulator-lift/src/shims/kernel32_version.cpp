// emulator-lift · version.dll 的版本资源查询（读镜像自己的 `.rsrc` / RT_VERSION）
//
// ★ 口径（plan §4 D6 / §6 P0 + 用户 2026-11-04 改）：
//   · 版本信息读**镜像自己的** `VS_VERSIONINFO`：`GetFileVersionInfoSizeA` 返回该块的字节数，
//     `GetFileVersionInfoA` 把它拷进 guest 缓冲，`VerQueryValueA("\\")` 返回块内
//     `VS_FIXEDFILEINFO`（签名 0xFEEF04BD / 52 字节）的 guest 指针与长度。
//   · **AGERC.DLL 的版本**：AGERC 现在是**真实装载的 guest 模块**（`Machine::loadAgercModule`）⇒
//     有镜像就直接读 `cpu.shim.agerc_image` 的 RT_VERSION（⛔ 不重读宿主文件）；没有镜像就按路径
//     走 VFS / `loadPeFile`。读不到资源 ⇒ `ERROR_RESOURCE_TYPE_NOT_FOUND(1813)`；文件不存在 ⇒
//     `ERROR_FILE_NOT_FOUND(2)`。⛔ 不再 hard error（2026-11-04 前的口径已废止）。
//     ★ 实测（`.lst`）：`sub_48E640` 就是拿 `"AGERC.DLL"` 调 `GetFileVersionInfoSizeA`（0x48E65E），
//       再 `VerQueryValueA("\\StringFileInfo\\041103a4\\FileVersion")`（0x48E6AA），把 `FileVersion`
//       里第一个逗号前的整数 (`atoi`，实测 "39.1.0.0" ⇒ 39) 存进 `RCDLL+14h`；`_WinMain` 在
//       0x4BAF26 拿它与 `set:RCVersion` 比较 ⇒ **这个检查在真机上是"AGERC 的版本"**。
//   · 字符串子块（`\StringFileInfo\041103a4\FileVersion` 之类）已实现（P0）；AGE.EXE 的两个
//     调用点（0x4B8E5D / 0x4B8F2D）问的也是这两种子块。
//   · 其它路径：VFS 解析后 `loadPeFile` + `PeResources`；没有版本资源 ⇒ Windows 的真实回答
//     **FALSE + ERROR_RESOURCE_TYPE_NOT_FOUND(1813)**；文件不存在 ⇒ FALSE + ERROR_FILE_NOT_FOUND(2)。
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/pe_resource.h"
#include "runtime/vfs.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::uint32_t kErrFileNotFound = 2u;
constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrInsufficientBuffer = 122u;
constexpr std::uint32_t kErrResourceTypeNotFound = 1813u;

constexpr const char* kVersion = "version";

// 版本块的来源（日志 / 诊断用）
struct VersionBlock {
    std::vector<std::uint8_t> bytes;  // 整块 VS_VERSIONINFO（= GetFileVersionInfoSize 的返回值）
    std::string source;               // "AGE.EXE（cpu.image，不重读）" / 宿主路径
};

void requireMem(X86& cpu, const char* api) {
    if (!cpu.mem) shimAbort(kVersion, api, "cpu.mem 为空（Machine 没装配 guest 内存）");
}

std::string asciiLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

std::string baseName(const std::string& path) {
    const auto pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// 去扩展名（`.exe` / `.dll`）的模块键：`AGE.EXE` ⇒ `age`
std::string moduleStem(std::string base) {
    base = asciiLower(base);
    for (const char* ext : {".exe", ".dll"}) {
        const std::size_t n = std::strlen(ext);
        if (base.size() > n && base.compare(base.size() - n, n, ext) == 0) {
            base.resize(base.size() - n);
            break;
        }
    }
    return base;
}

// "这个路径是不是指向 AGERC"（指向 AGERC ⇒ 优先用**真实装载的镜像**，见 loadVersionBlock）
bool looksLikeAgerc(const std::string& path) {
    const std::string base = baseName(path);
    return asciiLower(base) == "agerc.dll" || moduleStem(base) == "agerc";
}

// 同一个文件名的两种写法（guest 路径 vs 宿主路径 / cpu.image->path）
bool sameImageFile(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    const std::string ba = asciiLower(baseName(a));
    const std::string bb = asciiLower(baseName(b));
    if (ba == bb) return true;
    const std::string sa = moduleStem(ba);
    const std::string sb = moduleStem(bb);
    return !sa.empty() && sa == sb;
}

// guest 路径 → 版本块。失败时**自己设好 last_error 并打日志**（调用方只管返回 0）。
bool loadVersionBlock(X86& cpu, const std::string& guest_path, const char* api, std::uint32_t call_ea,
                      VersionBlock* out) {
    (void)call_ea;  // 2026-11-04 起 AGERC 不再 hard error ⇒ 调用点 EA 只留在日志/历史里
    out->bytes.clear();
    out->source.clear();
    if (guest_path.empty()) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog(kVersion, api, "lptstrFilename 为 NULL / 空串 ⇒ 返回 0 + 错误 87");
        return false;
    }
    // ★ 2026-11-04 改口径：AGERC.DLL 是**真实装载的 guest 模块**。
    //   有镜像 ⇒ 直接读它的 `.rsrc / RT_VERSION`（⛔ 不重读宿主）；没有镜像 ⇒ 按路径走 VFS/文件读。
    //   读不到 ⇒ 1813；文件不存在 ⇒ 2。⛔ 不再 hard error。
    if (looksLikeAgerc(guest_path)) {
        if (cpu.shim.agerc_image != nullptr && !cpu.shim.agerc_image->bytes.empty()) {
            PeResources res;
            std::string err;
            if (!res.init(*cpu.shim.agerc_image, &err) || !res.versionInfo(&out->bytes, &err)) {
                cpu.shim.last_error = kErrResourceTypeNotFound;
                shimLog(kVersion, api,
                        "\"%s\" 解析到**真实装载的 AGERC 镜像**（base=0x%08X），但读不到 RT_VERSION（%s）"
                        " ⇒ 返回 0 + 错误 1813",
                        guest_path.c_str(), cpu.shim.agerc_base, err.c_str());
                return false;
            }
            out->source = "AGERC.DLL（cpu.shim.agerc_image，真实装载，不重读宿主）";
            return true;
        }
        shimLog(kVersion, api,
                "AGERC.DLL 没有装载进 guest（agerc_image 为空）⇒ 按**文件**读（VFS）；"
                "读不到 ⇒ 1813 / 文件不存在 ⇒ 2");
    }

    // ① 主镜像（basename 相同 ⇒ 直接用已经载入的那份，⛔ 不重读）
    if (cpu.image && !cpu.image->bytes.empty() && sameImageFile(guest_path, cpu.image->path)) {
        PeResources res;
        std::string err;
        if (!res.init(*cpu.image, &err)) {
            cpu.shim.last_error = kErrResourceTypeNotFound;
            shimLog(kVersion, api, "AGE.EXE 的资源目录解析失败（%s）⇒ 返回 0 + 错误 1813", err.c_str());
            return false;
        }
        if (!res.versionInfo(&out->bytes, &err)) {
            cpu.shim.last_error = kErrResourceTypeNotFound;
            shimLog(kVersion, api,
                    "\"%s\" 解析到主镜像 AGE.EXE，但它没有可用的 RT_VERSION（%s）⇒ 返回 0 + 错误 1813",
                    guest_path.c_str(), err.c_str());
            return false;
        }
        out->source = "AGE.EXE（cpu.image，不重读）";
        return true;
    }

    // ② 其它路径：VFS 解析到宿主，再按 PE 读
    if (!cpu.vfs) {
        shimAbort(kVersion, api, "Machine 没有装配 VFS，无法把 guest 路径 \"%s\" 映射到宿主",
                  guest_path.c_str());
    }
    std::string host;
    std::string verr;
    // ★ must_exist = true：真机对"文件不存在"的回答是 FALSE + ERROR_FILE_NOT_FOUND(2)，
    //   ⛔ 不是"打不开文件 ⇒ 没有版本资源(1813)"（那是**文件存在但没有 VS_VERSIONINFO**）。
    if (!cpu.vfs->resolve(guest_path, &host, &verr, true, true)) {
        cpu.shim.last_error = kErrFileNotFound;
        shimLog(kVersion, api,
                "VFS 解析不了 \"%s\"（%s）⇒ 返回 0 + 错误 2（ERROR_FILE_NOT_FOUND；真机同样失败）",
                guest_path.c_str(), verr.c_str());
        return false;
    }
    // 解析出来的宿主路径如果就是主镜像（例如 GetModuleFileNameA 给的是安装树里的 AGE.EXE）
    // ⇒ 仍用已经载入的那份（缓存不重读，且避免读到壳里的假资源）
    if (cpu.image && !cpu.image->bytes.empty() && sameImageFile(host, cpu.image->path)) {
        PeResources res;
        std::string err;
        if (!res.init(*cpu.image, &err) || !res.versionInfo(&out->bytes, &err)) {
            cpu.shim.last_error = kErrResourceTypeNotFound;
            shimLog(kVersion, api, "\"%s\" → \"%s\" 是主镜像，但读不到 RT_VERSION（%s）⇒ 返回 0 + 错误 1813",
                    guest_path.c_str(), host.c_str(), err.c_str());
            return false;
        }
        out->source = "AGE.EXE（cpu.image，宿主路径命中主镜像）";
        return true;
    }

    PeImage img;
    std::string lerr;
    if (!loadPeFile(host, &img, &lerr)) {
        cpu.shim.last_error = kErrResourceTypeNotFound;
        shimLog(kVersion, api, "\"%s\" → \"%s\" 不是可解析的 PE（%s）⇒ 返回 0 + 错误 1813",
                guest_path.c_str(), host.c_str(), lerr.c_str());
        return false;
    }
    PeResources res;
    std::string rerr;
    if (!res.init(img, &rerr)) {
        cpu.shim.last_error = kErrResourceTypeNotFound;
        shimLog(kVersion, api, "\"%s\" 的资源目录坏了（%s）⇒ 返回 0 + 错误 1813", host.c_str(),
                rerr.c_str());
        return false;
    }
    if (!res.versionInfo(&out->bytes, &rerr)) {
        cpu.shim.last_error = kErrResourceTypeNotFound;
        shimLog(kVersion, api, "\"%s\" 没有可用的 RT_VERSION（%s）⇒ 返回 0 + 错误 1813",
                host.c_str(), rerr.c_str());
        return false;
    }
    out->source = host;
    return true;
}

// ---- GetFileVersionInfoSizeA(path, lpdwHandle) ----
void shimGetFileVersionInfoSizeA(X86& cpu, ShimCall& call) {
    const char* api = "GetFileVersionInfoSizeA";
    requireMem(cpu, api);
    const std::string path = call.str(0);
    const std::uint32_t handle_ea = call.u32(1);
    VersionBlock vb;
    if (!loadVersionBlock(cpu, path, api, call.ret_ea, &vb)) {
        call.result = 0;
        return;
    }
    if (handle_ea != 0) {
        if (!cpu.mem->committed(handle_ea, 4)) {
            shimAbort(kVersion, api, "lpdwHandle = 0x%08X 未提交（guest 出参非法）", handle_ea);
        }
        cpu.mem->wr32(handle_ea, 0);  // 文档：保留参数，填 0
    }
    shimLog(kVersion, api,
            "\"%s\" → VS_VERSIONINFO = %zu 字节（来源：%s；D6：读镜像自己的资源）", path.c_str(),
            vb.bytes.size(), vb.source.c_str());
    call.result = static_cast<std::uint32_t>(vb.bytes.size());
}

// ---- GetFileVersionInfoA(path, dwHandle, dwLen, lpData) ----
void shimGetFileVersionInfoA(X86& cpu, ShimCall& call) {
    const char* api = "GetFileVersionInfoA";
    requireMem(cpu, api);
    const std::string path = call.str(0);
    const std::uint32_t dw_handle = call.u32(1);  // 文档：忽略
    const std::uint32_t dw_len = call.u32(2);
    const std::uint32_t data_ea = call.u32(3);
    VersionBlock vb;
    if (!loadVersionBlock(cpu, path, api, call.ret_ea, &vb)) {
        call.result = 0;
        return;
    }
    const std::size_t need = vb.bytes.size();
    if (data_ea == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog(kVersion, api, "lpData 为 NULL ⇒ 返回 0 + 错误 87");
        call.result = 0;
        return;
    }
    if (dw_len < need) {
        cpu.shim.last_error = kErrInsufficientBuffer;
        shimLog(kVersion, api,
                "dwLen=%u 小于版本块大小 %zu ⇒ 返回 0 + 错误 122（ERROR_INSUFFICIENT_BUFFER）", dw_len,
                need);
        call.result = 0;
        return;
    }
    if (!cpu.mem->committed(data_ea, need)) {
        shimAbort(kVersion, api, "lpData = 0x%08X 起 %zu 字节未提交（guest 缓冲区非法）", data_ea, need);
    }
    cpu.mem->writeBytes(data_ea, vb.bytes.data(), need);
    if (dw_handle != 0) {
        shimLog(kVersion, api, "dwHandle=0x%08X 被忽略（文档：这个参数保留）", dw_handle);
    }
    shimLog(kVersion, api, "把 \"%s\" 的 VS_VERSIONINFO（%zu 字节，来源：%s）拷进 guest 0x%08X（dwLen=%u）",
            path.c_str(), need, vb.source.c_str(), data_ea, dw_len);
    call.result = 1;
}


// ---- VS_VERSIONINFO 的字符串子块（P0：AGERC 的 FileVersion 需要它） ----
struct VerNode {
    std::size_t off = 0;
    std::uint16_t w_length = 0;
    std::uint16_t w_value_length = 0;
    std::uint16_t w_type = 0;
    std::string key;
    std::size_t value_off = 0;
    std::size_t value_bytes = 0;
    std::size_t child_off = 0;
};

std::size_t align4(std::size_t v) { return (v + 3u) & ~static_cast<std::size_t>(3u); }

bool readVerNode(const std::vector<std::uint8_t>& b, std::size_t off, std::size_t limit, VerNode* n,
                 std::string* err) {
    if (off + 6 > limit || off + 6 > b.size()) {
        if (err) *err = "VS_VERSIONINFO 节点头越界";
        return false;
    }
    auto rd16 = [&](std::size_t p) {
        return static_cast<std::uint16_t>(b[p] | (static_cast<std::uint16_t>(b[p + 1]) << 8));
    };
    n->off = off;
    n->w_length = rd16(off);
    n->w_value_length = rd16(off + 2);
    n->w_type = rd16(off + 4);
    if (n->w_length < 6 || off + n->w_length > limit || off + n->w_length > b.size()) {
        if (err) *err = "VS_VERSIONINFO 节点的 wLength 越界";
        return false;
    }
    std::size_t key_off = off + 6;
    std::string key;
    while (key_off + 1 < off + n->w_length) {
        const char16_t c = static_cast<char16_t>(b[key_off] | (static_cast<std::uint16_t>(b[key_off + 1]) << 8));
        key_off += 2;
        if (c == 0) break;
        if (c > 0x7E) {
            if (err) *err = "VS_VERSIONINFO 的键不是 ASCII";
            return false;
        }
        key.push_back(static_cast<char>(c));
    }
    n->key = key;
    n->value_off = align4(key_off);
    if (n->value_off > off + n->w_length) {
        if (err) *err = "VS_VERSIONINFO 的 value 偏移越界";
        return false;
    }
    // wType=1（文本）时 wValueLength 是 WCHAR 数；否则是字节数。
    n->value_bytes = (n->w_type == 1) ? static_cast<std::size_t>(n->w_value_length) * 2u
                                      : static_cast<std::size_t>(n->w_value_length);
    if (n->value_off + n->value_bytes > off + n->w_length) {
        // 有些链接器把字符串的 wValueLength 写成字节数；按 wLength 截断，仍给调用方一个合法区间。
        n->value_bytes = (off + n->w_length > n->value_off) ? (off + n->w_length - n->value_off) : 0;
    }
    n->child_off = align4(n->value_off + n->value_bytes);
    return true;
}

bool asciiEqualNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
    }
    return true;
}

bool findVerChild(const std::vector<std::uint8_t>& b, const VerNode& parent, const std::string& key,
                  VerNode* out, std::string* err) {
    std::size_t off = parent.child_off;
    const std::size_t end = parent.off + parent.w_length;
    while (off + 6 <= end) {
        VerNode n;
        if (!readVerNode(b, off, end, &n, err)) return false;
        if (n.w_length == 0) break;
        if (asciiEqualNoCase(n.key, key)) {
            *out = n;
            return true;
        }
        off = align4(off + n.w_length);
    }
    if (err) *err = "VS_VERSIONINFO 里没有键 \"" + key + "\"";
    return false;
}

// \StringFileInfo\<langcp>\<Field> ⇒ 块内偏移 + 字符数（不含结尾 NUL 的保守值）
bool findVersionString(const std::vector<std::uint8_t>& b, const std::string& sub, std::size_t* off,
                       std::uint32_t* chars, std::string* err) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : sub) {
        if (c == '\\') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.size() != 3 || !asciiEqualNoCase(parts[0], "StringFileInfo")) {
        if (err) *err = "只支持 \\StringFileInfo\\<langcp>\\<Field>";
        return false;
    }
    VerNode root;
    if (!readVerNode(b, 0, b.size(), &root, err)) return false;
    VerNode sfi;
    if (!findVerChild(b, root, "StringFileInfo", &sfi, err)) return false;
    VerNode table;
    if (!findVerChild(b, sfi, parts[1], &table, err)) return false;
    VerNode field;
    if (!findVerChild(b, table, parts[2], &field, err)) return false;
    *off = field.value_off;
    *chars = static_cast<std::uint32_t>((field.value_bytes / 2u));
    if (*chars == 0) {
        if (err) *err = "字符串子块的 wValueLength=0";
        return false;
    }
    return true;
}

// ---- VerQueryValueA(pBlock, lpSubBlock, lplpBuffer, puLen) ----
void shimVerQueryValueA(X86& cpu, ShimCall& call) {
    const char* api = "VerQueryValueA";
    requireMem(cpu, api);
    const std::uint32_t block_ea = call.u32(0);
    const std::string sub = call.str(1);
    const std::uint32_t buf_out_ea = call.u32(2);
    const std::uint32_t len_out_ea = call.u32(3);
    if (block_ea == 0 || buf_out_ea == 0 || len_out_ea == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog(kVersion, api, "pBlock / lplpBuffer / puLen 有 NULL ⇒ 返回 0 + 错误 87");
        call.result = 0;
        return;
    }
    if (!cpu.mem->committed(buf_out_ea, 4) || !cpu.mem->committed(len_out_ea, 4)) {
        shimAbort(kVersion, api, "lplpBuffer=0x%08X / puLen=0x%08X 未提交（guest 出参非法）",
                  buf_out_ea, len_out_ea);
    }
    // 文档：失败时 *lplpBuffer = NULL、*puLen = 0
    cpu.mem->wr32(buf_out_ea, 0);
    cpu.mem->wr32(len_out_ea, 0);

    // 块头：wLength(2) / wValueLength(2) / wType(2)；长度不符 ⇒ 这不是 GetFileVersionInfoA 填的块
    if (!cpu.mem->committed(block_ea, 6)) {
        shimAbort(kVersion, api, "pBlock = 0x%08X 起 6 字节未提交（不是 GetFileVersionInfoA 填的块）",
                  block_ea);
    }
    const std::uint16_t w_length = cpu.mem->rd16(block_ea);
    const std::uint16_t w_value_length = cpu.mem->rd16(block_ea + 2);
    if (w_length < 6) {
        cpu.shim.last_error = kErrResourceTypeNotFound;
        shimLog(kVersion, api, "pBlock 的 wLength=%u < 6 ⇒ 不是 VS_VERSIONINFO ⇒ 返回 0 + 错误 1813",
                w_length);
        call.result = 0;
        return;
    }
    if (!cpu.mem->committed(block_ea, w_length)) {
        shimAbort(kVersion, api, "pBlock = 0x%08X 起 wLength=%u 字节未提交（块被截断）", block_ea,
                  w_length);
    }
    std::vector<std::uint8_t> block(w_length);
    for (std::uint32_t i = 0; i < w_length; ++i) block[i] = cpu.mem->rd8(block_ea + i);

    // 字符串子块（引擎对 AGERC 的 FileVersion 用它；`\` 仍走 VS_FIXEDFILEINFO）
    if (sub != "\\") {
        std::size_t str_off = 0;
        std::uint32_t chars = 0;
        std::string serr;
        if (!findVersionString(block, sub, &str_off, &chars, &serr)) {
            cpu.shim.last_error = kErrResourceTypeNotFound;
            shimLog(kVersion, api, "子块 \"%s\" 解析失败（%s）⇒ 返回 0 + 错误 1813", sub.c_str(),
                    serr.c_str());
            call.result = 0;
            return;
        }
        const std::uint32_t str_ea = block_ea + static_cast<std::uint32_t>(str_off);
        cpu.mem->wr32(buf_out_ea, str_ea);
        cpu.mem->wr32(len_out_ea, chars);
        shimLog(kVersion, api,
                "\"%s\" 命中字符串子块（块内 +0x%zX，%u 字符）⇒ guest 指针 0x%08X",
                sub.c_str(), str_off, chars, str_ea);
        call.result = 1;
        return;
    }

    std::size_t off = 0;
    std::string err;
    if (!PeResources::findFixedFileInfo(block, &off, &err)) {
        cpu.shim.last_error = kErrResourceTypeNotFound;
        shimLog(kVersion, api, "\"\\\\\" 失败：%s ⇒ 返回 0 + 错误 1813", err.c_str());
        call.result = 0;
        return;
    }
    const std::uint32_t ffi_ea = block_ea + static_cast<std::uint32_t>(off);
    std::uint32_t ms = 0;
    std::uint32_t ls = 0;
    // dwFileVersionMS / dwFileVersionLS 紧跟签名后的 dwStrucVersion（各 4 字节）
    if (off + 16 <= block.size()) {
        std::memcpy(&ms, block.data() + off + 8, 4);
        std::memcpy(&ls, block.data() + off + 12, 4);
    }
    cpu.mem->wr32(buf_out_ea, ffi_ea);
    cpu.mem->wr32(len_out_ea, kVsFixedFileInfoSize);
    shimLog(kVersion, api,
            "\"\\\\\" 命中 VS_FIXEDFILEINFO（块内 +0x%zX，%u 字节；wValueLength=%u；文件版本 %u.%u.%u.%u）"
            " ⇒ guest 指针 0x%08X",
            off, kVsFixedFileInfoSize, w_value_length, (ms >> 16) & 0xFFFFu, ms & 0xFFFFu,
            (ls >> 16) & 0xFFFFu, ls & 0xFFFFu, ffi_ea);
    call.result = 1;
}

}  // namespace

void registerVersionShims(ShimRegistry& s) {
    // ★ 已经是**实现**（不是 stub）：读 AGE.EXE / 真实装载的 AGERC 镜像的 RT_VERSION；
    //   读不到 ⇒ 真实失败码 1813 / 文件不存在 ⇒ 2（⛔ 不再 hard error）。
    s.add("version", "GetFileVersionInfoSizeA", Conv::Stdcall, 2, &shimGetFileVersionInfoSizeA);
    s.add("version", "GetFileVersionInfoA", Conv::Stdcall, 4, &shimGetFileVersionInfoA);
    s.add("version", "VerQueryValueA", Conv::Stdcall, 4, &shimVerQueryValueA);
}

}  // namespace lift
