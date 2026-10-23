/**
 * 分析入口（**核心层**）：一份脚本字节 / 一个安装目录 → `ScriptReport`。
 *
 * ★ 入口无关：`--script` 调试口与 `--install` 真实入口共用 `analyzeBytes`。
 * ★ 全部 `async`：取件（索引 / 松散文件 / 归档切片）、配置读取、脚本执行都在同一条 `await` 链上。
 * ★ 平台侧只需要准备好纯数据：`Host`（文件系统）+ 指令表 + 配置键词表。
 *   核心**不读** `data/spec`；那是 `platform/node/spec.ts` 的事。
 */
import { codeLimit, parseHeader, walk } from './decode.ts';
import type { Gap } from './decode.ts';
import { ConfigStore, parseIni } from './config.ts';
import { generateIntKey } from './values.ts';
import { HANDLERS } from './handlers.ts';
import { Machine, effectScope } from './machine.ts';
import type { Diagnostic, EffectRecord, Stop } from './machine.ts';
import { startupReport } from './boot.ts';
import type { ConfigSummary, StartupReport } from './boot.ts';
import { Install, defaultConfigPath, ensureConfigDir, readConfigFile, readFile, writeConfigFile } from './install.ts';
import type { Host } from './install.ts';
import type { ConfigKeySchema, OpcodeRow } from './spec.ts';
import type { RenderHost } from './render.ts';
import type { MovieHost } from './movie.ts';
import type { InputHost } from './input.ts';

/** 一份脚本的"结构 + 执行"报告（decode / run / 缺 key 三种形态共用；可选字段由 mode 决定） */
export interface ScriptReport {
  bytes: number;
  signature: string;
  version: string;
  headerLength: number;
  tables: { len: number; off: number }[];
  bodyLength: number;
  codeLimit: number;
  decoded: number;
  mode: 'decode' | 'run';
  stop: string;
  gap: Gap | null;
  key?: string | null;
  steps?: number;
  ticks?: number;
  atMs?: number;
  stopText?: string;
  executed?: number;
  diagnostics?: Diagnostic[];
  /** 外部请求 + 未建模请求按 kind 的计数（CLI 没处理的那一类；全量明细见 `--effects`） */
  effectCounts?: Record<string, number>;
  /** 已建模的内部记账按 kind 的计数（`string.set` / `array.*` 等；`--trace-all` 才逐条打印） */
  internalEffectCounts?: Record<string, number>;
  /** 全量效果明细（只在 `--effects` / `--trace` 时填；可能上千条） */
  effects?: EffectRecord[];
  /** 场景错误缓冲（`0x20d` / `0x204` 等失败路径写的文本；空 = 无） */
  sceneError?: string;
  /** 进入过的脚本 id（`0x3` / `0x8` 的实参；证明"启动链走到了哪个脚本"） */
  entered?: number[];
  /** 仅安装入口：从 WinMain 到主循环的启动阶段记账 */
  startup?: StartupReport;
  /** 仅安装入口：配置库摘要 */
  config?: ConfigSummary;
  /** 输入侧的可观察记账（CLI 收口 / 守卫用） */
  input?: {
    word: number;
    keyTotal: number;
    mode: number;
    samples: number;
    mouseDispatches: number;
    keyDispatches: number;
    unmodeled: string[];
  };
}

/** 核心运行期需要的**纯数据** + 平台接口（由平台层装配） */
export interface CoreEnv {
  host: Host;
  /** `data/spec/format/opcode-table.json` 的行（平台读入，核心建表） */
  table: Map<number, OpcodeRow>;
  /** `data/spec/format/config-keys.json`（平台读入；安装入口用它建配置库） */
  configKeys: ConfigKeySchema;
  /**
   * **渲染宿主**（`60-ops-render.md` 的平台边界）：`0x20d` / `0x204` 的外部请求走它。
   * ★ 平台入口注入真实现，CLI 注入 `HeadlessRenderHost`；**缺省 = 没有后端**
   *   （`0x20d` 走错误通道，`0x204` 的入口条件 ② 不成立）。
   */
  renderHost?: RenderHost;
  /** 影片播放后端（`0x20f`）；缺省 = 没有后端 ⇒ 影片位保持门关闭 */
  movieHost?: MovieHost;
  /** 输入后端（`60-ops-input.md`）；缺省 = 没有设备 */
  inputHost?: InputHost;
}

