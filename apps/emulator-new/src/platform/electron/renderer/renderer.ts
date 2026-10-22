/**
 * `apps/emulator-new/src/platform/electron/renderer/renderer.ts` —— 渲染进程的入口。
 *
 * 职责：
 *  1. 等 Pixi / WebGL 就绪 → 调 `window.amayui.ready(...)`（失败则**不** ready，见协议）；
 *  2. 接 main 的 `start`，把 core 会话（`./session.ts`）跑起来；
 *  3. game 模式只留 canvas；diagnostics / boot / "忽略 / 未建模 / 未取证"投影给独立 debug 窗口
 *     （同一个 bundle 的 `?debug=1` 分支，只读）。
 *
 * ★ 2026-10 重构后，**core 在这个进程里**：`session.ts` 直接 import `core/**`，
 *   文件 / 窗口请求走 `window.amayui.invoke`，渲染直接调本地 `PixiRenderHost`。
 *   本文件只做 DOM / 诊断 / 启动接线。
 * ★ `window.amayui` 由 preload 注入。没有 preload（浏览器手工打开 `dist/renderer/index.html`）时
 *   装一个最小 mock：它只发 demo 启动指令，不是引擎观测。
 */

import type { AmayuiBridge, DebugEvent, RendererBootInfo, StartJob } from '../protocol.ts';
import regularFontUrl from '../../../../../../corpus/assets/fonts/Amayui-CN_cnjp.ttf?url';
import boldFontUrl from '../../../../../../corpus/assets/fonts/Amayui-CN_cnjp-Bold.ttf?url';
import { DISCLOSURES, GAME_FONT_FAMILY, PixiRenderHost } from './pixi-host.ts';
import type { HostDiagnostics } from './pixi-host.ts';
import { runStartJob } from './session.ts';

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
let bootInfo: RendererBootInfo | null = null;
let lastStatus: { level: 'info' | 'warn' | 'error'; text: string } | null = null;
let statusCount = 0;
/** debug 窗口：最近一次从 main 收到的 game renderer 诊断投影 */
let lastDiagnostics: HostDiagnostics | null = null;
let debugMode = false;
let lastDiagnosticsSentAt = 0;
const debugLogs: string[] = [];
/** Pixi 就绪前到达的 start（main 不该这么干，但协议不禁止） */
let pendingJob: StartJob | null = null;

let diagnosticsBody: HTMLElement | null = null;
let diagnosticsSummary: HTMLElement | null = null;
let diagnosticsRoot: HTMLElement | null = null;
let fatalBox: HTMLElement | null = null;
let fatalText: HTMLElement | null = null;
let diagnosticsQueued = false;

/**
 * 每帧合并一次：
 * - game 窗口：不画 DOM（面板隐藏），每 100ms 把 `HostDiagnostics` 投影发给 main；
 * - debug 窗口：画收到的投影（只读）。
 */
function scheduleDiagnostics(): void {
  if (diagnosticsQueued) return;
  diagnosticsQueued = true;
  requestAnimationFrame(() => {
    diagnosticsQueued = false;
    if (debugMode) {
      renderDiagnostics();
      return;
    }
    if (!host) return;
    const now = Date.now();
    if (now - lastDiagnosticsSentAt < 100) return;
    lastDiagnosticsSentAt = now;
    bridge.report({ kind: 'diagnostics', data: host.diagnostics() });
  });
}

