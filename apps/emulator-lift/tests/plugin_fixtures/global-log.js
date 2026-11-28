// S3 夹具 · 独立全局 `log`（info / warn / error / debug）
//
// 判据（每个 mode 对应一条 C++ 用例；红了意味着什么）：
//   · `all`    ：四路各写一行 —— ① `start` 里 `log.info`；② `ctx.task` 的 continuation
//                （`await Promise.resolve()` 之后）里 `log.warn`；③ tick handler 里 `log.error`；
//                ④ `render` 里 `log.debug`。另加一行 `ctx.log`（正控制：`ctx.log` 仍然可用）。
//                任一路缺 ⇒ `logLines()` 增量对不上；四个方法都传同一个级别 ⇒ `lastLogLevel()` 对不上。
//   · `timeout`：`ctx.once` 到期 ⇒ catch 在**泵**里 drain 到，`log.error` 可用（Run 随后 fail 是
//                既有语义：timeout = 失败截止，用户 catch 了也照样 fail）。
//   · `number` ：`log.info(42, {…})` ⇒ 行文本 = `String(42)` = `42`（非字符串先 `String`；
//                `data` 不进日志行）。
const SCRIPT = "FIELD.BIN";
const POS = 0x0100;

const plugin = {
    start(ctx) {
        const mode = ctx.params.mode;
        if (mode === "all") {
            // 形状 / 冻结：对象 + 四个方法，且**不可写 / 不可配置**（module strict 下写 ⇒ TypeError）。
            const desc = Object.getOwnPropertyDescriptor(globalThis, "log");
            if (desc === undefined || desc.writable !== false || desc.configurable !== false) {
                throw new Error("global-log: log 必须是不可写 / 不可配置的全局");
            }
            for (const name of ["info", "warn", "error", "debug"]) {
                if (typeof log[name] !== "function") throw new Error("global-log: 缺方法 " + name);
            }
            log.info("global-log: start");
            ctx.task(async () => {
                await Promise.resolve();
                log.warn("global-log: task");
            });
            // tick handler：`ctx.at` 的订阅参与链 / 算 Run 存活。
            const handle = ctx.at({ script: SCRIPT, pos: POS }, (tick, next) => {
                log.error("global-log: tick");
                next();
            });
            if (typeof handle !== "number") throw new Error("global-log: ctx.at failed");
            return null;  // Run 靠 at 订阅活着（下面还要 dispatch / render）
        }
        if (mode === "timeout") {
            ctx.task(async () => {
                try {
                    await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 1 });
                    log.error("global-log: unexpected resolve");
                } catch (e) {
                    // ★ 这条 catch 在**泵**里跑（timeout 的非 tick 点 drain）。
                    log.error("global-log: timeout");
                }
            });
            return null;
        }
        if (mode === "number") {
            log.info(42, { ignored: true });
            return null;
        }
        throw new Error("global-log: unknown mode: " + String(mode));
    },
    render() {
        log.debug("global-log: render");
    },
};

export default plugin;
