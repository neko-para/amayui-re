// C2a 夹具 · `control-script/config-roundtrip`：`ctx.config` 的**读写面**（真 `PluginHost` + 临时 appdata）
//
// 判据（红了意味着什么）：域式配置的读写语义坏了 —— 优先级 / 深拷贝快照 / 回执 / TypeError /
// 持久化（宿主在回调末 flush 一次）。C++ 侧还会核对写出的 `plugin-config.json`：
//   · 预置的 `test.config.pre = 123` 被读到（= attach 真的读了这个 appdata 根的文件）；
//   · 本次 `set` 的值落盘、且未知域 / 未知键**没有被重建掉**（前向兼容）。
//
// ★ 本夹具只判 API 语义，⛔ 不上报游戏读数；失败走 `ctx.fail`（reason 带逐项名字）。
const DOMAIN = "test.config";

const plugin = {
    start(ctx) {
        const bad = [];
        const expectTypeError = (tag, fn) => {
            let threw = false;
            try {
                fn();
            } catch (err) {
                threw = true;
                if (!(err instanceof TypeError)) bad.push(`${tag}:not-type-error`);
            }
            if (!threw) bad.push(`${tag}:no-throw`);
        };

        if (ctx.config === undefined || ctx.config === null) {
            ctx.fail("config-roundtrip", "missing:config");
            return [];
        }
        if (ctx.config.available !== true) {
            ctx.fail("config-roundtrip", `available:${String(ctx.config.available)}`);
            return [];
        }
        const d = ctx.config.domain(DOMAIN);
        if (d === undefined || d === null) {
            ctx.fail("config-roundtrip", "missing:domain");
            return [];
        }
        // ---- 预置值（C++ 侧写进 appdata 根的 plugin-config.json）----
        if (d.get("pre") !== 123) bad.push(`pre:${String(d.get("pre"))}`);
        if (d.get("missing", "fallback") !== "fallback") bad.push("fallback");
        if (d.has("missing") !== false) bad.push("has:missing");

        // ---- set 的六种原生值 + 回执 ----
        const cases = [
            ["num", 7],
            ["frac", 1.5],
            ["str", "hello"],
            ["bool", true],
            ["nul", null],
            ["arr", [1, "two", null]],
            ["obj", { a: 1, nested: { b: [2, 3] } }],
        ];
        for (const [key, value] of cases) {
            const receipt = d.set(key, value);
            if (receipt === undefined || receipt === null || receipt.applied !== true) {
                bad.push(`receipt:${key}`);
            }
            if (receipt !== undefined && receipt !== null && "code" in receipt) {
                bad.push(`receipt.code:${key}:${String(receipt.code)}`);
            }
            if (d.has(key) !== true) bad.push(`has.after.set:${key}`);
        }
        if (d.get("num") !== 7) bad.push(`get.num:${String(d.get("num"))}`);
        if (d.get("str") !== "hello") bad.push(`get.str:${String(d.get("str"))}`);
        if (d.get("arr")[1] !== "two") bad.push("get.arr");
        if (d.get("obj").nested.b[1] !== 3) bad.push("get.obj");

        // ---- all() 是深拷贝快照：改它不影响 store ----
        const snap = d.all();
        if (snap === undefined || snap === null || snap.num !== 7) bad.push("all.num");
        if (snap !== undefined && snap !== null) snap.num = 999;
        if (d.get("num") !== 7) bad.push("all.not-deep");

        // ---- 同一 appdata 的域共享同一 store（同一次 domain() 调用之外）----
        if (ctx.config.domain(DOMAIN).get("num") !== 7) bad.push("domain.not-shared");

        // ---- delete ----
        if (d.set("temp", 1).applied !== true) bad.push("temp.set");
        if (d.delete("temp") !== true) bad.push("temp.delete");
        if (d.has("temp")) bad.push("temp.still-there");
        if (d.delete("temp") !== false) bad.push("temp.delete-twice");

        // ---- 校验错误 = TypeError（⛔ 不是回执 / 静默 no-op）----
        expectTypeError("proto-key", () => d.set("__proto__", 1));
        expectTypeError("proto-get", () => d.get("prototype"));
        expectTypeError("proto-domain", () => ctx.config.domain("constructor"));
        expectTypeError("nan", () => d.set("nan", NaN));
        expectTypeError("undefined", () => d.set("undef", undefined));
        expectTypeError("function", () => d.set("fn", () => 1));

        if (bad.length !== 0) ctx.fail("config-roundtrip", bad.join(","));
        return [];
    },
    // ⛔ 成对：start 存在就必须有 onTrigger（合同 §4.6）；本夹具的用例只用 start。
    onTrigger() {
        return [];
    },
};

export default plugin;
