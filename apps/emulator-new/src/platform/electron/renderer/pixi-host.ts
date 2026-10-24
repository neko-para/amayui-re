/**
 * `apps/emulator-new/src/platform/electron/renderer/pixi-host.ts` —— **渲染进程**的 Pixi 后端。
 *
 * 职责：把 `RenderCall` 翻译成 Pixi 操作，并把 `SceneSnapshot` 画出来。
 * ★ 2026-10 之后 core 就在同一个 renderer 里：本文件**不再经过 IPC**，由 `session.ts` 的
 *   `LocalRenderHost` 直接调用 `execute` / `applyScene` / `canvasSize`。
 * ★ 只 import **纯核心**的 `applyColorKey`（无 Node / DOM）；⛔ 不 import `node:*`，也不反向 import
 *   `session.ts`。core 的 `RenderHost` 适配在 `session.ts` 里，不在这里。
 * ★ 快照里的 `width/height/left/top/right/bottom` 都是**客户区像素**，画布按 `init` 的尺寸、
 *   1:1 坐标绘制；Retina 由 Pixi 的 `resolution` 处理（画布的 CSS 尺寸仍是客户区像素）。
 *
 * ════════════════ 有据的部分（0x204 的取证包）与仍然未取证的选择 ════════════════
 *
 * ★ `0x204` 已拿到逐字证据（取证包，锚是 EA；见回报）：**Shift-JIS 字节串**（`0x46F3D4` 前导字节判定 /
 *   `0x46F4CD` 双字节合成 / `0x8140` 全角空格特例）；`Font::init` 的默认值 —— size **24**
 *   （`0x4654C3`）、主色 **0xFFFFFF**（`0x4655FB`）、副色 **0**（`0x465605`）、绘制模式 **1**
 *   （`0x465611`，默认带 (1,1) 阴影）；坐标 `(x, y)` 是**行顶**（top）。
 *   ⇒ 这些在下面按"已证"处理；**未取证**的是像素级结果（字形栅格化、抗锯齿、阴影偏移的精确形状）。
 *
 * ════════════════════════════ 仍然未取证的选择（⛔ 不是 spec 结论）════════════════════════════
 *
 * spec（`data/spec/chapters/60-ops-render.md`）只规定"引擎向平台要什么"；下面这些都是平台侧实现
 * 在**没有像素级观察**时做的选择，UI 的 diagnostics 面板会把它们逐条显示给人看：
 *
 * 1. 文本解码：核心的 `text` 是 **latin1 字节串**（每个 charCode 0..255），本实现按
 *    `TextDecoder('shift_jis')` 解成 Unicode —— **编码已证是 Shift-JIS**（见上），这一条不是猜测。
 *    ★ 仍然属于**选择**的只有**失败回退**：解码失败（非法字节序列）时退回**逐字节 latin1**
 *    （`String.fromCharCode`，与 `core/bytes.ts` 的 latin1 口径一致 —— ⛔ 不用 `TextDecoder('latin1')`，
 *    那是 windows-1252 的别名，会改 0x80–0x9F）。引擎自己没有这条回退（它按规定逐字节解 Shift-JIS），
 *    这里只是让平台层不因为一条坏字节整帧失败，并**记一条日志**。
 * 2. `RES-RENDER-TEXT-FONT-STATE`（spec 未定）：字号 / 主色 / 副色 / 绘制模式有 `Font::init` 的默认值
 *    （24 / 0xFFFFFF / 0 / 1），但**face name** 来自 `0x1a5` 写入的 `Font+0x4EC`，默认**未建立**
 *    ⇒ `font === null` 时用固定回退栈 `Yu Gothic → Yu Gothic UI → MS Gothic → Meiryo → sans-serif`；
 *    `font !== null` 时把它当成 **CSS font-family 名**并追加同一回退栈（它也可能是 Win32 的 LOGFONT
 *    字面名 —— 未取证）。**哪一款字模被真正选中**没有观察。
 * 3. `RES-RENDER-TEXT-SURFACE`（spec 未定）：坐标是**行顶**已证 ⇒ 用 Pixi `Text` 的 top-left 定位
 *    （⛔ 不自己加 ascent）；但**字形像素**（栅格化 / 抗锯齿 / 阴影形状）没有取证，diagnostics 明写。
 * 4. 颜色有**两套口径**，⛔ 别混：
 *    - **字体色**（`0x204` 的主色 / 副色）= **24 位 RGB（COLORREF）**，**已证**：`0x76` / `0x77` 写
 *      `Font+0x550` / `Font+0x554` 时只取操作数低 24 位、高 8 位丢弃 ⇒ 字永远不透明，默认白是白的。
 *    - **绘制项颜色 / 场景混合色** = **ARGB**（`0x202` / `0x203` / `0x33f` 的合成结果，高 8 位 = 透明度）。
 *      ★ 高 8 位是 0 时按这个口径就是"完全透明、看不见" —— 照字面执行，并在 diagnostics 里显示。
 * 5. **绘制模式**（`0x465611` 起的分支，取值 0/1/2/3）：0 = 只画主色；1 = 先副色在 (+1,+1) 一遍再主色；
 *    2 = 主色两遍；3 = 副色多 pass 径向。本实现做 0/1/2 的近似，**3 只做 8 向近似**并在面板里
 *    标"未实现逐像素"（⛔ 不声称与引擎逐像素一致）。
 * 6. `RES-RENDER-BLEND-PIXEL`（spec 未定）：`0x33f` 的模式格是**原样写入的整数**，spec 明说
 *    "读取方怎样分支属外部模块" ⇒ 下面的 `BLEND_MODE_MAP` 是**映射表**，除 0 = normal 是任务约定外
 *    全部未取证。未知值 ⇒ normal + 一条日志（⛔ 不猜）。
 * 7. `present` 渲染到纹理目标时：空白槽（`hasContent = false`）用 `clear: true`；已有文件像素 /
 *    离屏文字的槽用 `clear: false`，否则会把它们擦掉。呈现与目标的次序本版未取证 ⇒ diagnostics 标出。
 * 8. 文件纹理由 core 的 AGF 解码 + `uploadTexture` 上传；**绘制项的消费**按
 *    `CORE-OPS-TEXTURE-DRAW-CONSUMER` 走纯规划器 `scene-draw-plan.ts`（空槽 / 无目标 / 绘制失败
 *    ⇒ 上报并**跳过**，⛔ 不建占位块；夹空 ⇒ 静默跳过）。空白创建（`0x1f8`）的初始像素未定义。
 * 9. `0x1fb` 的四条边是**来源矩形**、两个浮点是**目标位置**；来源矩形**夹到纹理对象自己的逻辑矩形**
 *    （不是表面尺寸），左 / 上被裁时位置补回同样的量。位置档位（`[20,30)` 缩放 + 半上取整、场景偏移）
 *    由规划器按 spec 5.1 算；**场景相机模型本实现还没有** ⇒ 按"无相机 / 无缩放"档（缩放 1、偏移 0），
 *    诊断面板会标出这条缺口。⛔ 不缩放 / 不拉伸纹理（拉伸是否会篡改像素未取证）。
 */

import { Application, Container, Graphics, Rectangle, RenderTexture, Sprite, Text, Texture } from 'pixi.js';
import type { BLEND_MODES } from 'pixi.js';
import { applyColorKey } from '../../../core/agf.ts';
import { orderedDrawables, planCommandPassForScene } from './scene-order.ts';
import { drawFailedReport, planDraw } from './scene-draw-plan.ts';
import type { DrawPlanNote, DrawPlanSlot } from './scene-draw-plan.ts';
import type {
  RendererInit,
  RendererReady,
  RenderCall,
  SceneSnapshot,
  SurfaceRef,
  TextureSnapshot,
} from '../protocol.ts';

/* ─────────────────────────────── 常量（有据 / 未取证的选择集中在这里） ─────────────────────────────── */

/** 后缓冲的 `surfaceId`（与 `RenderCall.getBackBuffer` 的回复一致；纹理表面从 1 开始分配） */
export const BACK_BUFFER_SURFACE_ID = 0;

/**
 * `drawText` 的字号默认值 = **24**。
 * ★ 有据：`Font::init` 把 size 写成 24（`0x4654C3`，取证包）。协议里没有字号参数时用它；
 *   若 `drawText.params` 后来带上 `size`，以参数为准。
 */
export const TEXT_FONT_SIZE = 24;

/**
 * 主色默认值 = **0xFFFFFF**（`Font::init`，`0x4655FB`）。只在 `color === null` 时用。
 * ★ 字体色是 **24 位 RGB（COLORREF 口径，已证）**：`0x76` / `0x77` 写 `Font+0x550` / `+0x554` 时
 *   只取操作数低 24 位、高 8 位丢弃 ⇒ `0xFFFFFF` 是**不透明的白**，⛔ 不要按 ARGB 解成 alpha = 0。
 */
export const DEFAULT_PRIMARY_COLOR = 0xffffff;

/**
 * 副色默认值 = **0**（`Font::init`，`0x465605`）。
 * ★ 同样是 24 位 RGB ⇒ 0 = **不透明黑**（默认绘制模式 1 的 (1,1) 阴影就是这个颜色）。
 */
export const DEFAULT_SECONDARY_COLOR = 0x000000;

/** 绘制模式默认值 = **1**（`Font::init`，`0x465611`：先副色在 (+1,+1) 画一遍，再主色） */
export const DEFAULT_DRAW_MODE = 1;

/**
 * 游戏文字统一使用的字体族：**更纱黑体 / Amayui CN（cnjp 变体）**。
 * ★ 引擎的 face name（`0x1a5` → `Font+0x4EC`）仍记录在诊断里，但**不再参与选择**：
 *   所有 `0x204` 文字都映射到这一族；Regular / Bold 由 renderer 预先 `FontFace` 加载。
 * ★ 这是产品选择（中文汉化字体），不是 spec 的引擎语义。
 */
export const GAME_FONT_FAMILY = 'Amayui CN';

