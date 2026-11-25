// emulator-lift · **`ctx.config` 的宿主实现**（`PluginHost` 持有；PIMPL 藏 nlohmann）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.5 / §4.6 / §6 行 41–42；值类型 / 上限在
//       `runtime/config.h`（语言无关；⛔ 本头不 include nlohmann / quickjs）。
//
// ★ 一个 appdata ⇒ 一个实例（v1 文档口径：⛔ 不做文件锁；同一 appdata 起两个进程 =
//   后写者覆盖，见 `plugin-runtime.md` §4.5）。
// ★ 生命周期：`attach(<appdata 根>)` 在 `PluginHost::initFromFile` **之前**（与 `attachAppData`
//   同一时刻）；文件缺失 ⇒ 空文档（可用）；文件坏 ⇒ `attach` 返回 false（调用方 **exit 2**）。
// ★ 持久化：`set` / `delete` 只改内存 + 标脏；`PluginHost` 在**每个插件回调末**调一次
//   `flushIfDirty`（临时文件 + 原子替换）。第一次写盘失败 ⇒ 返回一行 warn（调用方 `ctx.log("warn")`
//   一次），之后**不再重试**（⛔ 不重试风暴）。
// ★ 读 / 写宿主段都不在这里包 `HostPause`：那是 QJS trampoline 的事（`script_quickjs.cpp`）。
#pragma once

#include <memory>
#include <string>

#include "runtime/config.h"

namespace lift::plugin {

class PluginConfig {
public:
    PluginConfig();
    ~PluginConfig();
    PluginConfig(const PluginConfig&) = delete;
    PluginConfig& operator=(const PluginConfig&) = delete;

    // `appdata_root` = **绝对宿主路径**（UTF-8；空 = 没有 `--appdata` ⇒ available=false）。
    // 读 `plugin-config.json`：不存在 ⇒ 空文档；坏 JSON / 顶层不是对象 / version != 1 /
    // `domains` 不是对象 / 域值不是对象 / **值超过深度/节点/串长上限** / 非法 UTF-8 /
    // 超过字节上限 ⇒ false，`*err` 带**文件路径**（坏 JSON 还带 byte 位置）。返回 false 是
    // **装载失败**（调用方 exit 2），与"没有 appdata"不同。
    bool attach(const std::string& appdata_root, std::string* err);

    bool available() const;
    // 只读：**配置文件存在但打不开写**（attach 期试开一次）。根目录还不存在 / 文件还不存在 ⇒
    // false（flush 会 `create_directories`；⛔ 不因为"根还没建出来"就永久只读）。
    bool readOnly() const;
    // 绝对文件路径（UTF-8；不可用 ⇒ 空串）。只用于错误消息 / 测试读数。
    const std::string& path() const;

    // 域 id / key 的形状校验（保留名 / 字符集 / 长度）。false ⇒ `*why` 是原因（JS 侧转 TypeError）。
    static bool checkDomain(const std::string& id, std::string* why);
    static bool checkKey(const std::string& key, std::string* why);

    // 读：`*found=false` ⇒ 没有这个 key（调用方用 fallback）。false ⇒ 校验错误（`*why`）。
    bool get(const std::string& domain, const std::string& key, ConfigValue* out, bool* found,
             std::string* why) const;
    bool has(const std::string& domain, const std::string& key, bool* found, std::string* why) const;
    // 写：false ⇒ 校验错误（`*why`：域 id / key / 值类型 / 深度 / 节点数 / 字符串长度 / UTF-8 /
    // 域数 / 每域 key 数）；true 时 `*receipt` 是**内存态**结果 —— `applied` = 内存已改
    // （⛔ 不代表已落盘）；只读 / 无 appdata 走 `code`。持久化失败只 `flushIfDirty` 的 warn，不回填。
    bool set(const std::string& domain, const std::string& key, const ConfigValue& value,
             ConfigReceipt* receipt, std::string* why);
    // 删：true = 真的删掉了（不存在 ⇒ false）；只读 / 无 appdata ⇒ false。
    bool erase(const std::string& domain, const std::string& key, bool* removed, std::string* why);
    // 该域的深拷贝快照（不存在 ⇒ 空对象）。
    ConfigValue all(const std::string& domain) const;

    // 有脏数据才写盘；第一次失败把一行 warn 写进 `*warn`（空 = 无事 / 已警告过）。
    void flushIfDirty(std::string* warn);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lift::plugin
