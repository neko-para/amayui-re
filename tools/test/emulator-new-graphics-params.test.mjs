/** @env pure @kind contract @why 设备创建请求的实参一旦由"调用方注入"而不是由配置键推导，PresentInterval 就会透传 3、虚拟全屏不会让呈现变窗口化、纹理位深会读错键、后缓冲尺寸三个分支会退化成同一个 —— 每一条都让"设备按 spec 建出来"变成无法验证的话 */
/**
 * tools/test/emulator-new-graphics-params.test.mjs —— `06-window.md`「设备创建请求的实参」+「后缓冲尺寸」的
 * **pure** 守卫（`apps/emulator-new` 核心）。
 *
 * 被测：`apps/emulator-new/src/core/window.ts` 的三维设备路线。
 * ★ 判据为什么是这些（都会红，不是复述实现）：
 *   - `display:PresentInterval` 是**映射**不是透传（3 ⇒ 4、4 ⇒ 8；其它 ⇒ 平台默认）；
 *   - 是否全屏呈现 = NOT（形态字 S ≠ 0 OR 虚拟全屏 ≠ 0）——只看 `screenMode` 会漏掉虚拟全屏；
 *   - 后缓冲尺寸三条互斥分支（窗口化 / 虚拟全屏类型 1 / 其余全屏）——合并成一条就会把 1024×768
 *     的窗口态当成 1920×1080 的桌面；
 *   - 纹理位深读 `display:TextureBit`，⛔ 不是 `display:FullScreenBit`（两者取不同值就能证伪）；
 *   - 多重采样启用 ⇒ 交换效果 = 丢弃、后缓冲可锁定 = 关；
 *   - 设备类型 / 顶点处理取 0 时按 spec 的**尝试序列**真重试（设备类型外层、顶点处理内层、新设备类型重置
 *     顶点处理；0/0 六次、单键 2 / 3 次、穷尽才失败）；
 *   - 旧式路线实参：行为位 = UseDirectDrawEmuration |（形态字 ≠ 0 ⇒ 2）、FullScreenBit 独立、不看虚拟全屏；
 *   - 路线选择是**两个独立位**（键 3 ⇒ 旧式与三维都跑，旧式在前）；
 *   - 设备建成之后四步（对话框 → 能力 → 32 位格式 → 清目标）的次序与三类后果
 *     （对话框失败 = 错误文本但继续 / 格式不可用 = 只警告 / 能力不足 = 致命）；
 *   - 分辨率校验只在 VFS = 0 分支做：**请求对整对覆盖**（⛔ 逐分量回退）+ 真实判据（`pickResolution`：
 *     尺寸闸 / 0.7 两池 / 七符号 / 池只留最小一档 / 收口两支不对称）+ 写回 / **失败不写回** + 失败致命；
 *   - 日志落点（`system:OutErrorLog` + 宿主 `openErrorLog`）区分"记录了"与"送出去了"（落点无效 ⇒ 静默丢弃，
 *     ⛔ 不改变任何判定）；
 *   - 宿主缺少**可选**方法 ⇒ 诊断 / 忽略清单里有痕迹（⛔ 不静默）。
 *
 * ★ 不读游戏安装：键词表来自 `data/spec/format/config-keys.json`，配置用合成默认表 + 显式运行期键。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';

import {
  ERROR_LOG_FILE_NAME,
  ERROR_LOG_MAX_PATH_BYTES,
  GRAPHICS_DIALOG_BOX_MODE_ERROR_TEXT,
  GRAPHICS_INIT_ERROR_TEXT,
  GRAPHICS_RESOLUTION_REJECTED_TEXT,
  GRAPHICS_STEP_OUTCOME,
  RESOLUTION_ASPECT_THRESHOLD,
  RULE_GRAPHICS_INIT,
  TEXTURE_FORMAT_32_WARNING_TEXT,
  bootWindow,
  deviceCreationAttempts,
  joinEnginePath,
  legacyBehaviorBitsOf,
  pickResolution,
} from '../../apps/emulator-new/src/core/window.ts';
import { ConfigStore } from '../../apps/emulator-new/src/core/config.ts';
import { loadConfigKeys } from '../../apps/emulator-new/src/platform/node/spec.ts';

const CONFIG_KEYS = await loadConfigKeys();
const ENTRY = { instance: 0x400000, resourceInstance: 0x500000, showMode: 5 };

/** 发行默认表的合成样本（键名走真词表：写错键 ⇒ `config-unknown-key` 当场红） */
const DEFAULTS = [
  { key: 'SCREENX', value: '1024' },
  { key: 'SCREENY', value: '768' },
  { key: 'TEXWIDTH', value: '512' },
  { key: 'TEXHEIGHT', value: '256' },
  { key: 'CREATEOBJECT', value: '2' },
  { key: 'SAVEWINPOS', value: '0' },
  { key: 'FORCESCREEN', value: '0' },
];

function makeConfig(defaults = DEFAULTS) {
  const store = new ConfigStore(CONFIG_KEYS);
  store.applyDefaults(defaults);
  // 设备建成之后读的两个键（不在发行默认表里，属运行期命名键）
  store.setInt('display:TextureBit', 32);
  store.setInt('display:PixelShader', 3);
  return store;
}

/**
 * 假宿主：实现完整 `BootHost`（启动期真正会被调用的那几个请求至少全部覆盖）。
 * `postDevice: false` ⇒ 四个**可选**的"设备建成之后"方法故意缺席（own 属性 = undefined，
 * 遮住原型方法）⇒ 核心必须记 `session.diagnostics`。
 */
class GraphicsFakeHost {
  constructor(opts = {}) {
    this.calls = [];
    this.frame = { frameWidth: 3, frameHeight: 5, captionHeight: 19, menuHeight: 20 };
    this.screen = opts.screen ?? { width: 1280, height: 720 };
    this.mode = opts.mode ?? { width: 1920, height: 1080, format: 22 };
    this.deviceHandle = opts.deviceHandle === undefined ? 8 : opts.deviceHandle;
    this.caps = opts.caps ?? { maxTextureWidth: 4096, maxTextureHeight: 4096 };
    this.format32 = opts.format32 === undefined ? true : opts.format32;
    // ★ 失败注入：头 N 次 `createDevice` 返回 null（`null` 本来就是产品级失败通道）
    this.deviceFailures = opts.deviceFailures ?? 0;
    this.deviceCalls = 0;
    this.legacyHandle = opts.legacyHandle === undefined ? null : opts.legacyHandle;
    this.dialogBoxModeResult = opts.dialogBoxModeResult;
    this.resizeModes = opts.modes ?? null;
    if (this.resizeModes !== null) {
      // 可选方法：只有给了模式清单时才存在（缺席 ⇒ 核心必须显式记账）
      this.enumerateAdapterDisplayModes = async (params) => {
        this.record('enumerateAdapterDisplayModes', params);
        return this.resizeModes;
      };
    }
    // 日志落点（spec「日志通道的落点与丢弃」）：可选方法，缺省可用（真打开 ⇒ body 由核心决定用不用）
    this.errorLogOpens = opts.errorLogOpens === undefined ? true : opts.errorLogOpens;
    this.openErrorLogCalls = 0;
    if (opts.openErrorLog === false) {
      this.openErrorLog = undefined;
    } else {
      this.openErrorLog = async (spec) => {
        this.openErrorLogCalls += 1;
        this.record('openErrorLog', spec);
        return this.errorLogOpens;
      };
    }
    // 配置根（spec：`set:UseAppDataFolder` **恰为 1** 时问一次宿主）。缺省可用；`false` ⇒ 故意缺席。
    this.appData = typeof opts.appDataDir === 'string' ? opts.appDataDir : 'C:\\Users\\u\\AppData\\Local';
    this.appDataDirCalls = 0;
    if (opts.appDataDir === false) {
      this.appDataDir = undefined;
    } else {
      this.appDataDir = async () => {
        this.appDataDirCalls += 1;
        this.record('appDataDir');
        return this.appData;
      };
    }
    if (opts.postDevice === false) {
      this.setDialogBoxMode = undefined;
      this.queryDeviceCaps = undefined;
      this.checkTextureFormat32 = undefined;
      this.clearRenderTarget = undefined;
    }
  }

  record(name, args) {
    this.calls.push({ name, args });
  }

  names() {
    return this.calls.map((c) => c.name);
  }