/** 统一字体栈；`sans-serif` 只是字体加载失败时的最后兜底（正常路径第一项就命中）。 */
export const FALLBACK_FONT_FAMILIES: readonly string[] = [GAME_FONT_FAMILY, 'sans-serif'];

/** diagnostics 里保留的日志条数 */
const MAX_NOTES = 200;

/**
 * **混合模式映射表**（`0x33f` 的模式格 → Pixi `BLEND_MODES`）。
 *
 * ★ spec 明说模式格"原样收下操作数、不 clamp、不查表"，"读取方怎样分支属外部模块，本版不规定"
 *   （`60-ops-render.md` 的 `CORE-OPS-RENDER-BLEND-33F` + `RES-RENDER-BLEND-PIXEL`）。
 * ⇒ 本表**不是规范结论**：除 `0 = normal` 是本实现的约定外，其余都是"常见枚举顺序"的猜测。
 * ⛔ 不要因为这张表把某个数值写进 spec / 台账 —— 要的是像素级观察。
 */
export const BLEND_MODE_MAP: readonly { mode: number; pixi: BLEND_MODES; evidence: string }[] = [
  { mode: 0, pixi: 'normal', evidence: '本实现约定（spec 只给"模式格原样收下"，初值 0）' },
  { mode: 1, pixi: 'add', evidence: '未取证（按常见混合枚举顺序猜）' },
  { mode: 2, pixi: 'multiply', evidence: '未取证（按常见混合枚举顺序猜）' },
  { mode: 3, pixi: 'screen', evidence: '未取证（按常见混合枚举顺序猜）' },
];

/** diagnostics 面板固定显示的"我们没做 / 没建模 / 没取证"清单（⛔ 不许悄悄省略） */
export const DISCLOSURES: readonly string[] = [
  '影片：pl_mpeg WASM 解码 MPEG-1/PS，仅视频（音频忽略）；不重编码 / 不做音频同步；帧尺寸来自码流，成品像素由 pl_mpeg 决定',
  '全屏语义未实现：screenMode=0 被平台降级为 set:WinX×set:WinY 的普通窗口（不做分辨率切换 / 缩放）；这是模拟器决策，不是 spec 结论',
  '网格渲染仅支持“4 顶点、四色一致”的全屏四边形（LOGO 版权页 / TITLE 背景）；逐顶点布局 / topology 未取证，其它网格跳过并诊断。绘制顺序是**引擎侧行为**：无命令记录时按 `CORE-SCENE-DRAW-ORDER` 的 key 升序 / 同键网格先于绘制项；负键口径按表分（网格 / 辅助表的负键不参与，**绘制项不做符号检查**）。**命令档（有命令记录时的第一档）只规划、不执行**：`renderer/scene-command-plan.ts` 是纯规划器（命令键升序 / 时间戳盖章 / 三表合并序 / 按种类的阶段 / 区间之下 / clearTable），诊断是 `scene-command-pass-planned` 与 `scene-command-pass-render-unmodeled` —— 阶段一个都没发，画面仍来自收尾扫描（⛔ 不假装执行了命令档）',
  '纹理加载：AGF 解码 + uploadTexture 已实现；BMP 分支未成篇，颜色键的 16 位通道装配未取证，空白纹理的初始像素未定义',
  '字体对象部分建模：size 24 / 主色 0xFFFFFF / 副色 0 / 绘制模式 1 来自 Font::init 的逐字证据（0x4654C3 / 0x4655FB / 0x465605 / 0x465611）；face name（0x1a5 → Font+0x4EC）只记录',
  '★ 产品选择：所有文字统一映射到 Amayui CN（更纱黑体 cnjp 变体，随仓字体资产 Amayui-CN_cnjp*.ttf）；引擎 face name 不参与字体选择',
  '字形像素未取证：字形栅格化 / 抗锯齿 / 阴影形状与引擎逐像素不一致（⛔ 不声称等价）',
  '文本编码 = Shift-JIS（已证：0x46F3D4 前导字节判定 / 0x46F4CD 双字节合成 / 0x8140 全角空格）；解码失败退回逐字节 latin1 是本实现的防御路径，引擎没有这条回退',
  '字体色 = 24 位 RGB（COLORREF 口径，已证：0x76 / 0x77 写 Font+0x550 / +0x554 只取低 24 位）⇒ 字永远不透明；绘制项 / 场景混合色才是 ARGB',
  '纵向对位：alignMode != 0 时引擎会做 y 修正，修正量未取证 ⇒ 本实现不改 y（未实现，不是"等价"）',
  '混合模式映射未取证：只有 0 = normal 是本实现约定，1/2/3 = add/multiply/screen 是猜测（RES-RENDER-BLEND-PIXEL）',
  '渲染到纹理目标时：空白目标用背景色 clear；已有文件像素 / 离屏文字的目标不清屏（呈现与目标的次序未取证）',
  '★ 绘制项的消费按 `CORE-OPS-TEXTURE-DRAW-CONSUMER` 走纯规划器（`scene-draw-plan.ts`）：空纹理槽 / 无渲染目标 / 绘制失败 ⇒ 上报（带编号）并**跳过**（⛔ 不建占位块）；源矩形夹空 ⇒ **静默**跳过；矩形**夹到纹理对象自己的逻辑矩形**（不是表面尺寸）；**自引用不跳过**（引擎不比较源与目标 —— 平台把同一张纹理当源采样属未定义行为）',
  '★ 位置档位的第 2 / 3 步（`[20,30)` 缩放 + 半上取整、场景偏移的两档）**已按 spec 写进规划器**，但本实现**没有场景相机模型** ⇒ 相机档按"无相机 / 无缩放"（缩放 1、偏移 0），只有取整那一档生效；诊断面板会把 `scene-view-unmodeled` 标出来（⛔ 不是 spec 结论）',
  '★ 附加组派生（首字第 2 位）：颜色三角波混合 / 变换乘入并强制有效 / 源矩形步进 的**语义**已在规划器里实现，但本实现缺三样取值 —— 附加组终色（`+0x240` 没有已建模的写入方）、步进量（残差 `RES-TEX-DRAW-CONSUMER-RECTSTEP`）、哪一张矩阵是变换通道（未取证） ⇒ 默认不混色、不步进、不乘矩阵；⛔ 不编常数',
  '★ 本步变换：规划器会给出 `base × 附加组 → 档位矩阵`，但 Pixi 路径**不消费矩阵**（Sprite 用位置实参定位）；变换的像素结果未实现，⛔ 不声称等价',
];

/* ─────────────────────────────── 纯函数（可单测） ─────────────────────────────── */

let cp932Decoder: TextDecoder | null = null;

/** `TextDecoder('shift_jis', { fatal: true })`：非法字节序列会抛，靠它判"解码失败" */
function cp932(): TextDecoder {
  cp932Decoder ??= new TextDecoder('shift_jis', { fatal: true });
  return cp932Decoder;
}

/** 核心的 latin1 字节串 → 码元数组（每个 charCode 0..255；越界值按低 8 位截） */
export function latin1Bytes(raw: string): Uint8Array {
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i += 1) out[i] = raw.charCodeAt(i) & 0xff;
  return out;
}

/**
 * `drawText.text` 的解码结果。
 * ★ `encoding` 是**本实现实际用了哪条路**（不是规范字段）：`shift_jis` = cp932 成功，
 *   `latin1` = cp932 解码失败后的回退。
 */
export interface DecodedText {
  unicode: string;
  encoding: 'shift_jis' | 'latin1';
  /** 回退原因（只有回退时非 null；要进 renderer 日志） */
  note: string | null;
}

/**
 * 把核心的 latin1 字节串按 **Shift-JIS** 解成 Unicode；失败退回**逐字节 latin1**。
 * ★ 编码**已证**（取证包：`0x46F3D4` / `0x46F4CD` / `0x8140`）；只有"失败回退"是本实现的防御选择。
 */
export function decodeTextBytes(raw: string): DecodedText {
  const bytes = latin1Bytes(raw);
  try {
    return { unicode: cp932().decode(bytes), encoding: 'shift_jis', note: null };
  } catch {
    let out = '';
    // ⛔ 不用 TextDecoder('latin1')：那是 windows-1252 的别名，会改 0x80–0x9F（见 README §3）
    for (let i = 0; i < bytes.length; i += 1) out += String.fromCharCode(bytes[i] ?? 0);
    return {
      unicode: out,
      encoding: 'latin1',
      note: `Shift-JIS 解码失败 ⇒ 退回逐字节 latin1（${bytes.length} 字节；引擎没有这条回退）`,
    };
  }
}

/**
 * ARGB（高 8 位 = 透明度）→ `{ rgb, alpha }`；`alpha` 归一化到 0..1。
 *
 * ★ **只用于绘制项颜色 / 场景混合色**（`0x202` / `0x203` / `0x33f` 的合成结果：高 8 位才是透明度）。
 * ⛔ **不要用它解 `drawText` 的字体色**：那是 24 位 COLORREF（无 alpha，已证，见 `drawText`）。
 */
export function argbOf(color: number): { rgb: number; alpha: number } {
  const value = color >>> 0;
  return { rgb: value & 0xffffff, alpha: ((value >>> 24) & 0xff) / 255 };
}

/**
 * `drawText` 请求的参数类型（直接从线协议里取，⛔ 不复制一份 shape —— 协议改了这里自动跟上）。
 * ★ `size` / `weight` / `shadowColor` / `drawMode` / `alignMode` 是协议的**可选**字段；
 *   缺省走 `Font::init` 的默认值（24 / 无 / 0 / 1 / 0）。
 */
export type DrawTextParams = Extract<RenderCall, { method: 'drawText' }>['params'];

/** Pixi 接受的字体粗细（`TextStyleFontWeight` 的子集；⛔ 不复述 pixi 的完整类型） */
const FONT_WEIGHTS = [
  'normal',
  'bold',
  'bolder',
  'lighter',
  '100',
  '200',
  '300',
  '400',
  '500',
  '600',
  '700',
  '800',
  '900',
] as const;
type FontWeight = (typeof FONT_WEIGHTS)[number];

