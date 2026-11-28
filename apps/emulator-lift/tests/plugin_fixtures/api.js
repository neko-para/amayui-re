// v2 夹具 · `plugin/api-surface`
//
// 判据（红了意味着什么）：`RunCtx` 的形状与 d.ts 不一致 —— 缺一个成员（插件写不出来），
// 或者**多**一个已删除的 v1 成员（`trigger` / `hook` / `now` / `state` / `ui` / `log` /
// `draw` / `run` / `dispatch` / `sample` / `screenshot`）。
//
// ★ 这些断言写在**插件自己**里：宿主侧读不到 JS 对象，而 `ctx.fail` 会把名字带进 readout。
const REQUIRED = [
    "read",
    // ★ 异步面（`once` 返回 Promise<Tick>；`task` 登记协程；`delay` / `waitAny`）。
    "once",
    "task",
    "delay",
    "waitAny",
    // ★ 同步订阅面（`at` 返回订阅 id；`off` 撤一条、幂等）。
    "at",
    "off",
    "profile",
    "params",
    "appdata",
    "config",
    "fail",
    "finish",
    "quit",
];

const FORBIDDEN = [
    "edit",
    "input",
    "trigger",
    "hook",
    "now",
    "state",
    "ui",
    "log",
    "draw",
    "run",
    "dispatch",
    "sample",
    "screenshot",
];

// `ctx.once` 的形状探针用（**只喂非法参数** ⇒ 不会真的装 hook）。
const SCRIPT_FOR_SHAPE = "FIELD.BIN";

