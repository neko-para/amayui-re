// emulator-lift · **`ctx.config` 的值类型与上限**（语言无关；宿主侧实现 = `plugin/plugin_config.*`）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.5 / §4.6 / §6 行 41–42。
// 裁决（用户口径，2026-11-25）：配置文件 = `<--appdata>/plugin-config.json`（appdata 根，⛔ 不在 `SAVE/`）；
//   信封 `{"version":1,"domains":{"<id>":{...}}}`；值 = 原生 JS 值（对象 / 数组 / 字符串 / 有限数字 /
//   布尔 / null），⛔ 不是 JSON 字符串；未知顶层键 / 未知域内键**保留**（前向兼容）。
//
// ★ 为什么这里有一份**中立值树**而不是直接用 nlohmann：本头同时被 `ui/plugin.h`（接口）与
//   `script_quickjs.cpp`（QJS 唯一 TU）include ⇒ **⛔ 不能把 nlohmann 拖进第二个 TU**。
//   `plugin/plugin_config.cpp` 是唯一做 JSON 文本 ↔ 本树转换的 TU（PIMPL）。
// ★ 数值只有一种宿主表示（double）：`NaN` / `±Infinity` 在**校验**阶段拒绝（⛔ 不静默变 0）。
// ★ 上限是**合同的一部分**（改了 = 改 d.ts / 文档）；域 id 形状 `^[A-Za-z0-9._-]{1,64}$`。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lift::plugin {

enum class ConfigValueKind : std::uint8_t {
    Null = 0,
    Bool = 1,
    Number = 2,
    String = 3,
    Array = 4,
    Object = 5,
};

// 一棵原生值树（深拷贝语义；`all()` 返回的快照与 store 互不影响）。
struct ConfigValue {
    ConfigValueKind kind = ConfigValueKind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;                                    // kind == String
    std::vector<ConfigValue> items;                      // kind == Array
    std::vector<std::pair<std::string, ConfigValue>> members;  // kind == Object（保序）

    static ConfigValue null() { return ConfigValue{}; }
    static ConfigValue fromBool(bool value) {
        ConfigValue v;
        v.kind = ConfigValueKind::Bool;
        v.boolean = value;
        return v;
    }
    static ConfigValue fromNumber(double value) {
        ConfigValue v;
        v.kind = ConfigValueKind::Number;
        v.number = value;
        return v;
    }
    static ConfigValue fromString(std::string value) {
        ConfigValue v;
        v.kind = ConfigValueKind::String;
        v.text = std::move(value);
        return v;
    }
    static ConfigValue array() {
        ConfigValue v;
        v.kind = ConfigValueKind::Array;
        return v;
    }
    static ConfigValue object() {
        ConfigValue v;
        v.kind = ConfigValueKind::Object;
        return v;
    }

    ConfigValue& push(ConfigValue value) {
        kind = ConfigValueKind::Array;
        items.push_back(std::move(value));
        return *this;
    }
    ConfigValue& put(std::string key, ConfigValue value) {
        kind = ConfigValueKind::Object;
        members.emplace_back(std::move(key), std::move(value));
        return *this;
    }
    bool isObject() const { return kind == ConfigValueKind::Object; }
};

// `set` / `delete` 的回执（与 d.ts 的 `ConfigReceipt` 逐字一致）。
// ★ 校验错误（key 非法 / 值类型非法 / 超限）**不走回执**：它们抛 `TypeError`（宿主侧返回 false + why）。
struct ConfigReceipt {
    bool applied = false;
    const char* code = nullptr;  // "read_only" | "io" | "no_appdata"；空 = 无错误码
};

inline const char* configReceiptCodeName(const ConfigReceipt& receipt) {
    return receipt.code != nullptr ? receipt.code : "";
}

// ---- 上限（单一真源；`tests/plugin_config.cpp` 的 `limits` 用例钉住行为）----
inline constexpr std::size_t kConfigMaxDomainIdLength = 64;
inline constexpr std::size_t kConfigMaxKeyLength = 128;
// 值树：容器嵌套层数（根值 = 第 1 层）+ 节点总数 + 单个字符串字节数。
inline constexpr std::size_t kConfigMaxDepth = 8;
inline constexpr std::size_t kConfigMaxNodes = 1024;
inline constexpr std::size_t kConfigMaxStringBytes = 8192;
// 整份配置文件的字节上限（读入 / 写盘两侧同一条；超过 ⇒ 装载失败 / 拒绝写盘 + 一行 warn）。
inline constexpr std::size_t kConfigMaxDocumentBytes = 1u << 20;
// 一份配置里最多几个域 / 每个域最多几个 key（防病态文件把宿主拖住）。
inline constexpr std::size_t kConfigMaxDomains = 256;
inline constexpr std::size_t kConfigMaxKeysPerDomain = 1024;

}  // namespace lift::plugin