/**
 * `weight`（协议：`0x2BD`；**700 = bold，0 = normal**）→ Pixi 的 `fontWeight`。
 * ★ 0 特别处理成 normal（不是夹到 100）；其它数值按百位取整、夹到 100..900（近似；
 *   Win32 `lfWeight` 的完整值域映射没有取证）。CSS 关键字原样接受。
 */
export function normalizeFontWeight(weight: number | string): FontWeight {
  if (typeof weight === 'number') {
    if (!Number.isFinite(weight) || weight <= 0) return 'normal';
    const clamped = Math.min(900, Math.max(100, Math.round(weight / 100) * 100));
    return String(clamped) as FontWeight;
  }
  const text = weight.trim().toLowerCase();
  return (FONT_WEIGHTS as readonly string[]).includes(text) ? (text as FontWeight) : 'normal';
}

/** 一次绘制 pass 的计划（`drawPassesOf` 的输出） */
export interface DrawPassPlan {
  passes: DrawPassState[];
  /** 近似 / 未知模式时的说明（要进日志；`null` = 有据的路径） */
  note: string | null;
}

/**
 * **绘制模式的 pass 序列**（`0x465611` 起的分支，取证包的取值 0/1/2/3）。
 *
 * - `0`：只画主色；
 * - `1`（`Font::init` 默认）：先副色在 **(+1,+1)** 一遍，再主色（= 右下投影）；
 * - `2`：主色两遍（不透明时与一遍看不出差别 —— 不声称逐像素）；
 * - `3`：副色多 pass 径向 ⇒ 本实现**只做 8 向半径 1 的近似**，并给出 `note`；
 * - 未知值：只画主色 + `note`（⛔ 不猜）。
 *
 * ★ 这些 pass 是**近似**：字形栅格化 / 抗锯齿 / 真实的多 pass 半径都没有取证。
 */
export function drawPassesOf(
  mode: number,
  primary: { rgb: number; alpha: number },
  secondary: { rgb: number; alpha: number },
): DrawPassPlan {
  switch (mode) {
    case 0:
      return { passes: [{ role: 'primary', dx: 0, dy: 0, ...primary }], note: null };
    case 1:
      return {
        passes: [
          { role: 'secondary', dx: 1, dy: 1, ...secondary },
          { role: 'primary', dx: 0, dy: 0, ...primary },
        ],
        note: null,
      };
    case 2:
      return {
        passes: [
          { role: 'primary', dx: 0, dy: 0, ...primary },
          { role: 'primary', dx: 0, dy: 0, ...primary },
        ],
        note: '绘制模式 2（主色两遍）：不透明时与一遍看不出差别（近似，不声称逐像素）',
      };
    case 3: {
      const ring: DrawPassState[] = [];
      for (const [dx, dy] of [
        [-1, 0],
        [1, 0],
        [0, -1],
        [0, 1],
        [-1, -1],
        [1, -1],
        [-1, 1],
        [1, 1],
      ]) {
        ring.push({ role: 'secondary', dx, dy, ...secondary });
      }
      return {
        passes: [...ring, { role: 'primary', dx: 0, dy: 0, ...primary }],
        note: '绘制模式 3（副色多 pass 径向）只做了 8 向半径 1 的近似 —— 未实现逐像素',
      };
    }
    default:
      return {
        passes: [{ role: 'primary', dx: 0, dy: 0, ...primary }],
        note: `未知绘制模式 ${mode} ⇒ 只画主色（⛔ 不猜别的分支）`,
      };
  }
}

/** 这张纹理槽当前有没有**可画的像素**（上传 / 写字 / 被当目标写过；`0x1f8` 重置为 false） */
export interface TextureSlot {
  snapshot: TextureSnapshot;
  rt: RenderTexture | null;
  /** 已分配的表面句柄；`null` = 没分配或被 `releaseSurface` 摘掉了（RT 本身仍归场景） */
  surfaceId: number | null;
  /** `drawText` 成功写入的次数 */
  drawCount: number;
  /** 被 `present` 当纹理目标写过的次数 */
  renderTargetWrites: number;
  writeCount: number;
  /**
   * 有没有真实内容（文件上传 / 离屏文字 / 目标渲染）。
   * ★ 只影响**呈现**（`present` 的 clear）与影片覆盖顺序 —— ⛔ 它**不是**绘制项的"可画"判据：
   *   槽里有对象就照画（像素未定义属平台），见 `CORE-OPS-TEXTURE-DRAW-CONSUMER` 第 3 条。
   */
  hasContent: boolean;
  lastDraw: LastDrawState | null;
}

/** 一次绘制 pass（`0x204` 的绘制模式决定 pass 序列；诊断用） */
export interface DrawPassState {
  role: 'primary' | 'secondary';
  dx: number;
  dy: number;
  rgb: number;
  alpha: number;
}

/** 最近一次 `drawText` 的解码 / 字体 / 颜色 / 绘制模式（诊断用；不是引擎状态） */
export interface LastDrawState {
  textureId: number;
  x: number;
  y: number;
  /** 原始字节数（latin1 口径） */
  byteLength: number;
  encoding: 'shift_jis' | 'latin1';
  unicode: string;
  /** 实际用的字体栈（首项是核心给的 font；null 时首项就是回退栈） */
  fontFamilies: string[];
  fontFromEngine: string | null;
  /** 实际字号（参数给了就用参数，否则 `TEXT_FONT_SIZE` = 24） */
  size: number;
  /** 绘制模式：0/1/2/3，或协议没给时的默认 1 */
  drawMode: number;
  /** 纵向对位模式（`Font+0x313D0`；本实现不改 y —— 修正量未取证） */
  alignMode: number;
  /**
   * 主色（`null` = 核心没建立 ⇒ 默认白）。
   * ★ **24 位 RGB（COLORREF 口径，已证）**：没有 alpha 字节，字永远不透明。
   */
  color: number | null;
  /** 实际使用的主色 RGB（已按 24 位截） */
  rgb: number;
  /**
   * 副色（`undefined`/`null` = 协议没给 ⇒ 默认 0 = 不透明黑）。
   * ★ 同样是 24 位 RGB（`Font+0x554`）。
   */
  shadowColor: number | null;
  /** 实际执行的 pass 序列（含偏移） */
  passes: DrawPassState[];
  ok: boolean;
  note: string | null;
}

/** 一次 `present` 的去向（诊断用） */
export interface LastPresentState {
  kind: 'backbuffer' | 'texture';
  textureId: number | null;
  /** 目标纹理槽不在场景里时的降级标记 */
  fellBackToBackBuffer: boolean;
}

/** 场景构建的计数（诊断用：数字对不上时能一眼看出"有东西没画"） */
export interface SceneBuildStats {
  width: number;
  height: number;
  target: number;
  textures: number;
  drawItems: number;
  activeItems: number;
  sprites: number;
  /**
   * 空纹理槽（槽里没有对象）⇒ **上报 + 跳过**（⛔ 不建占位块）。
   * ★ 与"条目无效"是两条不同的跳过：后者静默（见 `activeItems` 的差）。
   */
  missingTextures: number;
  /** 当前没有渲染目标 ⇒ 上报 + 跳过 */
  noTarget: number;
  /** 首字第 16 位（本轮已消费）⇒ 静默跳过 */
  consumedSkips: number;
  /** 源矩形夹到纹理逻辑矩形后为空 ⇒ **静默**跳过（不上报、不建占位物） */
  clippedEmpty: number;
  /** 绘制入口失败（纹理不可用 / 建 frame 抛异常）⇒ 上报 + 跳过 */
  drawFailed: number;
  /** 附加组「跨步进 ⇒ 场景标脏」的次数（本层没有回写路径，只记数与诊断） */
  dirtyRequests: number;
  /** 源矩形窗口「到期 ⇒ 写回四条边」的次数（4.1；本层没有回写路径，只记数与诊断） */
  rectWriteBacks: number;
  /** 快照里的网格数 / 画出的 4 顶点四边形数 / 因布局未知跳过的网格数 */
  meshes: number;
  meshQuads: number;
  meshSkipped: number;
  /** 实际绘制顺序（`item#10 → mesh#200000 → …`，最多列 16 项；诊断用） */
  order: string;
  /**
   * 命令档**规划**统计（本实现只规划、不执行：阶段一个都没发，画面仍由收尾扫描产生）。
   * ★ 命令数 / 阶段数 / 本帧被记成「本轮已消费」的键数 —— 与诊断 `scene-command-pass-planned` 同源。
   */
  commandPassCommands: number;
  commandPassPhases: number;
  commandPassConsumedKeys: number;
}

/** diagnostics 面板要的全部数字（**纯数据**；DOM 由 `renderer.ts` 画） */
export interface HostDiagnostics {
  backend: string;
  /** 当前帧（名称 / id / slot / ip）；场景还没来时为 null */
  script: SceneSnapshot['script'];
  resolution: number;
  canvas: { width: number; height: number };
  init: RendererInit | null;
  /** 形态字 S：0 = 占满屏态，非 0 = 窗口态（`06-window.md` 的口径） */
  modeLabel: string;
  /**
   * `setRenderTarget` 记下的当前目标。
   * ★ `textureId` 是**纹理槽号**（-1 = 后缓冲）：`0x20d` 的成功路径是"取表面 → 设目标 → 归还表面"，
   *   所以在设目标那一刻就把槽号记下来 —— 归还之后表面表里已经没有这个 id 了。
   */
  currentSurface: {
    surfaceId: number | null;
    surfaceReleased: boolean;
    kind: 'backbuffer' | 'texture';
    textureId: number | null;
  };
  surfaceCount: number;
  nextSurfaceId: number;
  scene: SceneBuildStats | null;
  blend: { mode: number; mapped: string; unknown: boolean; color: number } | null;
  textures: TextureSlotView[];
  lastDraw: LastDrawState | null;
  lastPresent: LastPresentState | null;
  presentCount: number;
  notes: string[];
}

