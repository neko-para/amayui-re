#pragma once

#include "core/alf.h"
#include "core/bytes.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace core {

// 配置值。同一个键只有一种类型；列表键只追加。
struct ConfigValue {
    enum class Kind { Int, String, List } kind = Kind::Int;
    std::int32_t integer = 0;
    std::string text;
    std::vector<std::string> list;
};

// 配置词表：format/config-keys.json 的三个数组。
class ConfigVocabulary {
public:
    struct DefaultKey {
        std::string index_key;
        std::string internal;
        std::string kind;
        std::string special;
        struct Extra {
            std::string internal;
            std::string from;
            std::string condition;
        };
        std::vector<Extra> extras;
    };

    struct IniKey {
        std::string section;
        std::string key;
        std::string internal;
        std::string kind;
    };

    static ConfigVocabulary load_from_file(const std::filesystem::path& path);

    const DefaultKey* find_default(const std::string& index_key) const;
    const IniKey* find_ini(const std::string& section, const std::string& key) const;

private:
    std::vector<DefaultKey> defaults_;
    std::vector<IniKey> ini_;
};

// 引擎配置库：命名键表 + 脚本键表。发行默认表与玩家文件都只写命名键表。
class ConfigStore {
public:
    ConfigStore() = default;
    explicit ConfigStore(ConfigVocabulary vocabulary) : vocabulary_(std::move(vocabulary)) {}

    void load_defaults(const std::vector<ConfigDefault>& entries);
    void load_player_file(const std::filesystem::path& path);

    std::int32_t get_int(const std::string& key) const;
    bool has_named(const std::string& key) const { return named_.find(key) != named_.end(); }
    std::string get_string(const std::string& key) const;
    const std::vector<std::string>* get_list(const std::string& key) const;
    void set_int(const std::string& key, std::int32_t value);
    void set_string(const std::string& key, std::string value);

    // 脚本键：type 3 整数 / 5 字符串；index 是池下标。
    std::int32_t load_script_int(std::uint32_t index) const;
    void save_script_int(std::uint32_t index, std::int32_t value);
    std::string load_script_string(std::uint32_t index) const;
    void save_script_string(std::uint32_t index, std::string value);

    void set_font_smoothing_available(bool available) { font_smoothing_available_ = available; }
    const ConfigVocabulary& vocabulary() const { return vocabulary_; }
    std::size_t named_size() const { return named_.size(); }
    std::size_t script_size() const { return script_.size(); }

private:
    static std::string script_key(char type, std::uint32_t index);
    static std::int32_t parse_int_prefix(const std::string& text);
    static bool parse_bool(const std::string& text);

    ConfigVocabulary vocabulary_;
    bool font_smoothing_available_ = true;
    std::map<std::string, ConfigValue> named_;
    std::map<std::string, ConfigValue> script_;
};

}  // namespace core