export interface RunOpts {
  decodeOnly?: boolean;
  key?: number;
  stopAtOpcode?: number;
  maxSteps?: number;
  maxTicks?: number;
  tickMs?: number;
  /** 宿主毫秒时钟（可选；给了就覆盖虚拟步长，见 `MachineOptions.clock`） */
  clock?: () => number;
  /** 报告里是否带**全量**效果明细（可能上千条；`--effects` / `--trace` 用） */
  includeEffects?: boolean;
  uninitializedFloat?: 'throw' | 'zero';
  config?: ConfigStore;
  /** 换脚本装载器（真实入口才有；`--script` 调试口没有） */
  loadScript?: (id: number) => Promise<Uint8Array | null>;
  /** 纹理文件装载器（真实入口才有；`0x1f9` 用） */
  loadImage?: (id: number) => Promise<Uint8Array | null>;
  /** 宿主字体名表（`0x2de` 的输入；缺省空表 ⇒ 未命中 ⇒ 引擎回退分支） */
  fontNames?: string[];
  /** 渲染宿主；给了就覆盖 `CoreEnv.renderHost`（缺省沿用 env 的那一个） */
  renderHost?: RenderHost;
  /** 影片后端；给了就覆盖 `CoreEnv.movieHost`（缺省沿用 env 的那一个） */
  movieHost?: MovieHost;
  /** 输入后端；给了就覆盖 `CoreEnv.inputHost`（缺省沿用 env 的那一个） */
  inputHost?: InputHost;
  /** 宿主睡眠（`0xc8` 的短睡眠）；缺省 ⇒ 该支响亮失败 */
  sleepHost?: (ms: number) => Promise<void>;
  /**
   * **迭代头的平台泵点**（`70-vm.md` 的 `CORE-VM-LOOP`）：每次迭代在门之前调一次。
   * ★ 平台入口把窗口 / timer 消息泵接在这里（`WindowSession.pump()`）；缺省 `undefined` = 不泵。
   */
  onIteration?: () => void | Stop | Promise<void | Stop>;
  /**
   * **根脚本装载之前的平台阶段**（`06-window.md` 的启动顺序：图形设备 → **根脚本** → 周期定时器）。
   *
   * ★ 只有安装入口（`analyzeInstall`）调它，时机是"配置库已建立（含 `io.initConfig` 的写回）、
   *   **还没读根脚本**"。平台入口在这里 `await bootWindow(...)` 跑到"图形设备就绪"为止：
   *   失败 ⇒ 抛错 ⇒ 根脚本根本不会被读、脚本不会跑（spec 的共享出口）。
   * ★ 参数是**已经建好的**配置库（不是让平台自己再建一份）：`bootWindow` 读的就是这一份。
   * ★ 缺省 `undefined` ⇒ 什么都不做（CLI / `--script` 调试口的行为不变）。
   */
  onBeforeScript?: (ctx: {
    install: Install;
    config: ConfigStore;
    configPath: string;
  }) => void | Promise<void>;
  /**
   * **`new Machine` 之后、`machine.run` 之前**的平台钩子。
   *
   * ★ 时机对应 `06-window.md` 的"根脚本装载夹在图形设备与周期定时器之间"：
   *   根脚本 body 已经在 `Machine` 的 0 号槽里 ⇒ 平台在这里调
   *   `WindowSession.beginMainLoop()`（建周期定时器）。
   * ★ 平台可以把 `machine` 存到闭包里，供 `onIteration`（泵 / 场景快照）使用。
   * ★ 缺省 `undefined` ⇒ 什么都不做。
   */
  onMachine?: (machine: Machine, ctx: { config?: ConfigStore }) => void | Promise<void>;
}

