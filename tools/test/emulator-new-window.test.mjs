/** @env pure @kind contract @why 启动期窗口 / 图形设备 / 事件入口写错了不会抛：注册失败会静默继续、ForceScreen 不写回、外框尺寸算错、图形失败被吞掉、定时器 / 泵 / 退出异常的次序颠倒 —— 每一条都让"窗口建出来、设备就绪、能正常退出"变成无法验证的话 */
/**
 * tools/test/emulator-new-window.test.mjs —— `06-window.md` 的 **pure** 契约守卫（`apps/emulator-new` 核心）。
 *
 * 被测：`apps/emulator-new/src/core/window.ts`（平台无关核心）+ 假宿主（`FakeHost`）。
 * ★ 假宿主把**每个平台请求**记进 `host.calls`；核心自己也留痕（`session.trace`）⇒ 两侧独立对账：
 *   "核心以为自己发了"与"宿主真的收到"必须同时成立。
 * ★ 判据为什么是这些（都会红，不是复述实现）：
 *   - 类名 / 风格 / 光标 / 背景刷 / 菜单号 / 形态风格 / 位置哨兵 / 定时器周期 —— 全是 spec 常量，
 *     写错一位就是 spec 错（`06-window.md` 的表格）；
 *   - 窗口尺寸 = 客户区 + **2 × 边框宽**（宽）/ **2 × 边框高 + 标题高 + 菜单高**（高）：
 *     四类外框量是**单边厚度**、引擎纯手算（没有 `AdjustWindowRect`）—— 少乘 2、少加标题 / 菜单、
 *     或让平台自动换算，尺寸就不同；
 *   - `ForceScreen == 1` ⇒ S=1 **且** `display:ScreenMode` 被写 1（先于建窗）；
 *     缺键 ⇒ S=0 占满屏（缺键不是 1、不是 2）；
 *   - `set:CreateObject` 的两个位**独立**（键 3 ⇒ 旧式与三维都跑，旧式在前；2 / 1 只跑一条；0 都不跑）；
 *   - 旧式实参由配置推导（行为位 = UseDirectDrawEmuration |（形态字 ≠ 0 ⇒ 2）、FullScreenBit 独立）；
 *   - 两条图形路线失败 ⇒ 结束启动（⛔ 静默吞掉就是这条测试红），日志类别按逐步骤表；
 *   - 销毁 ⇒ 隐藏 → PostQuitMessage(0) → 泵抛 EngineExit（链断在任何一环都红）；
 *   - 关闭 ⇒ 引擎收尾 → 杀周期定时器 → 销毁窗口（次序颠倒就红）；
 *   - 被忽略项必须显式（`disposition: 'ignored'` + 规则 id + 非空原因），且**不许**把必做请求标成 ignored。
 *
 * ★ 不读游戏安装：键词表来自 `data/spec/format/config-keys.json`，配置用合成默认表 + 显式运行期键。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  ADAPTER_INDEX,
  D3D9_SDK_VERSION,
  DEFAULT_WINDOW_POSITION,
  EXECUTION_STATE_KEEP_AWAKE,
  EngineExit,
  FULLSCREEN_EX_STYLE,
  FULLSCREEN_STYLE,
  GRAPHICS_INIT_ERROR_TEXT,
  HOVER_TIMER_ID,
  HOVER_TIMER_PERIOD_MS,
  MENU_CATEGORY_INSTALL,
  PERIODIC_TIMER_ID,
  PERIODIC_TIMER_PERIOD_MS,
  RULE_EVENT_LOOP,
  RULE_GRAPHICS_INIT,
  RULE_WINDOW_ATTACH,
  RULE_WINDOW_CLASS,
  RULE_WINDOW_CREATE,
  WINDOW_BACKGROUND_BRUSH,
  WINDOW_CLASS_NAME,
  WINDOW_CLASS_STYLE,
  WINDOW_CURSOR_RESOURCE_ID,
  WINDOW_ICON_RESOURCE_NAME,
  WINDOW_MENU_NAME,
  WINDOW_MENU_RESOURCE_ID,
  WINDOW_RULES,
  WINDOWED_EX_STYLE,
  WINDOWED_STYLE,
  bootWindow,
} from '../../apps/emulator-new/src/core/window.ts';
import { ConfigStore } from '../../apps/emulator-new/src/core/config.ts';
import { loadConfigKeys } from '../../apps/emulator-new/src/platform/node/spec.ts';

/** 键词表是只读规范数据：读一次，多个 store 共用 */
const CONFIG_KEYS = await loadConfigKeys();

/** 入口参数（三个量：进程实例 / 辅助模块的资源实例 / 显示方式） */
const ENTRY = { instance: 0x400000, resourceInstance: 0x500000, showMode: 5 };

/** 队列里的"退出消息"：`GetMessage` 取到它 ⇒ 返回 0（引擎级退出） */
const QUIT = Symbol('quit');

/** 假宿主：每个平台请求都进 `calls`（`{ name, args }`），失败分支用选项注入 */
class FakeHost {
  constructor(opts = {}) {
    this.calls = [];
    // 外框四类**单边厚度**（左右 / 上下对称；引擎自己乘 2 或加一次）
    this.frame = opts.frame ?? { frameWidth: 3, frameHeight: 5, captionHeight: 19, menuHeight: 20 };
    this.screen = opts.screen ?? { width: 1280, height: 720 };
    this.classAtom = opts.classAtom === undefined ? 11 : opts.classAtom;
    this.windowHandle = opts.windowHandle === undefined ? 100 : opts.windowHandle;
    this.d3dHandle = opts.d3dHandle === undefined ? 7 : opts.d3dHandle;
    this.mode = opts.mode === undefined ? { width: 1920, height: 1080, format: 22 } : opts.mode;
    this.deviceHandle = opts.deviceHandle === undefined ? 8 : opts.deviceHandle;
    this.caps = opts.caps ?? { maxTextureWidth: 4096, maxTextureHeight: 4096 };
    this.format32 = opts.format32 === undefined ? true : opts.format32;
    this.drawHandle = opts.drawHandle === undefined ? 9 : opts.drawHandle;
    this.foreground = opts.foreground === undefined ? true : opts.foreground;
    this.menuHandle = opts.menuHandle === undefined ? 501 : opts.menuHandle;
    this.queue = [...(opts.queue ?? [])];
  }