function renderDiagnostics(): void {
  if (!diagnosticsBody) return;
  const d = lastDiagnostics ?? host?.diagnostics() ?? null;
  const body = diagnosticsBody;
  if (d === null) {
    body.replaceChildren(textEl('p', 'muted', '等待 game renderer 的第一份诊断投影…'));
    return;
  }
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
        : textEl('p', 'muted', bootInfo ? '（core 声明没有忽略项）' : 'core 还没完成 boot'),
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
          ['纹理槽 / 绘制项 / 网格', `${s.textures} 槽；${s.activeItems}/${s.drawItems} 条有效绘制项；${s.meshes} 个网格`],
          ['画成什么', `Sprite ${s.sprites}；占位块 ${s.placeholders}（其中标签 ${s.labels}）；网格四边形 ${s.meshQuads}（跳过 ${s.meshSkipped}）`],
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
                ? `file ${texture.file}`
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

  if (debugLogs.length > 0) {
    body.appendChild(section('debug 事件日志', list(debugLogs.slice(-12).reverse())));
  }
}

function showFatal(message: string): void {
  if (!fatalBox || !fatalText) return;
  fatalText.textContent = message;
  fatalBox.hidden = false;
}

/* ─────────────────────────────── 启动接线 ─────────────────────────────── */