  argsOf(name) {
    const hit = this.calls.find((c) => c.name === name);
    assert.ok(hit, `假宿主没收到 ${name}（实际：${this.names().join(', ') || '(无)'}）`);
    return hit.args;
  }

  /* WindowHost */

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
    return 11;
  }

  createWindow(params) {
    this.record('createWindow', params);
    return 100;
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
    return 501;
  }

  operateMenu(params) {
    this.record('operateMenu', params);
  }

  drawMenuBar(window) {
    this.record('drawMenuBar', { window });
  }

  isForegroundWindow(window) {
    this.record('isForegroundWindow', { window });
    return true;
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

  /* MessageHost（启动期不泵消息，但接口要完整） */

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
    return false;
  }

  getMessage() {
    this.record('getMessage');
    return null;
  }

  translateMessage(message) {
    this.record('translateMessage', { message: message.kind });
  }

  /* GraphicsHost */

  async createLegacyDraw(params) {
    this.record('createLegacyDraw', params);
    return this.legacyHandle;
  }

  async createD3d9(params) {
    this.record('createD3d9', params);
    return 7;
  }

  async queryAdapterDisplayMode(params) {
    this.record('queryAdapterDisplayMode', params);
    return this.mode;
  }

  async createDevice(params) {
    this.record('createDevice', params);
    this.deviceCalls += 1;
    if (this.deviceCalls <= this.deviceFailures) return null;
    return this.deviceHandle;
  }

  async setDialogBoxMode(params) {
    this.record('setDialogBoxMode', params);
    return this.dialogBoxModeResult;
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
    return 9;
  }
}

/**
 * 一次启动。`display` 是运行期命名键（`display:*`）的覆盖；`screenMode` 缺省 2（窗口态）。
 * @returns {{ host: GraphicsFakeHost, config: ConfigStore, session: object }}
 */
async function bootWith(display = {}, opts = {}) {
  const config = makeConfig(opts.defaults);
  config.setInt('display:ScreenMode', opts.screenMode === undefined ? 2 : opts.screenMode);
  for (const [key, value] of Object.entries(display)) config.setInt(key, value);
  // 字符串型配置键（`set:RegFilePath` 之类）只能走 setString
  for (const [key, value] of Object.entries(opts.strings ?? {})) config.setString(key, String(value));
  const host = opts.host ?? new GraphicsFakeHost(opts.hostOpts);
  const session = await bootWindow({ host, config, entry: { ...ENTRY }, ...(opts.extra ?? {}) });
  return { host, config, session };
}

/* ────────────────────────── 呈现间隔：映射，不是透传 ────────────────────────── */

test('★★ display:PresentInterval 是映射：3 ⇒ 4、4 ⇒ 8、0 ⇒ 立即；其它 ⇒ 平台默认；缺键 ⇒ 0', async () => {
  const table = [
    [0, 0],
    [1, 1],
    [2, 2],
    [3, 4],
    [4, 8],
  ];
  for (const [configured, expected] of table) {
    const { host } = await bootWith({ 'display:PresentInterval': configured });
    assert.equal(
      host.argsOf('createDevice').presentationInterval,
      expected,
      `display:PresentInterval = ${configured} ⇒ 请求里的 ${expected}（⛔ 不是透传原值）`,
    );
  }

  // 其它值（5 及以上 / 负数）⇒ 平台默认 ⇒ 用 null 表示（配置原值 3 若被透传就红）
  for (const other of [5, 7, -1]) {
    const { host } = await bootWith({ 'display:PresentInterval': other });
    assert.equal(
      host.argsOf('createDevice').presentationInterval,
      null,
      `display:PresentInterval = ${other} ⇒ 平台默认（null）`,
    );
  }

  // 键缺 ⇒ 配置库读 0 ⇒ 立即
  const { host } = await bootWith();
  assert.equal(host.argsOf('createDevice').presentationInterval, 0, '键缺 ⇒ 读 0 ⇒ 立即呈现');
});

/* ────────────────────────── 是否全屏呈现 ────────────────────────── */

test('★★ fullscreenPresent：S=0 + 虚拟全屏=1 ⇒ false（窗口化）；S≠0 ⇒ false；S=0 且 VFS=0 ⇒ true', async () => {
  const virtual = await bootWith({ 'display:VirtualFullScreen': 1 }, { screenMode: 0 });
  assert.equal(virtual.session.screenMode, 0);
  assert.equal(
    virtual.host.argsOf('createDevice').fullscreenPresent,
    false,
    '虚拟全屏非 0 ⇒ 一律窗口化（与形态字 S 无关）',
  );
  assert.equal(virtual.session.fullscreenPresent, false);

  const windowed = await bootWith({}, { screenMode: 2 });
  assert.equal(windowed.host.argsOf('createDevice').fullscreenPresent, false, 'S ≠ 0 ⇒ 窗口化');

  const fullscreen = await bootWith({}, { screenMode: 0 });
  assert.equal(fullscreen.host.argsOf('createDevice').fullscreenPresent, true, 'S = 0 且 VFS = 0 ⇒ 全屏呈现');
  assert.equal(fullscreen.session.fullscreenPresent, true);
});

/* ────────────────────────── 后缓冲尺寸：三分支 ────────────────────────── */

test('★★ 后缓冲尺寸 1/3：窗口化（S ≠ 0）⇒ 客户区宽 / 高（⛔ 不是 display:FullScreenWidth）', async () => {
  const { host } = await bootWith(
    { 'display:FullScreenWidth': 1600, 'display:FullScreenHeight': 900 },
    { screenMode: 2 },
  );
  const device = host.argsOf('createDevice');
  assert.equal(device.backBufferWidth, 1024, '窗口化 ⇒ 客户区宽（set:WinX）');
  assert.equal(device.backBufferHeight, 768, '窗口化 ⇒ 客户区高（set:WinY）');
});

test('★★ 后缓冲尺寸 2/3：S=0 + 虚拟全屏非 0 + 类型 1 ⇒ 桌面（适配器显示模式）', async () => {
  const { host } = await bootWith(
    {
      'display:VirtualFullScreen': 1,
      'display:VirtualFullScreenType': 1,
      'display:FullScreenWidth': 1600,
      'display:FullScreenHeight': 900,
    },
    { screenMode: 0, hostOpts: { mode: { width: 1920, height: 1080, format: 22 } } },
  );
  const device = host.argsOf('createDevice');
  assert.equal(device.backBufferWidth, 1920, '虚拟全屏类型 1 ⇒ 桌面宽（显示模式）');
  assert.equal(device.backBufferHeight, 1080, '虚拟全屏类型 1 ⇒ 桌面高（显示模式）');
});

test('★★ 后缓冲尺寸 3/3：其余全屏 ⇒ display:FullScreenWidth/Height（取 0 ⇒ 客户区）', async () => {
  const configured = await bootWith(
    { 'display:FullScreenWidth': 1600, 'display:FullScreenHeight': 900 },
    { screenMode: 0 },
  );
  assert.equal(configured.host.argsOf('createDevice').backBufferWidth, 1600);
  assert.equal(configured.host.argsOf('createDevice').backBufferHeight, 900);

  // 类型不是 1（这里是 2）⇒ 不取桌面，仍走"其余全屏"
  const otherType = await bootWith(
    { 'display:VirtualFullScreen': 1, 'display:VirtualFullScreenType': 2, 'display:FullScreenWidth': 1600, 'display:FullScreenHeight': 900 },
    { screenMode: 0 },
  );
  assert.equal(otherType.host.argsOf('createDevice').backBufferWidth, 1600, '类型 ≠ 1 ⇒ 全屏宽来自配置');

  // 全屏 + 配置为 0 ⇒ 退回客户区（占满屏态的客户区缓存 = 屏幕尺寸）
  const fallback = await bootWith({}, { screenMode: 0, hostOpts: { screen: { width: 1280, height: 720 } } });
  assert.equal(fallback.host.argsOf('createDevice').backBufferWidth, 1280, '全屏宽 = 0 ⇒ 客户区宽');
  assert.equal(fallback.host.argsOf('createDevice').backBufferHeight, 720, '全屏高 = 0 ⇒ 客户区高');
});

/* ────────────────────────── 纹理位深：读对键 ────────────────────────── */