  record(name, args) {
    this.calls.push({ name, args });
  }

  names() {
    return this.calls.map((c) => c.name);
  }

  argsOf(name) {
    const hit = this.calls.find((c) => c.name === name);
    assert.ok(hit, `假宿主没收到 ${name}（实际：${this.names().join(', ')}）`);
    return hit.args;
  }

  frameMetrics() {
    this.record('frameMetrics');
    return { ...this.frame };
  }

  screenSize() {
    this.record('screenSize');
    return { ...this.screen };
  }

  registerWindowClass(params) {
    this.record('registerWindowClass', params);
    return this.classAtom;
  }

  createWindow(params) {
    this.record('createWindow', params);
    return this.windowHandle;
  }

  showWindow(window, showMode) {
    this.record('showWindow', { window, showMode });
  }

  updateWindow(window) {
    this.record('updateWindow', { window });
  }

  associateInputContext(window, context) {
    this.record('associateInputContext', { window, context });
  }

  getMenu(window) {
    this.record('getMenu', { window });
    return this.menuHandle;
  }

  operateMenu(params) {
    this.record('operateMenu', params);
  }

  drawMenuBar(window) {
    this.record('drawMenuBar', { window });
  }

  isForegroundWindow(window) {
    this.record('isForegroundWindow', { window });
    return this.foreground;
  }

  hideWindow(window) {
    this.record('hideWindow', { window });
  }

  destroyWindow(window) {
    this.record('destroyWindow', { window });
  }

  postQuitMessage(exitCode) {
    this.record('postQuitMessage', { exitCode });
  }

  postWindowMessage(window, message) {
    this.record('postWindowMessage', { window, message });
  }

  setThreadExecutionState(flags) {
    this.record('setThreadExecutionState', { flags });
  }

  setTimer(window, params) {
    this.record('setTimer', { window, ...params });
  }

  killTimer(window, id) {
    this.record('killTimer', { window, id });
  }

  peekMessage(params) {
    this.record('peekMessage', params);
    return this.queue.length > 0;
  }

  getMessage() {
    this.record('getMessage');
    const next = this.queue.shift();
    return next === QUIT ? null : next;
  }

  translateMessage(message) {
    this.record('translateMessage', { message: message.kind });
  }

  async createLegacyDraw(params) {
    this.record('createLegacyDraw', params);
    return this.drawHandle;
  }

  async createD3d9(params) {
    this.record('createD3d9', params);
    return this.d3dHandle;
  }

  async queryAdapterDisplayMode(params) {
    this.record('queryAdapterDisplayMode', params);
    return this.mode;
  }

  async createDevice(params) {
    this.record('createDevice', params);
    return this.deviceHandle;
  }

  async setDialogBoxMode(params) {
    this.record('setDialogBoxMode', params);
  }

  async queryDeviceCaps(params) {
    this.record('queryDeviceCaps', params);
    return this.caps;
  }

  async checkTextureFormat32(params) {
    this.record('checkTextureFormat32', params);
    return this.format32;
  }

  async clearRenderTarget(params) {
    this.record('clearRenderTarget', params);
  }

  async createDrawObject(params) {
    this.record('createDrawObject', params);
    return this.drawHandle;
  }
}

/** 发行默认表的合成样本（键名走真词表：写错键 ⇒ `config-unknown-key` 当场红） */
const WINDOWED_DEFAULTS = [
  { key: 'SCREENX', value: '1024' },
  { key: 'SCREENY', value: '768' },
  { key: 'TEXWIDTH', value: '512' },
  { key: 'TEXHEIGHT', value: '256' },
  { key: 'CREATEOBJECT', value: '2' },
  { key: 'SAVEWINPOS', value: '0' },
  { key: 'FULLSCREENBIT', value: '32' },
  { key: 'FULLSCREENWIDTH', value: '1600' },
  { key: 'FULLSCREENHEIGHT', value: '900' },
  { key: 'FORCESCREEN', value: '0' },
];

function makeConfig(defaults = WINDOWED_DEFAULTS) {
  const store = new ConfigStore(CONFIG_KEYS);
  store.applyDefaults(defaults);
  // 运行期命名键（不在发行默认表里）：设备建成之后读的两个绘制对象参数
  store.setInt('display:TextureBit', 32);
  store.setInt('display:PixelShader', 3);
  return store;
}

/**
 * 一次启动。`screenMode === null` ⇒ 不写 `display:ScreenMode`（缺键 ⇒ 0）。
 * @returns {{ host: FakeHost, config: ConfigStore, session: object }}
 */
async function boot(opts = {}) {
  const host = opts.host ?? new FakeHost(opts.hostOpts);
  const config = opts.config ?? makeConfig(opts.defaults);
  if (opts.screenMode !== null) config.setInt('display:ScreenMode', opts.screenMode ?? 2);
  const session = await bootWindow({ host, config, entry: { ...ENTRY }, ...(opts.extra ?? {}) });
  return { host, config, session };
}

const traceNames = (session) => session.trace.map((r) => r.request);

/* ────────────────────────────── 内建常量（spec 表格逐格） ────────────────────────────── */