/** main 的 start：Pixi 还没建好就先存着，建好后立刻跑。 */
function startJob(job: StartJob): void {
  if (!host) {
    pendingJob = job;
    return;
  }
  void runStartJob(job, bridge, host, {
    boot(info) {
      bootInfo = info;
      document.title = `${info.title} — amayui renderer`;
      scheduleDiagnostics();
    },
  });
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
    bridge.report({ kind: 'log', level: 'info', text: `Amayui CN 已加载（Regular + Bold，document.fonts.check=${document.fonts.check('24px "' + GAME_FONT_FAMILY + '"')}）` });
    return true;
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    try {
      bridge.report({ kind: 'log', level: 'warn', text: `Amayui CN 字体加载失败 ⇒ 文字会退回 sans-serif（${message}）` });
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

  // ★ 主窗口只留 canvas：diagnostics 面板搬到独立的 debug 窗口（`?debug=1`）。
  diagnosticsRoot.hidden = true;

  // ① 先拿 bridge（没有 preload 就装 mock），再挂 start handler；早到的 start 会排队。
  bridge = 'amayui' in window ? window.amayui : createMockBridge();
  bridge.onStart(startJob);

  // ② 先加载统一字体，再建 Pixi（Pixi 的 Text 会缓存首次量字时的字体度量）。
  const fontLoaded = await loadGameFont();
  if (!fontLoaded) {
    try {
      bridge.report({ kind: 'log', level: 'warn', text: 'Amayui CN 未加载：所有文字会使用 sans-serif 兜底（这不是规格行为）' });
    } catch {
      /* ignore */
    }
  }

  // ③ 建 Pixi。失败 ⇒ 不 ready（协议：失败时 renderer 不 ready）+ 全屏错误。
  try {
    host = await PixiRenderHost.create(hostElement, {
      log: (text) => {
        try {
          bridge.report({ kind: 'log', level: 'info', text });
        } catch {
          /* 同上：日志不能影响渲染 */
        }
      },
    });
  } catch (error) {
    const message = error instanceof Error ? `${error.message}\n${error.stack ?? ''}` : String(error);
    showFatal(`Pixi / WebGL 初始化失败：\n${message}`);
    try {
      bridge.report({ kind: 'log', level: 'error', text: `Pixi 初始化失败（不会 ready）：${message}` });
    } catch {
      /* ignore */
    }
    return;
  }

  // ④ 放行排队中的 start（顺序 = 到达顺序）
  if (pendingJob !== null) {
    const job = pendingJob;
    pendingJob = null;
    startJob(job);
  }
  scheduleDiagnostics();

  // ⑤ 就绪回报：只调一次
  bridge.ready(host.readyInfo());
  scheduleDiagnostics();
  // game 窗口不画 diagnostics；低频把投影送给 debug 窗口（100ms 节流在 scheduleDiagnostics 里）。
  setInterval(() => scheduleDiagnostics(), 200);
}

/* ═══════════════════════════ debug 窗口（只读诊断） ═══════════════════════════ */

function setupDebugDom(): void {
  diagnosticsRoot = mustGetById('diagnostics');
  diagnosticsBody = mustGetById('diagnostics-body');
  diagnosticsSummary = mustGetById('diagnostics-summary');
  fatalBox = mustGetById('fatal');
  fatalText = mustGetById('fatal-text');
  diagnosticsRoot.hidden = false;
  const stage = document.getElementById('stage-host');
  if (stage !== null) stage.hidden = true;
  document.body.dataset.debug = '1';
  document.title = 'amayui debug — 只读诊断';

  const toggle = mustGetById('diagnostics-toggle');
  toggle.addEventListener('click', () => {
    if (!diagnosticsRoot) return;
    const collapsed = diagnosticsRoot.classList.toggle('is-collapsed');
    toggle.textContent = collapsed ? '展开' : '折叠';
  });
}

function handleDebugEvent(event: DebugEvent): void {
  switch (event.kind) {
    case 'diagnostics':
      lastDiagnostics = event.data as HostDiagnostics;
      break;
    case 'boot':
      bootInfo = event.info;
      document.title = `amayui debug — ${event.info.title}`;
      break;
    case 'status':
      lastStatus = { level: event.level, text: event.text };
      statusCount += 1;
      break;
    case 'log':
      debugLogs.push(`[${event.level}] ${event.text}`);
      if (debugLogs.length > 100) debugLogs.shift();
      break;
  }
  scheduleDiagnostics();
}

async function debugMain(): Promise<void> {
  debugMode = true;
  setupDebugDom();
  bridge = 'amayui' in window ? window.amayui : createMockBridge();
  bridge.onDebug(handleDebugEvent);
  renderDiagnostics();
  requestAnimationFrame(() => renderDiagnostics());
}

const isDebugWindow = new URLSearchParams(window.location.search).has('debug');
if (isDebugWindow) void debugMain();
else void main();

/* ═══════════════════════════ 手工验证用的 mock（⛔ 不是引擎观测） ═══════════════════════════ */

/**
 * 没有 `window.amayui` 时（浏览器里直接打开 `dist/renderer/index.html`）用的最小 bridge。
 * ★ 它只发一条 **demo** 启动指令，让 core 的真实 handlers + Pixi 走一遍；
 *   没有文件系统、没有安装目录，数字全部是 demo 合成配置。
 * ★ ⛔ 不许把它的输出当引擎观测 / spec 结论。
 */
function createMockBridge(): AmayuiBridge {
  const startHandlers: ((job: StartJob) => void)[] = [];
  let started = false;
  const job: StartJob = {
    mode: 'demo',
    configKeys: { defaults: [], ini: [], persist: [] },
    platform: { sep: '/', homeDir: '', env: { APPDATA: null, XDG_CONFIG_HOME: null } },
    smoke: false,
  };

  return {
    ready: () => {
      if (started) return;
      started = true;
      queueMicrotask(() => {
        for (const handler of startHandlers) handler(job);
      });
    },
    onStart: (handler) => {
      startHandlers.push(handler);
      if (started) queueMicrotask(() => handler(job));
    },
    onWindowMessage: () => {
      /* mock 没有窗口消息 */
    },
    onDebug: () => {
      /* mock 没有 game renderer 的诊断投影 */
    },
    invoke: async (method) => {
      switch (method) {
        case 'boot.frameMetrics':
          return { frameWidth: 0, frameHeight: 0, captionHeight: 0, menuHeight: 0 };
        case 'boot.screenSize':
          return { width: window.innerWidth, height: window.innerHeight };
        case 'boot.registerWindowClass':
        case 'boot.createWindow':
          return 1;
        case 'boot.getMenu':
          return null;
        default:
          return undefined;
      }
    },
    report: (event) => {
      if (event.kind === 'log') console.info('[amayui mock]', event.text);
      else if (event.kind === 'done' && !event.ok) console.error('[amayui mock]', event.error);
    },
  };
}