test('★★ 纹理位深读 display:TextureBit（不是 display:FullScreenBit），且不是设备创建请求的参数', async () => {
  const textureBit = await bootWith({ 'display:TextureBit': 32, 'display:FullScreenBit': 16 });
  assert.equal(textureBit.session.textureBitDepth, 32, 'textureBitDepth 来自 display:TextureBit');
  assert.equal(textureBit.host.argsOf('createDrawObject').textureBitDepth, 32);
  assert.equal('textureBitDepth' in textureBit.host.argsOf('createDevice'), false,
    '⛔ 纹理位深不是设备创建请求的参数');

  // 反向：两个键对调 ⇒ 结果跟着 display:TextureBit 走（读错键的实现会得到 32）
  const swapped = await bootWith({ 'display:TextureBit': 16, 'display:FullScreenBit': 32 });
  assert.equal(swapped.host.argsOf('createDrawObject').textureBitDepth, 16,
    '纹理位深跟着 display:TextureBit，不跟 display:FullScreenBit');
});

/* ────────────────────────── 多重采样 ⇒ 交换效果 / 可锁定 ────────────────────────── */

test('★★ display:MultiSample 启用 ⇒ 交换效果 = 丢弃、后缓冲可锁定 = 关；未启用按窗口化 / 全屏取', async () => {
  const enabled = await bootWith({ 'display:MultiSample': 2 }, { screenMode: 2 });
  const device = enabled.host.argsOf('createDevice');
  assert.equal(device.multisample.type, 2, '类型原值来自 display:MultiSample');
  assert.equal(device.swapEffect, 'discard', '多重采样启用 ⇒ 强制丢弃');
  assert.equal(device.backBufferLockable, false, '多重采样启用 ⇒ 后缓冲可锁定 = 关');

  const windowed = await bootWith({}, { screenMode: 2 });
  assert.equal(windowed.host.argsOf('createDevice').swapEffect, 'copy', '窗口化 ⇒ 复制');
  assert.equal(windowed.host.argsOf('createDevice').backBufferLockable, true);

  const fullscreen = await bootWith({}, { screenMode: 0 });
  assert.equal(fullscreen.host.argsOf('createDevice').swapEffect, 'discard', '全屏 ⇒ 丢弃');
  assert.equal(fullscreen.host.argsOf('createDevice').backBufferLockable, true);
});

/* ────────────────────────── 设备类型 / 顶点处理：映射 ────────────────────────── */

test('★ display:DeviceType / display:VertexProcessing 按 spec 表映射（值不为 0 ⇒ 只尝试一次）', async () => {
  const deviceCases = [
    [1, 'hal'],
    [2, 'reference'],
    [3, 'reference'],
  ];
  for (const [configured, expected] of deviceCases) {
    const { host } = await bootWith({ 'display:DeviceType': configured, 'display:VertexProcessing': 1 });
    assert.equal(host.argsOf('createDevice').deviceType, expected, `display:DeviceType = ${configured}`);
  }

  const vertexCases = [
    [1, 'hardware'],
    [2, 'software'],
    [3, 'mixed'],
  ];
  for (const [configured, expected] of vertexCases) {
    const { host } = await bootWith({ 'display:DeviceType': 1, 'display:VertexProcessing': configured });
    assert.equal(host.argsOf('createDevice').vertexProcessing, expected, `display:VertexProcessing = ${configured}`);
  }
});

/* ────────────────────────── 设备创建重试序列 ────────────────────────── */

test('★★ 重试 0/0 ⇒ 6 次：(1,1)→(1,2)→(1,3)→(2,1)→(2,2)→(2,3)；设备类型外层、每个新设备类型把顶点处理重置为 1', async () => {
  const expected = [
    ['hal', 'hardware'],
    ['hal', 'software'],
    ['hal', 'mixed'],
    ['reference', 'hardware'],
    ['reference', 'software'],
    ['reference', 'mixed'],
  ];
  // 假宿主头 5 次返回 null、第 6 次成功：`null` 就是产品级失败通道，无需专门的失败注入 API。
  const { host, session } = await bootWith(
    { 'display:DeviceType': 0, 'display:VertexProcessing': 0 },
    { hostOpts: { deviceFailures: 5 } },
  );
  const calls = host.calls.filter((c) => c.name === 'createDevice');
  assert.deepEqual(
    calls.map((c) => [c.args.deviceType, c.args.vertexProcessing]),
    expected,
    '序列必须是设备类型外层 / 顶点处理内层，且每个新设备类型的顶点处理从 1 重起（⛔ 不是 vp 外层）',
  );
  assert.equal(session.failure, null, '第 6 次成功即停');
  assert.equal(session.device, 8);

  // 呈现参数只在循环外构造一次：每次尝试里除设备类型 / 顶点处理外的字段必须完全一样
  const presentationFields = calls.map((c) => JSON.stringify({
    presentationInterval: c.args.presentationInterval,
    backBufferWidth: c.args.backBufferWidth,
    backBufferHeight: c.args.backBufferHeight,
    multisample: c.args.multisample,
    swapEffect: c.args.swapEffect,
    backBufferLockable: c.args.backBufferLockable,
    fullscreenPresent: c.args.fullscreenPresent,
  }));
  assert.equal(new Set(presentationFields).size, 1, '呈现参数不随尝试重建（只换设备类型 / 顶点处理）');

  // 纯函数：与启动层同一条序列表
  assert.deepEqual(
    deviceCreationAttempts(0, 0),
    expected.map(([deviceType, vertexProcessing]) => ({ deviceType, vertexProcessing })),
  );
});

test('★★ 只一个键为 0：只设备类型 = 0 ⇒ 2 次；只顶点处理 = 0 ⇒ 3 次；都不为 0 ⇒ 1 次', async () => {
  const dtOnly = await bootWith(
    { 'display:DeviceType': 0, 'display:VertexProcessing': 2 },
    { hostOpts: { deviceFailures: 9 } },
  );
  assert.deepEqual(
    dtOnly.host.calls.filter((c) => c.name === 'createDevice')
      .map((c) => [c.args.deviceType, c.args.vertexProcessing]),
    [['hal', 'software'], ['reference', 'software']],
    '只设备类型为 0 ⇒ 2 次（顶点处理固定 = 键值 2）',
  );
  assert.equal(dtOnly.session.failure.step, 'create-device', '两次都失败 ⇒ 穷尽');

  const vpOnly = await bootWith(
    { 'display:DeviceType': 1, 'display:VertexProcessing': 0 },
    { hostOpts: { deviceFailures: 9 } },
  );
  assert.deepEqual(
    vpOnly.host.calls.filter((c) => c.name === 'createDevice')
      .map((c) => [c.args.deviceType, c.args.vertexProcessing]),
    [['hal', 'hardware'], ['hal', 'software'], ['hal', 'mixed']],
    '只顶点处理为 0 ⇒ 3 次（设备类型固定 = 键值 1）',
  );

  const neither = await bootWith(
    { 'display:DeviceType': 3, 'display:VertexProcessing': 7 },
    { hostOpts: { deviceFailures: 9 } },
  );
  assert.deepEqual(
    neither.host.calls.filter((c) => c.name === 'createDevice')
      .map((c) => [c.args.deviceType, c.args.vertexProcessing]),
    [['reference', 'mixed']],
    '都不为 0 ⇒ 1 次（键 > 2 按参考、键 > 3 按混合）',
  );
  assert.equal(deviceCreationAttempts(3, 7).length, 1);
});

test('★★ 重试穷尽 ⇒ 失败：写错误类文本 + 结束启动（复用"取显示模式"的文本路径）', async () => {
  const { host, session } = await bootWith(
    { 'display:DeviceType': 0, 'display:VertexProcessing': 0 },
    { hostOpts: { deviceFailures: 99 } },
  );
  assert.equal(host.calls.filter((c) => c.name === 'createDevice').length, 6, '6 次全失败才收口');
  assert.equal(session.failure.rule, RULE_GRAPHICS_INIT);
  assert.equal(session.failure.step, 'create-device');
  assert.deepEqual(session.errors, [GRAPHICS_INIT_ERROR_TEXT], '错误类文本必须留下');
  assert.equal(session.device, null);
  assert.equal(session.drawObject, null);
  assert.equal(host.names().includes('setDialogBoxMode'), false, '失败后不再发后继请求');
});

/* ────────────────────────── 旧式绘制路线的实参 ────────────────────────── */

