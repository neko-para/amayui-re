/**
 * 平台层（Node）：CLI。只做"参数 → 核心调用 → 文本输出 / 退出码"。
 *
 *   # 真实入口：安装目录 → SYS4INI.BIN（ALF 索引）→ 统一文件 id → 松散文件 / 归档切片
 *   node apps/emulator-new/src/main.ts --install <dir> [--key <u32>] [--root-id 0] [--stop-at <opcode>]
 *
 *   # 调试入口：直接给一份脚本文件
 *   node apps/emulator-new/src/main.ts --script <file> [--key <u32>] [--stop-at <opcode>]
 *   node apps/emulator-new/src/main.ts --install <dir> --decode [--json]
 *
 * 默认模式：**执行**。遇到没有实现的 opcode 就停在它前面并报缺口（未实现即抛错，不 no-op）。
 * `--decode`：只解码走查（不执行），用于检验 CORE-ISA 的结构规则。
 * ★ `--key` 是整型编码的运行期 key；规范要求实现**运行期生成**一个非 0 值。
 *   缺省自动生成；给 `--key` 只用于可复现的位模式对照（0 不是合法值）。
 * ★ 核心是平台无关的（`core/runner.ts`）；本文件只是它 + Node 的 `Host` + `data/spec` 的装配点。
 */
import { analyze, analyzeInstall } from '../../core/runner.ts';
import type { Machine } from '../../core/machine.ts';
import { effectScope } from '../../core/machine.ts';
import { decodeCp932 } from '../../core/bytes.ts';
import type { CoreEnv, RunOpts } from '../../core/runner.ts';
import { loadConfigKeys, loadOpcodeTable } from './spec.ts';
import { nodeHost } from './host.ts';

interface Args {
  install: string | null;
  script: string | null;
  rootId: number;
  decodeOnly: boolean;
  json: boolean;
  strict: boolean;
  key: number | null;
  stopAt: number | null;
  tickMs: number;
  /** 用宿主毫秒时钟（等待门按墙钟走）而不是虚拟 tickMs；缺省 false */
  realTime: boolean;
  /** 流式打印效果 / 诊断（stderr；stdout 仍只有最终报告） */
  trace: boolean;
  /** `--trace` 默认只打 external + unmodeled；`--trace-all` 也打 internal 记账 */
  traceAll: boolean;
  /** 每 N 步打印一次进度（stderr；0 = 关闭） */
  progress: number;
  /** 报告里带全量效果明细（`--json` 用；可能上千条） */
  effects: boolean;
  uninitializedFloat: 'throw' | 'zero';
  config: string | null;
  initConfig: boolean;
}

function parseU32(text: string, what: string): number {
  const v = /^0x/i.test(text) ? Number.parseInt(text.slice(2), 16) : Number.parseInt(text, 10);
  if (!Number.isInteger(v) || v < 0 || v > 0xffffffff) throw new Error(`${what} 必须是 u32：${text}`);
  return v >>> 0;
}

const USAGE = [
  '用法：node apps/emulator-new/src/main.ts --install <dir> [--key <u32>] [--root-id 0] [--stop-at <opcode>] [--tick-ms N] [--uninitialized-float throw|zero] [--real-time] [--trace|--trace-all] [--progress N] [--effects] [--config <SYS4REG.INI>] [--init-config] [--json] [--strict]',
  '      node apps/emulator-new/src/main.ts --script <file> [--key <u32>] [--stop-at <opcode>] [--json] [--strict]',
  '      node apps/emulator-new/src/main.ts --install <dir> --decode [--json]',
  '      node apps/emulator-new/src/main.ts --help',
].join('\n');