test('★ 内建常量：类名 / 类风格 / 图标 / 光标 / 背景刷 / 菜单名 / 两套形态风格 / 位置哨兵 / 定时器', async () => {
  assert.equal(WINDOW_CLASS_NAME, 'ARCGameEngine');
  assert.equal(WINDOW_CLASS_STYLE, 0x40);
  assert.equal(WINDOW_ICON_RESOURCE_NAME, 'IDI_ICON1');
  assert.equal(WINDOW_CURSOR_RESOURCE_ID, 0x7f00);
  assert.equal(WINDOW_BACKGROUND_BRUSH, 4);
  assert.equal(WINDOW_MENU_NAME, 0);
  assert.equal(WINDOW_MENU_RESOURCE_ID, 110);
  assert.equal(WINDOWED_STYLE, 0x90cb0000);
  assert.equal(WINDOWED_EX_STYLE, 0);
  assert.equal(FULLSCREEN_STYLE, 0x90000000);
  assert.equal(FULLSCREEN_EX_STYLE, 0x8);
  assert.equal(DEFAULT_WINDOW_POSITION, 0x80000000);
  assert.equal(PERIODIC_TIMER_ID, 2);
  assert.equal(PERIODIC_TIMER_PERIOD_MS, 10000);
  assert.equal(HOVER_TIMER_ID, 1);
  assert.equal(HOVER_TIMER_PERIOD_MS, 100);
  assert.equal(EXECUTION_STATE_KEEP_AWAKE, 3);
  assert.equal(D3D9_SDK_VERSION, 32);
  assert.equal(ADAPTER_INDEX, 0);
  assert.equal(MENU_CATEGORY_INSTALL, 1);
  assert.equal(GRAPHICS_INIT_ERROR_TEXT.length > 0, true);
});

/* ────────────────────────────── 窗口态 + 三维设备（主路径） ────────────────────────────── */

test('★★ 窗口态 + 三维设备：请求次序 / 类参数 / 窗口尺寸 / 窗口位置哨兵 / 菜单 / 图形四步', async () => {
  const { host, session } = await boot();

  // 核心自己的留痕（一侧）
  assert.deepEqual(traceNames(session), [
    'register-window-class',
    'measure-window-frame',
    'create-window',
    'show-window',
    'update-window',
    'associate-input-context',
    'get-menu',
    'operate-menu',
    'draw-menu-bar',
    'record-service-window',
    'create-d3d9',
    'query-adapter-display-mode',
    'create-device-attempt',
    'create-device',
    'set-dialog-box-mode',
    'query-device-caps',
    'check-texture-format-32',
    'clear-render-target',
    'create-draw-object',
    'set-thread-execution-state',
  ]);
  // 宿主收到的请求（另一侧，独立对账）
  assert.deepEqual(host.names(), [
    'registerWindowClass',
    'frameMetrics',
    'createWindow',
    'showWindow',
    'updateWindow',
    'associateInputContext',
    'getMenu',
    'operateMenu',
    'drawMenuBar',
    'createD3d9',
    'queryAdapterDisplayMode',
    'createDevice',
    'setDialogBoxMode',
    'queryDeviceCaps',
    'checkTextureFormat32',
    'clearRenderTarget',
    'createDrawObject',
    'setThreadExecutionState',
  ]);
  assert.equal(session.failure, null);
  assert.equal(session.screenMode, 2);
  assert.equal(session.trace.every((r) => WINDOW_RULES.includes(r.rule)), true, '每条留痕都得挂在一条 06-window 规则上');

  // 窗口类：类名 / 实例 / 图标 / 光标 / 背景刷 / 菜单名 / 风格 / 附加
  const cls = host.argsOf('registerWindowClass');
  assert.equal(cls.className, 'ARCGameEngine');
  assert.equal(cls.classStyle, 0x40);
  assert.equal(cls.instance, ENTRY.instance, '类的实例 = 入口给的进程实例');
  assert.equal(cls.iconResourceName, 'IDI_ICON1');
  assert.equal(cls.resourceInstance, ENTRY.resourceInstance, '图标 / 菜单来自辅助模块的资源实例（与进程实例不同）');
  assert.equal(cls.cursorResourceId, 0x7f00);
  assert.equal(cls.backgroundBrush, 4);
  assert.equal(cls.menuName, 0);
  assert.equal(cls.classExtra, 0);
  assert.equal(cls.windowExtra, 0);

  // 窗口：客户区 1024×768；外框单边厚度 = 边框宽 3 / 边框高 5 / 标题 19 / 菜单 20（引擎纯手算）
  const win = host.argsOf('createWindow');
  assert.equal(win.className, 'ARCGameEngine');
  assert.equal(win.instance, ENTRY.instance);
  assert.equal(win.clientWidth, 1024);
  assert.equal(win.clientHeight, 768);
  assert.equal(win.width, 1024 + 2 * 3, '窗口宽 = 客户区宽 + 2 × 边框宽（没有 AdjustWindowRect）');
  assert.equal(win.height, 768 + 2 * 5 + 19 + 20, '窗口高 = 客户区高 + 2 × 边框高 + 标题高 + 菜单高');
  assert.equal(win.x, DEFAULT_WINDOW_POSITION, 'set:SaveWinPos = 0 ⇒ 平台默认位置哨兵');
  assert.equal(win.y, DEFAULT_WINDOW_POSITION);
  assert.equal(win.style, 0x90cb0000);
  assert.equal(win.exStyle, 0);
  assert.equal(win.menuResourceId, 110, '窗口态菜单资源号 110');
  assert.equal(win.parent, null);
  assert.equal(win.param, null);

  // 显示 / 重绘：显示方式原样转发（返回值不检查）
  assert.equal(host.argsOf('showWindow').showMode, ENTRY.showMode);
  assert.equal(host.argsOf('showWindow').window, 100);
  assert.equal(host.argsOf('updateWindow').window, 100);

  // 附加：解绑输入法（显式 null = 传 0）→ 记录服务窗口槽 → 装菜单（类别 1）+ 重画
  assert.equal(host.argsOf('associateInputContext').context, null);
  assert.equal(host.argsOf('getMenu').window, 100);
  assert.deepEqual(host.argsOf('operateMenu'), { category: 1, window: 100, menu: 501, extra: 0 });
  assert.equal(host.argsOf('drawMenuBar').window, 100);
  assert.equal(session.serviceWindow, 100);
  assert.equal(session.window, 100);
  // 次序：解绑 → GetMenu → 操作菜单 → 重画 → **最后**写服务窗口槽
  const attach = traceNames(session).filter((r) => [
    'associate-input-context', 'get-menu', 'operate-menu', 'draw-menu-bar', 'record-service-window',
  ].includes(r));
  assert.deepEqual(attach, [
    'associate-input-context', 'get-menu', 'operate-menu', 'draw-menu-bar', 'record-service-window',
  ]);

  // 三维路线：D3D9（SDK 32）→ 适配器 0 → 设备（参数全由配置键推导）→ 设备建成后的四步 → 绘制对象
  assert.equal(host.argsOf('createD3d9').sdkVersion, 32);
  assert.equal(host.argsOf('queryAdapterDisplayMode').adapter, 0);
  const device = host.argsOf('createDevice');
  assert.equal(device.screenMode, 2);
  assert.equal(device.backBufferWidth, 1024, '窗口化 ⇒ 后缓冲宽 = 客户区宽（不是 display:FullScreenWidth）');
  assert.equal(device.backBufferHeight, 768);
  assert.equal(device.adapterFormat, 22);
  assert.equal(device.presentationInterval, 0, 'display:PresentInterval 缺键 ⇒ 读 0 ⇒ 立即');
  assert.equal(device.multisample.type, 0);
  assert.equal(device.swapEffect, 'copy', '窗口化且无多重采样 ⇒ 交换效果 = 复制');
  assert.equal(device.backBufferLockable, true);
  // ★ 纹理位深 / 像素着色器版本**不是**设备创建请求的参数（spec 明确），它们出现在绘制对象上
  assert.equal('textureBitDepth' in device, false, '⛔ 纹理位深不许进设备创建请求');
  assert.equal('pixelShaderVersion' in device, false, '⛔ 像素着色器版本不许进设备创建请求');
  const draw = host.argsOf('createDrawObject');
  assert.equal(draw.textureWidth, 512, '纹理宽来自 set:TexWidth');
  assert.equal(draw.textureHeight, 256);
  assert.equal(draw.textureBitDepth, 32, '纹理位深来自 display:TextureBit');
  assert.equal(draw.pixelShader, 3, '像素着色器版本来自 display:PixelShader');
  assert.equal(session.textureBitDepth, 32);
  assert.deepEqual(host.argsOf('setDialogBoxMode'), { device: 8, enabled: true });
  assert.deepEqual(host.argsOf('queryDeviceCaps'), { device: 8 });
  assert.deepEqual(host.argsOf('checkTextureFormat32'), { device: 8 });
  assert.deepEqual(host.argsOf('clearRenderTarget'), { device: 8 });
  assert.equal(session.drawObject, 9);
  assert.equal(session.device, 8);
  assert.equal(session.graphicsRoute, 'three-dimensional');

  // 启动期在设备之后、根脚本之前直接请求过一次空闲抑制
  const exec = session.trace.filter((r) => r.request === 'set-thread-execution-state');
  assert.equal(exec.length, 1);
  assert.deepEqual(exec[0].args, { flags: 3, reason: 'startup-after-graphics' });
  assert.equal(host.calls.at(-1).name, 'setThreadExecutionState', '空闲抑制在图形设备之后');
  assert.equal(session.hasPeriodicTimer, false, '周期定时器要等 beginMainLoop（根脚本之后）');
  // 启动期没有 SetMenu：菜单是建窗的 hMenu 实参；菜单相关的请求只有这三条
  assert.deepEqual(
    traceNames(session).filter((r) => r.includes('menu')),
    ['get-menu', 'operate-menu', 'draw-menu-bar'],
    '菜单只经 GetMenu / 辅助入口 / DrawMenuBar —— ⛔ 不许出现 SetMenu 一类补装请求',
  );
});