/** 效果明细按 kind 计数（稳定排序由 CLI / 消费者决定） */
/** 按 scope 分组计数：`external`/`unmodeled` 合并进 `effectCounts`，`internal` 单独。 */
export function countEffectsByScope(effects: readonly EffectRecord[]): {
  effectCounts: Record<string, number>;
  internalEffectCounts: Record<string, number>;
} {
  const effectCounts: Record<string, number> = {};
  const internalEffectCounts: Record<string, number> = {};
  for (const effect of effects) {
    const bucket = effectScope(effect.kind) === 'internal' ? internalEffectCounts : effectCounts;
    bucket[effect.kind] = (bucket[effect.kind] ?? 0) + 1;
  }
  return { effectCounts, internalEffectCounts };
}

function describeStop(stop: Stop): string {
  switch (stop.kind) {
    case 'exit':
      return '退出';
    case 'stop-opcode':
      return `命中停止条件 opcode 0x${stop.opcode.toString(16)}（第 ${stop.index} 条）`;
    case 'unimplemented':
      return `未实现 opcode 0x${stop.opcode.toString(16)}（第 ${stop.index} 条 · 偏移 ${stop.offset} · argc ${stop.argc}）`;
    case 'unknown-opcode':
      return `指令表里没有 opcode 0x${stop.opcode.toString(16)}（第 ${stop.index} 条 · 偏移 ${stop.offset}）`;
    case 'truncated':
      return `指令越界（第 ${stop.index} 条 · 偏移 ${stop.offset}）`;
    case 'step-limit':
      return `步数上限（第 ${stop.index} 步）`;
    case 'tick-limit':
      return `时钟上限（第 ${stop.index} 步）`;
    case 'gate':
      return `门位未服务：0x${stop.bit.toString(16)}（第 ${stop.index} 条）`;
    case 'input-idle':
      return `输入脚本已耗尽且空闲 ${stop.now}ms（第 ${stop.index} 步）`;
  }
}

/** 执行 / 解码一份脚本字节（入口无关） */
export async function analyzeBytes(env: CoreEnv, buf: Uint8Array, opts: RunOpts = {}): Promise<ScriptReport> {
  const header = parseHeader(buf);
  const body = buf.subarray(header.headerLength);
  const limit = codeLimit(header, body.length);
  const walked = walk(body, env.table, limit);
  const base = {
    bytes: buf.length,
    signature: header.signature,
    version: header.version,
    headerLength: header.headerLength,
    tables: header.tables,
    bodyLength: body.length,
    codeLimit: limit,
    decoded: walked.instructions.length,
  };

  if (opts.decodeOnly) {
    return {
      ...base,
      mode: 'decode',
      stop: walked.gap ? walked.gap.reason : walked.stop,
      gap: walked.gap,
    };
  }

  // key 是运行期状态：调用方没给就让实现生成一个非 0 值（规范不允许把 0 当默认）。
  const key = opts.key ?? generateIntKey();
  const machine = new Machine({
    body, table: env.table, key, tickMs: opts.tickMs, clock: opts.clock, uninitializedFloat: opts.uninitializedFloat, config: opts.config, loadScript: opts.loadScript,
    loadImage: opts.loadImage,
    fontNames: opts.fontNames, renderHost: opts.renderHost ?? env.renderHost, movieHost: opts.movieHost ?? env.movieHost, inputHost: opts.inputHost ?? env.inputHost, sleepHost: opts.sleepHost, onIteration: opts.onIteration,
  });
  // 根脚本已在 0 号槽 ⇒ 平台在这里建周期定时器（`onMachine` 的注释见 `RunOpts`）
  await opts.onMachine?.(machine, { config: opts.config });
  const r = await machine.run({ stopAtOpcode: opts.stopAtOpcode, maxSteps: opts.maxSteps, maxTicks: opts.maxTicks });
  const effectScopes = countEffectsByScope(machine.effects);
  return {
    ...base,
    mode: 'run',
    key: `0x${key.toString(16)}`,
    steps: r.steps,
    ticks: machine.ticks,
    atMs: machine.now,
    stop: r.stop.kind,
    stopText: describeStop(r.stop),
    executed: r.ip,
    diagnostics: machine.diagnostics,
    effectCounts: effectScopes.effectCounts,
    internalEffectCounts: effectScopes.internalEffectCounts,
    ...(opts.includeEffects ? { effects: [...machine.effects] } : {}),
    sceneError: machine.scene.errorBuffer,
    input: {
      word: machine.input.word >>> 0,
      keyTotal: machine.input.keyTotal,
      mode: machine.input.mode,
      samples: machine.input.samples,
      mouseDispatches: machine.input.mouseDispatches,
      keyDispatches: machine.input.keyDispatches,
      unmodeled: [...machine.unmodeledSeen].map((op) => `0x${op.toString(16)}`),
    },
    entered: [...machine.enteredIds],
    gap:
      r.stop.kind === 'unimplemented'
        ? { index: r.stop.index, offset: r.stop.offset, opcode: r.stop.opcode, argc: r.stop.argc, reason: 'unimplemented' }
        : null,
  };
}

