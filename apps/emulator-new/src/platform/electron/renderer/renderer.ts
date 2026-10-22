/**
 * `apps/emulator-new/src/platform/electron/renderer/renderer.ts` —— 渲染进程的入口。
 *
 * 职责（只有三件）：
 *  1. 等 Pixi / WebGL 就绪 → 调 `window.amayui.ready(...)`（失败则**不** ready，见协议）；
 *  2. 把主进程发来的 `RenderCall` 交给 `PixiRenderHost` 执行并 `reply`；
 *  3. 把 `PushMessage`（`scene` / `status` / `boot`）画出来，并把"忽略 / 未建模 / 未取证"显式
 *     写进右上角的 diagnostics 面板。
 *
 * ★ `window.amayui` 由 **preload** 注入（`contextBridge`）。本文件 ⛔ 不 import Node / Electron /
 *   核心（`src/core/**`）：它只认 `../protocol.ts` 的线协议。
 * ★ 调用可能**比 Pixi 初始化更早**到达（主进程不该这么干，但协议没有禁止）⇒ 这里先注册 handler、
 *   把 call / push 排队，`ready` 之后再按序放行（⛔ 不丢消息、也不假装它没发生）。
 * ★ 没有 preload（例如直接用浏览器打开 `dist/renderer/index.html` 做手工验证）时，装一个**最小
 *   mock**：它按协议发一遍 init / boot / scene / getRenderSurface / drawText / present，
 *   面板上明写「mock，不是引擎观测」。
 */

import type {
  AmayuiBridge,
  PushMessage,
  RenderCall,
  RendererInit,
  RenderResult,
  SceneSnapshot,
  SurfaceRef,
} from '../protocol.ts';
import regularFontUrl from '../../../../../../corpus/assets/fonts/Amayui-CN_cnjp.ttf?url';
import boldFontUrl from '../../../../../../corpus/assets/fonts/Amayui-CN_cnjp-Bold.ttf?url';
import { DISCLOSURES, GAME_FONT_FAMILY, PixiRenderHost } from './pixi-host.ts';

/* ─────────────────────────────── DOM ─────────────────────────────── */

function mustGetById(id: string): HTMLElement {
  const node = document.getElementById(id);
  if (!node) throw new Error(`index.html 缺少 #${id}`);
  return node;
}

function el<K extends keyof HTMLElementTagNameMap>(tag: K, className?: string): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className) node.className = className;
  return node;
}

function textEl<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className: string | undefined,
  text: string,
): HTMLElementTagNameMap[K] {
  const node = el(tag, className);
  node.textContent = text;
  return node;
}

function section(title: string, ...body: Node[]): HTMLElement {
  const node = el('section');
  node.appendChild(textEl('h2', undefined, title));
  for (const child of body) node.appendChild(child);
  return node;
}

function list(items: readonly string[], className?: string): HTMLElement {
  const ul = el('ul', className);
  for (const item of items) ul.appendChild(textEl('li', undefined, item));
  return ul;
}

function kv(rows: readonly (readonly [string, string | Node])[]): HTMLElement {
  const dl = el('dl', 'kv');
  for (const [key, value] of rows) {
    dl.appendChild(textEl('dt', undefined, key));
    const dd = el('dd');
    if (typeof value === 'string') dd.textContent = value;
    else dd.appendChild(value);
    dl.appendChild(dd);
  }
  return dl;
}

function tag(text: string, kind: 'ok' | 'warn' | 'bad'): HTMLElement {
  return textEl('span', `tag tag--${kind}`, text);
}

/** 颜色显示：`-1` 是「无颜色」哨兵（`core/render.ts` 的 `NO_COLOR`），其它按 ARGB 打全 32 位 */
function fmtColor(color: number): string {
  if (color === -1) return '无颜色（-1）';
  return `0x${(color >>> 0).toString(16).padStart(8, '0')}`;
}

/* ─────────────────────────────── 状态 ─────────────────────────────── */

let host: PixiRenderHost | null = null;
let bridge: AmayuiBridge;
let bootInfo: (RendererInit & { title: string; ignored: string[] }) | null = null;
let lastStatus: { level: 'info' | 'warn' | 'error'; text: string } | null = null;
let statusCount = 0;
/** Pixi 就绪前到达的调用 / 推送（按到达顺序排队） */
const pendingCalls: RenderCall[] = [];
const pendingPushes: PushMessage[] = [];

