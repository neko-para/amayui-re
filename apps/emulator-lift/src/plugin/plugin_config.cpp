// emulator-lift · `ctx.config` 的宿主实现（JSON 文本 ↔ 中立值树；口径见头注释）
//
// ★ 本文件是**唯一** include `<nlohmann/json.hpp>` 的插件侧 TU（`plugin_config.h` 用 PIMPL）。
// ★ 失败面分两类，⛔ 不要合并：
//   · **装载失败**（attach 返回 false）= 文件的形状坏 / 读不到 / 太大 / 值超限 / 非法 UTF-8 ⇒ exit 2；
//     值上限（深度 / 节点 / 串长）在 attach 与 set **两侧都执行**（attach 用显式栈，⛔ 不在 get 爆栈）。
//   · **写盘失败**（flush 返回一行 warn）= 运行期 I/O；第一次失败后不再重试（⛔ 不重试风暴）。
//     序列化 / 写出超限不是 I/O ⇒ 只 warn、**不**永久熔断后续合法写入。
// ★ `ConfigReceipt` 只表达**内存态**（`applied` / `read_only` / `no_appdata`）；⛔ 没有不可达的 `io`。
#include "plugin/plugin_config.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include "runtime/host_encoding.h"

namespace lift::plugin {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

fs::path pathFromUtf8(const std::string& text) {
#ifdef _WIN32
    return fs::path(lift::utf8ToWide(text));
#else
    return fs::path(text);
#endif
}

std::string pathToUtf8(const fs::path& p) {
#ifdef _WIN32
    return lift::wideToUtf8(p.wstring().c_str());
#else
    return p.string();
#endif
}

bool isReservedName(const std::string& name) {
    return name == "__proto__" || name == "prototype" || name == "constructor";
}

bool hasControlBytes(const std::string& text) {
    for (const unsigned char c : text) {
        if (c == 0 || c < 0x20 || c == 0x7f) return true;
    }
    return false;
}

bool isDomainIdChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-';
}

// JSON → 中立值树。★ **带深度/节点护栏**：`attach` 已经校验过（见 `validateJsonValue`），这里是
//   第二条保险 —— 即使将来有人绕过 attach，也只会返回 false，⛔ 不会在 `get` 里把栈递归爆。
bool fromJson(const json& j, std::size_t depth, std::size_t* nodes, ConfigValue* out) {
    if (depth > kConfigMaxDepth) return false;
    if (++(*nodes) > kConfigMaxNodes) return false;
    if (j.is_null()) {
        *out = ConfigValue::null();
        return true;
    }
    if (j.is_boolean()) {
        *out = ConfigValue::fromBool(j.get<bool>());
        return true;
    }
    if (j.is_number()) {
        *out = ConfigValue::fromNumber(j.get<double>());
        return true;
    }
    if (j.is_string()) {
        *out = ConfigValue::fromString(j.get<std::string>());
        return true;
    }
    if (j.is_array()) {
        ConfigValue arr = ConfigValue::array();
        arr.items.reserve(j.size());
        for (const json& item : j) {
            ConfigValue child;
            if (!fromJson(item, depth + 1, nodes, &child)) return false;
            arr.items.push_back(std::move(child));
        }
        *out = std::move(arr);
        return true;
    }
    ConfigValue object = ConfigValue::object();
    if (j.is_object()) {
        object.members.reserve(j.size());
        for (auto it = j.begin(); it != j.end(); ++it) {
            ConfigValue child;
            if (!fromJson(it.value(), depth + 1, nodes, &child)) return false;
            object.members.emplace_back(it.key(), std::move(child));
        }
    }
    *out = std::move(object);
    return true;
}

