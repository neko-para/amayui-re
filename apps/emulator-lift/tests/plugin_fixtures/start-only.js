// v2 夹具 · **`start` 单独存在**（没有 `onTrigger`）⇒ 合法
const plugin = {
    start() {
        log.info("start-only: registered");
    },
};

export default plugin;
