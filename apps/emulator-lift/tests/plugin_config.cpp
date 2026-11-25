// emulator-lift · **`ctx.config` 的文件形状 / 值 / 持久化**判据（`plugin-config`，pure 档）
//
// 判据（红了意味着什么）——配置文件 = `<--appdata>/plugin-config.json`（appdata 根；⛔ 不在 `SAVE/`）：
//   · `config/roundtrip`        ：六种原生值（null/bool/number/string/array/object）set → get / has /
//                                 all / delete 往返；`all()` 是**深拷贝快照**（改返回值不影响 store）。
//   · `config/unknown-preserved`：未知**顶层键**与未知**域内键**在 set（= 一次写盘）之后仍在
//                                 （前向兼容；⛔ 不是"重建成只有本次写的 key"）。
//   · `config/domain-autocreate`：域不注册、不探测 ⇒ `set` 自动建；二次装载读得到。
//   · `config/bad-json-exit2`   ：坏 JSON ⇒ `attach` 返回 false（**attach 判据**；进程级 exit 2
//                                 由 `plugin-headless` 的 `plugin/bad-config-exit2` 真子进程钉），
//                                 错误消息带**文件路径**与 byte 位置。
//   · `config/bad-top-level`    ：顶层不是对象 / `version` 不是 1 / `domains` 不是对象 / 缺 `domains`
//                                 ⇒ 装载失败。
//   · `config/bad-domain-value` ：域值不是对象 ⇒ 装载失败（点名域）。
//   · `config/proto-key`        ：域 id 与 key 都拒 `__proto__` / `prototype` / `constructor`；
//                                 域 id 字符集 / 长度、key 长度也在这里钉。
//   · `config/limits`           ：set 的嵌套深度 / 节点数 / 字符串长度 / NaN 全拒。
//   · `config/limits-on-load`   ：**装载期**同一套上限（8 层 / 1024 节点；10000 层不崩）+ 深度 8 边界。
//   · `config/invalid-utf8`     ：非法 UTF-8 的 key / 值 / 对象 key 在 set 期 TypeError；之后合法写仍落盘。
//   · `config/no-appdata`       ：`attach("")` ⇒ available=false；set 的回执 code=`no_appdata`。
//   · `config/missing-root`     ：appdata 根不存在 ⇒ 可写；flush 建出文件（⛔ 不误标 read_only）。
//   · `config/document-size`    ：1 MiB 文件上限（读入）+ 写盘侧超限 ⇒ warn + 不写。
//   · `config/read-only-receipt`：文件只读 ⇒ attach 成功但 `readOnly()`；set 回执
//                                 `{applied:false, code:"read_only"}` 且**不改内存** / 不写盘。
//                                 （文件系统不把只读位当不可写时 `LIFT_SKIP` ⇒ exit 77。）
//   · `config/flush-on-callback`：`set` 只标脏（文件还不存在）⇒ `flushIfDirty` 才写盘；
//                                 **删掉文件后再 flush（没有新写）⇒ 文件仍不存在**（⛔ 不每次重写）。
//   · `config/cross-instance-read`：同一 appdata 二次 `attach` 读到上一次写盘的值。
//
// ★ 端口：只碰 `std::filesystem::temp_directory_path()` 下的临时树（零游戏安装 / 零 GPU /
//   零窗口）⇒ 顶层无条件注册 = pure 档（`AGENTS.md` §12.4）。
// ★ 本用例只判**稳定词表**（回执 code / 错误消息里的路径与 byte），⛔ 不断言平台 errno / 文案。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "plugin/plugin_config.h"
#include "runtime/config.h"
#include "runtime/host_encoding.h"

namespace {

namespace fs = std::filesystem;
using lift::plugin::ConfigReceipt;
using lift::plugin::ConfigValue;
using lift::plugin::PluginConfig;

std::uint32_t processId() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(::_getpid());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

std::string toUtf8(const fs::path& p) {
#ifdef _WIN32
    return lift::wideToUtf8(p.wstring().c_str());
#else
    return p.string();
#endif
}

struct TempRoot {
    fs::path path;
    bool ok = false;