function parseArgs(argv: string[]): Args {
  const out: Args = {
    install: null, script: null, rootId: 0, decodeOnly: false, json: false, strict: false,
    key: null, stopAt: null, tickMs: 16, realTime: false, trace: false, traceAll: false, progress: 0, effects: false,
    uninitializedFloat: 'throw', config: null, initConfig: false,
  };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--install') out.install = argv[++i] ?? null;
    else if (a === '--script') out.script = argv[++i] ?? null;
    else if (a === '--root-id') out.rootId = parseU32(argv[++i] ?? '', '--root-id');
    else if (a === '--decode') out.decodeOnly = true;
    else if (a === '--json') out.json = true;
    else if (a === '--strict') out.strict = true;
    else if (a === '--key') {
      const key = parseU32(argv[++i] ?? '', '--key');
      if (key === 0) throw new Error('--key 必须是运行期非 0 值；省略 --key 让实现自动生成');
      out.key = key;
    }
    else if (a === '--stop-at') out.stopAt = parseU32(argv[++i] ?? '', '--stop-at');
    else if (a === '--tick-ms') out.tickMs = parseU32(argv[++i] ?? '', '--tick-ms');
    else if (a === '--real-time') out.realTime = true;
    else if (a === '--trace') out.trace = true;
    else if (a === '--trace-all') { out.trace = true; out.traceAll = true; }
    else if (a === '--effects') out.effects = true;
    else if (a === '--progress') out.progress = parseU32(argv[++i] ?? '', '--progress');
    else if (a === '--config') out.config = argv[++i] ?? null;
    else if (a === '--init-config') out.initConfig = true;
    else if (a === '--uninitialized-float') {
      const v = argv[++i];
      if (v !== 'throw' && v !== 'zero') throw new Error('--uninitialized-float 只能是 throw / zero');
      out.uninitializedFloat = v;
    } else throw new Error(`unknown argument: ${a}`);
  }
  if (out.install && out.script) throw new Error('--install 与 --script 只能给一个');
  if (out.initConfig && !out.config) throw new Error('--init-config 需要同时给 --config <file>（不写安装目录）');
  return out;
}

/** 装配核心需要的纯数据（规范表由平台层读入；核心不碰 `data/spec`） */
async function buildEnv(): Promise<CoreEnv> {
  return { host: nodeHost, table: await loadOpcodeTable(), configKeys: await loadConfigKeys() };
}