/** 纹理槽的只读视图（diagnostics 用） */
export interface TextureSlotView {
  id: number;
  kind: TextureSnapshot['kind'];
  width: number;
  height: number;
  file: number;
  blank: number;
  hasSurface: boolean;
  writeCount: number;
  drawCount: number;
  renderTargetWrites: number;
  /** 有没有可画内容：真实上传 / `drawText` / `present` 目标写过（`0x1f8` 的空白创建 ⇒ false，但它**仍会照画**） */
  hasPixels: boolean;
}

/** 初始化 / 日志的注入点（renderer.ts 把 `window.amayui.log` 接进来） */
export interface PixiRenderHostOptions {
  /** 写日志（`session.ts` 把它转成 `bridge.report`）；⛔ 业务日志不走 `console` */
  log?: (text: string) => void;
  /**
   * **错误通道**（spec 的「错误通道」：空纹理槽 / 无渲染目标 / 绘制失败）。
   * ★ 与 `log` 分开：它要能被当成错误（`level: 'error'`）筛出来，否则"上报了"在日志里看不见。
   * ★ 缺省 ⇒ 退回 `log`（不丢上报）。
   */
  error?: (text: string) => void;
  /** 初始画布尺寸（`ready` 回报用；随后 `init` 会覆盖） */
  initialWidth?: number;
  initialHeight?: number;
}

/** 场景根**不挂到 `app.stage`**：避免它被 Pixi 自己的 ticker 再画一遍（重影） */
const EMPTY_CONTAINER = new Container();

/* ─────────────────────────────── 宿主 ─────────────────────────────── */

export class PixiRenderHost {
  private readonly app: Application;
  private readonly logSink: (text: string) => void;
  /** 错误通道（空槽 / 无目标 / 绘制失败）；缺省时就是 `logSink` */
  private readonly errorSink: (text: string) => void;

  /** 当前场景的根节点；⛔ 不在 `app.stage` 里（`present` 自己决定画到哪） */
  private readonly sceneRoot = new Container();
  /** 场景里由本类创建的节点（销毁时按 Text / 其它分别处理，⛔ 不要一把 `destroy({texture:true})`） */
  private built: Container[] = [];
  /** 子矩形 Sprite 用的临时 Texture（只包 frame，不拥有 source）；重建场景时逐个销毁 */
  private frameTextures: Texture[] = [];

  private readonly slots = new Map<number, TextureSlot>();
  /** 影片帧槽：场景重建时把这些槽的像素放在网格之后（否则 LOGO 收尾的黑网格会盖住影片） */
  private readonly movieSlots = new Set<number>();
  /** 表面表：`surfaceId → textureId`（`0` 是后缓冲，不在这张表里） */
  private readonly surfaces = new Map<number, number>();
  private nextSurfaceId = 1;
  /**
   * `setRenderTarget` 记下的**当前目标 = 纹理槽号**（`-1` = 后缓冲）。
   * ★ 记槽号而不是 surfaceId：`0x20d` 的成功路径是"取表面 → 设目标 → **归还表面**"，
   *   归还之后表面表里就没有那个 id 了 —— 用 surfaceId 存目标会在归还后丢掉目标。
   * ★ 这是**平台侧**记录；场景快照另有自己的 `target`（两者不一致时 `present` 会记日志）。
   */
  private currentTarget = -1;
  /** 最近一次 `setRenderTarget` 用的 `surfaceId`（只为 diagnostics 显示；归还后仍保留数值） */
  private currentSurfaceId: number | null = null;

  private initInfo: RendererInit | null = null;
  private scene: SceneSnapshot | null = null;
  private sceneStats: SceneBuildStats | null = null;
  private blendState: HostDiagnostics['blend'] = null;
  private lastDraw: LastDrawState | null = null;
  private lastPresent: LastPresentState | null = null;
  private presentCount = 0;
  /** 各类场景警告上次发出的文本（同一条不每帧重复写） */
  private readonly lastSceneNotes = new Map<string, string>();
  /**
   * 已发过的错误通道上报（`类别:纹理编号`）。
   * ★ 场景每帧重建 ⇒ 同一个空槽会每帧都"上报"；引擎是写错误缓冲，本实现只有日志/诊断两条出口
   *   ⇒ 同类同编号**只发一次**（不改变"上过报"这个事实，只是不让日志被刷满）。
   */
  private readonly reportedDrawErrors = new Set<string>();
  private readonly notes: string[] = [];

  private constructor(app: Application, options: PixiRenderHostOptions) {
    this.app = app;
    this.logSink = options.log ?? (() => {});
    this.errorSink = options.error ?? this.logSink;
    this.sceneRoot.label = 'amayui-scene-root';
  }

  /**
   * 建 Pixi `Application` 并挂上 canvas。
   * ★ 失败（WebGL 不可用）⇒ 抛异常：调用方**不要** `ready` —— 协议明写"失败时 renderer 不 ready"。
   * ★ `preference: ['webgl']`：只试 WebGL（数组形式会**排除**未列出的后端）。
   *   选 WebGL 而不是 WebGPU 是本实现的决定（Electron / 软件 GL 的兼容面更大），不是 spec 结论。
   */
  static async create(hostElement: HTMLElement, options: PixiRenderHostOptions = {}): Promise<PixiRenderHost> {
    const app = new Application();
    await app.init({
      width: Math.max(1, Math.round(options.initialWidth ?? hostElement.clientWidth ?? 1)),
      height: Math.max(1, Math.round(options.initialHeight ?? hostElement.clientHeight ?? 1)),
      background: 0x000000,
      backgroundAlpha: 1,
      antialias: false,
      // Retina 由 Pixi 处理；画布的 CSS 尺寸 = init 的客户区像素（autoDensity）
      resolution: Math.max(1, globalThis.devicePixelRatio || 1),
      autoDensity: true,
      autoStart: false,
      preference: ['webgl'],
    });
    const host = new PixiRenderHost(app, options);
    hostElement.appendChild(app.canvas);
    return host;
  }

  /** 当前画布像素尺寸（core 场景快照的客户区尺寸；init 之后 = 客户区） */
  canvasSize(): { width: number; height: number } {
    return { width: Math.round(this.app.renderer.width), height: Math.round(this.app.renderer.height) };
  }

  /** 画布元素（输入宿主把它当客户区坐标的参照） */
  canvasElement(): HTMLCanvasElement {
    return this.app.canvas;
  }

  /** `ready` 的回报体（Pixi / WebGL 已初始化） */
  readyInfo(): RendererReady {
    return {
      renderer: 'pixi',
      backend: this.app.renderer.name,
      width: Math.round(this.app.renderer.width),
      height: Math.round(this.app.renderer.height),
    };
  }

  /* ─────────────────────────── 协议入口 ─────────────────────────── */

  /**
   * 执行一次 `RenderCall`。
   * @returns 调用的结果（`undefined` = 只表示成功）
   * @throws Error ⇒ 调用方回 `{ ok: false, error }`（平台失败，不是引擎错误）
   */
  async execute(call: RenderCall): Promise<unknown> {
    switch (call.method) {
      case 'init':
        this.init(call.params);
        return undefined;
      case 'getRenderSurface':
        return this.getRenderSurface(call.params.textureId);
      case 'getBackBuffer':
        return { surfaceId: BACK_BUFFER_SURFACE_ID, kind: 'backbuffer' } satisfies SurfaceRef;
      case 'setRenderTarget':
        this.setRenderTarget(call.params.surfaceId);
        return undefined;
      case 'releaseSurface':
        this.releaseSurface(call.params.surfaceId);
        return undefined;
      case 'drawText':
        this.drawText(call.params);
        return undefined;
      case 'present':
        this.present();
        return undefined;
      default:
        return assertNever(call);
    }
  }

  /** `scene` / `status` / `boot` 之外的东西由 renderer.ts 处理；这里只收场景快照 */
  applyScene(scene: SceneSnapshot): void {
    this.scene = scene;
    this.clearBuilt();
    this.reconcileTextures(scene.textures);
    this.sceneStats = this.buildScene(scene);
  }

  /**
   * `0x1f8`：创建 / 重置一张空白纹理。
   * ★ 不写像素；`hasContent` 清 false（即使同 id 同尺寸也要重置，否则旧图会留下来）。
   */
  createTexture(params: { textureId: number; width: number; height: number; kind: 'normal' | 'divided' }): boolean {
    const { textureId, width, height, kind } = params;
    if (width <= 0 || height <= 0) {
      this.note(`createTexture(${textureId})：尺寸 ${width}×${height} 无效 ⇒ 创建失败`);
      return false;
    }
    let slot = this.slots.get(textureId);
    if (slot) {
      this.dropSlot(textureId);
      slot = undefined;
    }
    slot = {
      snapshot: { id: textureId, kind, width, height, file: -1, blank: 1 },
      rt: null,
      surfaceId: null,
      drawCount: 0,
      renderTargetWrites: 0,
      writeCount: 0,
      hasContent: false,
      lastDraw: null,
    };
    this.slots.set(textureId, slot);
    try {
      this.ensureTexture(slot);
      this.note(`createTexture(${textureId})：${kind} ${width}×${height}（空白；像素未定义）`);
      return true;
    } catch (error) {
      this.note(`createTexture(${textureId}) 失败：${messageOf(error)}`);
      return false;
    }
  }

