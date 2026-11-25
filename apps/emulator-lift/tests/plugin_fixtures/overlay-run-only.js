// S2 复审 I2 夹具 · **run-only** 插件（只有 start/onTrigger，没有 render）。
//
// 判据（`tests/aot_overlay.cpp` 的 `overlay-with-run-only-plugin`）：`--overlay` + 这个插件
// 仍要画 HUD（`(0,0)=#0D131A`）—— S2 之前的行为；⛔ 不许因为"插件没有 render"就静默无 HUD。
const plugin = {
    start() {
        return [];
    },
    onTrigger() {
        return [];
    },
};

export default plugin;