    TempRoot() {
        std::error_code ec;
        path = fs::temp_directory_path(ec) / ("lift-plugin-config-" + std::to_string(processId()));
        if (ec) return;
        fs::remove_all(path, ec);
        ec.clear();
        fs::create_directories(path, ec);
        ok = !ec;
    }
    ~TempRoot() {
        if (path.empty()) return;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;
};

bool writeFile(const fs::path& path, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    return static_cast<bool>(out);
}

bool readFile(const fs::path& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

ConfigValue number(double v) { return ConfigValue::fromNumber(v); }
ConfigValue text(std::string v) { return ConfigValue::fromString(std::move(v)); }

const ConfigValue* member(const ConfigValue& object, const std::string& key);

bool sameValue(const ConfigValue& a, const ConfigValue& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case lift::plugin::ConfigValueKind::Null:
            return true;
        case lift::plugin::ConfigValueKind::Bool:
            return a.boolean == b.boolean;
        case lift::plugin::ConfigValueKind::Number:
            return a.number == b.number;
        case lift::plugin::ConfigValueKind::String:
            return a.text == b.text;
        case lift::plugin::ConfigValueKind::Array:
            if (a.items.size() != b.items.size()) return false;
            for (std::size_t i = 0; i < a.items.size(); ++i) {
                if (!sameValue(a.items[i], b.items[i])) return false;
            }
            return true;
        case lift::plugin::ConfigValueKind::Object:
            if (a.members.size() != b.members.size()) return false;
            for (const auto& m : a.members) {
                const ConfigValue* other = member(b, m.first);
                if (other == nullptr) return false;
                if (!sameValue(m.second, *other)) return false;
            }
            return true;
    }
    return false;
}

const ConfigValue* member(const ConfigValue& object, const std::string& key) {
    for (const auto& m : object.members) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}

bool getValue(PluginConfig& cfg, const char* domain, const char* key, ConfigValue* out) {
    bool found = false;
    std::string why;
    LIFT_CHECK(cfg.get(domain, key, out, &found, &why));
    LIFT_CHECK(found);
    return found;
}

bool setValue(PluginConfig& cfg, const char* domain, const char* key, const ConfigValue& value,
              ConfigReceipt* receipt = nullptr) {
    ConfigReceipt local;
    std::string why;
    const bool ok = cfg.set(domain, key, value, &local, &why);
    if (!ok) {
        lift_test::failf(__FILE__, __LINE__, "set(%s/%s) 校验失败：%s", domain, key, why.c_str());
        return false;
    }
    if (receipt != nullptr) *receipt = local;
    LIFT_CHECK(local.applied);
    return ok;
}

bool attachFile(const fs::path& root, const std::string& json, std::string* err) {
    PluginConfig cfg;
    if (!writeFile(root / "plugin-config.json", json)) return false;
    return cfg.attach(toUtf8(root), err);
}

}  // namespace

int main() {
    TempRoot temp;
    if (!temp.ok) LIFT_SKIP("取不到宿主临时目录 / 建不了临时树");
    const fs::path root = temp.path / "appdata";
    std::error_code ec;
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);
    if (ec) return lift_test::finish("plugin-config");

    // ---- config/roundtrip：六种值 + 深拷贝快照 ----
    lift_test::g_current_case = "config/roundtrip";
    {
        PluginConfig cfg;
        std::string err;
        LIFT_CHECK(cfg.attach(toUtf8(root), &err));
        LIFT_CHECK(err.empty());
        LIFT_CHECK(cfg.available());
        bool found = true;
        ConfigValue missing;
        std::string why;
        LIFT_CHECK(cfg.get("d", "missing", &missing, &found, &why));
        LIFT_CHECK(!found);
        LIFT_CHECK(cfg.has("d", "missing", &found, &why));
        LIFT_CHECK(!found);

        ConfigValue object = ConfigValue::object();
        object.put("nul", ConfigValue::null())
            .put("bool", ConfigValue::fromBool(true))
            .put("num", number(42))
            .put("frac", number(1.5))
            .put("str", text("hello"));
        ConfigValue array = ConfigValue::array();
        array.push(number(1)).push(text("two")).push(ConfigValue::null());
        object.put("arr", array);
        LIFT_CHECK(setValue(cfg, "d", "v", object));
        LIFT_CHECK(cfg.has("d", "v", &found, &why) && found);

        ConfigValue back;
        LIFT_CHECK(getValue(cfg, "d", "v", &back));
        LIFT_CHECK(sameValue(back, object));

        // `all()` 深拷贝：改快照不影响 store。
        ConfigValue snapshot = cfg.all("d");
        LIFT_CHECK(snapshot.isObject());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(snapshot.members.size()), 1);
        for (auto& m : snapshot.members[0].second.members) {
            if (m.first == "num") m.second.number = 9999.0;
        }
        ConfigValue again;
        LIFT_CHECK(getValue(cfg, "d", "v", &again));
        LIFT_CHECK(sameValue(again, object));