test('★★ 旧式实参：行为位 = UseDirectDrawEmuration 原值 | (形态字≠0 ? 2 : 0)；FullScreenBit 独立第 4 实参；AlwaysBackupSurface 不是实参', async () => {
  const legacyDefaults = DEFAULTS.map((row) => (row.key === 'CREATEOBJECT' ? { key: 'CREATEOBJECT', value: '1' } : row));

  // 形态字 = 0（占满屏）+ 虚拟全屏 = 1：旧式**不看** VirtualFullScreen ⇒ 行为位不带 2。
  const fullscreen = await bootWith(
    {
      'display:ScreenMode': 0,
      'display:VirtualFullScreen': 1,
      'display:FullScreenBit': 9,
      'display:UseDirectDrawEmuration': 1,
      'set:AlwaysBackupSurface': 1,
    },
    { screenMode: 0, defaults: legacyDefaults, hostOpts: { legacyHandle: 9 } },
  );
  const params = fullscreen.host.argsOf('createLegacyDraw');
  assert.equal(params.behaviorBits, 1, '形态字 0 ⇒ 行为位不带 2（虚拟全屏在旧式路线上没有作用）');
  assert.equal(params.fullscreenBit, 9, 'display:FullScreenBit 原值直传（⛔ 不与任何键做或）');
  assert.equal(params.clientWidth, 1280, '客户区宽来自引擎级缓存（占满屏 = 屏幕宽）');
  assert.equal(params.clientHeight, 720);
  assert.deepEqual(
    Object.keys(params).sort(),
    ['behaviorBits', 'clientHeight', 'clientWidth', 'fullscreenBit', 'window'],
    '⛔ set:AlwaysBackupSurface 不是这条请求的实参',
  );
  assert.equal(fullscreen.host.names().includes('setAlwaysBackupSurface'), false);

  // 形态字 ≠ 0 ⇒ 行为位带 2；原值高位保留（5 | 2 = 7）。
  const windowed = await bootWith(
    { 'display:UseDirectDrawEmuration': 5, 'display:FullScreenBit': 16 },
    { screenMode: 2, defaults: legacyDefaults, hostOpts: { legacyHandle: 9 } },
  );
  const windowedParams = windowed.host.argsOf('createLegacyDraw');
  assert.equal(windowedParams.behaviorBits, 7, '行为位 = 原值 5 | 2（形态字 ≠ 0）');
  assert.equal(windowedParams.fullscreenBit, 16);

  // 纯函数：四个边界
  assert.equal(legacyBehaviorBitsOf(0, 0), 0);
  assert.equal(legacyBehaviorBitsOf(0, 2), 2);
  assert.equal(legacyBehaviorBitsOf(1, 0), 1);
  assert.equal(legacyBehaviorBitsOf(1, 2), 3);
  assert.equal(legacyBehaviorBitsOf(5, 2), 7);
});

/* ────────────────────────── 两个独立位：键 = 3 两条都跑 ────────────────────────── */

test('★★ set:CreateObject 的两个位独立：键 = 3 ⇒ 旧式与三维两条请求都出现（旧式在前）', async () => {
  const bothDefaults = DEFAULTS.map((row) => (row.key === 'CREATEOBJECT' ? { key: 'CREATEOBJECT', value: '3' } : row));
  const { host, session } = await bootWith(
    { 'display:FullScreenBit': 16 },
    { defaults: bothDefaults, hostOpts: { legacyHandle: 9 } },
  );
  assert.deepEqual(session.graphicsRoutes, ['legacy-draw', 'three-dimensional'], '键 = 3 ⇒ 两条路线都跑');
  assert.equal(session.graphicsRoute, 'legacy-draw', '兼容字段 = 第一条实际跑的路线');
  assert.equal(host.names().includes('createLegacyDraw'), true);
  assert.equal(host.names().includes('createD3d9'), true);
  assert.ok(
    host.names().indexOf('createLegacyDraw') < host.names().indexOf('createD3d9'),
    '旧式段在前（spec 表次序）',
  );
  assert.equal(session.failure, null);

  // 旧式失败 ⇒ 三维段根本走不到（失败即结束，⛔ 不是"再试另一条"）
  const legacyFails = await bootWith(
    {},
    { defaults: bothDefaults, hostOpts: { legacyHandle: null } },
  );
  assert.equal(legacyFails.session.failure.step, 'create-legacy-draw');
  assert.equal(legacyFails.host.names().includes('createD3d9'), false, '旧式段失败即结束，不回退到三维段');

  // 键 = 0 ⇒ 两条都不跑（不是"默认跑三维"）
  const noneDefaults = DEFAULTS.map((row) => (row.key === 'CREATEOBJECT' ? { key: 'CREATEOBJECT', value: '0' } : row));
  const none = await bootWith({}, { defaults: noneDefaults });
  assert.deepEqual(none.session.graphicsRoutes, []);
  assert.equal(none.session.graphicsRoute, null);
  assert.equal(none.host.names().includes('createD3d9'), false);
  assert.equal(none.host.names().includes('createLegacyDraw'), false);
});

/* ────────────────────────── 设备参数完全由配置键推导 ────────────────────────── */

test('★★ 设备参数由配置键推导（⛔ 不接受调用方注入）', async () => {
  const { host } = await bootWith(
    { 'display:PresentInterval': 1, 'display:DeviceType': 1, 'display:MultiSample': 0 },
    { screenMode: 2 },
  );
  const device = host.argsOf('createDevice');
  assert.equal(device.presentationInterval, 1, '呈现间隔必须由 display:PresentInterval 推导');
  assert.equal(device.deviceType, 'hal', '设备类型必须由 display:DeviceType 推导');
  assert.equal(device.vertexProcessing, 'hardware', '顶点处理必须由 display:VertexProcessing（缺键 0）推导');
  assert.equal(device.multisample.type, 0, '多重采样必须由 display:MultiSample 推导');
  assert.equal(device.fullscreenPresent, false, '是否全屏呈现必须由形态字 + 虚拟全屏推导');
});

/* ────────────────────────── 设备建成之后的四步 ────────────────────────── */

test('★★ 设备建成之后：对话框模式 → 能力查询 → 32 位格式检查 → 清渲染目标 → 绘制对象', async () => {
  const { host, session } = await bootWith();
  const names = host.names();
  const at = (name) => {
    const index = names.indexOf(name);
    assert.ok(index >= 0, `宿主没收到 ${name}`);
    return index;
  };
  assert.ok(at('createDevice') < at('setDialogBoxMode'), '对话框模式在设备建成之后');
  assert.ok(at('setDialogBoxMode') < at('queryDeviceCaps'), '能力查询在对话框模式之后');
  assert.ok(at('queryDeviceCaps') < at('checkTextureFormat32'), '格式检查在能力查询之后');
  assert.ok(at('checkTextureFormat32') < at('clearRenderTarget'), '清目标在格式检查之后');
  assert.ok(at('clearRenderTarget') < at('createDrawObject'), '绘制对象在清目标之后');

  const trace = session.trace.map((r) => r.request);
  const atTrace = (name) => trace.indexOf(name);
  assert.ok(atTrace('create-device') < atTrace('set-dialog-box-mode'));
  assert.ok(atTrace('set-dialog-box-mode') < atTrace('query-device-caps'));
  assert.ok(atTrace('query-device-caps') < atTrace('check-texture-format-32'));
  assert.ok(atTrace('check-texture-format-32') < atTrace('clear-render-target'));
  assert.ok(atTrace('clear-render-target') < atTrace('create-draw-object'));

  assert.deepEqual(host.argsOf('setDialogBoxMode'), { device: 8, enabled: true }, '参数 = 开');
  assert.deepEqual(host.argsOf('clearRenderTarget'), { device: 8 });
  assert.deepEqual(session.warnings, [], '格式可用时没有警告');
  assert.deepEqual(
    session.diagnostics.filter((d) => !d.startsWith('config-missing-key:')),
    [],
    '四步都成功时没有图形诊断（缺键诊断属日志落点用例，不在这里）',
  );
});

