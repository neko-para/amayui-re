#include "core/config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace core {
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

ConfigVocabulary ConfigVocabulary::load_from_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw CoreError("config-vocabulary-open-failed:" + path);
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
