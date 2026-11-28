// v2 夹具 · **Run 收口之后** present 仍会每帧调 render ⇒ 不许再泵 / 再 settle / 跑用户 JS
const SCRIPT = "FIELD.BIN";
const POS = 0x0100;

const plugin = {
    start(ctx) {
        ctx.task(async () => {
            try {
                await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 60000 });
                log.info("render-after-finish: unexpected resolve");
            } catch (e) {
                log.info("render-after-finish: task resumed (should-not-run)");
            }
        });
        log.info("render-after-finish: armed");
    },
    render() {
        log.info("render-after-finish: render-ran");
    },
};

export default plugin;