let diagnosticsBody: HTMLElement | null = null;
let diagnosticsSummary: HTMLElement | null = null;
let diagnosticsRoot: HTMLElement | null = null;
let fatalBox: HTMLElement | null = null;
let fatalText: HTMLElement | null = null;
let diagnosticsQueued = false;

/** 每次调用 / 推送后刷新面板（rAF 合并；面板很便宜，但没必要一帧刷多次） */
function scheduleDiagnostics(): void {
  if (diagnosticsQueued) return;
  diagnosticsQueued = true;
  requestAnimationFrame(() => {
    diagnosticsQueued = false;
    renderDiagnostics();
  });
}

function renderDiagnostics(): void {
  if (!host || !diagnosticsBody) return;
  const d = host.diagnostics();
  const body = diagnosticsBody;
  body.replaceChildren();

  const modeShort = d.init ? (d.init.screenMode === 0 ? '占满屏' : '窗口') : '未 init';
  if (diagnosticsSummary) {
    diagnosticsSummary.textContent = `${modeShort} · ${d.backend} · ${d.canvas.width}×${d.canvas.height}`;
  }

  body.appendChild(
    section(
      '运行状态',
      kv([
        ['模式', d.modeLabel],
        ['画布', `${d.canvas.width}×${d.canvas.height}（客户区像素；Pixi resolution ${d.resolution}）`],
        ['Pixi 后端', d.backend],
        [
          '平台当前目标',
          d.currentSurface.kind === 'texture'
            ? `纹理槽 ${d.currentSurface.textureId}（surfaceId ${d.currentSurface.surfaceId ?? '—'}${
                d.currentSurface.surfaceReleased ? '，句柄已归还' : ''
              }）`
            : '后缓冲（surfaceId 0）',
        ],
        ['表面表', `${d.surfaceCount} 个已分配句柄；下一个 surfaceId = ${d.nextSurfaceId}`],
        [
          'present',
          `${d.presentCount} 次${
            d.lastPresent
              ? `；最近 → ${
                  d.lastPresent.kind === 'texture' ? `纹理槽 ${d.lastPresent.textureId}` : '后缓冲'
                }${d.lastPresent.fellBackToBackBuffer ? '（目标槽缺失，已降级）' : ''}`
              : ''
          }`,
        ],
      ]),
    ),
  );

  body.appendChild(section('未建模 / 未取证（显式）', list(DISCLOSURES)));

  const ignored = bootInfo?.ignored ?? [];
  body.appendChild(
    section(
      `boot.ignored（${ignored.length} 条）`,
      ignored.length > 0
        ? list(ignored)
        : textEl('p', 'muted', bootInfo ? '（主进程声明没有忽略项）' : '主进程还没推 boot'),
    ),
  );

  const statusNode = el('div');
  if (lastStatus) {
    const kind = lastStatus.level === 'error' ? 'bad' : lastStatus.level === 'warn' ? 'warn' : 'ok';
    const line = el('p');
    line.appendChild(tag(lastStatus.level, kind));
    line.appendChild(document.createTextNode(lastStatus.text));
    statusNode.appendChild(line);
    statusNode.appendChild(textEl('p', 'muted', `共收到 ${statusCount} 条 status；这里只显示最近一条`));
  } else {
    statusNode.appendChild(textEl('p', 'muted', '（还没收到 status）'));
  }
  body.appendChild(section('最近 status', statusNode));

  if (d.scene) {
    const s = d.scene;
    body.appendChild(
      section(
        '场景快照',
        kv([
          ['客户区', `${s.width}×${s.height}（绘制坐标 = 客户区像素，1:1）`],
          ['目标', s.target < 0 ? '后缓冲（-1）' : `纹理槽 ${s.target}`],
          ['纹理槽 / 绘制项', `${s.textures} 槽；${s.activeItems}/${s.drawItems} 条有效绘制项`],
          ['画成什么', `Sprite ${s.sprites}；占位块 ${s.placeholders}（其中标签 ${s.labels}）`],
          [
            '跳过 / 异常',
            `自引用 ${s.selfTargetSkips}；零面积 ${s.zeroArea}；缺纹理槽 ${s.missingSlots}；矩形与纹理尺寸不符 ${s.sizeMismatch}`,
          ],
          [
            '场景混合',
            d.blend
              ? `模式 ${d.blend.mode} → ${d.blend.mapped}${d.blend.unknown ? '（未知 ⇒ normal）' : ''}，颜色 ${fmtColor(d.blend.color)}（tint）`
              : '（还没收到）',
          ],
        ]),
      ),
    );

    if (d.textures.length > 0) {
      const table = el('table');
      const head = el('tr');
      for (const title of ['槽', '尺寸', '来源', '表面', '写入', '像素']) {
        head.appendChild(textEl('th', undefined, title));
      }
      table.appendChild(head);
      for (const texture of d.textures) {
        const row = el('tr');
        row.appendChild(textEl('td', undefined, `#${texture.id}`));
        row.appendChild(textEl('td', undefined, `${texture.width}×${texture.height}`));
        row.appendChild(
          textEl(
            'td',
            undefined,
            texture.blank !== 0
              ? `blank（0x1f8）`
              : texture.file >= 0
                ? `file ${texture.file}（未解码）`
                : '无来源',
          ),
        );
        row.appendChild(textEl('td', undefined, texture.hasSurface ? '有' : '—'));
        row.appendChild(textEl('td', undefined, `t${texture.renderTargetWrites} / d${texture.drawCount}`));
        row.appendChild(
          textEl('td', 'muted', texture.hasPixels ? '已写入' : texture.blank !== 0 ? '空白' : '占位'),
        );
        table.appendChild(row);
      }
      body.appendChild(section('纹理槽', table));
    }
  } else {
    body.appendChild(section('场景快照', textEl('p', 'muted', '（还没收到 scene）')));
  }

  if (d.lastDraw) {
    const t = d.lastDraw;
    const passText = t.passes
      .map(
        (pass) =>
          `${pass.role}@(${pass.dx >= 0 ? '+' : ''}${pass.dx},${pass.dy >= 0 ? '+' : ''}${pass.dy}) ` +
          `#${pass.rgb.toString(16).padStart(6, '0')} a=${pass.alpha.toFixed(2)}`,
      )
      .join('；');
    const rows: (readonly [string, string | Node])[] = [
      ['目标 / 位置', `槽 ${t.textureId} @ (${t.x}, ${t.y})（行顶 ⇒ Text 左上角）`],
      ['字节 → 文本', `${t.byteLength} 字节 → ${t.encoding} → ${JSON.stringify(t.unicode)}`],
      ['字号 / 绘制模式', `${t.size} / ${t.drawMode}（默认来自 Font::init：24 / 1）`],
      [
        '纵向对位',
        t.alignMode === 0 ? '0（默认，不改 y）' : `${t.alignMode} ⇒ 引擎会修正 y，修正量未取证 ⇒ 未实现`,
      ],
      [
        '字体',
        `统一 ${GAME_FONT_FAMILY}（Amayui CN / 更纱黑体 cnjp）· 引擎 face name = ${
          t.fontFromEngine ?? 'null（默认未建立）'
        }（仅记录，不参与选择）`,
      ],
      [
        '主色（24 位 RGB）',
        t.color === null
          ? `null ⇒ 默认 #${t.rgb.toString(16).padStart(6, '0')}（Font::init 0xFFFFFF）`
          : `0x${(t.color & 0xffffff).toString(16).padStart(6, '0')} ⇒ #${t.rgb
              .toString(16)
              .padStart(6, '0')}（COLORREF：无 alpha，永远不透明）`,
      ],
      [
        '副色（24 位 RGB）',
        t.shadowColor === null
          ? '未给 ⇒ 默认 0 = 不透明黑（Font::init）'
          : `0x${(t.shadowColor & 0xffffff).toString(16).padStart(6, '0')}（COLORREF）`,
      ],
      ['pass 序列', passText],
    ];
    if (t.note) rows.push(['说明', t.note]);
    body.appendChild(section('最近 drawText（0x204）', kv(rows)));
  }

  if (d.notes.length > 0) {
    body.appendChild(section('最近日志', list(d.notes.slice(-12).reverse())));
  }
}