  /**
   * `0x1f9`：把 RGBA 上传到该槽（top-down，长度 = w×h×4）。
   * ★ 颜色键命中像素整条清 0；临时 `Texture` / `Sprite` 只用于 blit，随后销毁。
   */
  uploadTexture(params: {
    textureId: number;
    width: number;
    height: number;
    kind: 'normal' | 'divided';
    rgba: Uint8Array;
    color: number;
    /** 影片帧：不写诊断日志（每帧一条会刷屏） */
    quiet?: boolean;
    /** 影片帧：保留 movieSlots 标记，让本次重建把影片放在网格之后 */
    movie?: boolean;
  }): boolean {
    const { textureId, width, height, kind, rgba, color } = params;
    if (!params.movie) this.movieSlots.delete(textureId);
    if (width <= 0 || height <= 0) {
      this.note(`uploadTexture(${textureId})：尺寸 ${width}×${height} 无效 ⇒ 上传失败`);
      return false;
    }
    if (rgba.length !== width * height * 4) {
      this.note(`uploadTexture(${textureId})：RGBA 长度 ${rgba.length} ≠ ${width}×${height}×4 ⇒ 上传失败`);
      return false;
    }
    let slot = this.slots.get(textureId);
    if (!slot) {
      slot = {
        snapshot: { id: textureId, kind, width, height, file: -1, blank: 0 },
        rt: null,
        surfaceId: null,
        drawCount: 0,
        renderTargetWrites: 0,
        writeCount: 0,
        hasContent: false,
        lastDraw: null,
      };
      this.slots.set(textureId, slot);
    } else {
      slot.snapshot = { ...slot.snapshot, kind, width, height, blank: 0 };
      slot.drawCount = 0;
      slot.renderTargetWrites = 0;
      slot.writeCount = 0;
      slot.lastDraw = null;
    }
    try {
      const rt = this.ensureTexture(slot);
      const pixels = new Uint8Array(rgba); // 不把核心里可复用的缓冲交给平台
      // ★ `0` 是"无颜色键"哨兵（`0x1f9` 的负数操作数归一化成 0），不是黑键：
      //   引擎的上传比较含 A 字节（见 `applyColorKey`），键 0 不会命中不透明像素；
      //   这里显式跳过而不是靠比较兜底，否则日志会把它印成"颜色键 #000000"误导排查。
      if (color !== 0) applyColorKey(pixels, color);
      const canvas = document.createElement('canvas');
      canvas.width = width;
      canvas.height = height;
      const ctx = canvas.getContext('2d');
      if (ctx === null) throw new Error('2d context unavailable');
      const image = ctx.createImageData(width, height);
      image.data.set(pixels);
      ctx.putImageData(image, 0, 0);
      const texture = Texture.from(canvas);
      const sprite = new Sprite(texture);
      try {
        this.app.renderer.render({ container: sprite, target: rt, clear: true });
      } finally {
        sprite.destroy();
        texture.destroy(true);
      }
      slot.hasContent = true;
      slot.writeCount += 1;
      if (!params.quiet) {
        this.note(color === 0
          ? `uploadTexture(${textureId})：${width}×${height}，无颜色键`
          : `uploadTexture(${textureId})：${width}×${height}，颜色键 #${(color & 0xffffff).toString(16).padStart(6, '0')}（#0 = 无键）`);
      }
      this.rebuildSceneFromLastSnapshot();
      return true;
    } catch (error) {
      this.note(`uploadTexture(${textureId}) 失败：${messageOf(error)}`);
      return false;
    }
  }

  /** 影片帧上传：不应用颜色键、不每帧写诊断；随后由影片 host 调 `presentNow()` */
  uploadMovieFrame(params: { textureId: number; width: number; height: number; rgba: Uint8Array }): boolean {
    // ★ 必须先标记再上传：`uploadTexture` 内部会重建场景；若等它返回再标记，重建出的场景
    //   仍把影片画在黑网格下面 ⇒ 每次上传后的第一次 present 都会黑一帧（正是闪烁的来源）。
    this.movieSlots.add(params.textureId);
    const ok = this.uploadTexture({ ...params, kind: 'normal', color: -1, quiet: true, movie: true });
    if (!ok) this.movieSlots.delete(params.textureId);
    return ok;
  }

  /** 影片 host 的同步呈现入口（`present` 是 private；视频帧不能每帧等 async 协议） */
  movieNote(text: string): void {
    this.note(text);
  }

  presentNow(): void {
    this.present();
  }

  /** `0x1fa`：释放平台纹理（幂等） */
  releaseTexture(textureId: number): void {
    if (!this.slots.has(textureId)) {
      this.note(`releaseTexture(${textureId})：槽不存在 ⇒ 幂等忽略`);
      return;
    }
    this.dropSlot(textureId);
    // ★ 不能用**旧快照**重建：核心的 0x1f7 已经删掉绘制项，但 renderer 的上一份快照可能
    //   还包含它；释放 43 时会把“引用空槽的白色绘制项”画成整屏白色 placeholder。
    //   先把这份快照里对该纹理的引用一并剥掉，再做重建 / 呈现。
    this.scrubTextureFromSnapshot(textureId);
    this.note(`releaseTexture(${textureId})：已销毁平台纹理`);
    this.rebuildSceneFromLastSnapshot();
    // ★ 释放纹理后**立刻重画一次**：旧 RT / 子节点已销毁，若等到下一次迭代才 present，
    //   合成器可能先拿一帧“无内容”的白色后缓冲（版权页结束释放全屏纹理时最容易看到）。
    //   重画的是**当前场景**（已不含被释放的槽），清屏色仍是 renderer 背景。
    this.present();
  }

  /** diagnostics 面板的数据源（每次调用后重新取；纯数据） */
  diagnostics(): HostDiagnostics {
    const textures: TextureSlotView[] = [...this.slots.values()]
      .sort((a, b) => a.snapshot.id - b.snapshot.id)
      .map((slot) => ({
        id: slot.snapshot.id,
        kind: slot.snapshot.kind,
        width: slot.snapshot.width,
        height: slot.snapshot.height,
        file: slot.snapshot.file,
        blank: slot.snapshot.blank,
        hasSurface: slot.surfaceId !== null,
        writeCount: slot.writeCount,
        drawCount: slot.drawCount,
        renderTargetWrites: slot.renderTargetWrites,
        hasPixels: slot.hasContent,
      }));

    const currentKind: 'backbuffer' | 'texture' = this.currentTarget < 0 ? 'backbuffer' : 'texture';
    const surfaceReleased =
      this.currentSurfaceId !== null &&
      this.currentSurfaceId !== BACK_BUFFER_SURFACE_ID &&
      !this.surfaces.has(this.currentSurfaceId);

    return {
      backend: this.app.renderer.name,
      script: this.scene?.script ?? null,
      resolution: this.app.renderer.resolution,
      canvas: { width: Math.round(this.app.renderer.width), height: Math.round(this.app.renderer.height) },
      init: this.initInfo,
      modeLabel: modeLabelOf(this.initInfo),
      currentSurface: {
        surfaceId: this.currentSurfaceId,
        surfaceReleased,
        kind: currentKind,
        textureId: currentKind === 'texture' ? this.currentTarget : null,
      },
      surfaceCount: this.surfaces.size,
      nextSurfaceId: this.nextSurfaceId,
      scene: this.sceneStats,
      blend: this.blendState,
      textures,
      lastDraw: this.lastDraw,
      lastPresent: this.lastPresent,
      presentCount: this.presentCount,
      notes: [...this.notes],
    };
  }

  /** 一条诊断（同时写一份进主进程日志；`notes` 有上限） */
  note(text: string): void {
    if (this.notes.length >= MAX_NOTES) this.notes.shift();
    this.notes.push(text);
    this.logSink(text);
  }

  /**
   * **错误通道**上报（`CORE-OPS-TEXTURE-DRAW-CONSUMER` 第 3 / 4 / 7 条：空纹理槽 / 无渲染目标 / 绘制失败）。
   * ★ 文本里**必须带纹理编号**（否则日志定位不到坏条目）；内容本版不规定。
   * ★ 同类同编号只发一次：场景每帧重建，否则日志会被同一条刷满（"上报过"这件事不变）。
   */
  private errorNote(category: string, textureId: number, text: string): void {
    if (this.notes.length >= MAX_NOTES) this.notes.shift();
    this.notes.push(text);
    const key = `${category}:${textureId}`;
    if (this.reportedDrawErrors.has(key)) return;
    this.reportedDrawErrors.add(key);
    this.errorSink(text);
  }

  /* ─────────────────────────── init / 表面 ─────────────────────────── */

  private init(params: RendererInit): void {
    const width = Math.max(1, Math.round(params.width));
    const height = Math.max(1, Math.round(params.height));
    if (this.app.renderer.width !== width || this.app.renderer.height !== height) {
      this.app.renderer.resize(width, height);
    }
    // `background` 在协议里写作"ARGB 的 0xRRGGBB" ⇒ 只取低 24 位当颜色（高位若被置位不参与）
    const background = params.background & 0xffffff;
    this.app.renderer.background.color = background;
    this.initInfo = params;
    document.body.dataset.screenMode = params.screenMode === 0 ? 'fullscreen' : 'window';
    this.note(
      `init：${width}×${height}，${modeLabelOf(params)}，背景 #${background.toString(16).padStart(6, '0')}` +
        `，fullscreenPresent = ${params.fullscreenPresent}`,
    );
    // 场景还没来：先把画布清成背景色（否则看到的是上一帧 / 未初始化内容）
    this.app.renderer.render({ container: EMPTY_CONTAINER, clear: true });
  }

  /** `0x20d` / `0x204` 共用的"取渲染表面"（`RES-RENDER-TEXT-SURFACE`：平台表面） */
  private getRenderSurface(textureId: number): SurfaceRef {
    const slot = this.slots.get(textureId);
    if (!slot) {
      throw new Error(`纹理槽 ${textureId} 不在最近一次场景快照里（renderer 只认识快照里出现过的槽）`);
    }
    const { width, height } = slot.snapshot;
    if (width <= 0 || height <= 0) {
      throw new Error(`纹理槽 ${textureId} 的尺寸无效：${width}×${height}`);
    }
    const surfaceId = this.ensureSurface(slot);
    return { surfaceId, kind: 'texture' } satisfies SurfaceRef;
  }

