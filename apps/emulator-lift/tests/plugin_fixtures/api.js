// C2a 夹具 · `plugin/api-surface`
//
// 判据（红了意味着什么）：`RunCtx` 的形状与 d.ts 不一致 —— 缺一个成员（插件写不出来），
// 或者**多**一个本切片不该有的成员（`draw` / `run` / `dispatch` / `sample` / `screenshot` /
// `read.frameHash`：声明了却不存在的东西会让"声明与实现"长期说谎）。
//
// ★ 这些断言写在**插件自己**里：宿主侧读不到 JS 对象，而 `ctx.fail` 会把名字带进 readout。
// ★ D-C2-6：`ctx.read.frameHash` **不安装**（⛔ 不返回 0）⇒ 这里断言它 `undefined`。
// ★ C2b：`ctx.draw` 在 **RunCtx** 里仍然缺席（draw 只装在 RenderCtx 上 —— `plugin/render-surface`），
//   而 `ctx.read.frames` / `ctx.read.scriptPos` 同样只装在 RenderCtx.read 上 ⇒ 这里也钉缺席。
const REQUIRED = [
    "edit",
    "input",
    "read",
    "trigger",
    "ui",
    "profile",
    "state",
    "params",
    "appdata",
    "config",
    "log",
    "fail",
    "finish",
    "quit",
    "now",
];

const FORBIDDEN = ["draw", "run", "dispatch", "sample", "screenshot"];

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
        // ★ C2b：`frames` / `scriptPos` 只装在 **RenderCtx.read** 上（Run 语境的位置读数走
        //   `ctx.now.pos` / `ev.pos`）；RunCtx 里安装它们等于在多一个"没有读数源"的口
        //   ⇒ 这里钉住**缺席**（`plugin/render-surface` 钉住那边的**在场**）。
        if (typeof ctx.read.frames !== "undefined") bad.push("present:read.frames");
        if (typeof ctx.read.scriptPos !== "undefined") bad.push("present:read.scriptPos");

        // ---- EngineRead 的形状（C2a 只有 global.get）----
        if (typeof ctx.read.global.get !== "function") bad.push("missing:read.global.get");

        // ---- 回执形状：Edit（立即）与 Input（只入队）----
        const edit = ctx.edit.global.set("int", 0xb1b6, 0);
        if (!("accepted" in edit) || !("queued" in edit) || !("applied" in edit)) {
            bad.push("edit.receipt.shape");
        }
        if (edit.accepted !== true || edit.applied !== true || edit.queued !== false) {
            bad.push(`edit.receipt.values:accepted=${edit.accepted},applied=${edit.applied},queued=${edit.queued}`);
        }
        const click = ctx.input.click(10, 20);
        if (!("id" in click) || typeof click.id !== "number" || click.id <= 0) {
            bad.push("input.receipt.id");
        }
        const status = ctx.input.status(click.id);
        if (status !== "queued") bad.push(`input.status:${status}`);

        // ---- ctx.now 的形状（D-C2-7：本回调内有效）----
        if (typeof ctx.now !== "object" || ctx.now === null) {
            bad.push("now:not-object");
        } else {
            if (typeof ctx.now.readIndex !== "number") bad.push("now.readIndex");
            if (typeof ctx.now.posValid !== "boolean") bad.push("now.posValid");
        }

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

        if (bad.length !== 0) {
            ctx.fail("api-surface", bad.join(","));
        }
        return [];
    },
    // ⛔ 成对：start 存在就必须有 onTrigger（合同 §4.6）；本夹具的用例只用 start。
    onTrigger() {
        return [];
    },
};

export default plugin;
