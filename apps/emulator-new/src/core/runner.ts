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
import { HANDLERS } from './handlers.ts';
import { Machine } from './machine.ts';
import type { Diagnostic, Stop } from './machine.ts';
import { startupReport } from './boot.ts';
import type { ConfigSummary, StartupReport } from './boot.ts';
import { Install, defaultConfigPath, ensureConfigDir, readConfigFile, readFile, writeConfigFile } from './install.ts';
import type { Host } from './install.ts';
import type { ConfigKeySchema, OpcodeRow } from './spec.ts';

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
  /** 进入过的脚本 id（`0x3` / `0x8` 的实参；证明"启动链走到了哪个脚本"） */
  entered?: number[];
  /** 仅安装入口：从 WinMain 到主循环的启动阶段记账 */
  startup?: StartupReport;
  /** 仅安装入口：配置库摘要 */
  config?: ConfigSummary;
}

/** 核心运行期需要的**纯数据** + 平台接口（由平台层装配） */
export interface CoreEnv {
  host: Host;
  /** `data/spec/format/opcode-table.json` 的行（平台读入，核心建表） */
  table: Map<number, OpcodeRow>;
  /** `data/spec/format/config-keys.json`（平台读入；安装入口用它建配置库） */
  configKeys: ConfigKeySchema;
}

export interface RunOpts {
  decodeOnly?: boolean;
  key?: number;
  stopAtOpcode?: number;
  maxSteps?: number;
  maxTicks?: number;
  tickMs?: number;
  uninitializedFloat?: 'throw' | 'zero';
  config?: ConfigStore;
  /** 换脚本装载器（真实入口才有；`--script` 调试口没有） */
  loadScript?: (id: number) => Promise<Uint8Array | null>;
  /** 宿主字体名表（`0x2de` 的输入；缺省空表 ⇒ 未命中 ⇒ 引擎回退分支） */
  fontNames?: string[];
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

  if (opts.key === undefined) {
    // 不执行就不需要 key；但也不许把它当成"执行成功"
    const firstUnimplemented = walked.instructions.find((x) => !HANDLERS.has(x.opcode)) ?? null;
    return {
      ...base,
      mode: 'run',
      key: null,
      steps: 0,
      stop: 'missing-key',
      gap: firstUnimplemented
        ? { index: firstUnimplemented.index, offset: firstUnimplemented.offset, opcode: firstUnimplemented.opcode, argc: firstUnimplemented.argc, reason: 'unimplemented' }
        : null,
    };
  }

  const machine = new Machine({
    body, table: env.table, key: opts.key, tickMs: opts.tickMs, uninitializedFloat: opts.uninitializedFloat, config: opts.config, loadScript: opts.loadScript,
    fontNames: opts.fontNames,
  });
  const r = await machine.run({ stopAtOpcode: opts.stopAtOpcode, maxSteps: opts.maxSteps, maxTicks: opts.maxTicks });
  return {
    ...base,
    mode: 'run',
    key: `0x${opts.key.toString(16)}`,
    steps: r.steps,
    ticks: machine.ticks,
    atMs: machine.now,
    stop: r.stop.kind,
    stopText: describeStop(r.stop),
    executed: r.ip,
    diagnostics: machine.diagnostics,
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

/** 真实入口：安装目录 → 索引 → 统一文件 id → 松散文件 / 归档切片 */
export async function analyzeInstall(
  env: CoreEnv,
  dir: string,
  rootId: number,
  opts: RunOpts = {},
  io: { config?: string | null; initConfig?: boolean } = {},
): Promise<ScriptReport & { source: Record<string, unknown> }> {
  const install = await Install.open(env.host, dir);
  const script = await install.readById(rootId);
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
  const config: ConfigSummary = { path: configPath, source, ...store.stats() };
  const result = await analyzeBytes(env, script.bytes, {
    ...opts,
    config: store,
    // 取不到 ⇒ readById 自己响亮失败（游标越界 / 归档读不到），⛔ 不在这里静默换成 null
    loadScript: async (id) => {
      const f = await install.readById(id);
      return f.bytes.subarray(parseHeader(f.bytes).headerLength);
    },
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
