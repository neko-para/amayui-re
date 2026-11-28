// v2 夹具 · `plugin/config-host-pause`：大域 `all()` 的**宿主 CPU** 不计进用户 2 ms
const plugin = {
    start(ctx) {
        const all = ctx.config.domain("big").all();
        const keys = Object.keys(all);
        if (keys.length !== 1024) {
            ctx.fail("config-host-pause", `keys=${keys.length}`);
        }
    },
};

export default plugin;
