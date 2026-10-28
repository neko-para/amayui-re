#include "core/config.h"

#include "core/encoding.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace core {

std::filesystem::path player_config_root(const std::filesystem::path& install_root,
                                         bool use_app_data_folder,
                                         const ConfigRootEnvironment& env) {
    if (!use_app_data_folder) return install_root;
    // ★ 顺序固定：本地应用数据 → XDG 数据目录 → `~/.local/share` → 安装目录。
    //   ⛔ `env.roaming_app_data` 不在这里出现 —— 它只是诊断用的对照值。
    if (!env.local_app_data.empty()) return env.local_app_data;
    if (!env.data_home.empty()) return env.data_home;
    if (!env.home.empty()) return env.home / ".local" / "share";
    return install_root;
}

bool path_is_within(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (root.empty()) return false;
    const auto relative = path.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty()) return true;  // 同一路径 / 相对路径无法表达 ⇒ 当作在里面
    const std::string first = relative.begin()->string();
    return first != "..";
}

bool should_save_player_config(const std::filesystem::path& player_path,
                               const std::filesystem::path& config_root, bool explicit_config_path,
                               bool save_requested, ConfigSavePolicy policy) {
    if (!save_requested) return false;
    if (policy == ConfigSavePolicy::Never) return false;
    if (policy == ConfigSavePolicy::Allow) return true;
    // `Default`：显式指路了就写（用户自己选的落点，例如 `--config <副本>`）；
    // 否则只写"解析出来的根之外"的路径 —— 真实玩家配置 / 安装目录都算外部只读数据。
    if (explicit_config_path) return true;
    return !path_is_within(player_path, config_root);
}

namespace {
std::string upper_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return text;
}

std::string trim_080x20(const std::string& text, bool* had_leading = nullptr) {
    std::size_t begin = 0;
    while (begin < text.size() && static_cast<unsigned char>(text[begin]) <= 0x20) ++begin;
    std::size_t end = text.size();
    while (end > begin && static_cast<unsigned char>(text[end - 1]) <= 0x20) --end;
    if (had_leading != nullptr) *had_leading = begin > 0;
    return text.substr(begin, end - begin);
}

std::string strip_one_quote(const std::string& text, bool had_leading) {
    if (had_leading && text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

// 顶层 '='：不在双引号、括号内。返回前两个顶层分隔符的位置。
std::vector<std::size_t> top_level_equals(const std::string& line) {
    std::vector<std::size_t> result;
    bool quoted = false;
    int depth = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') quoted = !quoted;
        else if (!quoted && c == '(') ++depth;
        else if (!quoted && c == ')' && depth > 0) --depth;
        else if (!quoted && depth == 0 && c == '=') result.push_back(i);
    }
    return result;
}
}  // namespace

ConfigVocabulary ConfigVocabulary::load_from_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw CoreError("config-vocabulary-open-failed:" + path_to_utf8(path));
    const auto json = nlohmann::json::parse(input);

    ConfigVocabulary vocabulary;
    for (const auto& item : json.at("defaults")) {
        DefaultKey key;
        key.index_key = item.value("indexKey", "");
        key.internal = item.contains("internal") && !item["internal"].is_null()
            ? item["internal"].get<std::string>()
            : std::string{};
        key.kind = item.value("kind", "int");
        key.special = item.value("special", "");
        if (item.contains("extra")) {
            for (const auto& extra : item["extra"]) {
                key.extras.push_back(DefaultKey::Extra{
                    extra.value("internal", ""),
                    extra.value("from", ""),
                    extra.value("if", ""),
                });
            }
        }
        vocabulary.defaults_.push_back(std::move(key));
    }
    for (const auto& item : json.at("ini")) {
        vocabulary.ini_.push_back(IniKey{
            item.value("section", ""),
            item.value("key", ""),
            item.value("internal", ""),
            item.value("kind", "int"),
            item.value("if", ""),
        });
    }
    for (const auto& item : json.at("persist")) {
        vocabulary.persist_.push_back(IniKey{
            item.value("section", ""),
            item.value("key", ""),
            item.value("internal", ""),
            item.value("kind", "int"),
            item.value("if", ""),
        });
    }
    return vocabulary;
}