/* ────────────────────────────── 形态字：ForceScreen 与缺键 ────────────────────────────── */

test('★★ display:ForceScreen == 1：S = 1，且先于建窗把 display:ScreenMode 写成 1', async () => {
  const defaults = WINDOWED_DEFAULTS.map((row) => (row.key === 'FORCESCREEN' ? { key: 'FORCESCREEN', value: '1' } : row));
  const { config, session, host } = await boot({ defaults, screenMode: 0 });

  assert.equal(config.getInt('display:ScreenMode'), 1, '写回发生在配置库（不是只改局部变量）');
  assert.equal(session.screenMode, 1);
  const trace = traceNames(session);
  const atWrite = trace.indexOf('write-config');
  assert.ok(atWrite >= 0, '必须有一条引擎级配置写入');
  assert.deepEqual(session.trace[atWrite].args, { key: 'display:ScreenMode', value: 1 });
  assert.ok(atWrite < trace.indexOf('create-window'), '配置写入先于建窗');
  // S=1 ⇒ 窗口态（客户区来自 set:WinX / set:WinY）
  assert.equal(host.argsOf('createWindow').style, 0x90cb0000);
  assert.equal(host.argsOf('createWindow').width, 1024 + 2 * 3);
});

test('★ display:ForceScreen != 1（这里是 2）不触发写回：S 仍取 display:ScreenMode', async () => {
  // ★ 默认表里的 FORCESCREEN 先归一成 0/1（`55-config.md`），所以"非 1"只能经运行期写入口造出来
  const config = makeConfig();
  config.setInt('display:ScreenMode', 0);
  config.setInt('display:ForceScreen', 2);
  const { session } = await boot({ config, screenMode: null });
  assert.equal(config.getInt('display:ScreenMode'), 0, '非 1 的 ForceScreen 不写配置');
  assert.equal(config.getInt('display:ForceScreen'), 2);
  assert.equal(session.screenMode, 0);
});