const plugin = {
    start(ctx) {
        const bad = [];
        for (const key of REQUIRED) {
            if (!(key in ctx)) bad.push(`missing:${key}`);
        }
        for (const key of FORBIDDEN) {
            if (typeof ctx[key] !== "undefined") bad.push(`present:${key}`);
        }
        if (typeof ctx.read.frameHash !== "undefined") bad.push("present:read.frameHash");

        // ---- EngineRead 的形状（C2a 只有 global.get）----
        if (typeof ctx.read.global.get !== "function") bad.push("missing:read.global.get");

        // ---- guest 写只走 Tick（P2b）：在 at handler 里判 Tick 成员表 + 回执形状 ----
        //   ★ 不在 start 里写 guest（RunCtx 没有 edit/input）；handler 命中一次就 finish。
        const handle = ctx.at({ script: SCRIPT_FOR_SHAPE, pos: 0x100 }, (tick) => {
            const tbad = [];
            for (const key of ["id", "now", "local", "edit", "input", "patch"]) {
                if (!(key in tick)) tbad.push("tick.missing:" + key);
            }
            // deprecated 平铺已删除（1 = 仍在）。
            for (const key of ["script", "pos", "opcode"]) {
                if (typeof tick[key] !== "undefined") tbad.push("tick.present:" + key);
            }
            // 回执：Edit（立即）与 Input（只入队）
            const edit = tick.edit.global.set("int", 0xb1b6, 0);
            if (!("accepted" in edit) || !("queued" in edit) || !("applied" in edit)) {
                tbad.push("edit.receipt.shape");
            }
            if (edit.accepted !== true || edit.applied !== true || edit.queued !== false) {
                tbad.push(`edit.receipt.values:accepted=${edit.accepted},applied=${edit.applied},queued=${edit.queued}`);
            }
            const click = tick.input.click(10, 20);
            if (!("id" in click) || typeof click.id !== "number" || click.id <= 0) {
                tbad.push("input.receipt.id");
            }
            const status = tick.input.status(click.id);
            if (status !== "queued") tbad.push(`input.status:${status}`);
            if (tbad.length !== 0) ctx.fail("api-surface", tbad.join(","));
            else ctx.finish(0);
        });
        if (typeof handle !== "number" || handle <= 0) bad.push("at.install:" + String(handle));

        // ---- ctx.params（只读；宿主给了什么就有什么）----
        if (ctx.params.probe !== "api-surface") bad.push(`params.probe:${String(ctx.params.probe)}`);

        // ---- ctx.appdata（只读 stat；RunCtx 恒在；本夹具**没有** attachAppData ⇒ available=false）----
        if (typeof ctx.appdata !== "object" || ctx.appdata === null) {
            bad.push("appdata:not-object");
        } else {
            if (typeof ctx.appdata.available !== "boolean") bad.push("appdata.available");
            if (ctx.appdata.available === false && ctx.appdata.path !== null) {
                bad.push(`appdata.path:${String(ctx.appdata.path)}`);
            }
            if (typeof ctx.appdata.stat !== "function") {
                bad.push("missing:appdata.stat");
            } else {
                const st = ctx.appdata.stat("probe-file.txt");
                if (typeof st !== "object" || st === null) {
                    bad.push("appdata.stat:not-object");
                } else if (ctx.appdata.available === false) {
                    // 没有 --appdata ⇒ 稳定错误码（⛔ 不是异常、也⛔ 不是 exists=true）。
                    if (st.exists !== false) bad.push(`appdata.stat.exists:${String(st.exists)}`);
                    if (st.error !== "no_appdata") bad.push(`appdata.stat.error:${String(st.error)}`);
                } else if (typeof st.exists !== "boolean") {
                    bad.push("appdata.stat.exists");
                }
            }
        }

        // ---- ctx.config（域式 JSON 配置；本夹具**没有** attachConfig ⇒ available=false）----
        //   判据：形状（available / domain）+ 不可用时 `domain()` 抛 `Error("no_appdata")`
        //   （⛔ 不降级成内存态 —— 内建插件必须先看 `available`）。
        if (typeof ctx.config !== "object" || ctx.config === null) {
            bad.push("config:not-object");
        } else {
            if (typeof ctx.config.available !== "boolean") bad.push("config.available");
            if (typeof ctx.config.domain !== "function") {
                bad.push("missing:config.domain");
            } else if (ctx.config.available === false) {
                let threw = false;
                try {
                    ctx.config.domain("probe");
                } catch (err) {
                    threw = true;
                    const message = err !== null && err !== undefined && "message" in err
                        ? String(err.message)
                        : String(err);
                    if (ctx.config.available !== false) bad.push("config.available:mutated");
                    if (message !== "no_appdata") bad.push(`config.domain:error:${message}`);
                }
                if (!threw) bad.push("config.domain:no-appdata-did-not-throw");
            }
        }

        // ---- S2b：异步面的**输入校验**（都是 TypeError，⛔ 不静默接受）----
        //   `once`：where 必须是 {script,pos}；script 非空、pos > 0；timeoutMs **必填且 > 0**。
        //   ★ 这里只喂**非法**参数 ⇒ 形状校验在注册之前抛，不会留下真的临时 hook。
        const badOnce = [
            () => ctx.once({ script: SCRIPT_FOR_SHAPE, pos: 0x100 }),
            () => ctx.once({ script: SCRIPT_FOR_SHAPE, pos: 0x100 }, {}),
            () => ctx.once({ script: SCRIPT_FOR_SHAPE, pos: 0x100 }, { timeoutMs: 0 }),
            () => ctx.once({ script: SCRIPT_FOR_SHAPE, pos: 0x100 }, { timeoutMs: -1 }),
            () => ctx.once({ script: "", pos: 0x100 }, { timeoutMs: 1 }),
            () => ctx.once({ script: SCRIPT_FOR_SHAPE, pos: 0 }, { timeoutMs: 1 }),
        ];
        for (const call of badOnce) {
            let threw = false;
            try {
                call();
            } catch (e) {
                threw = true;
                if (!e || e.name !== "TypeError") bad.push("once.bad-arg.error:" + String(e));
            }
            if (!threw) bad.push("once.bad-arg.accepted");
        }
        //   `task`：fn 必须返回 Promise（普通函数 ⇒ TypeError；⛔ 不把返回值当 task）。
        {
            let threw = false;
            try {
                ctx.task(() => 1);
            } catch (e) {
                threw = true;
                if (!e || e.name !== "TypeError") bad.push("task.non-promise.error:" + String(e));
            }
            if (!threw) bad.push("task.non-promise.accepted");
        }

        // ---- S2c：同步订阅面 `ctx.at` / `ctx.off` 的形状 ----
        //   · `at` 返回**订阅 id（number > 0）**；`off` 返回 boolean 且**幂等**（撤过 / 不存在 ⇒ false）。
        //   · `where` 形状错 ⇒ 当场 TypeError（⛔ 不静默 undefined）；S2c-2 放开五种形态
        //     （`{script,pos}` / `{script,lo,hi}` / `{script}` / `{opcode}` / `{script,opcode}`）。
        if (typeof ctx.at !== "function") bad.push("missing:at");
        if (typeof ctx.off !== "function") bad.push("missing:off");
        if (typeof ctx.at === "function" && typeof ctx.off === "function") {
            // ★ 五种**合法**形态：装得上（id > 0）且撤得掉（off ⇒ true）。⛔ 不留 hook。
            const goodAt = [
                { script: SCRIPT_FOR_SHAPE, pos: 0x100 },
                { script: SCRIPT_FOR_SHAPE, lo: 0x100, hi: 0x108 },
                { script: SCRIPT_FOR_SHAPE },
                { opcode: 114 },
                { script: SCRIPT_FOR_SHAPE, opcode: 114 },
            ];
            for (const where of goodAt) {
                const id = ctx.at(where, () => {});
                if (typeof id !== "number" || id <= 0) {
                    bad.push("at.where.rejected:" + JSON.stringify(where));
                } else if (ctx.off(id) !== true) {
                    bad.push("at.where.off:" + JSON.stringify(where));
                }
            }
            const applied = ctx.at({ script: SCRIPT_FOR_SHAPE, pos: 0x100 }, () => {});
            if (typeof applied !== "number" || applied <= 0) {
                bad.push(`at.return:${String(applied)}`);
            } else {
                if (ctx.off(applied) !== true) bad.push("off.true");
                if (ctx.off(applied) !== false) bad.push("off.false");
                if (ctx.off(999999) !== false) bad.push("off.missing");
            }
            const badAt = [
                () => ctx.at("FIELD.BIN", () => {}),
                () => ctx.at(null, () => {}),
                () => ctx.at({}, () => {}),
                () => ctx.at({ script: "" }, () => {}),
                () => ctx.at({ script: 7, pos: 0x100 }, () => {}),
                () => ctx.at({ pos: 0x100 }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, pos: 0 }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, pos: 256.5 }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, lo: 0x100 }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, hi: 0x108 }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, lo: 2, hi: 2 }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, lo: 1, hi: 2, pos: 0x100 }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, pos: 0x100, opcode: 114 }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, lo: 1, hi: 2, opcode: 114 }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, opcode: -1 }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, opcode: 4294967296 }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, pos: 0x100 }, 1),
                // ★ S2c-2 复审 F1：键**存在**但值是 null/undefined ⇒ TypeError（⛔ 不算"没给"）。
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, pos: undefined }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, pos: null }, () => {}),
                () => ctx.at({ script: SCRIPT_FOR_SHAPE, lo: null, hi: 0x108 }, () => {}),
                () => ctx.at({ script: null }, () => {}),
                () => ctx.at({ opcode: undefined }, () => {}),
            ];
            for (const call of badAt) {
                let threw = false;
                try {
                    call();
                } catch (e) {
                    threw = true;
                    if (!e || e.name !== "TypeError") bad.push("at.bad-arg.error:" + String(e));
                }
                if (!threw) bad.push("at.bad-arg.accepted");
            }
        }

        if (bad.length !== 0) {
            ctx.fail("api-surface", bad.join(","));
        }
    },
};

export default plugin;