function showFatal(message: string): void {
  if (!fatalBox || !fatalText) return;
  fatalText.textContent = message;
  fatalBox.hidden = false;
}

/* ─────────────────────────────── 线协议 ─────────────────────────────── */

function replySafely(result: RenderResult): void {
  try {
    bridge.reply(result);
  } catch (error) {
    // 回执本身失败没法再回执：只写控制台（不是业务结论）
    console.error('[amayui renderer] reply 失败', error);
  }
}

async function handleCall(call: RenderCall): Promise<void> {
  if (!host) {
    pendingCalls.push(call);
    return;
  }
  try {
    const result = await host.execute(call);
    if (result === undefined) replySafely({ id: call.id, ok: true });
    else replySafely({ id: call.id, ok: true, result });
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    replySafely({ id: call.id, ok: false, error: message });
    try {
      bridge.log(`[${call.method}#${call.id}] 平台失败：${message}`);
    } catch {
      /* 日志通道也失败就算了（不要因为日志炸掉渲染） */
    }
  }
  scheduleDiagnostics();
}

function handlePush(message: PushMessage): void {
  if (!host) {
    pendingPushes.push(message);
    return;
  }
  switch (message.kind) {
    case 'scene':
      host.applyScene(message.scene);
      break;
    case 'status':
      lastStatus = { level: message.level, text: message.text };
      statusCount += 1;
      break;
    case 'boot':
      bootInfo = message.info;
      document.title = `${message.info.title} — amayui renderer`;
      break;
  }
  scheduleDiagnostics();
}