test('★★ S = 0（缺键 0 与显式 0 都一样）：占满屏态 —— 屏幕尺寸 / (0,0) / 另一套风格 / 无菜单 / 不装菜单', async () => {
  const missing = await boot({ screenMode: null });
  assert.equal(missing.session.screenMode, 0, '缺键按配置库规则读成 0');
  assert.equal(missing.host.argsOf('createWindow').width, 1280);
  assert.equal(missing.host.argsOf('createWindow').height, 720);

  const { host, session } = await boot({ screenMode: 0 });
  const win = host.argsOf('createWindow');
  assert.equal(win.x, 0);
  assert.equal(win.y, 0);
  assert.equal(win.width, 1280, '占满屏尺寸 = 平台报告的屏幕宽');
  assert.equal(win.height, 720);
  assert.equal(win.clientWidth, 1280);
  assert.equal(win.style, 0x90000000);
  assert.equal(win.exStyle, 0x8);
  assert.equal(win.menuResourceId, null, '占满屏态没有菜单');
  assert.equal(host.names().includes('frameMetrics'), false, '占满屏态不查外框尺寸');
  assert.equal(host.names().includes('getMenu'), false, '占满屏态不取菜单');
  assert.equal(host.names().includes('operateMenu'), false, '占满屏态不安装菜单');
  assert.equal(host.names().includes('drawMenuBar'), false);
  assert.equal(host.names().includes('associateInputContext'), true, '解绑输入法在所有形态下都做');
  assert.equal(session.serviceWindow, 100, '占满屏态仍然记录服务窗口槽');
  assert.equal(traceNames(session).includes('record-service-window'), true);
});

/* ────────────────────────────── 位置与客户区（不编默认值） ────────────────────────────── */

test('★★ set:SaveWinPos != 0：位置取运行期命名键；客户区为 0 就照传 0（⛔ 不替引擎编分辨率）', async () => {
  const defaults = WINDOWED_DEFAULTS.map((row) => (row.key === 'SAVEWINPOS'
    ? { key: 'SAVEWINPOS', value: '1' }
    : row.key === 'SCREENX'
      ? { key: 'SCREENX', value: '0' }
      : row.key === 'SCREENY'
        ? { key: 'SCREENY', value: '0' }
        : row));
  const config = makeConfig(defaults);
  config.setInt('set:WinPosLeft', 40);
  config.setInt('set:WinPosTop', -5);
  const { host } = await boot({ config, screenMode: 2 });
  const win = host.argsOf('createWindow');
  assert.equal(win.x, 40);
  assert.equal(win.y, -5);
  assert.equal(win.clientWidth, 0, '客户区 0 就是 0（宿主决定，核心不编默认分辨率）');
  assert.equal(win.clientHeight, 0);
  assert.equal(win.width, 0 + 2 * 3);
  assert.equal(win.height, 0 + 2 * 5 + 19 + 20);
});

/* ────────────────────────────── 失败 = 共享出口 ────────────────────────────── */

test('★★ 窗口类注册失败 ⇒ 共享出口：只发注册请求、不建窗、不显示消息、不装载后续', async () => {
  const { host, session } = await boot({ hostOpts: { classAtom: null } });
  assert.deepEqual(host.names(), ['registerWindowClass'], '注册之后就结束');
  assert.equal(session.failure.rule, RULE_WINDOW_CLASS);
  assert.equal(session.failure.reason, 'class-register-failed');
  assert.equal(session.failure.step, null);
  assert.equal(session.failure.errorText, null, '共享出口没有用户可见输出');
  assert.equal(session.failure.sharedExit, true);
  assert.equal(session.windowClass, null);
  assert.equal(session.window, null);
  assert.equal(session.drawObject, null);
  assert.deepEqual(session.errors, [], '类注册失败不写错误通道');
  assert.throws(() => session.beginMainLoop(), /window-session-failed/);
  assert.throws(() => session.handleMessage({ kind: 'close' }), /window-session-not-ready/);
});

test('★★ 建窗失败 ⇒ 共享出口：不显示 / 不重绘、不解绑输入法、不初始化图形、不显示消息', async () => {
  const { host, session } = await boot({ hostOpts: { windowHandle: null } });
  assert.deepEqual(host.names(), ['registerWindowClass', 'frameMetrics', 'createWindow']);
  assert.equal(session.failure.rule, RULE_WINDOW_CREATE);
  assert.equal(session.failure.reason, 'window-create-failed');
  assert.equal(session.failure.errorText, null);
  assert.equal(session.window, null);
  assert.equal(session.serviceWindow, null);
  assert.equal(session.drawObject, null);
  assert.deepEqual(session.errors, []);
  assert.throws(() => session.beginMainLoop(), /window-session-failed/);
  assert.throws(() => session.pump(), /window-session-failed/);
});

/* ────────────────────────────── 图形路线：两个独立位 ────────────────────────────── */

