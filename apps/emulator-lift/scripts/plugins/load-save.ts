// scripts/plugins/load-save.ts —— **根入口**：`loadSave` 的参考插件（S1 从 `lib/save.ts` 拆出来；v2）
//
// 为什么在根（用户口径 2026-11-25）：
//   · `--plugin` 的 id 只查 `scripts/plugins/` **根**（非递归），`lib/` **永远不是 id 目标**；
//   · `lib/` 只放被 import 的库 ⇒ "需要单独加载的入口"必须住在根上，库的"参考入口"也拆到根。
//   ⇒ `lib/save.ts` 只剩 `loadSave` / `SAVE_DIR_REL` 等（没有 default 导出）；本文件是唯一的载入入口。
//   id = `load-save`（文件名 stem）。路径写法仍可用（`scripts/plugins/load-save.ts`）。
//
// ★ import 口径：入口根 = 本文件所在目录（`scripts/plugins/`）⇒ 库一律 `./lib/...`（**向下** import
//   允许；⛔ `../` 越出 plugin 根会被 QJS 拒）。
//
// 参考入口（**只是参考**；库消费者用 `./lib/save.ts` 的 `loadSave`）
//
// 参数来源（2026-11-25 起）：**config > --plugin-param > 默认**
//   · config = `<--appdata>/plugin-config.json` 的域 **`save`**（ctx.config；没 appdata ⇒ 完全回退）；
//     key：`slot`（number）、`expect`（string[] 或逗号串）、可选 `saveDir`（string，相对 appdata 根）、
//     `timeoutMs`（number）、`precheck`（boolean）。
//   · params = `--plugin-param k=v`（夹具 / 诊断口）；五个 key（`slot` / `expect` / `saveDir` /
//     `timeoutMs` / `precheck`）都支持 params 回退（后三个是 2026-11-25 加的，值 = 字符串）。
//   · 默认：`expect` 缺 ⇒ `FIELD,SC0000` 并打一行 info；`slot` 没有默认值（缺 ⇒ `loadSave` 响亮 throw）。
// ★ "给了"的判据是 `has()`（⛔ 不是真值）：`slot: 0` 这种 falsy 值也算给了。
// ★ 2026-11-25：**取配置的六件事全部搬进 `./lib/settings.ts`**（`openConfigDomain` / `slotSetting` /
//   `scriptListSetting` / `textSetting` / `intSetting` / `boolSetting`）；本文件只保留"哪几个 key + 默认值"。
//   行为逐字不变（`slot` 的整数校验 / `expect` 归一 / `precheck` 并集语义 / `timeoutMs` 的正整数）。
//
// `slot`：非整数 / 越界 ⇒ `loadSave` 调用时 throw ⇒ 宿主记 `run=failed`（进程 exit 不受影响）。
// `expect`：裸名自动补 `.BIN`，带 `.BIN` 也接受。
//
// ★ v2 形状（2026-11-28）：参考入口**自带 TITLE 门**（`./lib/boot-to-title.ts`）—— 它可以在
//   **任意启动点**当插件直接跑，所以形状 = 一条 `ctx.task`：
//     `await bootToTitle(t, …)`（拿 TITLE 命中那一拍的 Tick）→ `await loadSave(t, slot, options, tick)`。
//   ⛔ 没有 `onTrigger` / `route`；两段就是顺序 `await`（前缀分流的概念随 `lib/chains.ts` 一起删了）。
//   ⚠ 这与库接口不同：`loadSave(...)` 单独用时**调用方**保证已在 TITLE（`./lib/save.ts` 文件头）。
// ---------------------------------------------------------------------------
import type { ConfigDomain, PluginModule, RunCtx } from "@amayui/lift-plugin";

import { bootToTitle } from "./lib/boot-to-title.ts";
import { loadSave, validateLoadSave } from "./lib/save.ts";
import type { LoadSaveOptions } from "./lib/save.ts";
import {
    boolSetting,
    intSetting,
    openConfigDomain,
    scriptListSetting,
    slotSetting,
    textSetting,
} from "./lib/settings.ts";