const ConfigVocabulary::DefaultKey* ConfigVocabulary::find_default(const std::string& index_key) const {
    const std::string wanted = upper_ascii(index_key);
    for (const auto& key : defaults_) {
        if (upper_ascii(key.index_key) == wanted) return &key;
    }
    return nullptr;
}

const ConfigVocabulary::IniKey* ConfigVocabulary::find_ini(
    const std::string& section,
    const std::string& key) const {
    for (const auto& entry : ini_) {
        if (entry.section == section && entry.key == key) return &entry;
    }
    return nullptr;
}

std::int32_t ConfigStore::parse_int_prefix(const std::string& text) {
    const char* begin = text.c_str();
    char* end = nullptr;
    const long value = std::strtol(begin, &end, 10);
    return end == begin ? 0 : static_cast<std::int32_t>(value);
}

bool ConfigStore::parse_bool(const std::string& text) {
    return parse_int_prefix(text) != 0;
}

void ConfigStore::set_int(const std::string& key, std::int32_t value) {
    ConfigValue entry;
    entry.kind = ConfigValue::Kind::Int;
    entry.integer = value;
    named_[key] = std::move(entry);
}

void ConfigStore::set_string(const std::string& key, std::string value) {
    ConfigValue entry;
    entry.kind = ConfigValue::Kind::String;
    entry.text = std::move(value);
    named_[key] = std::move(entry);
}

std::int32_t ConfigStore::get_int(const std::string& key) const {
    const auto it = named_.find(key);
    if (it == named_.end()) return 0;
    if (it->second.kind == ConfigValue::Kind::Int) return it->second.integer;
    if (it->second.kind == ConfigValue::Kind::String) return parse_int_prefix(it->second.text);
    return 0;
}

std::string ConfigStore::get_string(const std::string& key) const {
    const auto it = named_.find(key);
    if (it == named_.end()) return {};
    if (it->second.kind == ConfigValue::Kind::String) return it->second.text;
    return {};
}

const std::vector<std::string>* ConfigStore::get_list(const std::string& key) const {
    const auto it = named_.find(key);
    if (it == named_.end() || it->second.kind != ConfigValue::Kind::List) return nullptr;
    return &it->second.list;
}