  /** 保证该槽有 `RenderTexture`，返回它 */
  private ensureTexture(slot: TextureSlot): RenderTexture {
    const width = Math.max(1, Math.round(slot.snapshot.width));
    const height = Math.max(1, Math.round(slot.snapshot.height));
    const resolution = this.app.renderer.resolution;
    if (slot.rt) {
      // `rt.width` 是**逻辑**宽度（source.width / resolution），与快照的客户区像素可直接比
      if (slot.rt.width === width && slot.rt.height === height && slot.rt.source.resolution === resolution) {
        return slot.rt;
      }
      slot.rt.destroy(true);
      slot.rt = null;
    }
    const rt = RenderTexture.create({ width, height, resolution, antialias: false });
    slot.rt = rt;
    return rt;
  }

  /** 保证该槽有一个**表面句柄**（同一个槽复用同一个 `surfaceId`，直到被 `releaseSurface` 摘掉） */
  private ensureSurface(slot: TextureSlot): number {
    this.ensureTexture(slot);
    if (slot.surfaceId === null) {
      slot.surfaceId = this.nextSurfaceId;
      this.nextSurfaceId += 1;
      this.surfaces.set(slot.surfaceId, slot.snapshot.id);
    }
    return slot.surfaceId;
  }

  private setRenderTarget(surfaceId: number): void {
    if (surfaceId === BACK_BUFFER_SURFACE_ID) {
      this.currentSurfaceId = BACK_BUFFER_SURFACE_ID;
      this.currentTarget = -1;
      return;
    }
    const textureId = this.surfaces.get(surfaceId);
    if (textureId === undefined) {
      throw new Error(`未知表面 ${surfaceId}：不在表面表里（从未分配，或已被 releaseSurface 摘掉）`);
    }
    this.currentSurfaceId = surfaceId;
    // ★ 记**槽号**，不是 surfaceId：成功路径紧接着就会 releaseSurface
    this.currentTarget = textureId;
  }

  /**
   * 归还一份表面引用。
   * ★ 只从**表面表**移除句柄：`RenderTexture` 本身归场景所有（快照还要显示它），
   *   销毁它会让"设过目标之后归还"变成"纹理内容消失"（`0x20d` 的成功路径正是取表面 → 设目标 → 归还）。
   * ★ 后缓冲（`0`）**忽略**（协议明写）；未知 id 按幂等处理（不失败）。
   */
  private releaseSurface(surfaceId: number): void {
    if (surfaceId === BACK_BUFFER_SURFACE_ID) {
      this.note('releaseSurface(0)：后缓冲不是可归还的表面 ⇒ 忽略');
      return;
    }
    const textureId = this.surfaces.get(surfaceId);
    if (textureId === undefined) {
      this.note(`releaseSurface(${surfaceId})：不在表面表里 ⇒ 幂等忽略`);
      return;
    }
    this.surfaces.delete(surfaceId);
    const slot = this.slots.get(textureId);
    if (slot && slot.surfaceId === surfaceId) slot.surfaceId = null;
    // 当前目标**不复位**：`0x20d` 的次序是"取表面 → 设目标 → 归还"，归还在写目标之前，
    // 平台侧必须保持已设的目标（复位会破坏"归还发生在写当前目标之前"这条合同）。
    if (this.currentSurfaceId === surfaceId) {
      this.note(`releaseSurface(${surfaceId})：仍是当前渲染目标 ⇒ 保留（归还发生在写目标之前）`);
    }
  }

  /* ─────────────────────────── 文字 ─────────────────────────── */

  /**
   * `0x204` 的一次同步绘制：解码文本 → 按**绘制模式**决定 pass 序列 → 逐 pass 画进该槽的 RT。
   *
   * ★ 绘制模式来自 `Font::init` 的默认值 1（`0x465611`）与 `0x465611` 起的分支（取证包）：
   *   0 = 只画主色；1 = 先副色在 (+1,+1) 一遍再主色；2 = 主色两遍；3 = 副色多 pass 径向。
   * ★ `(x, y)` 是**行顶**（已证）⇒ Text 用 top-left 定位，⛔ 不加 ascent。
   * ★ **字体色是 24 位 RGB（COLORREF 口径，已证）**：`0x76` / `0x77` 写 `Font+0x550` / `Font+0x554`
   *   时只取操作数低 24 位、高 8 位**丢弃**。⇒ 字体色**永远不透明**，⛔ 不按 ARGB 解、⛔ 不因为
   *   "高 8 位 = 0"把文字画成透明（默认主色 0xFFFFFF 是白，不是透明黑）。
   *   ★ `argbOf` 只用于**绘制项颜色 / 场景混合色**（那些的高 8 位才是透明度），别把两套口径搞混。
   * ★ `size` / `weight` / `shadowColor` / `drawMode` / `alignMode` 是协议的可选字段（`protocol.ts`）：
   *   缺省走 `Font::init` 的默认值（24 / 无 / 0 / 1 / 0）。
   */
  private drawText(params: DrawTextParams): void {
    const slot = this.slots.get(params.textureId);
    if (!slot) {
      throw new Error(`drawText 的目标纹理槽 ${params.textureId} 不在最近一次场景快照里`);
    }
    const rt = this.ensureTexture(slot);
    const decoded = decodeTextBytes(params.text);

    // 核心的入口条件 ③ 已判"文本非空"，这里只是不再画空串（保持 RT 原样）
    if (decoded.unicode.length === 0) {
      this.lastDraw = null;
      this.note(`drawText：槽 ${params.textureId} 的文本解码后为空（${params.text.length} 字节）⇒ 不画`);
      return;
    }
    if (decoded.note) this.note(`drawText：槽 ${params.textureId} ${decoded.note}`);

    const fontFromEngine = params.font !== null && params.font.length > 0 ? params.font : null;
    // ★ 所有字体统一映射到 Amayui CN：引擎 face name 只记录、不参与选择。
    const fontFamilies = [GAME_FONT_FAMILY];
    const size = params.size !== undefined && params.size > 0 ? params.size : TEXT_FONT_SIZE;

    // 字体色 = 24 位 RGB（COLORREF）：主色 / 副色都不带 alpha，也不做透明度分支
    const primary = { rgb: params.color === null ? DEFAULT_PRIMARY_COLOR : params.color & 0xffffff, alpha: 1 };
    const secondary = {
      rgb: params.shadowColor === undefined ? DEFAULT_SECONDARY_COLOR : params.shadowColor & 0xffffff,
      alpha: 1,
    };
    const drawMode = params.drawMode ?? DEFAULT_DRAW_MODE;
    const alignMode = params.alignMode ?? 0;
    const plan = drawPassesOf(drawMode, primary, secondary);
    if (plan.note) this.note(`drawText：槽 ${params.textureId} ${plan.note}`);
    if (alignMode !== 0) {
      // 纵向对位模式（Font+0x313D0）：spec 只说 "== 1 时 y 修正"，修正量没有取证 ⇒ 不改 y
      this.note(
        `drawText：槽 ${params.textureId} alignMode = ${alignMode} ⇒ 引擎会做纵向对位修正，` +
          '修正量未取证，本实现不改 y（标为未实现，不是"等价"）',
      );
    }

    for (const pass of plan.passes) {
      this.renderTextPass(decoded.unicode, params.x + pass.dx, params.y + pass.dy, size, fontFamilies, params.weight, pass, rt);
    }

    const notes: string[] = [];
    notes.push(
      `字体统一映射到 ${GAME_FONT_FAMILY}（Amayui CN / 更纱黑体 cnjp 变体）；引擎 face name = ${
        fontFromEngine ?? 'null（默认未建立）'
      } 仅记录，不参与选择`,
    );
    if (params.size === undefined) notes.push(`size 未给 ⇒ 默认 ${TEXT_FONT_SIZE}（Font::init）`);
    if (params.shadowColor === undefined) notes.push('副色未给 ⇒ 默认 0 = 不透明黑（Font::init）');
    notes.push('字体色 24 位 RGB（COLORREF 口径），不透明');
    if (alignMode !== 0) notes.push(`alignMode = ${alignMode} 的 y 修正未实现`);

    slot.drawCount += 1;
    slot.writeCount += 1;
    slot.hasContent = true;
    slot.lastDraw = {
      textureId: params.textureId,
      x: params.x,
      y: params.y,
      byteLength: params.text.length,
      encoding: decoded.encoding,
      unicode: decoded.unicode,
      fontFamilies,
      fontFromEngine,
      size,
      drawMode,
      alignMode,
      color: params.color,
      rgb: primary.rgb,
      shadowColor: params.shadowColor ?? null,
      passes: plan.passes,
      ok: true,
      note: notes.join('；'),
    };
    this.lastDraw = slot.lastDraw;

    // ★ 纹理槽刚拿到像素：用**同一份快照**重建一次场景节点，否则下一次 `present` 还会画
    //   上一帧的节点（快照不含像素，`writeCount` 是渲染进程自己的状态）。
    //   ⛔ 不伪造场景内容：只是把"同一份快照 + 新像素"重新画一遍。
    this.rebuildSceneFromLastSnapshot();
  }

  /** 画一个 pass（临时 `Text` → 目标 RT → 立即销毁，含它自带的 canvas 纹理） */
  private renderTextPass(
    text: string,
    x: number,
    y: number,
    size: number,
    fontFamilies: string[],
    weight: number | string | undefined,
    pass: DrawPassState,
    rt: RenderTexture,
  ): void {
    const label = new Text({
      text,
      style: {
        fontFamily: fontFamilies,
        fontSize: size,
        fill: pass.rgb,
        align: 'left',
        ...(weight !== undefined ? { fontWeight: normalizeFontWeight(weight) } : {}),
      },
    });
    label.position.set(x, y);
    label.alpha = pass.alpha;
    try {
      // `0x204` 的一次同步绘制：画进该槽的 RenderTexture，`clear: false` 叠在已有内容上
      this.app.renderer.render({ container: label, target: rt, clear: false });
    } finally {
      label.destroy({ texture: true, textureSource: true });
    }
  }

  /* ─────────────────────────── 场景 ─────────────────────────── */