/* ─────────────────────────────── 启动 ─────────────────────────────── */

/**
 * 预加载 **Amayui CN**（更纱黑体 cnjp 变体）的 Regular / Bold。
 * ★ 必须在 Pixi 第一次量字之前完成，否则 Text 会按回退字体度量并缓存。
 * ★ 字体文件来自随仓字体资产（LFS），由 Vite 作为 asset 复制到 dist。
 */
async function loadGameFont(): Promise<boolean> {
  try {
    const faces = [
      new FontFace(GAME_FONT_FAMILY, `url(${JSON.stringify(regularFontUrl)})`, { weight: '400', style: 'normal' }),
      new FontFace(GAME_FONT_FAMILY, `url(${JSON.stringify(boldFontUrl)})`, { weight: '700', style: 'normal' }),
    ];
    for (const face of faces) document.fonts.add(face);
    await Promise.all(faces.map((face) => face.load()));
    bridge.log(
      `Amayui CN 已加载（Regular + Bold，document.fonts.check=${document.fonts.check('24px "' + GAME_FONT_FAMILY + '"')}）`,
    );
    return true;
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    try {
      bridge.log(`Amayui CN 字体加载失败 ⇒ 文字会退回 sans-serif（${message}）`);
    } catch {
      /* ignore */
    }
    return false;
  }
}

async function main(): Promise<void> {
  diagnosticsRoot = mustGetById('diagnostics');
  diagnosticsBody = mustGetById('diagnostics-body');
  diagnosticsSummary = mustGetById('diagnostics-summary');
  fatalBox = mustGetById('fatal');
  fatalText = mustGetById('fatal-text');
  const hostElement = mustGetById('stage-host');

  const toggle = mustGetById('diagnostics-toggle');
  toggle.addEventListener('click', () => {
    if (!diagnosticsRoot) return;
    const collapsed = diagnosticsRoot.classList.toggle('is-collapsed');
    toggle.textContent = collapsed ? '展开' : '折叠';
  });

  // ① 先拿 bridge（没有 preload 就装 mock），再把 handler 挂上：早到的调用只会排队，不会丢。
  bridge = 'amayui' in window ? window.amayui : createMockBridge();
  bridge.onCall((call) => {
    void handleCall(call);
  });
  bridge.onPush(handlePush);

  // ② 先加载统一字体，再建 Pixi（Pixi 的 Text 会缓存首次量字时的字体度量）。
  const fontLoaded = await loadGameFont();
  if (!fontLoaded) {
    try {
      bridge.log('Amayui CN 未加载：所有文字会使用 sans-serif 兜底（这不是规格行为）');
    } catch {
      /* ignore */
    }
  }

  // ③ 建 Pixi。失败 ⇒ 不 ready（协议：失败时 renderer 不 ready）+ 全屏错误。
  try {
    host = await PixiRenderHost.create(hostElement, {
      log: (text) => {
        try {
          bridge.log(text);
        } catch {
          /* 同上：日志不能影响渲染 */
        }
      },
    });
  } catch (error) {
    const message = error instanceof Error ? `${error.message}\n${error.stack ?? ''}` : String(error);
    showFatal(`Pixi / WebGL 初始化失败：\n${message}`);
    try {
      bridge.log(`Pixi 初始化失败（不会 ready）：${message}`);
    } catch {
      /* ignore */
    }
    return;
  }

  // ④ 放行排队中的消息（顺序 = 到达顺序）
  for (const call of pendingCalls.splice(0)) void handleCall(call);
  for (const message of pendingPushes.splice(0)) handlePush(message);
  scheduleDiagnostics();

  // ⑤ 就绪回报：只调一次
  bridge.ready(host.readyInfo());
  requestAnimationFrame(() => renderDiagnostics());
}