test('★★ set:CreateObject 的两个位独立：3 ⇒ 旧式与三维都跑（旧式在前）；2 / 1 ⇒ 只跑一条；0 ⇒ 都不跑', async () => {
  const configWith = (value) => makeConfig(WINDOWED_DEFAULTS.map((row) => (row.key === 'CREATEOBJECT'
    ? { key: 'CREATEOBJECT', value }
    : row)));
  const both = await boot({ config: configWith('3'), screenMode: 2 });
  assert.deepEqual(both.session.graphicsRoutes, ['legacy-draw', 'three-dimensional'], '键 = 3 ⇒ 两条路线都跑');
  assert.equal(both.session.graphicsRoute, 'legacy-draw', '兼容字段 = 第一条实际跑的路线');
  assert.equal(both.host.names().includes('createLegacyDraw'), true);
  assert.equal(both.host.names().includes('createD3d9'), true);
  assert.ok(
    both.host.names().indexOf('createLegacyDraw') < both.host.names().indexOf('createD3d9'),
    '旧式段在前（spec 表次序）',
  );

  const threeConfig = makeConfig(WINDOWED_DEFAULTS.map((row) => (row.key === 'CREATEOBJECT'
    ? { key: 'CREATEOBJECT', value: '2' }
    : row)));
  const three = await boot({ config: threeConfig, screenMode: 2 });
  assert.deepEqual(three.session.graphicsRoutes, ['three-dimensional']);
  assert.equal(three.host.names().includes('createD3d9'), true);
  assert.equal(three.host.names().includes('createLegacyDraw'), false);

  const legacyConfig = makeConfig(WINDOWED_DEFAULTS.map((row) => (row.key === 'CREATEOBJECT'
    ? { key: 'CREATEOBJECT', value: '1' }
    : row)));
  const legacy = await boot({ config: legacyConfig, screenMode: 2 });
  assert.deepEqual(legacy.session.graphicsRoutes, ['legacy-draw']);
  assert.equal(legacy.host.names().includes('createLegacyDraw'), true);
  assert.equal(legacy.host.names().includes('createD3d9'), false, '位 1 = 0 ⇒ 不走三维段');
  assert.equal(legacy.session.device, null, '旧式路线没有三维设备对象');

  const noneConfig = makeConfig(WINDOWED_DEFAULTS.map((row) => (row.key === 'CREATEOBJECT'
    ? { key: 'CREATEOBJECT', value: '0' }
    : row)));
  const none = await boot({ config: noneConfig, screenMode: 2 });
  assert.deepEqual(none.session.graphicsRoutes, [], '两个位都为 0 ⇒ 两条都不跑（不是"默认跑三维"）');
  assert.equal(none.session.graphicsRoute, null);
});

test('★★ 旧式绘制参数：行为位 = UseDirectDrawEmuration | (形态字≠0 ? 2 : 0)；FullScreenBit 独立；AlwaysBackupSurface 不是实参', async () => {
  const legacyConfig = makeConfig(WINDOWED_DEFAULTS.map((row) => (row.key === 'CREATEOBJECT'
    ? { key: 'CREATEOBJECT', value: '1' }
    : row)));
  const { host } = await boot({ config: legacyConfig, screenMode: 2 });
  const params = host.argsOf('createLegacyDraw');
  assert.equal(params.window, 100);
  assert.equal(params.clientWidth, 1024, '客户区宽来自引擎级缓存（set:WinX）');
  assert.equal(params.clientHeight, 768);
  assert.equal(params.behaviorBits, 2, '形态字 ≠ 0 ⇒ 行为位带 2（UseDirectDrawEmuration 缺键 = 0）');
  assert.equal(params.fullscreenBit, 32, 'display:FullScreenBit 原值直传（发行默认 32）');
  assert.deepEqual(
    Object.keys(params).sort(),
    ['behaviorBits', 'clientHeight', 'clientWidth', 'fullscreenBit', 'window'],
    '⛔ set:AlwaysBackupSurface 不是这条请求的实参',
  );
});

test('★★ 两条图形路线失败 ⇒ 结束启动（⛔ 不静默）；日志类别按逐步骤表', async () => {
  const legacyConfig = makeConfig(WINDOWED_DEFAULTS.map((row) => (row.key === 'CREATEOBJECT'
    ? { key: 'CREATEOBJECT', value: '1' }
    : row)));
  const legacy = await boot({ config: legacyConfig, screenMode: 2, hostOpts: { drawHandle: null } });
  assert.equal(legacy.session.failure.rule, RULE_GRAPHICS_INIT);
  assert.equal(legacy.session.failure.reason, 'graphics-init-failed');
  assert.equal(legacy.session.failure.step, 'create-legacy-draw');
  assert.equal(legacy.session.failure.errorText, GRAPHICS_INIT_ERROR_TEXT);
  assert.deepEqual(legacy.session.errors, [], '旧式路线不写日志通道（文本直接送用户 ⇒ failure.errorText）');
  assert.equal(legacy.session.drawObject, null);
  assert.equal(legacy.host.calls.at(-1).name, 'createLegacyDraw', '失败后不再发空闲抑制请求');
  assert.equal(legacy.session.hasPeriodicTimer, false, '不建主循环');
  assert.throws(() => legacy.session.beginMainLoop(), /graphics-init-failed/);

  // 三维路线四个失败点：前三步写错误类文本；"上层绘制对象"按 spec 不写日志（只给用户可见文本）
  const cases = [
    { hostOpts: { d3dHandle: null }, step: 'create-d3d9', last: 'createD3d9', logged: true },
    { hostOpts: { mode: null }, step: 'query-adapter-display-mode', last: 'queryAdapterDisplayMode', logged: true },
    { hostOpts: { deviceHandle: null }, step: 'create-device', last: 'createDevice', logged: true },
    { hostOpts: { drawHandle: null }, step: 'create-draw-object', last: 'createDrawObject', logged: false },
  ];
  for (const c of cases) {
    const { host, session } = await boot({ screenMode: 2, hostOpts: c.hostOpts });
    assert.equal(session.failure.rule, RULE_GRAPHICS_INIT, c.step);
    assert.equal(session.failure.step, c.step);
    assert.equal(session.failure.errorText, GRAPHICS_INIT_ERROR_TEXT, `用户可见文本（${c.step}）`);
    assert.deepEqual(
      session.errors,
      c.logged ? [GRAPHICS_INIT_ERROR_TEXT] : [],
      c.logged ? `错误通道必须有文本（${c.step}）` : `该步不写日志通道（${c.step}）`,
    );
    assert.equal(host.calls.at(-1).name, c.last, `失败点 ${c.step}`);
    assert.equal(session.drawObject, null);
    assert.equal(session.hasPeriodicTimer, false);
  }
});