  /** 每个场景快照重建一次：先销毁上一批节点，再把纹理槽与绘制项对齐 */
  private reconcileTextures(textures: TextureSnapshot[]): void {
    const seen = new Set<number>();
    for (const texture of textures) {
      if (seen.has(texture.id)) {
        this.note(`场景快照里纹理槽 ${texture.id} 重复出现 ⇒ 只取第一条`);
        continue;
      }
      seen.add(texture.id);
      const existing = this.slots.get(texture.id);
      if (!existing) {
        this.slots.set(texture.id, {
          snapshot: { ...texture },
          rt: null,
          surfaceId: null,
          drawCount: 0,
          renderTargetWrites: 0,
          writeCount: 0,
          hasContent: false,
          lastDraw: null,
        });
        continue;
      }
      const resized = existing.snapshot.width !== texture.width || existing.snapshot.height !== texture.height;
      existing.snapshot = { ...texture };
      if (resized && existing.rt) {
        existing.rt.destroy(true);
        existing.rt = null;
        existing.hasContent = false;
        this.note(`纹理槽 ${texture.id} 尺寸变化（${texture.width}×${texture.height}）⇒ 重建 RenderTexture（已画内容丢失）`);
      }
    }
    for (const id of [...this.slots.keys()]) {
      if (seen.has(id)) continue;
      this.dropSlot(id);
    }
  }

  private dropSlot(id: number): void {
    this.movieSlots.delete(id);
    const slot = this.slots.get(id);
    if (!slot) return;
    if (slot.surfaceId !== null) {
      this.surfaces.delete(slot.surfaceId);
      if (this.currentSurfaceId === slot.surfaceId) {
        this.note(`纹理槽 ${id} 消失时仍是当前渲染目标 ⇒ 保留表面号（未取证：是否应复位）`);
      }
      if (this.currentTarget === id) {
        this.note(`纹理槽 ${id} 消失时仍是当前渲染目标 ⇒ 目标保留槽号 ${id}（未取证：是否应复位到后缓冲）`);
      }
    }
    slot.rt?.destroy(true);
    this.slots.delete(id);
  }

  /** 从 renderer 的最近快照里删掉一个已释放纹理槽的引用（防止旧快照把空槽画成 placeholder） */
  private scrubTextureFromSnapshot(textureId: number): void {
    if (!this.scene) return;
    const scene = this.scene;
    this.scene = {
      ...scene,
      textures: scene.textures.filter((texture) => texture.id !== textureId),
      drawItems: scene.drawItems.filter((item) => item.textureId !== textureId),
    };
  }

  /** 用最近一次快照重建场景节点（`drawText` 改了纹理内容之后用；⛔ 不改快照本身） */
  private rebuildSceneFromLastSnapshot(): void {
    if (!this.scene) return;
    this.clearBuilt();
    this.sceneStats = this.buildScene(this.scene);
  }

  private buildScene(scene: SceneSnapshot): SceneBuildStats {
    this.applyBlend(scene.blend);

    // ★ 命令档：**只规划、不执行**（S0/S1）。这里用规划结果做统计与诊断；⛔ 一个阶段都不发，
    //   下面的绘制仍然只来自收尾扫描 —— 所以 renderer 的诊断里有 `scene-command-pass-render-unmodeled`。
    const commandPass = planCommandPassForScene(scene);

    const stats: SceneBuildStats = {
      width: scene.width,
      height: scene.height,
      target: scene.target,
      textures: scene.textures.length,
      drawItems: scene.drawItems.length,
      activeItems: 0,
      sprites: 0,
      missingTextures: 0,
      noTarget: 0,
      consumedSkips: 0,
      clippedEmpty: 0,
      drawFailed: 0,
      dirtyRequests: 0,
      rectWriteBacks: 0,
      meshes: scene.meshes.length,
      meshQuads: 0,
      meshSkipped: 0,
      order: '',
      commandPassCommands: commandPass.commands.length,
      commandPassPhases: commandPass.commands.reduce((sum, command) => sum + command.phases.length, 0),
      commandPassConsumedKeys: commandPass.consumedKeys.length,
    };

    // 纹理槽解析（`CORE-OPS-TEXTURE-DRAW-CONSUMER` 第 3 条）：**快照里没有这个编号 ⇒ 空槽**。
    // ★ 快照的 `textures` 只装"有对象的槽"（核心的 `textureObjects`），所以"数组里没有"就是"空槽"；
    //   ⛔ 不要拿渲染侧自己的 `this.slots` 当解析源（那是平台资产表，不是引擎的纹理槽表）。
    const planSlots = new Map<number, DrawPlanSlot>();
    for (const texture of scene.textures) {
      planSlots.set(texture.id, { width: texture.width, height: texture.height });
    }
    // 有没有渲染目标：后缓冲（-1）永远有；其它编号要求"那个纹理槽还在"（对象已被释放 ⇒ 没有目标）。
    const hasTarget = scene.target < 0 || planSlots.has(scene.target);
    /** 规划器报上来的实现缺口（去重后一次性进诊断，⛔ 不是引擎错误） */
    const planNotes = new Set<DrawPlanNote>();

    // ★ 绘制项与网格按 `CORE-SCENE-DRAW-ORDER`：**key 严格升序**，同键网格先于绘制项（见 `scene-order.ts`）。
    //   TITLE 的黑色背景网格 key 0 必须落到前景绘制项下面，否则整屏被它盖黑。
    const ordered = orderedDrawables(scene);
    stats.order = ordered
      .slice(0, 16)
      .map((entry) => `${entry.kind}#${entry.key}`)
      .join(' → ') + (ordered.length > 16 ? ` → …（共 ${ordered.length}）` : '');

    for (const entry of ordered) {
      if (entry.kind === 'mesh') {
        const mesh = entry.mesh;
        if ((mesh.flags & 1) === 0) continue;
        if (mesh.count !== 4 || mesh.copies[0].length < 4 || mesh.copies[1].length < 4) {
          stats.meshSkipped += 1;
          continue;
        }
        const color = mesh.color >= 0 ? mesh.color : (mesh.colors[0] ?? -1);
        if (color < 0 || mesh.colors.some((value) => value !== color)) {
          stats.meshSkipped += 1;
          continue;
        }
        const xs = mesh.copies[0].slice(0, 4).map(f32FromBits);
        const ys = mesh.copies[1].slice(0, 4).map(f32FromBits);
        if (![...xs, ...ys].every(Number.isFinite)) {
          stats.meshSkipped += 1;
          continue;
        }
        const argb = argbOf(color);
        const graphics = new Graphics();
        // LOGO 的 copies 是 strip 顺序：0-1-2 与 1-3-2 两个三角形。
        graphics.poly([xs[0]!, ys[0]!, xs[1]!, ys[1]!, xs[2]!, ys[2]!]).fill({ color: argb.rgb, alpha: argb.alpha });
        graphics.poly([xs[1]!, ys[1]!, xs[3]!, ys[3]!, xs[2]!, ys[2]!]).fill({ color: argb.rgb, alpha: argb.alpha });
        this.addBuilt(graphics);
        stats.meshQuads += 1;
        continue;
      }

      const item = entry.item;
      // ★ 每个条目**只由规划器**决定"发不发请求、实参是什么"（`scene-draw-plan.ts`）。
      //   ⛔ 这里不再有"空槽画占位块"、"零面积"、"自引用跳过"这些实现自造的规则。
      const plan = planDraw({
        item,
        resolveSlot: (textureId) => planSlots.get(textureId) ?? null,
        target: { id: scene.target, present: hasTarget },
        view: scene.view,
        now: scene.clock,
        // 场景自身记录（6.1）：核心侧已建模键区间 / 平移；派生矩阵未建模（`matrix: null`）
        selfTransform: scene.selfRecord,
        // 收尾扫描**总是**不跳过额外变换（命令档执行未建模 ⇒ 现在没有传 true 的调用方）
        skipExtraTransform: false,
      });
      for (const note of plan.notes) planNotes.add(note);
      if (plan.markDirty) stats.dirtyRequests += 1;
      if (plan.rectWriteBack !== null) stats.rectWriteBacks += 1;
      if ((item.flags & 1) !== 0) stats.activeItems += 1;

      if (plan.skip !== null) {
        switch (plan.skip) {
          case 'no-valid':
            // 静默：不画、不报错
            break;
          case 'consumed':
            stats.consumedSkips += 1;
            break;
          case 'missing-texture':
            stats.missingTextures += 1;
            if (plan.report) this.errorNote('missing-texture', plan.report.textureId, plan.report.text);
            break;
          case 'no-target':
            stats.noTarget += 1;
            if (plan.report) this.errorNote('no-target', plan.report.textureId, plan.report.text);
            break;
          case 'empty-clip':
            // 夹空是**静默**的：不上报、不建占位物
            stats.clippedEmpty += 1;
            break;
        }
        continue;
      }

      const args = plan.draw;
      if (args === null) continue; // 规划器的约定：skip === null ⇒ draw 非 null（防御）
      const slot = this.slots.get(args.textureId);
      if (!slot) {
        // 快照里有纹理对象、平台侧却没有这个槽：按「绘制失败」上报并跳过（⛔ 不建占位物）
        stats.drawFailed += 1;
        this.errorNote('draw-failed', args.textureId,
          drawFailedReport(args.textureId, '平台侧没有该纹理槽').text);
        continue;
      }
      try {
        // 全图时不建 frame；子矩形按 RenderTexture 的物理 / 逻辑比例换算（裁剪后的矩形来自规划器）。
        const rt = slot.rt ?? this.ensureTexture(slot);
        let texture = rt as Texture;
        const frameWidth = args.rect.right - args.rect.left;
        const frameHeight = args.rect.bottom - args.rect.top;
        if (args.rect.left !== 0 || args.rect.top !== 0
          || frameWidth !== rt.width || frameHeight !== rt.height) {
          const u = rt.source.width / Math.max(1, rt.width);
          const v = rt.source.height / Math.max(1, rt.height);
          texture = new Texture({
            source: rt.source,
            frame: new Rectangle(args.rect.left * u, args.rect.top * v, frameWidth * u, frameHeight * v),
          });
          this.frameTextures.push(texture);
        }
        const sprite = new Sprite(texture);
        // 位置 = 裁剪补偿 / 档位缩放取整 / 场景偏移之后的三个浮点格（第三格不参与显示）
        sprite.position.set(args.position[0], args.position[1]);
        // 本步颜色是**一个** 32 位值（含透明度）；默认不透明白
        const color = argbOf(args.color);
        sprite.tint = color.rgb;
        sprite.alpha = color.alpha;
        // ★ 本步变换（`args.transform`）**没有在这里应用**：Pixi 的 Sprite 用位置实参定位，
        //   而"绘制入口怎么解释矩阵"属平台（见 DISCLOSURES）；⛔ 不要拿它再算一遍位置（会双重变换）。
        this.addBuilt(sprite);
        stats.sprites += 1;
      } catch (error) {
        stats.drawFailed += 1;
        this.errorNote('draw-failed', args.textureId, drawFailedReport(args.textureId, messageOf(error)).text);
      }
    }

    // 影片帧：放在网格之后覆盖（LOGO 收尾的黑色网格属于场景，不应盖住影片）。
    // ★ 这是本实现的呈现顺序选择；引擎的影片/场景次序未成篇（RES-MOVIE-DRAW-ORDER）。
    for (const textureId of this.movieSlots) {
      const slot = this.slots.get(textureId);
      if (!slot || !slot.rt || !slot.hasContent) continue;
      const sprite = new Sprite(slot.rt as Texture);
      this.addBuilt(sprite);
      stats.sprites += 1;
    }

    // ★ 场景是每帧重建的，同一条警告会重复几十次 ⇒ 按 kind 记住"上次发出的文本"，
    //   只在文本（通常是计数）变化时记一条；否则 diagnostics 的"最近日志"会被同一行刷满。
    this.sceneNote(
      'draw-source-missing',
      stats.missingTextures > 0
        ? `场景：${stats.missingTextures} 条绘制项的纹理槽里没有对象 ⇒ 已上报并跳过（⛔ 不建占位物）`
        : null,
    );
    this.sceneNote(
      'draw-target-missing',
      stats.noTarget > 0 ? `场景：${stats.noTarget} 条绘制项没有渲染目标 ⇒ 已上报并跳过` : null,
    );
    this.sceneNote(
      'draw-failed',
      stats.drawFailed > 0 ? `场景：${stats.drawFailed} 条绘制项绘制失败 ⇒ 已上报并跳过` : null,
    );
    this.sceneNote(
      'clipped-empty',
      stats.clippedEmpty > 0
        ? `场景：${stats.clippedEmpty} 条绘制项的源矩形夹到纹理逻辑矩形后为空 ⇒ 静默跳过（不上报）`
        : null,
    );
    this.sceneNote(
      'scene-command-pass',
      commandPass.commands.length > 0
        ? `场景：命令档规划 ${commandPass.commands.length} 条 / ${stats.commandPassPhases} 阶段 / 覆盖键 `
          + `${stats.commandPassConsumedKeys} 个（本实现只规划、不执行 ⇒ 画面仍来自收尾扫描）`
        : null,
    );
    this.sceneNote(
      'draw-plan-gaps',
      planNotes.size > 0 ? `场景：绘制项消费的未建模 / 未取证档：${[...planNotes].join('、')}` : null,
    );
    this.sceneNote(
      'dirty-requests',
      stats.dirtyRequests > 0
        ? `场景：${stats.dirtyRequests} 条绘制项请求标脏（本层没有回写路径 ⇒ 只记账）`
        : null,
    );
    this.sceneNote(
      'rect-writeback-requests',
      stats.rectWriteBacks > 0
        ? `场景：${stats.rectWriteBacks} 条绘制项的源矩形窗口到期、要求写回四条边（本层没有回写路径 ⇒ 只记账）`
        : null,
    );
    this.sceneNote(
      'mesh-unsupported',
      stats.meshSkipped > 0
        ? `场景：${stats.meshSkipped} 个网格不是“4 顶点、四色一致”的 LOGO 形状 ⇒ 未画（逐顶点布局 / topology 未取证）`
        : null,
    );
    this.sceneNote(
      'mesh-quads',
      stats.meshQuads > 0 ? `场景：画出 ${stats.meshQuads} 个网格四边形（4 顶点 / 单色）` : null,
    );
    return stats;
  }

