// C2a/S2b 夹具 · **`onTrigger` 单独存在** ⇒ 装载必须失败（没有 `start` 就永远没人调它）
//
// ★ S2b 的 v2 兼容层**只**放宽了 `start` 单独存在（见 `start-only.js`）；
//   反方向仍拒绝：`onTrigger` 是一个回调面，没有 `start` 就永远没有 Run 去 arm 它。
const plugin = {
    onTrigger() {
        return [];
    },
};

export default plugin;