void main();

/* ═══════════════════════════ 手工验证用的 mock（⛔ 不是引擎观测） ═══════════════════════════ */

/**
 * 没有 `window.amayui` 时装一个**最小 mock**：它扮演主进程，按协议发一遍
 * `init → boot → scene → getRenderSurface → drawText → present`。
 *
 * ★ 用途只有"手工看一眼渲染进程能不能跑"：`vite build` 之后用浏览器打开
 *   `dist/renderer/index.html`（或 `pnpm --filter @amayui/emulator-new exec vite --config vite.config.ts`
 *   的 dev server），面板与画面都该出来。
 * ★ 场景里的数字是**编的**（纹理 8642 / 槽 9 根本不存在），⛔ 不许拿它当"引擎行为"的证据，
 *   也不许把它的输出写进 spec / 台账。面板顶部的 status 会明写这一点。
 */
function createMockBridge(): AmayuiBridge {
  const callHandlers: ((call: RenderCall) => void)[] = [];
  const pushHandlers: ((message: PushMessage) => void)[] = [];
  const waiting = new Map<number, (result: RenderResult) => void>();
  let nextId = 1;
  let started = false;

  const emitPush = (message: PushMessage): void => {
    for (const handler of pushHandlers) handler(message);
  };

  const invoke = (call: CallWithoutId): Promise<RenderResult> =>
    new Promise((resolve) => {
      const id = nextId;
      nextId += 1;
      waiting.set(id, resolve);
      const full = { ...call, id } as unknown as RenderCall;
      for (const handler of callHandlers) handler(full);
    });

  const bridge: AmayuiBridge = {
    ready: () => {
      if (started) return;
      started = true;
      window.setTimeout(() => {
        void runMockScript(invoke, emitPush);
      }, 0);
    },
    onCall: (handler) => {
      callHandlers.push(handler);
    },
    onPush: (handler) => {
      pushHandlers.push(handler);
    },
    reply: (result) => {
      const resolve = waiting.get(result.id);
      if (!resolve) return;
      waiting.delete(result.id);
      resolve(result);
    },
    log: (text) => {
      console.info('[amayui mock]', text);
    },
  };
  return bridge;
}

/** `RenderCall` 去掉 `id` 后的**分配式**联合（`Omit<union>` 会把成员压扁，拿不到 method↔params 的对应） */
type CallWithoutId = RenderCall extends infer C ? (C extends RenderCall ? Omit<C, 'id'> : never) : never;