  /** 同一条场景警告**只在内容变化时**记一次（`null` = 这一类已经没有了，下次再出现会重新记） */
  private sceneNote(kind: string, text: string | null): void {
    const previous = this.lastSceneNotes.get(kind) ?? '';
    const next = text ?? '';
    if (previous === next) return;
    this.lastSceneNotes.set(kind, next);
    if (next) this.note(next);
  }

  /**
   * 混合：把 `0x33f` 的模式格按**映射表**翻成 Pixi 的混合模式。
   * ★ 未命中 ⇒ normal + 日志（⛔ 不猜）。颜色非「无颜色」时按整体 tint 应用（未取证）。
   */
  private applyBlend(blend: SceneSnapshot['blend']): void {
    const entry = BLEND_MODE_MAP.find((x) => x.mode === blend.mode);
    const mapped: BLEND_MODES = entry ? entry.pixi : 'normal';
    if (!entry) {
      this.note(`场景混合：未知模式 ${blend.mode} ⇒ 退回 normal（映射未取证，见 RES-RENDER-BLEND-PIXEL）`);
    }
    this.sceneRoot.blendMode = mapped;
    this.sceneRoot.alpha = 1;
    if (blend.color !== -1) {
      const { rgb } = argbOf(blend.color);
      this.sceneRoot.tint = rgb;
    } else {
      this.sceneRoot.tint = 0xffffff;
    }
    this.blendState = { mode: blend.mode, mapped, unknown: !entry, color: blend.color };
  }

  private addBuilt(node: Container): void {
    this.built.push(node);
    this.sceneRoot.addChild(node);
  }

  /** 销毁上一批节点；⛔ 不碰 `RenderTexture`（那是纹理槽的资产，Sprite 只是引用） */
  private clearBuilt(): void {
    for (const node of this.built) destroyDisplay(node);
    this.built = [];
    for (const texture of this.frameTextures) texture.destroy(false);
    this.frameTextures = [];
    this.sceneRoot.removeChildren();
  }

  /* ─────────────────────────── 呈现 ─────────────────────────── */

  /**
   * 把当前场景提交到**它自己的目标**：
   * - `scene.target >= 0` ⇒ 渲染进该纹理槽的 `RenderTexture`（`clear: true`）；
   * - 否则 ⇒ 渲染到画布（后缓冲）。
   * ★ 用**快照里的 `target`**，不是 `setRenderTarget` 记的平台表面：快照是"这一帧要画到哪"的
   *   一次完整描述，`setRenderTarget` 是 `0x20d` 的历史状态。两者不一致时记一条日志（协议缺口，
   *   见回报 §4）。
   * ★ `sceneRoot` **不在** `app.stage` 里 ⇒ 这里画一次就是一次（⛔ 不会重影）。
   */
  private present(): void {
    const scene = this.scene;
    if (!scene) {
      this.note('present：还没有收到任何场景快照 ⇒ 无事可做');
      return;
    }

    const platformTarget = this.currentTarget;
    if (platformTarget !== scene.target) {
      this.note(
        `present：快照 target = ${scene.target}，但 setRenderTarget 记录的是 ${platformTarget}` +
          ' ⇒ 以快照 target 为准（协议缺口：两处目标没有保证一致）',
      );
    }

    if (scene.target >= 0) {
      const slot = this.slots.get(scene.target);
      if (slot) {
        const rt = this.ensureTexture(slot);
        // 已有文件像素 / 离屏文字时不清屏，否则会把它们擦掉（呈现与目标的次序本版未取证）
        this.app.renderer.render({ container: this.sceneRoot, target: rt, clear: !slot.hasContent });
        slot.renderTargetWrites += 1;
        slot.writeCount += 1;
        slot.hasContent = true;
        this.presentCount += 1;
        this.lastPresent = { kind: 'texture', textureId: scene.target, fellBackToBackBuffer: false };
        return;
      }
      this.note(`present：目标纹理槽 ${scene.target} 不在场景里 ⇒ 退回后缓冲（诊断，不是规范结论）`);
    }

    this.app.renderer.render({ container: this.sceneRoot, clear: true });
    this.presentCount += 1;
    this.lastPresent = {
      kind: 'backbuffer',
      textureId: null,
      fellBackToBackBuffer: scene.target >= 0,
    };
  }
}

/* ─────────────────────────────── 小工具 ─────────────────────────────── */

/** 形态字 → 人话（`06-window.md`：S = 0 占满屏，S ≠ 0 窗口态） */
export function modeLabelOf(info: RendererInit | null): string {
  if (!info) return '未初始化（还没收到 init）';
  const mode = info.screenMode === 0 ? '占满屏态（screenMode = 0）' : `窗口态（screenMode = ${info.screenMode}）`;
  return `${mode}，fullscreenPresent = ${info.fullscreenPresent}`;
}

/** 递归销毁：`Text` 连自带纹理一起回收，其它节点只销毁自己（⛔ 不要毁掉它引用的 RenderTexture） */
function destroyDisplay(node: Container): void {
  if (node instanceof Text) {
    node.destroy({ texture: true, textureSource: true });
    return;
  }
  for (const child of [...node.children]) destroyDisplay(child);
  node.destroy();
}

function assertNever(call: never): never {
  throw new Error(`未处理的 RenderCall：${JSON.stringify(call)}`);
}

/** 网格 copies 里的 32 位字按 float32 解释（LOGO 的 x / y 是 float 位模式） */
function f32FromBits(word: number): number {
  const view = new DataView(new ArrayBuffer(4));
  view.setUint32(0, word >>> 0, true);
  return view.getFloat32(0, true);
}

function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
