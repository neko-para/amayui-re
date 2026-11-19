// 典型用法：HUD 在 A 显示、在 B 隐藏；两个具名 Trigger 并发 armed（第五轮，v5.3.2）。
// render 每帧用 ctx.draw 发 display list。
//
// ⛔ 不写具体坐标 / 文本；所有数值都是占位。

import type {
    FiredEvent,
    PluginModule,
    RenderCtx,
    RunCtx,
    TimeoutEvent,
    Trigger,
} from "@amayui/lift-plugin";

const HUD_ID = "hud";
const HUD_HOVER_ID = "hud:hover"; // render 写 UI 状态（hover）

const N = {
    show: "hud:show",
    hide: "hud:hide",
} as const;

const ENTER_SCRIPT = "TITLE"; // 占位：进屏脚本
const ENTER_LO = 0x00000000; // 占位：进屏停点区间
const ENTER_HI = 0x00000000;
const LEAVE_SCRIPT = "FIELD"; // 占位：另一个正向位置（离屏的那个已知停点）
const LEAVE_LO = 0x00000000;
const LEAVE_HI = 0x00000000;

const HUD_ANCHOR_X = 0; // 占位
const HUD_ANCHOR_Y = 0; // 占位
const HUD_LINE_H = 0; // 占位
const HUD_PAD = 0; // 占位
const HUD_MIN_W = 0; // 占位
const HUD_FONT_SIZE = 0; // 占位
const HUD_FILL = ""; // 占位：颜色串
const HUD_STROKE = ""; // 占位
const HUD_TEXT_FILL = ""; // 占位
const HUD_HOVER_FILL = ""; // 占位

interface HudData {
    readonly title: string;
    readonly lines: readonly string[];
}

// Trigger 需要 ctx 才能创建 ⇒ 不能进静态 initialState；首读是 undefined，必须 guard。
interface HudRunState {
    show?: Trigger;
    hide?: Trigger;
}

function makeTriggers(ctx: RunCtx<HudRunState>): [Trigger, Trigger] {
    return [
        ctx.trigger.afterPosition(ENTER_SCRIPT, ENTER_LO, ENTER_HI, {
            name: N.show,
            mode: "reenter",
        }),
        ctx.trigger.afterPosition(LEAVE_SCRIPT, LEAVE_LO, LEAVE_HI, {
            name: N.hide,
            mode: "reenter",
        }),
    ];
}

function start(ctx: RunCtx<HudRunState>): Trigger[] {
    const [show, hide] = makeTriggers(ctx);
    ctx.state.show = show;
    ctx.state.hide = hide;
    return [show, hide];
}

function onTrigger(ctx: RunCtx<HudRunState>, ev: FiredEvent | TimeoutEvent): Trigger[] | null {
    if (ev.kind === "timeout") {
        ctx.ui.hide(HUD_ID);
        ctx.finish(5, `hud: gate timeout (${ev.name})`);
        return null;
    }

    const { show, hide } = ctx.state;
    if (show === undefined || hide === undefined) {
        ctx.fail(undefined, "hud: state not initialized");
        return null;
    }

    switch (ev.name) {
        case N.show: {
            const data: HudData = { title: "", lines: [] }; // 占位：文本由本地化提供
            ctx.ui.show(HUD_ID, data);
            break;
        }
        case N.hide:
            ctx.ui.hide(HUD_ID);
            break;
        default:
            ctx.fail(ev.name, "unknown state");
            return null;
    }

    // 返回完整的新 armed 集合：两个都是**同一个对象**，边沿/maxFires 继续有效。
    return [show, hide];
}

// ---------------------------------------------------------------------------
// render：每帧按 UI 状态画（v5.3.2：1280×720 逻辑系；z 序=调用顺序；每帧清空）
// ---------------------------------------------------------------------------
function render(ctx: RenderCtx): void {
    if (!ctx.ui.visible(HUD_ID)) return;

    // get<T> 是调用方断言；宿主不做运行时校验（d.ts:168）。
    const state = ctx.ui.get<HudData>(HUD_ID);
    if (state === undefined) return;

    const titleStyle = { size: HUD_FONT_SIZE, fill: HUD_TEXT_FILL };
    const title = ctx.draw.measure(state.title, titleStyle);
    const width = Math.max(HUD_MIN_W, title.width + HUD_PAD * 2);
    const height = title.height + HUD_PAD * 2 + state.lines.length * HUD_LINE_H;

    const mouse = ctx.game.mouse();
    const hover =
        mouse.x >= HUD_ANCHOR_X &&
        mouse.x < HUD_ANCHOR_X + width &&
        mouse.y >= HUD_ANCHOR_Y &&
        mouse.y < HUD_ANCHOR_Y + height;

    // render 也可写 ctx.ui（hover/动画）；只影响 UI 状态，不做 guest 写。
    ctx.ui.set(HUD_HOVER_ID, hover);

    // 后调用在上：先底框，再标题，再正文。
    ctx.draw.rect(HUD_ANCHOR_X, HUD_ANCHOR_Y, width, height, {
        fill: hover ? HUD_HOVER_FILL : HUD_FILL,
        stroke: HUD_STROKE,
    });
    ctx.draw.text(HUD_ANCHOR_X + HUD_PAD, HUD_ANCHOR_Y + HUD_PAD, state.title, titleStyle);

    state.lines.forEach((line, i) => {
        ctx.draw.text(
            HUD_ANCHOR_X + HUD_PAD,
            HUD_ANCHOR_Y + HUD_PAD + title.height + i * HUD_LINE_H,
            line,
            titleStyle,
        );
    });
}

const plugin = {
    initialState: {},
    render,
    start,
    onTrigger,
} satisfies PluginModule<HudRunState>;

export default plugin;