void ConfigStore::register_engine_defaults() {
    // ★ 引擎在**配置库构造期**自己登记的默认值（`Reg` 构造 `sub_491880` / EA 0x491880 里的逐条写）。
    //   它们**不在**发行默认表（ALF 索引里那份实测也没有这些键），也**不在**玩家 `SYS4REG.INI`：
    //   INI 只覆盖它。少了这一步的后果是真机读 5 / 500 / 2000 / 1，模拟器读 **0**
    //   —— 设置界面的初值、`0x1b8` 读回的时长、`0x1ba` 的同态判据都会跟着错。
    //   优先级最低：发行默认表 → 玩家文件 → 运行期写入 都会把它盖掉。
    struct Builtin {
        const char* key;
        std::int32_t value;
    };
    // 键名逐字 + 值 + 登记点 EA（取证：`Reg` 构造体内的立即数写）。
    static const Builtin kBuiltins[] = {
        {"sound:Music", 0},                  // 0x491919
        {"sound:Sound", 1},                  // 0x491932（总开关）
        {"sound:Voice", 1},                  // 0x49194F
        {"sound:SE", 1},                     // 0x49196C
        {"sound:Movie", 1},                  // 0x491989
        {"message:MessageSpeed", 5},         // 0x491D4D
        {"message:AutoMessageTime0", 500},   // 0x49201C
        {"message:AutoMessageTime1", 2000},  // 0x492039
        // ★ 抗锯齿的两个**子**开关也在构造期登记（⛔ 不在发行默认表里：本作 ALF 的 51 条没有它们）。
        //   `0x491F52` = 键名 `"message:UseAntiFont"` 在构造体内的唯一一处引用，`0x491F59` 把值槽写成 **1**
        //   （`0x491F63` 调"按键名写整数"的 upsert）；`0x492A04` / `0x492A0B` / `0x492A11` 是
        //   `"message:AntiFontLevel"` ⇒ 值取自 `ebx`，而全函数 `ebx` 只在 `0x4918C6` 被清零过 ⇒ **0**。
        //   ⇒ 全新玩家档（没有 SYS4REG.INI）的 AA 子开关就是"开 + 4 位"，与真机观测一致；
        //     总开关 `set:EnableAntiFont` 构造期登记 0（键 @0x492790 / 值 @0x492797）、由发行默认表盖成 1。
        {"message:UseAntiFont", 1},    // 0x491F59
        {"message:AntiFontLevel", 0},  // 0x492A0B
    };
    for (const auto& builtin : kBuiltins) {
        if (named_.find(builtin.key) != named_.end()) continue;
        ConfigValue value;
        value.kind = ConfigValue::Kind::Int;
        value.integer = builtin.value;
        named_[builtin.key] = value;
    }
    return;
}

void ConfigStore::load_defaults(const std::vector<ConfigDefault>& entries) {
    // 整表处理前一次性清空列表键（不是逐条清）。
    for (auto it = named_.begin(); it != named_.end();) {
        if (it->second.kind == ConfigValue::Kind::List) it = named_.erase(it);
        else ++it;
    }

    for (const auto& entry : entries) {
        const auto* key = vocabulary_.find_default(entry.key);
        if (key == nullptr) throw CoreError("config-unknown-default-key:" + entry.key);
        if (key->kind == "ignore") continue;

        if (key->kind == "list") {
            auto& value = named_[key->internal];
            value.kind = ConfigValue::Kind::List;
            value.list.push_back(entry.value);
            continue;
        }
        if (key->kind == "string") {
            set_string(key->internal, entry.value);
            continue;
        }

        std::int32_t value = parse_int_prefix(entry.value);
        if (key->special == "bool") value = value != 0 ? 1 : 0;
        if (key->special == "enableAntiFont" && value == 1 && !font_smoothing_available_) value = 0;
        if (key->special == "saveVersionSplit") {
            set_int(key->internal, value / 100);
            set_int("set:SaveVersion2", value % 100);
        } else {
            set_int(key->internal, value);
        }
        for (const auto& extra : key->extras) {
            if (extra.condition == "value!=0" && value == 0) continue;
            std::int32_t extra_value = value;
            if (extra.from == "value-1") extra_value = value - 1;
            set_int(extra.internal, extra_value);
        }
    }

    // 收尾修补：Menu_Save == 0 且主/次版本为 1/0 ⇒ 1/10。
    if (get_int("set:Menu_Save") == 0 &&
        get_int("set:SaveVersion1") == 1 &&
        get_int("set:SaveVersion2") == 0) {
        set_int("set:SaveVersion1", 1);
        set_int("set:SaveVersion2", 10);
    }
}