/** 调试入口：直接读一份脚本文件 */
export async function analyze(env: CoreEnv, file: string, opts: RunOpts = {}): Promise<ScriptReport & { source: Record<string, unknown> }> {
  return { source: { kind: 'script', file }, ...(await analyzeBytes(env, await readFile(env.host, file), opts)) };
}

/**
 * 真实入口：安装目录 → 索引 → 统一文件 id → 松散文件 / 归档切片。
 *
 * ★ 顺序是 `06-window.md` 的合同（本函数是唯一能把它摆正的地方）：
 *   打开索引 → 建配置库（发行默认表 + 玩家文件，必要时写回）→ **`onBeforeScript`**
 *   （平台在这里跑到"图形设备就绪"；失败即抛，根脚本不会被读）→ **读根脚本** → `analyzeBytes`
 *   （`new Machine` → `onMachine`（平台建周期定时器）→ `machine.run`（迭代头 `onIteration` 泵消息））。
 *   ⛔ 读根脚本**不许**回到 `onBeforeScript` 之前 —— 那会让图形设备失败后的启动继续读到根脚本。
 */
export async function analyzeInstall(
  env: CoreEnv,
  dir: string,
  rootId: number,
  opts: RunOpts = {},
  io: { config?: string | null; initConfig?: boolean } = {},
): Promise<ScriptReport & { source: Record<string, unknown> }> {
  const install = await Install.open(env.host, dir);
  const store = new ConfigStore(env.configKeys);
  store.applyDefaults(install.index.configDefaults);
  const configPath = io.config
    ?? defaultConfigPath(env.host, dir, store.getString('set:RegFilePath'), store.getInt('set:UseAppDataFolder') !== 0);
  await ensureConfigDir(env.host, configPath);
  let source: 'defaults' | 'file' = 'defaults';
  const raw = await readConfigFile(env.host, configPath);
  if (raw) {
    store.applyIni(parseIni(raw));
    source = 'file';
  }
  if (io.initConfig) await writeConfigFile(env.host, configPath, store.serializeIni());
  // ① 配置已就绪、根脚本**还没读**：窗口 / 图形设备阶段（`06-window.md`；失败即抛，启动到此结束）
  await opts.onBeforeScript?.({ install, config: store, configPath });
  // ② 根脚本（图形设备之后、周期定时器之前的那个阶段）
  const script = await install.readById(rootId);
  const config: ConfigSummary = { path: configPath, source, ...store.stats() };
  const result = await analyzeBytes(env, script.bytes, {
    ...opts,
    config: store,
    // 取不到 ⇒ readById 自己响亮失败（游标越界 / 归档读不到），⛔ 不在这里静默换成 null
    loadScript: async (id) => {
      const f = await install.readById(id);
      return f.bytes.subarray(parseHeader(f.bytes).headerLength);
    },
    // 纹理是整份文件（AGF），⛔ 不剥脚本头
    loadImage: async (id) => (await install.readById(id)).bytes,
  });
  return {
    source: {
      kind: 'install',
      dir,
      index: install.indexFile,
      indexSignature: install.index.signature,
      indexEntries: install.entryCount,
      archives: install.index.archives,
      id: script.id,
      name: script.name,
      side: script.side,
      entryLength: script.entryLength,
      actualLength: script.bytes.length,
      archive: script.archive,
    },
    config,
    startup: startupReport({ config }),
    ...result,
  };
}
