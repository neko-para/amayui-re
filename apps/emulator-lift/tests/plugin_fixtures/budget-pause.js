// v2 夹具 · **宿主调用暂停**：`ctx.profile.end` 自己的 CPU 不计入用户 JS 的 2 ms。
const plugin = {
    start(ctx) {
        ctx.profile.end();
    },
};

export default plugin;