json toJson(const ConfigValue& v) {
    switch (v.kind) {
        case ConfigValueKind::Null:
            return json(nullptr);
        case ConfigValueKind::Bool:
            return json(v.boolean);
        case ConfigValueKind::Number: {
            // ★ 整数按整数写（否则 `slot: 10` 会变成 `10.0`）；判据 = 在 2^53 内且无小数。
            constexpr double kMaxExactInteger = 9007199254740992.0;
            if (std::isfinite(v.number) && std::trunc(v.number) == v.number &&
                v.number >= -kMaxExactInteger && v.number <= kMaxExactInteger) {
                return json(static_cast<std::int64_t>(v.number));
            }
            return json(v.number);
        }
        case ConfigValueKind::String:
            return json(v.text);
        case ConfigValueKind::Array: {
            json out = json::array();
            for (const ConfigValue& item : v.items) out.push_back(toJson(item));
            return out;
        }
        case ConfigValueKind::Object: {
            json out = json::object();
            for (const auto& member : v.members) out[member.first] = toJson(member.second);
            return out;
        }
    }
    return json(nullptr);
}

// 值树校验：类型 / 有限数字 / 深度 / 节点数 / 字符串长度。false ⇒ `*why` 是原因（JS 侧 TypeError）。
bool validateValue(const ConfigValue& v, std::size_t depth, std::size_t* nodes, std::string* why) {
    if (depth > kConfigMaxDepth) {
        *why = "值嵌套太深（上限 " + std::to_string(kConfigMaxDepth) + " 层）";
        return false;
    }
    if (++(*nodes) > kConfigMaxNodes) {
        *why = "值太大（节点上限 " + std::to_string(kConfigMaxNodes) + "）";
        return false;
    }
    switch (v.kind) {
        case ConfigValueKind::Null:
        case ConfigValueKind::Bool:
            return true;
        case ConfigValueKind::Number:
            if (!std::isfinite(v.number)) {
                *why = "数字必须是有限的（NaN / Infinity 不支持）";
                return false;
            }
            return true;
        case ConfigValueKind::String:
            if (v.text.size() > kConfigMaxStringBytes) {
                *why = "字符串太长（上限 " + std::to_string(kConfigMaxStringBytes) + " 字节）";
                return false;
            }
            if (!lift::isValidUtf8(v.text)) {
                *why = "字符串必须是合法 UTF-8（孤立代理项 / 截断序列不支持）";
                return false;
            }
            return true;
        case ConfigValueKind::Array:
            for (const ConfigValue& item : v.items) {
                if (!validateValue(item, depth + 1, nodes, why)) return false;
            }
            return true;
        case ConfigValueKind::Object:
            for (const auto& member : v.members) {
                // ★ 对象 key 也写进 JSON：非法 UTF-8 会让 `dump` 抛 ⇒ 在 **set 期** 就拒绝。
                if (member.first.size() > kConfigMaxStringBytes) {
                    *why = "对象 key 太长（上限 " + std::to_string(kConfigMaxStringBytes) + " 字节）";
                    return false;
                }
                if (!lift::isValidUtf8(member.first)) {
                    *why = "对象 key 必须是合法 UTF-8（孤立代理项 / 截断序列不支持）";
                    return false;
                }
                if (!validateValue(member.second, depth + 1, nodes, why)) return false;
            }
            return true;
    }
    *why = "不认识的值类型";
    return false;
}

