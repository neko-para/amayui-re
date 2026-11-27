// S1b 夹具 · `lib/helper.ts`：被 `entry.ts` 的 `import type` 与运行期 import 各引一次。
// 判据（红了意味着什么）：宿主 `moduleLoad` 必须按**模块名扩展名**加 `JS_EVAL_FLAG_TYPESCRIPT`，
//   否则本文件在 `interface` / 返回类型注解处报语法错 ⇒ `entry.ts` 的依赖装载失败。
export interface Point {
    readonly x: number;
    readonly y: number;
    label?: string; // 可选属性
}

export type Pair<A, B> = readonly [A, B];

// 运行期坐标：`plugin/ts-module` 用它们判"helper 的导出真的被算进了 click"。
const ORIGIN = { x: 311, y: 207 } as const;

// 泛型函数 + 返回类型注解（`Point & { label?: Label }`）。
export function point<Label extends string>(label?: Label): Point & { label?: Label } {
    return label === undefined ? { ...ORIGIN } : { ...ORIGIN, label };
}

export function describe(points: readonly Point[]): string {
    return points.map((p) => `${p.label ?? "?"}:${p.x},${p.y}`).join("|");
}