/* ────────────────────────────── 周期定时器 + 泵 ────────────────────────────── */

test('★★ beginMainLoop：建周期定时器（标识 2 / 10000ms / 无回调），重复调用不重发', async () => {
  const { host, session } = await boot({ screenMode: 2 });
  session.beginMainLoop();
  const timer = host.argsOf('setTimer');
  assert.deepEqual(timer, { window: 100, id: 2, periodMs: 10000, callback: null });
  assert.equal(session.hasPeriodicTimer, true);
  session.beginMainLoop();
  const timers = host.calls.filter((c) => c.name === 'setTimer');
  assert.equal(timers.length, 1, '已在效就不重复建');
  assert.equal(session.trace.filter((r) => r.request === 'set-timer').length, 1);
});

test('★★ 泵：窥视（不移除）→ 取一条 → 翻译 → 派发 → 回到窥视，直到空', async () => {
  const { host, session } = await boot({ screenMode: 2 });
  session.beginMainLoop();
  host.calls.length = 0;
  session.trace.length = 0;
  host.queue.push({ kind: 'activate', active: true });

  const dispatched = session.pump();
  assert.equal(dispatched, 1);
  assert.deepEqual(host.names(), ['peekMessage', 'getMessage', 'translateMessage', 'peekMessage']);
  assert.deepEqual(host.calls[0].args, { remove: false }, '窥视不移除');
  assert.deepEqual(host.calls[3].args, { remove: false }, '取完一条回到窥视');
  assert.deepEqual(traceNames(session), [
    'peek-message', 'get-message', 'translate-message', 'dispatch-message', 'engine-activate', 'peek-message',
  ]);
  assert.deepEqual(session.trace.at(-1).args, { remove: false });
  assert.equal(session.trace[3].via, 'engine', 'DispatchMessage 调的就是本层的事件入口');
  assert.deepEqual(session.trace[3].args, { message: 'activate' });
  assert.equal(session.pump(), 0, '队列空 ⇒ 本趟派发 0 条');
});

test('★★ GetMessage 返回 0（退出消息）⇒ 泵抛 EngineExit（已证的唯一结束路径）', async () => {
  const { host, session } = await boot({ screenMode: 2 });
  session.beginMainLoop();
  host.queue.push(QUIT);
  let thrown = null;
  try {
    session.pump();
  } catch (err) {
    thrown = err;
  }
  assert.ok(thrown instanceof EngineExit, `应抛 EngineExit，实际 ${thrown}`);
  assert.equal(thrown.name, 'EngineExit');
  assert.match(thrown.message, /退出消息/);
  assert.equal(host.names().includes('translateMessage'), false, '退出消息不翻译、不派发');
  assert.equal(host.names().includes('getMessage'), true);
});

test('★★ 销毁消息：先隐藏，再 PostQuitMessage(0)；随后泵抛 EngineExit（退出链闭合）', async () => {
  const { host, session } = await boot({ screenMode: 2 });
  session.beginMainLoop();
  host.calls.length = 0;
  session.handleMessage({ kind: 'destroy' });
  assert.deepEqual(host.names(), ['hideWindow', 'postQuitMessage']);
  assert.deepEqual(host.argsOf('postQuitMessage'), { exitCode: 0 });

  host.queue.push(QUIT);
  assert.throws(() => session.pump(), EngineExit);
});

test('★★ 关闭消息：引擎收尾 → 杀周期定时器（标识 2）→ 销毁窗口（次序不许颠倒）', async () => {
  const order = [];
  const host = new FakeHost();
  host.record = (name, args) => {
    order.push(name);
    host.calls.push({ name, args });
  };
  const { session } = await boot({
    host,
    screenMode: 2,
    extra: { engineTeardown: () => host.record('engineTeardown') },
  });
  session.beginMainLoop();
  order.length = 0;
  session.handleMessage({ kind: 'close' });
  assert.deepEqual(order, ['engineTeardown', 'killTimer', 'destroyWindow']);
  assert.deepEqual(host.argsOf('killTimer'), { window: 100, id: 2 });
  assert.equal(session.hasPeriodicTimer, false);
});

/* ────────────────────────────── 悬停定时器 ────────────────────────────── */

test('★★ 鼠标在客户区内 ⇒ 建悬停定时器（标识 1 / 100ms）；离开或按下键 ⇒ 杀掉', async () => {
  const { host, session } = await boot({ screenMode: 2 });
  host.calls.length = 0;

  session.handleMessage({ kind: 'mousemove', inClient: true });
  assert.deepEqual(host.argsOf('setTimer'), { window: 100, id: 1, periodMs: 100, callback: null });
  assert.equal(session.hasHoverTimer, true);

  session.handleMessage({ kind: 'mousemove', inClient: true });
  assert.equal(host.calls.filter((c) => c.name === 'setTimer').length, 1, '已在效不重复建');

  session.handleMessage({ kind: 'mouseleave' });
  assert.deepEqual(host.argsOf('killTimer'), { window: 100, id: 1 });
  assert.equal(session.hasHoverTimer, false);

  session.handleMessage({ kind: 'mousemove', inClient: true });
  session.handleMessage({ kind: 'mousedown' });
  assert.equal(host.calls.filter((c) => c.name === 'killTimer').length, 2);
  assert.equal(session.hasHoverTimer, false);

  session.handleMessage({ kind: 'mousemove', inClient: true });
  session.handleMessage({ kind: 'mousemove', inClient: false });
  assert.equal(session.hasHoverTimer, false, '光标移出客户区也算离开');

  // 离开但没有定时器 ⇒ 不发 kill（避免无意义的 KillTimer）
  host.calls.length = 0;
  session.handleMessage({ kind: 'mouseleave' });
  assert.equal(host.names().includes('killTimer'), false);
});

