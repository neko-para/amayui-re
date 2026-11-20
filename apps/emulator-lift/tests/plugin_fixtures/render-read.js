// C2b 夹具 · `plugin/render-read`：**render 里**的 `ctx.read.global.get` / `ctx.log` 有宿主来路。
//
// 判据（红了意味着什么）：`PluginScript::setCallHost`（+ `PluginHost::readWired` 不绑回调）
//   没有接上 —— 那会让 render 的读数面**静默缺失**：插件在 render 里读一个 global 就 TypeError，
//   或者 `ctx.log` 直接抛。合同 §4.4c 的"render 只拿当帧 readings"**包括** `ctx.read`。
const plugin = {
    render(ctx) {
        const probe = ctx.params.probe;
        if (probe === "global") {
            const v = ctx.read.global.get("int", 0xb1b6);
            if (v !== Number(ctx.params.expect)) throw new Error("read.global.get:" + v);
            return;
        }
        if (probe === "log") {
            ctx.log("info", "from-render");
            ctx.log("info", "from-render");  // 连续重复 ⇒ 宿主去重（只记一行）
            return;
        }
        throw new Error("unknown probe: " + probe);
    },
};

export default plugin;