// `attach` 期对**文件里的每个域值**做与 `set` 同一套上限校验（深度 / 节点 / 串长 / UTF-8）。
// ★ 必须**非递归**（显式栈）：校验对象正是"可能有一万层嵌套"的文件 —— 递归校验自己就会爆栈
//   （B1 的原始症状在 `get` 的 `fromJson`，这里再用递归就是换个地方爆）。
bool validateJsonValue(const json& root, std::string* why) {
    struct Frame {
        const json* value;
        std::size_t depth;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{&root, 1});
    std::size_t nodes = 0;
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        const json& value = *frame.value;
        if (frame.depth > kConfigMaxDepth) {
            *why = "值嵌套太深（上限 " + std::to_string(kConfigMaxDepth) + " 层）";
            return false;
        }
        if (++nodes > kConfigMaxNodes) {
            *why = "值太大（节点上限 " + std::to_string(kConfigMaxNodes) + "）";
            return false;
        }
        if (value.is_number()) {
            if (!std::isfinite(value.get<double>())) {
                *why = "数字必须是有限的（NaN / Infinity 不支持）";
                return false;
            }
        } else if (value.is_string()) {
            const std::string& text = value.get_ref<const std::string&>();
            if (text.size() > kConfigMaxStringBytes) {
                *why = "字符串太长（上限 " + std::to_string(kConfigMaxStringBytes) + " 字节）";
                return false;
            }
            if (!lift::isValidUtf8(text)) {
                *why = "字符串必须是合法 UTF-8（孤立代理项 / 截断序列不支持）";
                return false;
            }
        } else if (value.is_array()) {
            for (auto it = value.rbegin(); it != value.rend(); ++it) {
                stack.push_back(Frame{&*it, frame.depth + 1});
            }
        } else if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (it.key().size() > kConfigMaxStringBytes) {
                    *why = "对象 key 太长（上限 " + std::to_string(kConfigMaxStringBytes) + " 字节）";
                    return false;
                }
                if (!lift::isValidUtf8(it.key())) {
                    *why = "对象 key 必须是合法 UTF-8（孤立代理项 / 截断序列不支持）";
                    return false;
                }
                stack.push_back(Frame{&it.value(), frame.depth + 1});
            }
        } else if (!value.is_null() && !value.is_boolean()) {
            *why = "不认识的值类型（只有 null / bool / 数字 / 字符串 / 数组 / 对象）";
            return false;
        }
    }
    return true;
}

}  // namespace

struct PluginConfig::Impl {
    bool available = false;
    bool read_only = false;
    bool dirty = false;
    bool warned = false;   // 写盘失败只警告一次
    bool gave_up = false;  // 之后不再尝试（⛔ 不重试风暴）
    fs::path file;
    fs::path temp;
    std::string path_utf8;  // 错误消息 / 测试读数
    json doc = {{"version", 1}, {"domains", json::object()}};
};

PluginConfig::PluginConfig() : impl_(std::make_unique<Impl>()) {}
PluginConfig::~PluginConfig() = default;