        bool removed = false;
        LIFT_CHECK(cfg.erase("d", "v", &removed, &why));
        LIFT_CHECK(removed);
        LIFT_CHECK(cfg.has("d", "v", &found, &why));
        LIFT_CHECK(!found);
        LIFT_CHECK(cfg.erase("d", "v", &removed, &why));
        LIFT_CHECK(!removed);

        std::string warn;
        cfg.flushIfDirty(&warn);
        LIFT_CHECK(warn.empty());
    }

    // ---- config/unknown-preserved：未知顶层键 / 未知域内键保留 ----
    lift_test::g_current_case = "config/unknown-preserved";
    {
        const fs::path dir = temp.path / "unknown";
        fs::create_directories(dir, ec);
        const std::string before =
            R"({"version":1,"x-extra":{"keep":true},"domains":{"d":{"known":1,"unknown":{"a":2}}}})";
        PluginConfig cfg;
        LIFT_CHECK(writeFile(dir / "plugin-config.json", before));
        std::string err;
        LIFT_CHECK(cfg.attach(toUtf8(dir), &err));
        LIFT_CHECK(setValue(cfg, "d", "known", number(2)));
        std::string warn;
        cfg.flushIfDirty(&warn);
        LIFT_CHECK(warn.empty());
        std::string after;
        LIFT_CHECK(readFile(dir / "plugin-config.json", &after));
        LIFT_CHECK(after.find("x-extra") != std::string::npos);
        LIFT_CHECK(after.find("\"keep\": true") != std::string::npos || after.find("\"keep\":true") != std::string::npos);
        LIFT_CHECK(after.find("unknown") != std::string::npos);
        PluginConfig reread;
        LIFT_CHECK(reread.attach(toUtf8(dir), &err));
        ConfigValue unknown;
        LIFT_CHECK(getValue(reread, "d", "unknown", &unknown));
        LIFT_CHECK(unknown.isObject());
        ConfigValue known;
        LIFT_CHECK(getValue(reread, "d", "known", &known));
        LIFT_CHECK(known.kind == lift::plugin::ConfigValueKind::Number && known.number == 2.0);
    }

    // ---- config/domain-autocreate：不注册、不探测冲突 ----
    lift_test::g_current_case = "config/domain-autocreate";
    {
        const fs::path dir = temp.path / "autocreate";
        fs::create_directories(dir, ec);
        PluginConfig cfg;
        std::string err;
        LIFT_CHECK(cfg.attach(toUtf8(dir), &err));
        LIFT_CHECK(setValue(cfg, "brand.new-domain", "k", text("v")));
        ConfigValue all = cfg.all("brand.new-domain");
        LIFT_CHECK(all.isObject());
        const ConfigValue* v = member(all, "k");
        LIFT_CHECK(v != nullptr && v->text == "v");
        LIFT_CHECK(cfg.path().find("plugin-config.json") != std::string::npos);
        std::string warn;
        cfg.flushIfDirty(&warn);
        LIFT_CHECK(warn.empty());
        std::string text_file;
        LIFT_CHECK(readFile(dir / "plugin-config.json", &text_file));
        LIFT_CHECK(text_file.find("brand.new-domain") != std::string::npos);
    }

    // ---- config/bad-json-exit2：坏 JSON ⇒ 装载失败（= 调用方 exit 2）----
    lift_test::g_current_case = "config/bad-json-exit2";
    {
        const fs::path dir = temp.path / "badjson";
        fs::create_directories(dir, ec);
        std::string err;
        LIFT_CHECK(!attachFile(dir, R"({"version":1,"domains":{)", &err));
        LIFT_CHECK(!err.empty());
        LIFT_CHECK(err.find("plugin-config.json") != std::string::npos);
        LIFT_CHECK(err.find("byte") != std::string::npos);
        std::printf("  [%s] %s\n", lift_test::g_current_case, err.c_str());
    }

    // ---- config/bad-top-level：顶层形状 / version / domains ----
    lift_test::g_current_case = "config/bad-top-level";
    {
        const fs::path dir = temp.path / "badtop";
        fs::create_directories(dir, ec);
        for (const char* bad : {
                 "[]",
                 R"("x")",
                 R"({"version":2,"domains":{}})",
                 R"({"version":"1","domains":{}})",
                 R"({"version":1,"domains":[]})",
                 R"({"version":1})",
                 R"({"domains":{}})",
             }) {
            std::string err;
            LIFT_CHECK(!attachFile(dir, bad, &err));
            LIFT_CHECK(!err.empty());
        }
    }

    // ---- config/bad-domain-value：域值不是对象 ----
    lift_test::g_current_case = "config/bad-domain-value";
    {
        const fs::path dir = temp.path / "baddomain";
        fs::create_directories(dir, ec);
        std::string err;
        LIFT_CHECK(!attachFile(dir, R"({"version":1,"domains":{"d":[]}})", &err));
        LIFT_CHECK(err.find("域 'd'") != std::string::npos);
        LIFT_CHECK(!attachFile(dir, R"({"version":1,"domains":{"d":1}})", &err));
    }

    // ---- config/proto-key：保留名 / 字符集 / 长度 ----
    lift_test::g_current_case = "config/proto-key";
    {
        std::string why;
        for (const char* name : {"__proto__", "prototype", "constructor"}) {
            LIFT_CHECK(!PluginConfig::checkDomain(name, &why));
            LIFT_CHECK(!why.empty());
            LIFT_CHECK(!PluginConfig::checkKey(name, &why));
        }
        LIFT_CHECK(PluginConfig::checkDomain("field.point-visible", &why));
        LIFT_CHECK(!PluginConfig::checkDomain("bad id", &why));
        LIFT_CHECK(!PluginConfig::checkDomain(std::string(65, 'a'), &why));
        LIFT_CHECK(!PluginConfig::checkDomain("", &why));
        LIFT_CHECK(!PluginConfig::checkKey("", &why));
        LIFT_CHECK(!PluginConfig::checkKey(std::string(129, 'k'), &why));
        LIFT_CHECK(!PluginConfig::checkKey(std::string("a\0b", 3), &why));
        LIFT_CHECK(!PluginConfig::checkKey("a\nb", &why));

        PluginConfig cfg;
        std::string err;
        LIFT_CHECK(cfg.attach(toUtf8(root), &err));
        ConfigReceipt receipt;
        LIFT_CHECK(!cfg.set("__proto__", "k", number(1), &receipt, &why));
        LIFT_CHECK(!cfg.set("d", "__proto__", number(1), &receipt, &why));
        ConfigValue out;
        bool found = true;
        LIFT_CHECK(!cfg.get("d", "constructor", &out, &found, &why));
        LIFT_CHECK(!cfg.has("d", "prototype", &found, &why));
        bool removed = false;
        LIFT_CHECK(!cfg.erase("d", "__proto__", &removed, &why));
    }

    // ---- config/limits：深度 / 节点 / 字符串 / NaN ----
    lift_test::g_current_case = "config/limits";
    {
        PluginConfig cfg;
        std::string err;
        LIFT_CHECK(cfg.attach(toUtf8(root), &err));
        std::string why;
        ConfigReceipt receipt;
        // 深度：kConfigMaxDepth 层的根再套一层 ⇒ 超限。
        ConfigValue deep = number(1);
        for (std::size_t i = 0; i < lift::plugin::kConfigMaxDepth; ++i) {
            ConfigValue outer = ConfigValue::array();
            outer.push(std::move(deep));
            deep = std::move(outer);
        }
        LIFT_CHECK(!cfg.set("d", "deep", deep, &receipt, &why));
        LIFT_CHECK(why.find("嵌套太深") != std::string::npos);
        // 节点数。
        ConfigValue wide = ConfigValue::object();
        for (std::size_t i = 0; i < lift::plugin::kConfigMaxNodes; ++i) {
            wide.put("k" + std::to_string(i), number(static_cast<double>(i)));
        }
        LIFT_CHECK(!cfg.set("d", "wide", wide, &receipt, &why));
        LIFT_CHECK(why.find("节点上限") != std::string::npos);
        // 字符串长度。
        LIFT_CHECK(!cfg.set("d", "long", text(std::string(lift::plugin::kConfigMaxStringBytes + 1, 'x')),
                            &receipt, &why));
        LIFT_CHECK(why.find("字符串太长") != std::string::npos);
        // NaN / Infinity。
        LIFT_CHECK(!cfg.set("d", "nan", number(std::nan("")), &receipt, &why));
        LIFT_CHECK(why.find("有限") != std::string::npos);
        LIFT_CHECK(!cfg.set("d", "inf", number(std::numeric_limits<double>::infinity()), &receipt, &why));        // 边界内仍然可以写（成对边界，防止"全都拒"的假绿）。
        ConfigValue okdeep = number(1);
        for (std::size_t i = 0; i + 1 < lift::plugin::kConfigMaxDepth; ++i) {
            ConfigValue outer = ConfigValue::array();
            outer.push(std::move(okdeep));
            okdeep = std::move(outer);
        }
        LIFT_CHECK(setValue(cfg, "d", "okdeep", okdeep));
        LIFT_CHECK(setValue(cfg, "d", "okstr", text(std::string(16, 'x'))));

        // ---- set 侧的域数 / 每域 key 数上限（新增的 TypeError 面）----
        const fs::path capdir = temp.path / "domain-caps";
        fs::create_directories(capdir, ec);
        PluginConfig cap;
        LIFT_CHECK(cap.attach(toUtf8(capdir), &err));
        for (std::size_t i = 0; i < lift::plugin::kConfigMaxDomains; ++i) {
            const std::string domain = "dom" + std::to_string(i);
            LIFT_CHECK(setValue(cap, domain.c_str(), "k", number(1)));
        }
        ConfigReceipt receipt2;
        std::string why2;
        LIFT_CHECK(!cap.set("dom-overflow", "k", number(1), &receipt2, &why2));
        LIFT_CHECK(why2.find("域太多") != std::string::npos);
        PluginConfig keys;
        const fs::path keydir = temp.path / "key-caps";
        fs::create_directories(keydir, ec);
        LIFT_CHECK(keys.attach(toUtf8(keydir), &err));
        for (std::size_t i = 0; i < lift::plugin::kConfigMaxKeysPerDomain; ++i) {
            const std::string key = "k" + std::to_string(i);
            LIFT_CHECK(setValue(keys, "d", key.c_str(), number(1)));
        }
        LIFT_CHECK(!keys.set("d", "k-overflow", number(1), &receipt2, &why2));
        LIFT_CHECK(why2.find("key 太多") != std::string::npos);
    }

    // ---- config/no-appdata：没有 --appdata ⇒ available=false ----
    lift_test::g_current_case = "config/no-appdata";
    {
        PluginConfig cfg;
        std::string err;
        LIFT_CHECK(cfg.attach("", &err));
        LIFT_CHECK(err.empty());
        LIFT_CHECK(!cfg.available());
        LIFT_CHECK(!cfg.readOnly());
        LIFT_CHECK(cfg.path().empty());
        ConfigReceipt receipt;
        std::string why;
        LIFT_CHECK(cfg.set("d", "k", number(1), &receipt, &why));
        LIFT_CHECK(!receipt.applied);
        LIFT_CHECK(std::string(lift::plugin::configReceiptCodeName(receipt)) == "no_appdata");
        ConfigValue out;
        bool found = true;
        LIFT_CHECK(cfg.get("d", "k", &out, &found, &why));
        LIFT_CHECK(!found);
        LIFT_CHECK(cfg.all("d").members.empty());
    }

    // ---- config/limits-key-name：域内 key 名也受 key 长 / UTF-8 上限（attach 期）----
    lift_test::g_current_case = "config/limits-key-name";
    {
        const fs::path dir = temp.path / "limits-key-name";
        fs::create_directories(dir, ec);
        std::string err;
        LIFT_CHECK(!attachFile(
            dir, R"({"version":1,"domains":{"d":{")" + std::string(lift::plugin::kConfigMaxKeyLength + 1, 'k') +
                     R"(":1}}})", &err));
        LIFT_CHECK(err.find("key 名太长") != std::string::npos);
        // 孤立代理项的 WTF-8 作为**域内 key 名**（解析器原样收下 ⇒ 必须在 attach 校验里拒绝）。
        const std::string bad("\xED\xA0\x80", 3);
        LIFT_CHECK(!attachFile(dir, R"({"version":1,"domains":{"d":{")" + ("\"" + bad + "\":1") + "}}}", &err));
        LIFT_CHECK(!err.empty());
        // 边界：128 B 的 key 名可装载。
        LIFT_CHECK(attachFile(
            dir, R"({"version":1,"domains":{"d":{")" + std::string(lift::plugin::kConfigMaxKeyLength, 'k') +
                     R"(":1}}})", &err));
    }

    // ---- config/limits-on-load：**装载期**就执行同一套上限（B1：不能只挡 set）----
    lift_test::g_current_case = "config/limits-on-load";
    {
        const fs::path dir = temp.path / "limits-on-load";
        fs::create_directories(dir, ec);
        const auto nested = [](int arrays) {
            std::string text;
            for (int i = 0; i < arrays; ++i) text += "[";
            text += "1";
            for (int i = 0; i < arrays; ++i) text += "]";
            return text;
        };
        const auto document = [](const std::string& value) {
            return R"({"version":1,"domains":{"d":{"k":)" + value + "}}}";
        };
        std::string err;
        // 8 层容器 + 叶子 = 节点深度 9 ⇒ 拒（当前 attach 会收）。
        LIFT_CHECK(!attachFile(dir, document(nested(8)), &err));
        LIFT_CHECK(err.find("超限") != std::string::npos);
        // 一万层（≤ 1 MiB）⇒ 拒且**不崩**（校验是非递归的）。
        LIFT_CHECK(!attachFile(dir, document(nested(10000)), &err));
        LIFT_CHECK(err.find("超限") != std::string::npos);
        // 节点数：1100 个元素的数组 ⇒ 1101 节点 > 1024 ⇒ 拒。
        std::string wide = "[";
        for (int i = 0; i < 1100; ++i) {
            if (i != 0) wide += ",";
            wide += "1";
        }
        wide += "]";
        LIFT_CHECK(!attachFile(dir, document(wide), &err));
        // 反向边界：7 层容器 + 叶子 = 深度 8 ⇒ attach 成功且 get 得到。
        LIFT_CHECK(attachFile(dir, document(nested(7)), &err));
        PluginConfig cfg;
        LIFT_CHECK(cfg.attach(toUtf8(dir), &err));
        bool found = false;
        ConfigValue out;
        std::string why;
        LIFT_CHECK(cfg.get("d", "k", &out, &found, &why));
        LIFT_CHECK(found);
    }

    // ---- config/invalid-utf8：非法 UTF-8 在 **set 期** 拒绝；之后合法写仍能落盘（I2）----
    lift_test::g_current_case = "config/invalid-utf8";
    {
        const fs::path dir = temp.path / "invalid-utf8";
        fs::create_directories(dir, ec);
        const std::string bad("\xED\xA0\x80", 3);  // U+D800 的 WTF-8（QJS 孤立代理项）
        std::string why;
        LIFT_CHECK(!PluginConfig::checkKey(bad, &why));
        LIFT_CHECK(why.find("UTF-8") != std::string::npos);
        PluginConfig cfg;
        std::string err;
        LIFT_CHECK(cfg.attach(toUtf8(dir), &err));
        ConfigReceipt receipt;
        LIFT_CHECK(!cfg.set("d", "k", text(bad), &receipt, &why));
        LIFT_CHECK(why.find("UTF-8") != std::string::npos);
        LIFT_CHECK(!cfg.set("d", bad, number(1), &receipt, &why));
        ConfigValue object = ConfigValue::object();
        object.put(bad, number(1));
        LIFT_CHECK(!cfg.set("d", "obj", object, &receipt, &why));
        // 坏输入之后**合法 set 仍能落盘**（⛔ 不能把 dump 异常当 I/O 永久熔断）。
        LIFT_CHECK(setValue(cfg, "d", "good", text("ok")));
        std::string warn;
        cfg.flushIfDirty(&warn);
        LIFT_CHECK(warn.empty());
        const fs::path file = dir / "plugin-config.json";
        LIFT_CHECK(fs::exists(file));
        PluginConfig reread;
        LIFT_CHECK(reread.attach(toUtf8(dir), &err));
        ConfigValue back;
        LIFT_CHECK(getValue(reread, "d", "good", &back));
        LIFT_CHECK(back.text == "ok");
    }

    // ---- config/missing-root：appdata 根还不存在 ⇒ 可写，flush 建出文件（I6）----
    lift_test::g_current_case = "config/missing-root";
    {
        const fs::path dir = temp.path / "not-yet";
        PluginConfig cfg;
        std::string err;
        LIFT_CHECK(cfg.attach(toUtf8(dir), &err));
        LIFT_CHECK(cfg.available());
        LIFT_CHECK(!cfg.readOnly());
        LIFT_CHECK(setValue(cfg, "d", "k", number(1)));
        std::string warn;
        cfg.flushIfDirty(&warn);
        LIFT_CHECK(warn.empty());
        LIFT_CHECK(fs::exists(dir / "plugin-config.json"));
    }

    // ---- config/document-size：文件 1 MiB 上限（读入侧）+ 写盘侧（minor 9）----
    lift_test::g_current_case = "config/document-size";
    {
        const fs::path dir = temp.path / "doc-size";
        fs::create_directories(dir, ec);
        std::string err;
        PluginConfig cfg;
        LIFT_CHECK(writeFile(dir / "plugin-config.json",
                             std::string(lift::plugin::kConfigMaxDocumentBytes + 1, 'x')));
        LIFT_CHECK(!cfg.attach(toUtf8(dir), &err));
        LIFT_CHECK(err.find("太大") != std::string::npos);
        fs::remove(dir / "plugin-config.json", ec);
        // 写盘侧：每个 key 合法，但 dump 后 > 1 MiB ⇒ warn 一次 + 不写（⛔ 不进程熔断）。
        PluginConfig huge;
        LIFT_CHECK(huge.attach(toUtf8(dir), &err));
        const std::string payload(1100, 'x');
        for (int i = 0; i < 1000; ++i) {
            const std::string key = "k" + std::to_string(i);
            LIFT_CHECK(setValue(huge, "d", key.c_str(), text(payload)));
        }
        std::string warn;
        huge.flushIfDirty(&warn);
        LIFT_CHECK(!warn.empty());
        LIFT_CHECK(!fs::exists(dir / "plugin-config.json"));
    }

    // ---- config/flush-on-callback：set 只标脏；**没有新写**时 flush 不重写（I8）----
    lift_test::g_current_case = "config/flush-on-callback";
    {
        const fs::path dir = temp.path / "flush";
        fs::create_directories(dir, ec);
        const fs::path file = dir / "plugin-config.json";
        PluginConfig cfg;
        std::string err;
        LIFT_CHECK(cfg.attach(toUtf8(dir), &err));
        LIFT_CHECK(!fs::exists(file));
        LIFT_CHECK(setValue(cfg, "d", "k", number(7)));
        LIFT_CHECK(!fs::exists(file));  // ⛔ set 不写盘
        std::string warn;
        cfg.flushIfDirty(&warn);
        LIFT_CHECK(warn.empty());
        LIFT_CHECK(fs::exists(file));
        std::string first;
        LIFT_CHECK(readFile(file, &first));
        LIFT_CHECK(first.find("\"k\": 7") != std::string::npos || first.find("\"k\":7") != std::string::npos);
        // ★ 行为断言（不是"重写同字节"）：删掉文件后再 flush（**没有新写**）⇒ 文件必须仍不存在。
        //   若实现变成"无视 dirty 每次都写"，这里会把它重新建出来 ⇒ 红。
        fs::remove(file, ec);
        LIFT_CHECK(!fs::exists(file));
        cfg.flushIfDirty(&warn);
        LIFT_CHECK(warn.empty());
        LIFT_CHECK(!fs::exists(file));
        // 反向：新的 set 之后 flush 仍能写（no-op 不是"永久不写"）。
        LIFT_CHECK(setValue(cfg, "d", "k2", number(8)));
        cfg.flushIfDirty(&warn);
        LIFT_CHECK(warn.empty());
        LIFT_CHECK(fs::exists(file));
        std::string after;
        LIFT_CHECK(readFile(file, &after));
        LIFT_CHECK(after.find("\"k\": 7") != std::string::npos);
        LIFT_CHECK(after.find("\"k2\": 8") != std::string::npos);
        LIFT_CHECK(!cfg.readOnly());
    }

    // ---- config/cross-instance-read：同 appdata 二次装载读到 ----
    lift_test::g_current_case = "config/cross-instance-read";
    {
        const fs::path dir = temp.path / "cross";
        fs::create_directories(dir, ec);
        {
            PluginConfig first;
            std::string err;
            LIFT_CHECK(first.attach(toUtf8(dir), &err));
            LIFT_CHECK(setValue(first, "d", "k", text("persisted")));
            std::string warn;
            first.flushIfDirty(&warn);
            LIFT_CHECK(warn.empty());
        }
        PluginConfig second;
        std::string err;
        LIFT_CHECK(second.attach(toUtf8(dir), &err));
        ConfigValue out;
        LIFT_CHECK(getValue(second, "d", "k", &out));
        LIFT_CHECK(out.kind == lift::plugin::ConfigValueKind::String && out.text == "persisted");
    }

    // ---- config/read-only-receipt：只读文件 ⇒ set 回执 + 不改内存 / 不写盘 ----
    //   ★ 放在**最后**：文件系统没把只读位反映成不可写（如 root）时 `LIFT_SKIP`（exit 77，CMake
    //     侧 `SKIP_RETURN_CODE 77`）—— ⛔ 不 printf 后继续（那会被记成绿）。
    lift_test::g_current_case = "config/read-only-receipt";
    {
        const fs::path dir = temp.path / "readonly";
        fs::create_directories(dir, ec);
        const fs::path file = dir / "plugin-config.json";
        const std::string original = R"({"version":1,"domains":{"d":{"known":1}}})";
        LIFT_CHECK(writeFile(file, original));
        fs::permissions(file, fs::perms::owner_read, fs::perm_options::replace, ec);
        LIFT_CHECK(!ec);
        PluginConfig cfg;
        std::string err;
        LIFT_CHECK(cfg.attach(toUtf8(dir), &err));
        if (!cfg.readOnly()) {
            // ★ 先恢复权限再 skip（否则 Windows 上留下只读文件，下一次 remove_all 失败）。
            fs::permissions(file, fs::perms::owner_all, fs::perm_options::replace, ec);
            LIFT_SKIP("文件系统没把只读位反映成不可写（可能是 root）⇒ read_only receipt 不可判定");
        }
        ConfigReceipt receipt;
        std::string why;
        LIFT_CHECK(cfg.set("d", "known", number(2), &receipt, &why));
        LIFT_CHECK(!receipt.applied);
        LIFT_CHECK(std::string(lift::plugin::configReceiptCodeName(receipt)) == "read_only");
        ConfigValue out;
        LIFT_CHECK(getValue(cfg, "d", "known", &out));
        LIFT_CHECK(out.kind == lift::plugin::ConfigValueKind::Number && out.number == 1.0);
        bool removed = false;
        LIFT_CHECK(cfg.erase("d", "known", &removed, &why));
        LIFT_CHECK(!removed);
        std::string warn;
        cfg.flushIfDirty(&warn);
        LIFT_CHECK(warn.empty());
        std::string after;
        LIFT_CHECK(readFile(file, &after));
        LIFT_CHECK(after == original);
        fs::permissions(file, fs::perms::owner_all, fs::perm_options::replace, ec);
    }

    return lift_test::finish("plugin-config");
}