/** 跑一次 CLI；返回进程退出码（0 = 成功 / 1 = `--strict` 下的缺口 / 2 = 用法错误） */
export async function main(argv: string[]): Promise<number> {
  if (argv.includes('--help') || argv.includes('-h')) {
    console.log(USAGE);
    return 0;
  }
  let args: Args;
  try {
    args = parseArgs(argv);
  } catch (err) {
    console.error(err instanceof Error ? err.message : String(err));
    console.error(USAGE);
    return 2;
  }
  const cliClockOrigin = Date.now();
  /** `--real-time` / `--trace` / `--progress` 时捕获 Machine */
  let cliMachine: Machine | null = null;
  const sleep = (ms: number): Promise<void> => new Promise((resolve) => { setTimeout(resolve, ms); });
  let effectCursor = 0;
  let printedEffectCount = 0;
  let diagnosticCursor = 0;
  let nextProgress = args.progress;
  const wantsHook = args.realTime || args.trace || args.progress > 0;
  /** trace 只做显示转码：字体名 / 文本按 CP932 解；⛔ 不改变 effect 的原始 detail。 */
  const printableEffect = (effect: { kind: string; detail: Record<string, unknown> }): Record<string, unknown> => {
    const detail: Record<string, unknown> = { ...effect.detail };
    for (const field of ['key', 'name', 'text']) {
      const value = detail[field];
      if (typeof value === 'string') {
        const preDecoded = detail[`${field}Text`];
        if (typeof preDecoded === 'string') {
          detail[field] = preDecoded;
          delete detail[`${field}Text`];
        } else {
          detail[field] = decodeCp932(value);
        }
      }
    }
    return detail;
  };
  if (!args.install && !args.script) {
    console.error(USAGE);
    return 2;
  }
  const opts: RunOpts = {
    decodeOnly: args.decodeOnly,
    key: args.key ?? undefined,
    stopAtOpcode: args.stopAt ?? undefined,
    tickMs: args.tickMs,
    // ★ 宿主时钟（可选）：等待门 / 渐变按墙钟走；缺省虚拟 tickMs（无头快跑）。
    ...(args.realTime ? { clock: () => Date.now() - cliClockOrigin } : {}),
    // ★ 流式 / 进度 / 墙钟都需要捕获 Machine；`onIteration` 在门之前，效果与诊断已经入账。
    ...(wantsHook ? {
      onMachine: (machine: Machine) => { cliMachine = machine; },
      onIteration: async () => {
        const m = cliMachine;
        if (m === null) return;
        if (args.trace) {
          while (effectCursor < m.effects.length) {
            const effect = m.effects[effectCursor];
            effectCursor += 1;
            if (args.traceAll || effectScope(effect.kind) !== 'internal') {
              printedEffectCount += 1;
              console.error(`[effect ${printedEffectCount}] ${effect.kind} ${JSON.stringify(printableEffect(effect))}`);
            }
          }
          while (diagnosticCursor < m.diagnostics.length) {
            const diag = m.diagnostics[diagnosticCursor];
            diagnosticCursor += 1;
            console.error(`[diagnostic] ${diag.kind}${diag.index === undefined ? '' : ` @ ${diag.index}`}`);
          }
        }
        if (args.progress > 0 && m.steps >= nextProgress) {
          console.error(`[progress] step=${m.steps} ip=0x${m.ip.toString(16)} now=${m.now}ms ticks=${m.ticks}`);
          while (nextProgress <= m.steps) nextProgress += args.progress;
        }
        if (args.realTime && (m.flags & 0x400) !== 0) await sleep(4);
      },
    } : {}),
    includeEffects: args.effects || args.trace,
    uninitializedFloat: args.uninitializedFloat,
  };
  const env = await buildEnv();
  const result = args.install
    ? await analyzeInstall(env, args.install, args.rootId, opts, { config: args.config, initConfig: args.initConfig })
    : await analyze(env, args.script as string, opts);

  if (args.json) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    const src = result.source as Record<string, unknown>;
    if (src.kind === 'install') {
      console.log(`安装        ${src.dir}`);
      console.log(`索引        ${src.indexSignature} · ${(src.archives as string[]).length} 个归档 · ${src.indexEntries} 条目`);
      console.log(`脚本        id ${src.id} · ${src.name} · ${src.side === 'loose' ? '松散文件' : `归档 ${src.archive}`} · ${src.actualLength} 字节（TOC 登记 ${src.entryLength}）`);
    } else {
      console.log(`文件        ${src.file}`);
    }
    console.log(`头          ${result.signature} ${result.version.trim()} · 头长 ${result.headerLength} · 表 ${result.tables.length} 组`);
    console.log(`body        ${result.bodyLength} 字节 · 代码区上限 ${result.codeLimit}`);
    if (result.startup) {
      const c = result.startup.counts;
      console.log(`启动阶段    ${c.modeled} 建模 · ${c.partial} 部分 · ${c['not-modeled']} 未建模 · ${c['host-only']} 宿主`);
      for (const stage of result.startup.stages) {
        if (stage.disposition === 'modeled') continue;
        console.log(`  ${stage.disposition.padEnd(11)} ${stage.id}：${stage.what}（${stage.why}）`);
      }
    }
    if (result.config) {
      console.log(`配置        ${result.config.path} · 来源 ${result.config.source} · ${result.config.ints} 整数 / ${result.config.strings} 字符串 / ${result.config.lists} 列表`);
    }
    if (result.mode === 'decode') {
      console.log(`解码        ${result.decoded} 条 · 停因 ${result.stop}`);
    } else if ('stopText' in result) {
      console.log(`执行        ${result.steps} 条 · 停因 ${result.stopText}`);
      console.log(`时钟        ${result.ticks} 格 · ${result.atMs}ms · 诊断 ${result.diagnostics?.length ?? 0} 条（--trace 看明细）`);
      if (result.effectCounts) {
        const entries = Object.entries(result.effectCounts).sort((a, b) => b[1] - a[1]);
        const total = entries.reduce((n, [, c]) => n + c, 0);
        const top = entries.slice(0, 8).map(([kind, count]) => `${kind} ${count}`).join(' · ');
        console.log(`效果/请求   ${total} 条 · ${entries.length} 类${top ? `：${top}${entries.length > 8 ? ' · …' : ''}` : ''}`);
      }
      if (result.internalEffectCounts) {
        const entries = Object.entries(result.internalEffectCounts).sort((a, b) => b[1] - a[1]);
        const total = entries.reduce((n, [, c]) => n + c, 0);
        if (total > 0) {
          const top = entries.slice(0, 5).map(([kind, count]) => `${kind} ${count}`).join(' · ');
          console.log(`内部记账    ${total} 条（--trace-all 才逐条打印）：${top}${entries.length > 5 ? ' · …' : ''}`);
        }
      }
      if (result.sceneError) console.log(`场景错误    ${result.sceneError}`);
    } else {
      console.log(`执行        未启动 · 缺口 ${result.gap ? `opcode 0x${result.gap.opcode.toString(16)}` : '无'}`);
    }
    if (result.gap) {
      console.log(`缺口        ${result.mode === 'decode' ? '解码' : '未实现'} @ 第 ${result.gap.index} 条 · 偏移 ${result.gap.offset} · opcode 0x${result.gap.opcode.toString(16)}${result.gap.argc === null ? '' : ` · argc ${result.gap.argc}`} · ${result.gap.reason}`);
    } else {
      console.log('缺口        无');
    }
  }
  if (args.strict && result.gap) return 1;
  return 0;
}