bool PluginConfig::attach(const std::string& appdata_root, std::string* err) {
    impl_ = std::make_unique<Impl>();
    Impl& s = *impl_;
    if (appdata_root.empty()) return true;  // 没有 --appdata：正常的"不可用"，不是错误
    s.file = pathFromUtf8(appdata_root) / "plugin-config.json";
    s.temp = pathFromUtf8(appdata_root) / "plugin-config.json.tmp";
    s.path_utf8 = pathToUtf8(s.file);

    std::error_code ec;
    const fs::file_status st = fs::status(s.file, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        if (err != nullptr) *err = s.path_utf8 + ": 无法检查配置文件（" + ec.message() + "）";
        return false;
    }
    const bool exists = !ec && fs::exists(st);
    if (exists) {
        if (!fs::is_regular_file(st)) {
            if (err != nullptr) *err = s.path_utf8 + ": 不是普通文件";
            return false;
        }
        std::error_code sec;
        const std::uintmax_t size = fs::file_size(s.file, sec);
        if (!sec && size > kConfigMaxDocumentBytes) {
            if (err != nullptr) {
                *err = s.path_utf8 + ": 配置文件太大（" + std::to_string(size) + " 字节，上限 " +
                       std::to_string(kConfigMaxDocumentBytes) + "）";
            }
            return false;
        }
        std::ifstream in(s.file, std::ios::binary);
        if (!in) {
            if (err != nullptr) *err = s.path_utf8 + ": 读不到配置文件（I/O / 权限）";
            return false;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.size() > kConfigMaxDocumentBytes) {
            if (err != nullptr) {
                *err = s.path_utf8 + ": 配置文件太大（上限 " +
                       std::to_string(kConfigMaxDocumentBytes) + " 字节）";
            }
            return false;
        }
        try {
            s.doc = json::parse(text);
        } catch (const json::parse_error& e) {
            if (err != nullptr) {
                *err = s.path_utf8 + ": JSON 解析失败（byte " + std::to_string(e.byte) + "）：" + e.what();
            }
            return false;
        } catch (const std::exception& e) {
            if (err != nullptr) *err = s.path_utf8 + ": JSON 解析失败：" + e.what();
            return false;
        }
        if (!s.doc.is_object()) {
            if (err != nullptr) *err = s.path_utf8 + ": 顶层必须是对象";
            return false;
        }
        if (!s.doc.contains("version") || !s.doc["version"].is_number_integer() ||
            s.doc["version"].get<std::int64_t>() != 1) {
            if (err != nullptr) *err = s.path_utf8 + ": `version` 必须是数字 1";
            return false;
        }
        if (!s.doc.contains("domains") || !s.doc["domains"].is_object()) {
            if (err != nullptr) *err = s.path_utf8 + ": `domains` 必须是对象";
            return false;
        }
        const json& domains = s.doc["domains"];
        if (domains.size() > kConfigMaxDomains) {
            if (err != nullptr) {
                *err = s.path_utf8 + ": 域太多（" + std::to_string(domains.size()) + "，上限 " +
                       std::to_string(kConfigMaxDomains) + "）";
            }
            return false;
        }
        for (auto it = domains.begin(); it != domains.end(); ++it) {
            if (!lift::isValidUtf8(it.key())) {
                if (err != nullptr) *err = s.path_utf8 + ": 域 id 不是合法 UTF-8";
                return false;
            }
            if (!it.value().is_object()) {
                if (err != nullptr) *err = s.path_utf8 + ": 域 '" + it.key() + "' 的值必须是对象";
                return false;
            }
            if (it.value().size() > kConfigMaxKeysPerDomain) {
                if (err != nullptr) {
                    *err = s.path_utf8 + ": 域 '" + it.key() + "' 的 key 太多（上限 " +
                           std::to_string(kConfigMaxKeysPerDomain) + "）";
                }
                return false;
            }
            for (auto key_it = it.value().begin(); key_it != it.value().end(); ++key_it) {
                std::string why;
                if (!validateJsonValue(key_it.value(), &why)) {
                    if (err != nullptr) {
                        *err = s.path_utf8 + ": 域 '" + it.key() + "' 的 key '" + key_it.key() +
                               "' 超限（" + why + "）";
                    }
                    return false;
                }
            }
        }
        // 只读探针：文件存在时试开一次写（不写内容）⇒ 明确"文件只读"。⛔ 文件不存在时**不探**
        //   （根目录可能还没被 guest 建出来；flush 会 `create_directories`；探针会把全新 appdata
        //   永久标成 read_only —— I6）。
        std::ofstream probe(s.file, std::ios::binary | std::ios::app);
        if (!probe) s.read_only = true;
    }
    // 文件不存在 ⇒ 空文档；`read_only` 保持 false（真正写失败在 flush 里有 warn，⛔ 不在这里猜）。
    s.available = true;
    return true;
}

bool PluginConfig::available() const { return impl_->available; }
bool PluginConfig::readOnly() const { return impl_->read_only; }
const std::string& PluginConfig::path() const { return impl_->path_utf8; }

bool PluginConfig::checkDomain(const std::string& id, std::string* why) {
    if (id.empty() || id.size() > kConfigMaxDomainIdLength) {
        if (why != nullptr) {
            *why = "域 id 长度必须 1–" + std::to_string(kConfigMaxDomainIdLength) + "（收到 " +
                   std::to_string(id.size()) + "）";
        }
        return false;
    }
    for (const unsigned char c : id) {
        if (!isDomainIdChar(c)) {
            if (why != nullptr) *why = "域 id 只允许 [A-Za-z0-9._-]（收到 '" + id + "'）";
            return false;
        }
    }
    if (isReservedName(id)) {
        if (why != nullptr) *why = "域 id 不许是保留名（__proto__ / prototype / constructor）";
        return false;
    }
    return true;
}