test('★★ 对话框绘制模式失败 ⇒ 写"错误"类文本但**不**中止（唯一"错误文本 + 继续"）', async () => {
  const { host, session } = await bootWith({}, { screenMode: 2, hostOpts: { dialogBoxModeResult: false } });
  assert.equal(session.failure, null, '⛔ 不许因为写了错误文本就结束启动');
  assert.deepEqual(session.errors, [GRAPHICS_DIALOG_BOX_MODE_ERROR_TEXT], '错误类文本必须留下');
  assert.deepEqual(session.warnings, [], '⛔ 不是警告类');
  assert.equal(session.drawObject, 9, '启动继续到绘制对象');
  for (const name of ['queryDeviceCaps', 'checkTextureFormat32', 'clearRenderTarget', 'createDrawObject']) {
    assert.equal(host.names().includes(name), true, `失败之后 ${name} 仍要照常发`);
  }
  assert.deepEqual(
    session.graphicsLog.filter((entry) => entry.step === 'set-dialog-box-mode'),
    [{ category: 'error', step: 'set-dialog-box-mode', text: GRAPHICS_DIALOG_BOX_MODE_ERROR_TEXT }],
    '日志带类别 + 步骤（⛔ 不是自由文本）',
  );
  assert.equal(GRAPHICS_STEP_OUTCOME['set-dialog-box-mode'].log, 'error');
  assert.equal(GRAPHICS_STEP_OUTCOME['set-dialog-box-mode'].fatal, false, '逐步骤表：错误文本 ≠ 致命');
});

test('★★ 纹理能力不足（最大纹理宽 / 高 < set:TexWidth / TexHeight）⇒ 致命：错误通道 + 结束启动', async () => {
  const narrow = await bootWith(
    { 'display:ScreenMode': 2 },
    { screenMode: 2, hostOpts: { caps: { maxTextureWidth: 256, maxTextureHeight: 4096 } } },
  );
  assert.equal(narrow.session.failure.rule, RULE_GRAPHICS_INIT);
  assert.equal(narrow.session.failure.reason, 'graphics-init-failed');
  assert.equal(narrow.session.failure.step, 'query-device-caps', '能力不足是致命失败（step = query-device-caps）');
  assert.deepEqual(narrow.session.errors, [GRAPHICS_INIT_ERROR_TEXT], '错误通道必须有文本');
  assert.equal(narrow.session.device, 8, '设备已建成（失败发生在能力比较）');
  assert.equal(narrow.session.drawObject, null, '不继续建绘制对象');
  assert.equal(narrow.host.names().includes('clearRenderTarget'), false, '失败后不再发后续请求');
  assert.throws(() => narrow.session.beginMainLoop(), /graphics-init-failed/);
  assert.deepEqual(
    narrow.session.graphicsLog.filter((entry) => entry.step === 'query-device-caps'),
    [{ category: 'error', step: 'query-device-caps', text: GRAPHICS_INIT_ERROR_TEXT }],
    '能力不足写的是错误类文本',
  );
  assert.equal(GRAPHICS_STEP_OUTCOME['query-device-caps'].fatal, true);

  const short = await bootWith({}, { screenMode: 2, hostOpts: { caps: { maxTextureWidth: 4096, maxTextureHeight: 128 } } });
  assert.equal(short.session.failure.step, 'query-device-caps', '高度不足同样致命');
});

test('★ 32 位纹理格式不可用 ⇒ 只写一条警告，不结束启动', async () => {
  const { session, host } = await bootWith({}, { screenMode: 2, hostOpts: { format32: false } });
  assert.equal(session.failure, null, '格式不可用不是致命失败');
  assert.deepEqual(session.errors, [], '⛔ 警告不许写进错误通道');
  assert.deepEqual(session.warnings, [TEXTURE_FORMAT_32_WARNING_TEXT], '必须留下一条警告');
  assert.equal(session.drawObject, 9, '启动继续到绘制对象');
  assert.equal(host.names().includes('clearRenderTarget'), true, '后续步骤照常');
  assert.deepEqual(
    session.graphicsLog.filter((entry) => entry.step === 'check-texture-format-32'),
    [{ category: 'warning', step: 'check-texture-format-32', text: TEXTURE_FORMAT_32_WARNING_TEXT }],
    '格式不可用写的是警告类文本',
  );
  assert.equal(GRAPHICS_STEP_OUTCOME['check-texture-format-32'].fatal, false);
});

/* ────────────────────────── 分辨率判据（纯函数 pickResolution） ────────────────────────── */

/** 六档常见分辨率的样例（顺序 = 宿主枚举顺序） */
const SIX_MODES = [
  { width: 800, height: 600, format: 22 },
  { width: 1024, height: 768, format: 22 },
  { width: 1280, height: 720, format: 22 },
  { width: 1366, height: 768, format: 22 },
  { width: 1600, height: 900, format: 22 },
  { width: 1920, height: 1080, format: 22 },
];
const modeText = (modes) => modes.map((m) => `${m.width}x${m.height}`).join(',');

test('★★ pickResolution：六档样例对请求 1280×720 ⇒ 选 1280×720；逐档符号 = × × ○ × ― ―', () => {
  const verdict = pickResolution(SIX_MODES, 1280, 720);
  assert.deepEqual(verdict.picked, { width: 1280, height: 720 });
  assert.deepEqual(
    verdict.perMode.map((m) => m.symbol),
    ['×', '×', '○', '×', '―', '―'],
    '800 / 1024 被尺寸闸淘汰；1280×720 比值相等进宽档池；1366×768 比请求更宽（r < R）⇒ ×；1600×900 / 1920×1080 过闸但被"池只留最小一档"挡下（―）',
  );
  assert.deepEqual(
    verdict.perMode.map((m) => `${m.width}x${m.height}`),
    SIX_MODES.map((m) => `${m.width}x${m.height}`),
    '逐档判定与输入**同序**',
  );
});

test('★★ pickResolution：请求 800×600（R ≥ 0.7）⇒ 窄档池胜出（宽档池在这一支恒空）', () => {
  // ★ spec 的收口第一支写的是"先取宽档池，若窄档池的宽严格更小则改取窄档池"；
  //   但当 R ≥ 0.7 时，宽档池里的模式满足 r < 0.7 ≤ R ⇒ r < R ⇒ 过不了比值闸 ⇒ 宽档池必空。
  //   ⇒ 这一支的**可观测后果**就是"窄档池胜出"；下面钉住它（实现仍逐字保留那个严格更小比较）。
  const narrow = [
    { width: 1280, height: 960, format: 22 }, // r = 0.75 = R，窄档池 ●
    { width: 1366, height: 768, format: 22 }, // r ≈ 0.562 < R ⇒ 宽档池 ×（尺寸闸通过）
  ];
  const verdict = pickResolution(narrow, 800, 600);
  assert.ok(verdict.picked, '必须挑出一档（窄档池胜出）');
  assert.deepEqual(verdict.picked, { width: 1280, height: 960 }, 'R ≥ 0.7 ⇒ 窄档池的档胜出');
  assert.deepEqual(
    verdict.perMode.map((m) => m.symbol),
    ['●', '×'],
    '窄档精确相等记 ●；宽档更宽记 ×（⛔ 不是 ―，它根本没进池）',
  );

  const exactRequest = pickResolution(
    [{ width: 1024, height: 768, format: 22 }, { width: 640, height: 480, format: 22 }],
    800,
    600,
  );
  assert.deepEqual(exactRequest.picked, { width: 1024, height: 768 }, '⛔ 不许"缩小凑合"到比请求小的 640×480');
  assert.deepEqual(exactRequest.perMode.map((m) => m.symbol), ['●', '×']);
});

test('★★ pickResolution：请求 1920×1080 + 只有更小的档 ⇒ 失败（picked = null，⛔ 不缩小凑合）', () => {
  const smaller = SIX_MODES.filter((m) => m.width < 1920);
  const verdict = pickResolution(smaller, 1920, 1080);
  assert.equal(verdict.picked, null, '没有一档过尺寸闸 ⇒ 失败');
  assert.equal(
    verdict.perMode.every((m) => m.symbol === '×'),
    true,
    '全部被尺寸闸淘汰（符号 ×）',
  );
  // 同请求 + 恰好有 1920×1080 ⇒ 比值相等进宽档池并选中（⛔ 不是"大于请求就失败"）
  assert.deepEqual(pickResolution(SIX_MODES, 1920, 1080).picked, { width: 1920, height: 1080 });
});