/* ────────────────────────────── 周期定时器到期 ────────────────────────────── */

test('★★ 周期定时器到期：先刷新空闲抑制；三合一判据成立才补发应用激活', async () => {
  // ① 引擎判据成立 + 前台 ⇒ 补发
  const yes = await boot({ screenMode: 2, extra: { timerActivationCondition: () => true } });
  yes.session.beginMainLoop();
  yes.host.calls.length = 0;
  yes.session.handleMessage({ kind: 'timer', timerId: 2 });
  assert.deepEqual(yes.host.names(), ['setThreadExecutionState', 'isForegroundWindow', 'postWindowMessage']);
  assert.deepEqual(yes.host.argsOf('setThreadExecutionState'), { flags: 3 });
  assert.deepEqual(yes.host.argsOf('postWindowMessage'), { window: 100, message: 'activate-app' });

  // ② 引擎判据成立但窗口不是前台 ⇒ 不补发
  const notForeground = await boot({ screenMode: 2, extra: { timerActivationCondition: () => true }, hostOpts: { foreground: false } });
  notForeground.session.beginMainLoop();
  notForeground.host.calls.length = 0;
  notForeground.session.handleMessage({ kind: 'timer', timerId: 2 });
  assert.deepEqual(notForeground.host.names(), ['setThreadExecutionState', 'isForegroundWindow']);
  assert.equal(notForeground.session.trace.some((r) => r.request === 'post-window-message'), false);

  // ③ 引擎判据不成立 ⇒ 连前台查询都不做
  const no = await boot({ screenMode: 2 });
  no.session.beginMainLoop();
  no.host.calls.length = 0;
  no.session.handleMessage({ kind: 'timer', timerId: 2 });
  assert.deepEqual(no.host.names(), ['setThreadExecutionState']);
});

test('★ 悬停定时器到期 / 未知定时器标识：不静默 —— 到期只留痕，未知标识响亮失败', async () => {
  const { host, session } = await boot({ screenMode: 2 });
  host.calls.length = 0;
  session.handleMessage({ kind: 'timer', timerId: 1 });
  assert.deepEqual(host.names(), [], '悬停定时器到期不发请求（行为本版不规定）');
  assert.equal(session.trace.at(-1).request, 'hover-timer-expiry');
  assert.throws(() => session.handleMessage({ kind: 'timer', timerId: 9 }), /window-message-unknown-timer:9/);
});

/* ────────────────────────────── 显式忽略 ────────────────────────────── */

test('★★ 被忽略项必须显式：规则 id 合法 / 原因非空 / 指到残差；必做请求不许标成 ignored', async () => {
  const { session } = await boot({ screenMode: 2 });
  const entries = session.ignored;
  assert.ok(entries.length >= 10, `忽略项太少（实际 ${entries.length}）—— 静默跳过会让这条测试失去意义`);

  for (const e of entries) {
    assert.equal(e.disposition, 'ignored');
    assert.ok(WINDOW_RULES.includes(e.rule), `忽略项的规则 id 非法：${e.rule}`);
    assert.ok(e.what.length > 0, '忽略项必须点名');
    assert.ok(e.reason.length > 10, `忽略项 ${e.what} 的原因太短（⛔ 不许空话）`);
    if (e.residual !== undefined) assert.match(e.residual, /^RES-BOOT-[A-Z0-9-]+$/, `残差 id 形状：${e.residual}`);
  }

  const keys = new Set(entries.map((e) => `${e.rule}/${e.what}`));
  for (const want of [
    `${RULE_WINDOW_CLASS}/boot-preamble`,
    `${RULE_WINDOW_CLASS}/icon-fallback`,
    `${RULE_WINDOW_CREATE}/window-title`,
    `${RULE_WINDOW_CREATE}/decoration-limit`,
    `${RULE_WINDOW_CREATE}/windowed-client-vs-screen`,
    `${RULE_WINDOW_ATTACH}/ime-rebind`,
    `${RULE_WINDOW_ATTACH}/menu-content`,
    `${RULE_GRAPHICS_INIT}/device-type-not-distinguishable`,
    `${RULE_GRAPHICS_INIT}/fullscreen-present-combination`,
    `${RULE_GRAPHICS_INIT}/resolution-enumeration`,
    `${RULE_GRAPHICS_INIT}/error-text`,
    `${RULE_EVENT_LOOP}/timer-activation-condition`,
    `${RULE_EVENT_LOOP}/pump-second-site`,
    `${RULE_EVENT_LOOP}/window-proc-other-messages`,
    `${RULE_EVENT_LOOP}/hover-timer-expiry`,
    `${RULE_EVENT_LOOP}/close-cleanup-items`,
    `${RULE_EVENT_LOOP}/loop-return-paths`,
  ]) {
    assert.ok(keys.has(want), `缺显式忽略标记：${want}`);
  }

  // ★ 必做请求必须真的发出去，且不许出现在 ignored 里（"忘了做"与"声明不做"必须分开）
  const required = [
    'register-window-class', 'measure-window-frame', 'create-window', 'show-window', 'update-window',
    'associate-input-context', 'record-service-window', 'operate-menu', 'draw-menu-bar',
    'create-d3d9', 'query-adapter-display-mode', 'create-device',
    'set-dialog-box-mode', 'query-device-caps', 'check-texture-format-32', 'clear-render-target',
    'create-draw-object',
  ];
  const requests = new Set(traceNames(session));
  for (const req of required) assert.ok(requests.has(req), `缺必做请求：${req}`);
  for (const e of entries) assert.equal(required.includes(e.what), false, `必做请求被标成 ignored：${e.what}`);

  // 每个失败的自洽：有 failure 就没有窗口 / 图形对象
  assert.equal(session.failure, null);
});