bool PluginConfig::checkKey(const std::string& key, std::string* why) {
    if (key.empty() || key.size() > kConfigMaxKeyLength) {
        if (why != nullptr) {
            *why = "key 长度必须 1–" + std::to_string(kConfigMaxKeyLength) + "（收到 " +
                   std::to_string(key.size()) + "）";
        }
        return false;
    }
    if (hasControlBytes(key)) {
        if (why != nullptr) *why = "key 不许含 NUL / 控制字符";
        return false;
    }
    if (!lift::isValidUtf8(key)) {
        if (why != nullptr) *why = "key 必须是合法 UTF-8（孤立代理项 / 截断序列不支持）";
        return false;
    }
    if (isReservedName(key)) {
        if (why != nullptr) *why = "key 不许是保留名（__proto__ / prototype / constructor）";
        return false;
    }
    return true;
}

bool PluginConfig::get(const std::string& domain, const std::string& key, ConfigValue* out, bool* found,
                       std::string* why) const {
    if (found != nullptr) *found = false;
    if (!checkDomain(domain, why) || !checkKey(key, why)) return false;
    const Impl& s = *impl_;
    if (!s.available) return true;
    const auto domains = s.doc.find("domains");
    if (domains == s.doc.end() || !domains->is_object()) return true;
    const auto dom = domains->find(domain);
    if (dom == domains->end() || !dom->is_object()) return true;
    const auto value = dom->find(key);
    if (value == dom->end()) return true;
    if (out != nullptr) {
        std::size_t nodes = 0;
        if (!fromJson(*value, 1, &nodes, out)) {
            if (why != nullptr) *why = "值超过上限 / 非法（正常应在装载期被拒绝）";
            return false;
        }
    }
    if (found != nullptr) *found = true;
    return true;
}

bool PluginConfig::has(const std::string& domain, const std::string& key, bool* found,
                       std::string* why) const {
    return get(domain, key, nullptr, found, why);
}

bool PluginConfig::set(const std::string& domain, const std::string& key, const ConfigValue& value,
                       ConfigReceipt* receipt, std::string* why) {
    if (receipt != nullptr) *receipt = ConfigReceipt{};
    if (!checkDomain(domain, why) || !checkKey(key, why)) return false;
    std::size_t nodes = 0;
    if (!validateValue(value, 1, &nodes, why)) return false;
    Impl& s = *impl_;
    if (!s.available) {
        if (receipt != nullptr) *receipt = ConfigReceipt{false, "no_appdata"};
        return true;
    }
    if (s.read_only) {
        if (receipt != nullptr) *receipt = ConfigReceipt{false, "read_only"};
        return true;
    }
    // ★ 未知顶层键 / 未知域内键都**保留**：只在既有对象上写这一项（⛔ 不重建 `domains` / 域对象）。
    json& domains = s.doc["domains"];
    if (!domains.contains(domain) || !domains[domain].is_object()) {
        if (domains.size() >= kConfigMaxDomains) {
            if (why != nullptr) *why = "域太多（上限 " + std::to_string(kConfigMaxDomains) + "）";
            return false;
        }
        domains[domain] = json::object();
    }
    json& dom = domains[domain];
    if (!dom.contains(key) && dom.size() >= kConfigMaxKeysPerDomain) {
        if (why != nullptr) {
            *why = "域 '" + domain + "' 的 key 太多（上限 " +
                   std::to_string(kConfigMaxKeysPerDomain) + "）";
        }
        return false;
    }
    dom[key] = toJson(value);
    s.dirty = true;
    if (receipt != nullptr) *receipt = ConfigReceipt{true, nullptr};
    return true;
}

