// S1b 夹具 · `plugin/ts-module`：**入口 `.ts` 经宿主 `initFromFile` 路径装载**。
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

interface RunCtxLike {
    input: { click(x: number, y: number): unknown };
}

export default {
    start(ctx: RunCtxLike) {
        const settings = { tag: TAG } as Settings;
        const tag = (settings.tag ?? BANNER.tag) as string;
        log.info(`${BANNER.text}#${PAIR[0]}@${tag}`);
        ctx.input.click(TARGET.x, TARGET.y);
    },
};
