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
        // 条件写出：非空时，只有该内部键的整数值非 0 才写出本条。
        std::string condition;
    };

    static ConfigVocabulary load_from_file(const std::filesystem::path& path);

    const DefaultKey* find_default(const std::string& index_key) const;
    const IniKey* find_ini(const std::string& section, const std::string& key) const;
    const std::vector<IniKey>& ini_keys() const { return ini_; }
    const std::vector<IniKey>& persist_keys() const { return persist_; }

private:
    std::vector<DefaultKey> defaults_;
    std::vector<IniKey> ini_;
    std::vector<IniKey> persist_;
};

// 玩家配置文件的根目录候选（宿主把环境填进来；核心**不**碰环境变量，因此这条可纯测）。
//
// ★ 引擎的根是**本地**应用数据目录（`%LOCALAPPDATA%`），⛔ 不是漫游（`%APPDATA%`）。
//   判据是实测路径：真机把 `SYS4REG.INI` 与 `SAVE/` 都写在
//   `%LOCALAPPDATA%\Eushully\<标题>\` 下；漫游根下的那份是**别的进程**写的，引擎从不读它。
//   ⇒ 字段 `roaming_app_data` **只用于诊断打印**，选择逻辑一次都不读它（守卫见自检）。
struct ConfigRootEnvironment {
    std::filesystem::path local_app_data;   // Windows：`%LOCALAPPDATA%`
    std::filesystem::path roaming_app_data;  // Windows：`%APPDATA%`（⛔ 不参与选择）
    std::filesystem::path data_home;         // 非 Windows：`$XDG_DATA_HOME`
    std::filesystem::path home;              // 非 Windows：`$HOME`
};

// `set:UseAppDataFolder` 的判据：**恰为 1** 才把根换成应用数据目录（0 / 2 / 负数都算"不换"）。
// ★ 与存档目录的根同一条口径（spec 的「窗口与呈现」配置根、「存档」目录两处都写"恰为 1"）。
constexpr bool use_app_data_folder_root(std::int32_t value) { return value == 1; }

// 玩家配置文件的根：`set:UseAppDataFolder` 恰为 1 ⇒ 本地应用数据目录；否则安装目录。
// 候选目录一个都没有 ⇒ 留在安装目录（⛔ 不回退到漫游根）。
std::filesystem::path player_config_root(const std::filesystem::path& install_root,
                                         bool use_app_data_folder,
                                         const ConfigRootEnvironment& env);

// 保存的宿主安全闸策略。`Default` = 只在**不是**真实玩家配置时才写。
enum class ConfigSavePolicy { Default, Allow, Never };

// `path` 是否落在 `root` 里面（按路径分量比较，不碰文件系统 ⇒ 可纯测）。
bool path_is_within(const std::filesystem::path& path, const std::filesystem::path& root);

// 退出时要不要把配置写回玩家文件。
// ★ `save_requested` = 引擎退出（窗口关闭 / 某个引擎停因）确实发生了（spec 的
//   `CORE-CONFIG-EXIT-SAVE`）；本函数只回答"**宿主**允不允许真的去写那个路径"。
//   真实玩家配置是**外部只读数据**（`AGENTS.md` §8）：默认不写，要写必须显式指路
//   （`--config <副本>` 或 `--config-save`）。
bool should_save_player_config(const std::filesystem::path& player_path,
                               const std::filesystem::path& config_root, bool explicit_config_path,
                               bool save_requested, ConfigSavePolicy policy);

// 引擎配置库：命名键表 + 脚本键表。发行默认表与玩家文件都只写命名键表。
class ConfigStore {
public:
    ConfigStore() = default;
    explicit ConfigStore(ConfigVocabulary vocabulary) : vocabulary_(std::move(vocabulary)) {}

    void load_defaults(const std::vector<ConfigDefault>& entries);
    /** 引擎**构造期**登记的默认值（优先级最低：发行默认表 / 玩家文件 / 运行期写入都会覆盖）。 */
    void register_engine_defaults();
    void load_player_file(const std::filesystem::path& path);
    // 按「保存」合同的固定段序 / 词表顺序整体重写玩家文件。
    void save_player_file(const std::filesystem::path& path) const;

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
    /** 命名键表的只读视图（宿主诊断用：把"界面到底写进配置库什么"导出来）。 */
    const std::map<std::string, ConfigValue>& named() const { return named_; }

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
