// S1b 夹具 · `plugin/ts-module`：**入口 `.ts` 经宿主 `initFromFile` 路径装载**。
// ★ P2b：guest 写只走 Tick（RunCtx 没有 input）⇒ click 放在 `ctx.at` 的 handler 里；
//   坐标仍来自 helper 的**运行期**导出（判据不变：不是"没报错"）。
import type { Pair, Point } from "./lib/helper.ts";
import { describe, point } from "./lib/helper.ts";

interface Banner {
    readonly text: string;
    tag?: string;
}

type SmokeTag = "ts-smoke";
type Settings = Readonly<{ tag?: string }>;

const TAG = "ts-smoke" as const;
const TARGET: Point = point<SmokeTag>(TAG);
const BANNER = {
    text: describe([TARGET, point()]),
    tag: TAG,
} as const satisfies Banner;
const PAIR: Pair<string, number> = ["entry", 1];

interface TickLike {
    readonly input: { click(x: number, y: number): unknown };
}

interface RunCtxLike {
    at(where: { script: string; pos: number }, fn: (tick: TickLike) => void): number;
    off(id: number): boolean;
}

export default {
    start(ctx: RunCtxLike) {
        const settings = { tag: TAG } as Settings;
        const tag = (settings.tag ?? BANNER.tag) as string;
        log.info(`${BANNER.text}#${PAIR[0]}@${tag}`);
        let handle = 0;
        handle = ctx.at({ script: "FIELD.BIN", pos: 0x100 }, (tick) => {
            ctx.off(handle);  // 撤掉唯一的存活来源 ⇒ 本入口收口（run=done）
            tick.input.click(TARGET.x, TARGET.y);
        });
    },
};