test('★★ pickResolution：尺寸闸 / 池替换 / 七符号（◆ 逐个符号都有生产点）', () => {
  // 尺寸闸：模式比请求小 ⇒ ×（⛔ 不许"缩小凑合"）
  assert.equal(pickResolution([{ width: 800, height: 600, format: 22 }], 1280, 720).picked, null);
  assert.equal(pickResolution([{ width: 800, height: 600, format: 22 }], 1280, 720).perMode[0].symbol, '×');

  // 池只留最小一档：后出现更小的 4:3 档会**替换**先出现的大档
  const replaced = pickResolution(
    [{ width: 1280, height: 960, format: 22 }, { width: 1024, height: 768, format: 22 }],
    640,
    480,
  );
  assert.deepEqual(replaced.picked, { width: 1024, height: 768 }, '池被更小的一档替换');
  assert.deepEqual(replaced.perMode.map((m) => m.symbol), ['●', '●']);

  // 反向顺序 ⇒ 更大的后到者记 ―，⛔ 不替换（"最小一档"不是"最后一档"）
  const kept = pickResolution(
    [{ width: 1024, height: 768, format: 22 }, { width: 1280, height: 960, format: 22 }],
    640,
    480,
  );
  assert.deepEqual(kept.picked, { width: 1024, height: 768 });
  assert.equal(kept.perMode[1].symbol, '―', '新模式在宽或高上更大 ⇒ 不替换，记 ―');

  // 只有一边变小不算"宽与高都不大于"：1024（宽）≤ 1280 但 1024（高）> 960 ⇒ ⛔ 不替换
  // （把替换条件里的 `||` 写成 `&&` 会在这里出错：它会误以为"更小"而替换掉 1280×960）
  const mixed = pickResolution(
    [{ width: 1280, height: 960, format: 22 }, { width: 1024, height: 1024, format: 22 }],
    640,
    480,
  );
  assert.equal(mixed.perMode[1].symbol, '―', '高变大了 ⇒ 不替换');
  assert.deepEqual(mixed.picked, { width: 1280, height: 960 }, '池里保留原档');

  // 七符号的七个生产点（请求 640×480，R = 0.75；○ 需要 r < 0.7 且 r == R ⇒ 请求 1280×720 + 1600×900）
  const symbolOf = (mode, w = 640, h = 480) => pickResolution([mode], w, h).perMode[0].symbol;
  assert.equal(symbolOf({ width: 1600, height: 900, format: 22 }, 1280, 720), '○', '宽档池 r == R ⇒ ○');
  assert.equal(symbolOf({ width: 1600, height: 900, format: 22 }), '×', '宽档池 r < R ⇒ ×（模式更宽）');
  assert.equal(symbolOf({ width: 1280, height: 1024, format: 22 }), '▲', '窄档池 r > R ⇒ ▲（模式更窄）');
  assert.equal(symbolOf({ width: 1024, height: 768, format: 22 }), '●', '窄档池 r == R ⇒ ●');
  assert.equal(symbolOf({ width: 1366, height: 1024, format: 22 }), '■', '窄档池 r < R ⇒ ■');
  // 阈值边界：r == 0.7 **属于窄档池**（记 ●；把 `>=` 写成 `>` 会让它落到宽档池记 ○）
  assert.equal(symbolOf({ width: 1050, height: 735, format: 22 }, 1050, 735), '●', 'r = 0.7 ⇒ 窄档池');
  assert.equal(symbolOf({ width: 640, height: 480, format: 22 }, 1280, 720), '×', '尺寸闸 ⇒ ×');
  assert.equal(
    pickResolution(
      [{ width: 1024, height: 768, format: 22 }, { width: 1280, height: 960, format: 22 }],
      640,
      480,
    ).perMode[1].symbol,
    '―',
    '池替换被拒 ⇒ ―',
  );
  // 阈值是引擎常量：改坏 0.7 会同时让 4:3 档的 ● / ▲ 分错池
  assert.equal(RESOLUTION_ASPECT_THRESHOLD, 0.7);
});

test('★★ pickResolution：收口两支不对称 —— R < 0.7 时宽档池非空即取，忽略**更小**的窄档', () => {
  // 请求 1280×720（R ≈ 0.5625）：宽档 1600×900（r = R ⇒ ○）；窄档 1366×1024（r ≈ 0.7496 ⇒ ▲）。
  // ⇒ R < 0.7 ⇒ 宽档池非空就取它，即使窄档的宽（1366）严格更小。
  const verdict = pickResolution(
    [{ width: 1600, height: 900, format: 22 }, { width: 1366, height: 1024, format: 22 }],
    1280,
    720,
  );
  assert.deepEqual(verdict.picked, { width: 1600, height: 900 }, '⛔ 不许合并成"取两边更小的"');
  assert.deepEqual(verdict.perMode.map((m) => m.symbol), ['○', '▲']);

  // 宽档池为空时窄档兜底（同一支的另一半）
  assert.deepEqual(
    pickResolution([{ width: 1366, height: 1024, format: 22 }], 1280, 720).picked,
    { width: 1366, height: 1024 },
    '宽档池空 ⇒ 取窄档池',
  );
});

/* ────────────────────────── 分辨率校验（VFS = 0 分支） ────────────────────────── */

test('★★ 分辨率校验用真实判据：六档对 1280×720 选 1280×720 + 写回 + 逐档诊断（⛔ 不再注入）', async () => {
  const { host, session } = await bootWith(
    { 'display:FullScreenWidth': 1280, 'display:FullScreenHeight': 720 },
    { screenMode: 0, hostOpts: { modes: SIX_MODES } },
  );
  assert.equal(session.failure, null);
  assert.equal(host.names().includes('enumerateAdapterDisplayModes'), true, 'VFS = 0 ⇒ 枚举适配器模式');
  assert.ok(session.diagnostics.includes('requested-resolution:1280x720'), '诊断：请求对');
  assert.ok(
    session.diagnostics.includes(`available-display-modes:format=${host.mode.format};${modeText(SIX_MODES)}`),
    '诊断：可用模式清单（数值 = 适配器显示模式的**格式字段**）',
  );
  for (const symbol of ['mode-verdict:800x600:×', 'mode-verdict:1280x720:○', 'mode-verdict:1920x1080:―']) {
    assert.ok(session.diagnostics.includes(symbol), `诊断：逐档符号 ${symbol}`);
  }
  assert.ok(session.diagnostics.includes('picked-resolution:1280x720'), '诊断：判定结果');
  assert.equal(host.argsOf('createDevice').backBufferWidth, 1280, '写回请求宽');
  assert.equal(host.argsOf('createDevice').backBufferHeight, 720, '写回请求高');
  assert.equal(
    session.ignored.some((e) => e.what === 'resolution-pick-rule'),
    false,
    '判据已解清 ⇒ ⛔ 不再记"挑选规则未解清"',
  );
  assert.equal(
    session.graphicsLog
      .filter((entry) => entry.step === 'validate-resolution')
      .every((entry) => entry.category === 'diagnostic'),
    true,
    '分辨率校验的每一行都落在 diagnostic 类别',
  );
});

test('★ 分辨率校验失败 ⇒ 诊断类文本 + **不写回** + 结束启动（⛔ 不是错误类文本）', async () => {
  const modes = [{ width: 1280, height: 720, format: 22 }, { width: 1600, height: 900, format: 22 }];
  const rejected = await bootWith(
    { 'display:FullScreenWidth': 1920, 'display:FullScreenHeight': 1080 },
    { screenMode: 0, hostOpts: { modes } },
  );
  assert.equal(rejected.session.failure.rule, RULE_GRAPHICS_INIT);
  assert.equal(rejected.session.failure.step, 'validate-resolution');
  assert.equal(rejected.session.failure.errorText, GRAPHICS_RESOLUTION_REJECTED_TEXT);
  assert.deepEqual(rejected.session.errors, [], '分辨率校验失败写的是**诊断**，不是错误');
  assert.ok(
    rejected.session.diagnostics.includes(GRAPHICS_RESOLUTION_REJECTED_TEXT),
    '诊断通道必须留下"请求的分辨率不可接受"那一行',
  );
  assert.equal(
    rejected.session.diagnostics.some((d) => d.startsWith('picked-resolution:')),
    false,
    '失败 ⇒ 没有"判定结果"那一行（不写回）',
  );
  assert.equal(rejected.host.names().includes('createDevice'), false, '校验失败 ⇒ 不建设备');
  assert.equal(rejected.session.logSink, 'unavailable', '日志落点与失败判定无关（缺键 ⇒ 丢弃）');
  assert.equal(GRAPHICS_STEP_OUTCOME['validate-resolution'].log, 'diagnostic');
  assert.equal(GRAPHICS_STEP_OUTCOME['validate-resolution'].fatal, true);
});