/** mock 的脚本（见 `createMockBridge` 的说明：⛔ 这是编的数据） */
async function runMockScript(
  invoke: (call: CallWithoutId) => Promise<RenderResult>,
  emitPush: (message: PushMessage) => void,
): Promise<void> {
  const init: RendererInit = {
    width: 960,
    height: 540,
    screenMode: 1,
    fullscreenPresent: false,
    background: 0x0e1116,
  };

  await invoke({ method: 'init', params: init });
  emitPush({
    kind: 'boot',
    info: {
      ...init,
      title: 'AMAYUI RENDERER（mock 手工验证）',
      ignored: [
        '（mock）真实 boot.ignored 由主进程按 core/window.ts 的 ignored 推来：顶部菜单 / IME / 图标 / 风格位…',
        '（mock）这里没有安装目录，也没有执行任何脚本 —— 画面上的每个数字都是编的',
      ],
    },
  });
  emitPush({
    kind: 'status',
    level: 'warn',
    text: '（mock）这是浏览器里的 mock 场景，不是从安装目录读出的引擎状态；⛔ 不要当观测证据',
  });

  const scene: SceneSnapshot = {
    width: 960,
    height: 540,
    textures: [
      { id: 0, kind: 'normal', width: 360, height: 180, file: 8642, blank: 0 },
      { id: 1, kind: 'normal', width: 200, height: 120, file: -1, blank: 1 },
      { id: 2, kind: 'divided', width: 64, height: 64, file: 17, blank: 0 },
    ],
    drawItems: [
      { key: 1, flags: 1, textureId: 0, left: 60, top: 60, right: 420, bottom: 240, color: 0xffffffff, aux: [0, 0, 0] },
      { key: 2, flags: 1, textureId: 1, left: 500, top: 60, right: 700, bottom: 180, color: 0xff66ccff, aux: [0, 0, 0] },
      // 故意引用一个不存在的槽：验证"无纹理槽"占位块
      { key: 3, flags: 1, textureId: 9, left: 500, top: 220, right: 700, bottom: 320, color: -1, aux: [0, 0, 0] },
      // flags 位 0 = 0 ⇒ 无效绘制项，不该出现在画面上
      { key: 4, flags: 0, textureId: 2, left: 60, top: 300, right: 124, bottom: 364, color: -1, aux: [0, 0, 0] },
    ],
    blend: { mode: 0, color: -1 },
    target: -1,
  };

  emitPush({ kind: 'scene', scene });
  await invoke({ method: 'present', params: {} });

  // 往槽 0 画一段 Shift-JIS 文本（'こんにちは' 的字节序列），再把场景推一次让 Sprite 用上新像素。
  // ★ 这里只给协议现有字段 ⇒ 走 `Font::init` 的默认值：字号 24、主色白、副色黑（都是 24 位 RGB）、
  //   绘制模式 1（右下阴影）。想手工验证其它分支就显式给 `size` / `drawMode` / `shadowColor` / `weight`。
  const surface = await invoke({ method: 'getRenderSurface', params: { textureId: 0 } });
  if (surface.ok) {
    const ref = surface.result as SurfaceRef;
    await invoke({
      method: 'drawText',
      params: {
        textureId: 0,
        x: 16,
        y: 16,
        // こんにちは（cp932 字节；核心给的是 latin1 字节串，每个 charCode 0..255）
        text: '\x82\xb1\x82\xf1\x82\xc9\x82\xbf\x82\xcd',
        font: null,
        color: null,
      },
    });
    emitPush({ kind: 'scene', scene });
    await invoke({ method: 'present', params: {} });

    // 演示"目标是纹理"：把槽 1 设为目标 → present 渲染进它的 RenderTexture（画布保持上一帧）
    const surface1 = await invoke({ method: 'getRenderSurface', params: { textureId: 1 } });
    if (surface1.ok) {
      const ref1 = surface1.result as SurfaceRef;
      await invoke({ method: 'setRenderTarget', params: { surfaceId: ref1.surfaceId } });
      emitPush({ kind: 'scene', scene: { ...scene, target: 1 } });
      await invoke({ method: 'present', params: {} });
      await invoke({ method: 'releaseSurface', params: { surfaceId: ref1.surfaceId } });
      await invoke({ method: 'setRenderTarget', params: { surfaceId: 0 } });
      emitPush({ kind: 'scene', scene });
      await invoke({ method: 'present', params: {} });
    }
    await invoke({ method: 'releaseSurface', params: { surfaceId: ref.surfaceId } });
  }

  emitPush({
    kind: 'status',
    level: 'info',
    text: '（mock）协议调用走完：init / boot / scene / getRenderSurface / drawText / setRenderTarget / releaseSurface / present',
  });
}
