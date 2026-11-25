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
// ★ `applied` = **内存已改**（不是"已落盘"）：写盘在宿主每个回调末 flush，失败只 `ctx.log("warn")`
//   一次（⛔ 不回填到本回执 —— 那会让"set 的返回值"依赖未来的 I/O）。
// ★ 校验错误（key 非法 / 值类型非法 / 超限 / 非法 UTF-8）**不走回执**：它们抛 `TypeError`
//   （宿主侧返回 false + why）。
struct ConfigReceipt {
    bool applied = false;
    const char* code = nullptr;  // "read_only" | "no_appdata"；空 = 无错误码
};

inline const char* configReceiptCodeName(const ConfigReceipt& receipt) {
    return receipt.code != nullptr ? receipt.code : "";
}

// =====================================================================================
// `ConfigBridge`：**两个渲染面共用的语言无关桥**（2026-11-25 父级裁决 D2）
//
// ★ 为什么需要它（而不是让每条路径各自 include `plugin/plugin_config.h`）：
//   · `RenderCtx.config`（插件 render）与经典回退 HUD 的 `host.config` **必须是同一份 JS 形状**
//     `{available, domain(id)}`；实现体（6 个 JS trampoline）也只该有一份 ⇒ 两边都只认这个桥；
//   · 本头是**语言无关**的（只认 `ConfigValue` / `ConfigReceipt`）⇒ `ui/script.h` / `aot_host.h`
//     可以只持有一个指针，⛔ 不把 `PluginConfig`（PIMPL + nlohmann）拖进 UI 接口头；
//   · ⛔ 不把 config 塞进全局 `game` 变量来绕过桥（那会让"写盘"变成渲染层的副作用）。
//
// ★ 实现 = `plugin/PluginHost`（薄适配 `PluginConfig`）；测试替身可直接实现本接口。
// ★ 门（"在 Run 回调里" vs "在 render 回调里"）**不在这里** —— 那是 `script_quickjs.cpp` 的
//   trampoline 的事（同一份实现体，两个面各有一条门）。本类只回答值 / 错误原因。
// ★ `flushIfDirty` = "每个渲染帧末尾 / 每个 Run 回调末尾"的同一条持久化口径：
//   只写脏数据，失败把一行 warn 写进 `*warn`（空 = 无事 / 已警告过）。
// =====================================================================================
class ConfigBridge {
public:
    virtual ~ConfigBridge() = default;
    // 有没有可用的 appdata 根（false ⇒ JS 侧 `available=false`，`domain()` 抛 `Error("no_appdata")`）。
    virtual bool available() const = 0;
    // 域 id 的形状校验（保留名 / 字符集 / 长度）。false ⇒ `*why` 是原因（JS 侧转 TypeError）。
    virtual bool checkDomain(const std::string& id, std::string* why) const = 0;

    // 读 / 写 / 删：false ⇒ 校验错误（`*why`）；true 时 `*found` / `*removed` / `*receipt` 有效。
    virtual bool get(const std::string& domain, const std::string& key, ConfigValue* out, bool* found,
                     std::string* why) = 0;
    virtual bool has(const std::string& domain, const std::string& key, bool* found,
                     std::string* why) = 0;
    virtual bool set(const std::string& domain, const std::string& key, const ConfigValue& value,
                     ConfigReceipt* receipt, std::string* why) = 0;
    virtual bool erase(const std::string& domain, const std::string& key, bool* removed,
                       std::string* why) = 0;
    // 该域的深拷贝快照（不存在 ⇒ 空对象）。
    virtual ConfigValue all(const std::string& domain) = 0;

    // 有脏数据才写盘；第一次失败把一行 warn 写进 `*warn`（空 = 无事 / 已警告过）。
    virtual void flushIfDirty(std::string* warn) = 0;
};

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