bool PluginConfig::erase(const std::string& domain, const std::string& key, bool* removed,
                         std::string* why) {
    if (removed != nullptr) *removed = false;
    if (!checkDomain(domain, why) || !checkKey(key, why)) return false;
    Impl& s = *impl_;
    if (!s.available || s.read_only) return true;
    json& domains = s.doc["domains"];
    if (!domains.contains(domain) || !domains[domain].is_object()) return true;
    json& dom = domains[domain];
    if (!dom.contains(key)) return true;
    dom.erase(key);
    s.dirty = true;
    if (removed != nullptr) *removed = true;
    return true;
}

ConfigValue PluginConfig::all(const std::string& domain) const {
    const Impl& s = *impl_;
    if (!s.available) return ConfigValue::object();
    const auto domains = s.doc.find("domains");
    if (domains == s.doc.end() || !domains->is_object()) return ConfigValue::object();
    const auto dom = domains->find(domain);
    if (dom == domains->end() || !dom->is_object()) return ConfigValue::object();
    ConfigValue out = ConfigValue::object();
    out.members.reserve(dom->size());
    for (auto it = dom->begin(); it != dom->end(); ++it) {
        ConfigValue child;
        std::size_t nodes = 0;
        // 装载期已校验；这里失败只可能是内部不一致 ⇒ 返回空域（⛔ 不递归爆栈、不抛）。
        if (!fromJson(it.value(), 1, &nodes, &child)) return ConfigValue::object();
        out.members.emplace_back(it.key(), std::move(child));
    }
    return out;
}

void PluginConfig::flushIfDirty(std::string* warn) {
    Impl& s = *impl_;
    if (!s.dirty || s.gave_up || !s.available) return;
    s.dirty = false;
    if (s.read_only) return;
    const auto fail = [&](const std::string& why) {
        if (!s.warned) {
            s.warned = true;
            if (warn != nullptr) {
                *warn = "plugin-config: 写盘失败：" + s.path_utf8 + "（" + why + "）；之后不再重试";
            }
        }
        s.gave_up = true;
    };
    std::string text;
    try {
        text = s.doc.dump(2);
    } catch (const std::exception& e) {
        // ★ **序列化失败是编程错误，不是 I/O 失败**：`set` 已经校验过 UTF-8 / 上限，正常到不了这里。
        //   ⛔ 不置 `gave_up`（那会让后续合法 set 也永久停写；I2）；只警告一次并把本次脏标记清掉。
        if (!s.warned) {
            s.warned = true;
            if (warn != nullptr) {
                *warn = "plugin-config: 内部序列化失败（" + s.path_utf8 + "）：" + e.what() +
                        "；本次不写盘，后续合法写入仍会重试";
            }
        }
        return;
    }
    text += "\n";
    if (text.size() > kConfigMaxDocumentBytes) {
        // ★ 同"序列化失败"：这是**上限/编程错误**，不是 I/O；⛔ 不把进程的配置写盘永久熔断。
        if (!s.warned) {
            s.warned = true;
            if (warn != nullptr) {
                *warn = "plugin-config: 写出的文件太大（上限 " +
                        std::to_string(kConfigMaxDocumentBytes) + " 字节）；本次不写盘";
            }
        }
        return;
    }
    std::error_code ec;
    fs::create_directories(s.file.parent_path(), ec);
    std::ofstream out(s.temp, std::ios::binary | std::ios::trunc);
    if (!out) {
        fail("打不开临时文件");
        return;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
        out.close();
        std::error_code rec;
        fs::remove(s.temp, rec);
        fail("写临时文件失败");
        return;
    }
    out.close();
#ifdef _WIN32
    // ★ 用户裁决的原子替换：临时文件 + MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH。
    if (::MoveFileExW(s.temp.wstring().c_str(), s.file.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const DWORD code = ::GetLastError();
        std::error_code rec;
        fs::remove(s.temp, rec);
        fail("MoveFileEx 失败（GetLastError=" + std::to_string(code) + "）");
    }
#else
    std::error_code rec;
    fs::rename(s.temp, s.file, rec);
    if (rec) {
        std::error_code rec2;
        fs::remove(s.temp, rec2);
        fail("rename 失败（" + rec.message() + "）");
    }
#endif
}

}  // namespace lift::plugin