/** 配置文件里的域（`<--appdata>/plugin-config.json`；只读）。 */
const CONFIG_DOMAIN = "save";
/** `expect` 的默认落点（只在该入口既没有 config 也没有 params 时用；打一行 info 说明）。 */
const DEFAULT_EXPECT: string[] = ["FIELD.BIN", "SC0000.BIN"];

/**
 * 本入口的四个可选值：**唯一**的取配置入口 = `./lib/settings.ts`（2026-11-25 抽取）。
 * ★ 这里的顺序就是"config > `--plugin-param` > 默认"；`slot` 的"没给"仍交给 `loadSave`（NaN / 越界）。
 *
 * @param ctx RunCtx。
 * @param domain 域对象（不可用 = null）。
 */
function optionsFor(ctx: RunCtx, domain: ConfigDomain | null): LoadSaveOptions {
    const expect = scriptListSetting(ctx, domain, "expect", CONFIG_DOMAIN);
    const options: LoadSaveOptions = { expect: expect === null ? DEFAULT_EXPECT : expect };
    const saveDir = textSetting(ctx, domain, "saveDir", undefined, { domainId: CONFIG_DOMAIN });
    if (saveDir !== undefined) options.saveDir = saveDir;
    const timeoutMs = intSetting(ctx, domain, "timeoutMs", undefined, {
        min: 1,
        domainId: CONFIG_DOMAIN,
        // ★ 逐字保留旧行为：config 里的 `timeoutMs` 也接受 `"5000"`（旧实现是 `Number(raw)`）。
        coerce: true,
    });
    if (timeoutMs !== undefined) options.timeoutMs = timeoutMs;
    const precheck = boolSetting(ctx, domain, "precheck", undefined, { domainId: CONFIG_DOMAIN });
    if (precheck !== undefined) options.precheck = precheck;
    return options;
}

const plugin = {
    start(ctx: RunCtx): void {
        // ★ config / params 的读取只在 `start`（Run 回调）里做一次：一次 Run 内它们不变
        //   （v1 用模块级 `entryChain` 缓存省掉的就是这个；v2 的 task 只捕这一份值）。
        const domain = openConfigDomain(ctx, CONFIG_DOMAIN);
        if (scriptListSetting(ctx, domain, "expect", CONFIG_DOMAIN) === null) {
            log.info(
                `load-save: expect 未给 ⇒ 用默认 ${DEFAULT_EXPECT.join(",")}` +
                    `（写在 plugin-config.json 的 ${CONFIG_DOMAIN}.expect，或用 --plugin-param expect=FIELD,SC0000）`,
            );
        }
        const slot = slotSetting(ctx, domain, undefined, CONFIG_DOMAIN);
        const options = optionsFor(ctx, domain);
        // ★ P1 复审 S3：**同步首段**先把纯参数校验跑掉（slot / expect / timeoutMs / saveDir / precheck），
        //   再进 `ctx.task`。理由 = 找回 v1 的"**构造时**失败"时序：v1 在 `chainFor` 里构造
        //   `loadSave(slot, …)`，坏 slot / 坏 expect 在**第一个 Run 回调**里就 throw ⇒ `run=failed`
        //   并点名参数；P1 把校验留在 `loadSave()` 里（= `await bootToTitle` **之后**）会让它晚一步 ——
        //   启动永远到不了 TITLE 时只报 boot 门超时，把"参数非法"与"启动没到"混成同一条。
        //   ★ 这里不重复实现判据：`validateLoadSave` 与 `loadSave` 内部用的是**同一个**函数
        //     （⛔ 不要在入口再抄一份范围 / 文案）。
        validateLoadSave(slot, options);
        ctx.task(async (t) => {
            // TITLE 门的超时沿用 `bootToTitle` 的缺省（30000 ms；v1 参考入口也是缺省）。
            const tick = await bootToTitle(t);
            await loadSave(t, slot, options, tick);
        });
    },
} satisfies PluginModule;

export default plugin;