test('★★ 请求对是**整对**覆盖：任一键非 0 ⇒ 整对取这两个键（⛔ 不做逐分量回退）', async () => {
  // (a) FullScreenWidth = 0、FullScreenHeight = 1080 ⇒ 请求对 = (0, 1080)，⛔ 不是 (客户区宽, 1080)
  const zeroWidth = await bootWith(
    { 'display:FullScreenWidth': 0, 'display:FullScreenHeight': 1080 },
    { screenMode: 0, hostOpts: { modes: [{ width: 1920, height: 1080, format: 22 }] } },
  );
  assert.ok(
    zeroWidth.session.diagnostics.includes('requested-resolution:0x1080'),
    `请求对必须整对取 (0, 1080)（实际：${zeroWidth.session.diagnostics.join(' | ')}）`,
  );
  assert.equal(
    zeroWidth.session.diagnostics.some((d) => d.startsWith('requested-resolution:1024x1080')),
    false,
    '⛔ 不许把 0 回退成客户区宽',
  );

  // (b) 可观测的判别：FullScreenWidth = 1280、FullScreenHeight = 0
  //     真请求对 (1280, 0) ⇒ R = 0 ⇒ 1366×768（r ≈ 0.5622 > R）进宽档池 ⇒ 选中；
  //     逐分量回退成 (1280, 720) ⇒ R ≈ 0.5625 ⇒ 同一档 r < R ⇒ × ⇒ 失败。
  const zeroHeight = await bootWith(
    { 'display:FullScreenWidth': 1280, 'display:FullScreenHeight': 0 },
    { screenMode: 0, hostOpts: { modes: [{ width: 1366, height: 768, format: 22 }] } },
  );
  assert.equal(zeroHeight.session.failure, null, '逐分量回退会让这一步失败 —— 它必须是选中的');
  assert.ok(zeroHeight.session.diagnostics.includes('requested-resolution:1280x0'));
  assert.ok(zeroHeight.session.diagnostics.includes('picked-resolution:1366x768'));
  assert.equal(zeroHeight.host.argsOf('createDevice').backBufferWidth, 1366, '写回选中的宽');
  assert.equal(zeroHeight.host.argsOf('createDevice').backBufferHeight, 768);

  // (c) 两个键都为 0 ⇒ 请求对 = 客户区对（窗口态 = set:WinX / set:WinY）
  const client = await bootWith({}, { screenMode: 2, hostOpts: { modes: SIX_MODES } });
  assert.ok(client.session.diagnostics.includes('requested-resolution:1024x768'), '两个键都为 0 ⇒ 客户区对');
});

test('★★ 日志落点：OutErrorLog 为 0 / 缺键 ⇒ unavailable；非 0 + 打开成功 ⇒ file；失败 / 缺方法 ⇒ unavailable', async () => {
  const missingKey = await bootWith({}, { screenMode: 2 });
  assert.equal(missingKey.session.logSink, 'unavailable', '缺键 ⇒ 读 0 ⇒ 不打开');
  assert.equal(missingKey.host.openErrorLogCalls, 0, '键为 0 / 缺键 ⇒ ⛔ 不请求打开');
  // spec：读缺失键会先弹一次模态错误框；本层没有 UI ⇒ 只记一条诊断（⛔ 不静默、⛔ 不伪造弹框）
  assert.ok(
    missingKey.session.diagnostics.includes('config-missing-key:system:OutErrorLog'),
    '缺键必须可见（config-missing-key:<key>），与"显式写 0"区分开',
  );

  const zero = await bootWith({ 'system:OutErrorLog': 0 }, { screenMode: 2 });
  assert.equal(zero.session.logSink, 'unavailable');
  assert.equal(zero.host.openErrorLogCalls, 0);
  assert.equal(
    zero.session.diagnostics.includes('config-missing-key:system:OutErrorLog'),
    false,
    '显式写 0 的键是"在的" ⇒ ⛔ 不记缺键诊断',
  );

  const file = await bootWith({ 'system:OutErrorLog': 1 }, { screenMode: 2 });
  assert.equal(file.session.logSink, 'file', '非 0 且宿主打开成功 ⇒ file');
  assert.equal(file.host.openErrorLogCalls, 1);
  assert.equal(
    file.session.trace.some((r) => r.request === 'open-error-log'),
    true,
    '打开日志文件必须留痕（平台请求）',
  );

  const openFailed = await bootWith(
    { 'system:OutErrorLog': 1 },
    { screenMode: 2, hostOpts: { errorLogOpens: false } },
  );
  assert.equal(openFailed.session.logSink, 'unavailable', '打开失败 ⇒ 句柄无效');
  assert.equal(openFailed.session.failure, null, '日志打不开不影响启动是否继续');

  const noMethod = await bootWith(
    { 'system:OutErrorLog': 1 },
    { screenMode: 2, hostOpts: { openErrorLog: false } },
  );
  assert.equal(noMethod.session.logSink, 'unavailable');
  assert.ok(
    noMethod.session.diagnostics.some((d) => d.includes('openErrorLog')),
    '宿主缺 openErrorLog ⇒ 记诊断（⛔ 不静默当成功）',
  );
});

/* ────────────────────────── 日志路径：纯函数 + 领域字段 ────────────────────────── */

test('★★ joinEnginePath 7 例：分隔符只在"拼到此处的结果非空"时补（⛔ 不是 path.join 的折叠语义）', () => {
  const f = (root, relativeDirectory) => joinEnginePath({
    root,
    relativeDirectory,
    fileName: ERROR_LOG_FILE_NAME,
  });

  // ① 根与相对目录都空 ⇒ 裸文件名（⛔ 不是 '\Error.log'）
  assert.equal(f('', ''), 'Error.log', '① 全空 ⇒ 裸文件名');

  // ② 根非空、相对目录空 ⇒ **连续两个分隔符**（引擎不折叠）
  assert.equal(
    f('C:\\Users\\u\\AppData\\Local', ''),
    'C:\\Users\\u\\AppData\\Local\\\\Error.log',
    '② 根非空 + 目录空 ⇒ 双分隔符',
  );

  // ③ 根空、相对目录非空 ⇒ 单分隔符 + 相对目录
  assert.equal(f('', 'savedata'), 'savedata\\Error.log', '③ 只有相对目录');

  // ④ 两者都有 ⇒ 两条分隔符都在
  assert.equal(f('C:\\base', 'savedata'), 'C:\\base\\savedata\\Error.log', '④ 根 + 相对目录');

  // ⑤ 根自带尾分隔符 ⇒ 也**不去重**（引擎只按"结果非空"再补一个）
  assert.equal(
    f('C:\\base\\', 'savedata'),
    'C:\\base\\\\savedata\\Error.log',
    '⑤ 尾分隔符不去重',
  );

  // ⑥ 超过 256 **字节** ⇒ 截断（400 个 x + 分隔符 + 文件名 ⇒ 前 256 字节全是 x）
  const long = joinEnginePath({ root: 'x'.repeat(400), relativeDirectory: '', fileName: ERROR_LOG_FILE_NAME });
  assert.equal(long.length, ERROR_LOG_MAX_PATH_BYTES, '⑥ 上限 256 字节');
  assert.equal(long, 'x'.repeat(256), '⑥ 截断留下的就是前 256 个字节');

  // ⑦ ⛔ 不是 path.join：同样的输入，path.win32.join 会吃掉空段、把双分隔符折叠成一个
  const root = 'C:\\Users\\u\\AppData\\Local';
  assert.equal(
    path.win32.join(root, '', ERROR_LOG_FILE_NAME),
    'C:\\Users\\u\\AppData\\Local\\Error.log',
    '⑦ 对照：path.win32.join 的折叠语义',
  );
  assert.notEqual(
    f(root, ''),
    path.win32.join(root, '', ERROR_LOG_FILE_NAME),
    '⑦ 本实现**必须**与 path.join 不同（双分隔符是合同）',
  );
});

