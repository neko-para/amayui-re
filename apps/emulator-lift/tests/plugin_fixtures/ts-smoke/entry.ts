// S1b 夹具 · `plugin/ts-module`：**入口 `.ts` 经宿主 `initFromFile` 路径装载**。
//
// 判据（红了意味着什么）：
//   · 插件入口没按扩展名加 `JS_EVAL_FLAG_TYPESCRIPT` ⇒ 本文件在 `interface` / 注解处报语法错；
//   · `moduleLoad` 没按**被导入模块名**的扩展名加 flag ⇒ `./lib/helper.ts` 被当 JS 解析 ⇒ 装载失败；
//   · `lib/helper.ts` 的运行期导出没被用到 ⇒ 下面的 click 坐标不会是 (311, 207)。
//
// ★ 同一份 helper 被 `import type`（必须整条擦除、零 module request）与**运行期 import** 各引一次。
import type { Pair, Point } from "./lib/helper.ts";
import { describe, point } from "./lib/helper.ts";

// `interface`（含可选属性）+ `type` 别名（泛型 + 元组）。
interface Banner {
    readonly text: string;
    tag?: string;
}

type SmokeTag = "ts-smoke";
type Settings = Readonly<{ tag?: string }>;

const TAG = "ts-smoke" as const;
// 显式泛型实参：只有 TS 语法才成立（`.js` 里 `point<SmokeTag>(TAG)` 会是比较）。
const TARGET: Point = point<SmokeTag>(TAG);
const BANNER = {
    text: describe([TARGET, point()]),
    tag: TAG,
} as const satisfies Banner; // `as const` + `satisfies`
const PAIR: Pair<string, number> = ["entry", 1];

interface RunCtxLike {
    log(level: string, message: string): void;
    input: { click(x: number, y: number): unknown };
}

export default {
    start(ctx: RunCtxLike) {
        const settings = { tag: TAG } as Settings; // `as`
        const tag = (settings.tag ?? BANNER.tag) as string;
        ctx.log("info", `${BANNER.text}#${PAIR[0]}@${tag}`);
        ctx.input.click(TARGET.x, TARGET.y);
        return [];
    },
    // 合同 §4.6：`start` 与 `onTrigger` 必须成对（本夹具只用 `start`）。
    onTrigger() {
        return [];
    },
};