void ConfigStore::load_player_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return;  // 文件不存在是正常状态

    // 按 LF 分行；一行最多 255 字节，读满 255 字节先交出一块。
    std::vector<std::string> lines;
    std::string line;
    char ch = 0;
    while (input.get(ch)) {
        if (ch == '\n') {
            lines.push_back(line);
            line.clear();
            continue;
        }
        line.push_back(ch);
        if (line.size() == 255) {
            lines.push_back(line);
            line.clear();
        }
    }
    // 文件末尾没有 LF 的残行被丢弃；只有读满 255 字节的块已在上面的分支交出。
    (void)lines;

    std::string section;
    for (std::string raw : lines) {
        const auto nul = raw.find('\0');
        if (nul != std::string::npos) raw.resize(nul);
        if (raw.empty()) continue;

        if (raw.front() == '[') {
            const auto close = raw.find_last_of(']');
            section = trim_080x20(raw.substr(1, close == std::string::npos ? std::string::npos : close - 1));
            continue;
        }

        const auto equals = top_level_equals(raw);
        std::string key_text;
        std::string value_text;
        if (equals.empty()) {
            key_text = raw;
        } else {
            key_text = raw.substr(0, equals[0]);
            const std::size_t value_end = equals.size() >= 2 ? equals[1] : raw.size();
            value_text = raw.substr(equals[0] + 1, value_end - equals[0] - 1);
        }

        bool key_had_leading = false;
        bool value_had_leading = false;
        key_text = strip_one_quote(trim_080x20(key_text, &key_had_leading), key_had_leading);
        value_text = strip_one_quote(trim_080x20(value_text, &value_had_leading), value_had_leading);

        const auto* key = vocabulary_.find_ini(section, key_text);
        if (key == nullptr) continue;

        // 条件键：只在开关非 0 时读取。
        if (section == "display" && key_text == "VirtualFullScreenType" &&
            get_int("display:VirtualFullScreen") == 0) {
            continue;
        }
        if (section == "display" && (key_text == "UseAntiFont" || key_text == "AntiFontLevel") &&
            get_int("set:EnableAntiFont") == 0) {
            continue;
        }

        if (key->kind == "int") set_int(key->internal, parse_int_prefix(value_text));
        else set_string(key->internal, value_text);
    }
}

std::string ConfigStore::script_key(char type, std::uint32_t index) {
    char buffer[9];
    std::snprintf(buffer, sizeof(buffer), "%08x", index);
    return std::string(1, type) + buffer;
}

std::int32_t ConfigStore::load_script_int(std::uint32_t index) const {
    const auto it = script_.find(script_key(3, index));
    if (it == script_.end() || it->second.kind != ConfigValue::Kind::Int) return 0;
    return it->second.integer;
}

void ConfigStore::save_player_file(const std::filesystem::path& path) const {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw CoreError("config-save-open-failed:" + path.string());
    static const char* kSections[] = {"display", "sound", "message", "system"};
    std::string current;
    for (const char* section : kSections) {
        for (const auto& key : vocabulary_.persist_keys()) {
            if (key.section != section) continue;
            if (!key.condition.empty() && get_int(key.condition) == 0) continue;
            const auto it = named_.find(key.internal);
            if (current != section) {
                out << '[' << section << "]\r\n";
                current = section;
            }
            out << key.key << '=';
            const bool is_string = it != named_.end() ? it->second.kind == ConfigValue::Kind::String
                                                      : key.kind == "string";
            if (is_string) out << (it == named_.end() ? std::string{} : it->second.text);
            else out << (it == named_.end() ? 0 : it->second.integer);
            out << "\r\n";
        }
    }
    if (!out) throw CoreError("config-save-write-failed:" + path.string());
}

void ConfigStore::save_script_int(std::uint32_t index, std::int32_t value) {
    ConfigValue entry;
    entry.kind = ConfigValue::Kind::Int;
    entry.integer = value;
    script_[script_key(3, index)] = std::move(entry);
}

std::string ConfigStore::load_script_string(std::uint32_t index) const {
    const auto it = script_.find(script_key(5, index));
    if (it == script_.end() || it->second.kind != ConfigValue::Kind::String) return {};
    return it->second.text;
}

void ConfigStore::save_script_string(std::uint32_t index, std::string value) {
    ConfigValue entry;
    entry.kind = ConfigValue::Kind::String;
    entry.text = std::move(value);
    script_[script_key(5, index)] = std::move(entry);
}

}  // namespace core