test('★★ 日志落点的领域字段：UseAppDataFolder **恰为 1** ⇒ 配置根 = 宿主应用数据目录；0 / 2 ⇒ 空串；缺 appDataDir ⇒ 诊断 + unavailable', async () => {
  const appData = 'C:\\Users\\u\\AppData\\Local';
  const one = await bootWith(
    { 'system:OutErrorLog': 1, 'set:UseAppDataFolder': 1 },
    { screenMode: 2, strings: { 'set:RegFilePath': 'savedata' }, hostOpts: { appDataDir: appData } },
  );
  assert.equal(one.session.logSink, 'file');
  const oneSpec = one.host.argsOf('openErrorLog');
  assert.equal(oneSpec.useAppDataFolder, true, '恰为 1 ⇒ true');
  assert.equal(oneSpec.baseDirectory, appData, '配置根 = 宿主给的用户**本地**应用数据目录');
  assert.equal(oneSpec.relativeDirectory, 'savedata', '相对目录 = set:RegFilePath 原值（不展开）');
  assert.equal(oneSpec.fileName, 'Error.log', '文件名固定');
  assert.equal(oneSpec.maxPathBytes, 256, '整条路径上限 256 字节');
  assert.ok(
    one.host.names().indexOf('appDataDir') < one.host.names().indexOf('openErrorLog'),
    '先问配置根，再请宿主打开',
  );

  const zero = await bootWith(
    { 'system:OutErrorLog': 1, 'set:UseAppDataFolder': 0 },
    { screenMode: 2, strings: { 'set:RegFilePath': 'savedata' } },
  );
  const zeroSpec = zero.host.argsOf('openErrorLog');
  assert.equal(zeroSpec.useAppDataFolder, false, '0 ⇒ false');
  assert.equal(zeroSpec.baseDirectory, '', '其它值 ⇒ 配置根 = 空串（⇒ 相对路径）');
  assert.equal(zero.host.names().includes('appDataDir'), false, '不是恰为 1 ⇒ ⛔ 不问配置根');

  const two = await bootWith(
    { 'system:OutErrorLog': 1, 'set:UseAppDataFolder': 2 },
    { screenMode: 2 },
  );
  assert.equal(
    two.host.argsOf('openErrorLog').useAppDataFolder,
    false,
    '判据是**恰为 1**，⛔ 不是"非 0"（2 也不算）',
  );

  const noAppData = await bootWith(
    { 'system:OutErrorLog': 1, 'set:UseAppDataFolder': 1 },
    { screenMode: 2, hostOpts: { appDataDir: false } },
  );
  assert.equal(noAppData.session.logSink, 'unavailable', '配置根解析不出来 ⇒ 落点无效');
  assert.equal(noAppData.host.openErrorLogCalls, 0, '没有配置根 ⇒ ⛔ 不猜一个目录去打开');
  assert.ok(
    noAppData.session.diagnostics.some((d) => d.includes('appDataDir')),
    '缺 appDataDir ⇒ 记诊断（⛔ 不静默当成功）',
  );
  assert.equal(noAppData.session.failure, null, '日志落点不影响启动是否继续');
});

test('★★ 日志文件的打开时机：窗口附加（record-service-window）之后、图形设备之前；每次启动一次', async () => {
  const threeD = await bootWith({ 'system:OutErrorLog': 1 }, { screenMode: 2 });
  const names = threeD.session.trace.map((r) => r.request);
  const iAttach = names.indexOf('record-service-window');
  const iOpen = names.indexOf('open-error-log');
  const iD3d = names.indexOf('create-d3d9');
  assert.ok(iAttach >= 0 && iOpen > iAttach, '日志必须在窗口附加之后才打开（之前没有落点）');
  assert.ok(iD3d >= 0 && iOpen < iD3d, '日志必须在图形设备初始化之前打开');
  assert.equal(names.filter((n) => n === 'open-error-log').length, 1, '每次启动只打开一次');

  const legacyDefaults = DEFAULTS.map((row) => (
    row.key === 'CREATEOBJECT' ? { key: 'CREATEOBJECT', value: '1' } : row
  ));
  const legacy = await bootWith({ 'system:OutErrorLog': 1 }, { screenMode: 2, defaults: legacyDefaults });
  const legacyNames = legacy.session.trace.map((r) => r.request);
  assert.ok(
    legacyNames.indexOf('open-error-log') < legacyNames.indexOf('create-legacy-draw'),
    '旧式绘制路线也在图形设备之前（两条路线同一个打开点）',
  );
});

test('★ available-display-modes 的数值是**显示模式的格式字段**（⛔ 不是宽）；宿主没报格式 ⇒ format=?', async () => {
  const request = { 'display:FullScreenWidth': 1280, 'display:FullScreenHeight': 720 };
  const distinct = await bootWith(request, {
    screenMode: 0,
    hostOpts: { modes: SIX_MODES, mode: { width: 1920, height: 1080, format: 87 } },
  });
  const line = distinct.session.diagnostics.find((d) => d.startsWith('available-display-modes:'));
  assert.equal(
    line,
    `available-display-modes:format=87;${modeText(SIX_MODES)}`,
    '格式号取自适配器显示模式的 format 字段',
  );
  assert.equal(line.includes('format=1920'), false, '⛔ 不是把宽当格式印出来');

  const unknown = await bootWith(request, {
    screenMode: 0,
    hostOpts: { modes: SIX_MODES, mode: { width: 1920, height: 1080 } },
  });
  assert.ok(
    unknown.session.diagnostics.includes(`available-display-modes:format=?;${modeText(SIX_MODES)}`),
    '宿主没报格式 ⇒ format=?（⛔ 不编一个数）',
  );
});

test('★ 宿主不枚举 ⇒ 显式记账（实现侧能力缺口，⛔ 不静默）', async () => {
  const noEnumeration = await bootWith({}, { screenMode: 0 });
  assert.ok(
    noEnumeration.session.ignored.some((e) => e.rule === RULE_GRAPHICS_INIT && e.what === 'resolution-enumeration'),
    '宿主不枚举 ⇒ 实现侧能力缺口必须显式记账',
  );
  assert.equal(noEnumeration.session.failure, null);

  // VFS ≠ 0 ⇒ 这一步**完全不做**（也不枚举）
  const virtual = await bootWith(
    { 'display:VirtualFullScreen': 1, 'display:VirtualFullScreenType': 2 },
    { screenMode: 0, hostOpts: { modes: SIX_MODES } },
  );
  assert.equal(virtual.host.names().includes('enumerateAdapterDisplayModes'), false, 'VFS ≠ 0 ⇒ 不做分辨率校验');
  assert.equal(virtual.session.failure, null);

  // 窗口化（S ≠ 0）也做校验（VFS = 0 分支），但写回**不影响**后缓冲（后缓冲取客户区）
  const windowed = await bootWith(
    { 'display:FullScreenWidth': 1600, 'display:FullScreenHeight': 900 },
    { screenMode: 2, hostOpts: { modes: SIX_MODES } },
  );
  assert.ok(windowed.session.diagnostics.includes('picked-resolution:1600x900'));
  assert.equal(windowed.host.argsOf('createDevice').backBufferWidth, 1024, '窗口化后缓冲 = 客户区宽（写回不可见）');
  assert.equal(windowed.host.argsOf('createDevice').backBufferHeight, 768);
});

test('★★ 宿主缺少可选方法 ⇒ session.diagnostics 逐条诊断（⛔ 不静默）', async () => {
  const { host, session } = await bootWith({}, { screenMode: 2, hostOpts: { postDevice: false } });
  assert.equal(session.failure, null, '缺可选方法不是失败（但必须可见）');
  for (const method of ['setDialogBoxMode', 'queryDeviceCaps', 'checkTextureFormat32', 'clearRenderTarget']) {
    assert.ok(
      session.diagnostics.some((d) => d.includes(method)),
      `缺 ${method} 必须有一条诊断（实际：${session.diagnostics.join(' | ') || '(空)'}）`,
    );
    assert.equal(host.names().includes(method), false, `缺 ${method} 时不可能收到该请求`);
  }
  assert.equal(
    session.diagnostics.filter((d) => !d.startsWith('config-missing-key:')).length,
    4,
    '四条缺方法诊断（缺键诊断不算）',
  );
  assert.equal(session.drawObject, 9, '缺可选方法时仍继续建绘制对象');
});

test('★★ 池替换的判据是"宽或高任一更小"（⛔ 不是两者都更小）', () => {
  // 1600×900 先进池；1500×1000 宽更小、高更大 ⇒ 旧判据"任一更小"⇒ 不替换（―）；
  // 若错写成"两者都更小"⇒ 会替换成 1500×1000，收口结果就变了。
  const verdict = pickResolution(
    [
      { width: 1600, height: 900 },
      { width: 1500, height: 1000 },
    ],
    1280,
    720,
  );
  assert.equal(verdict.perMode[1].symbol, '―', '宽更小、高更大 ⇒ 不替换');
  assert.deepEqual(verdict.picked, { width: 1600, height: 900 });
});
