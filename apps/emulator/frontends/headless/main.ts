/**
 * apps/emulator/frontends/headless/main.ts —— **headless 前端入口**（Node 侧）
 *
 * ```
 * node apps/emulator/frontends/headless/main.ts \
 *      --install <游戏安装目录> [--user <用户根>] [--instance <id>] \
 *      [--script LOGO.BIN] [--stop-at play-movie] [--log -|<文件>] [--json]
 * ```
 *
 * ## 它做四件事，顺序不许变
 * 1. **解析根**（`paths.ts` 是唯一解析点；环境变量只在这里读一次）；
 * 2. **装配实例**（`run.ts`：只读来源 + 可写区 + 日志 + 虚拟时钟 + 配置）；
 * 3. **装载脚本并跑**（`LOGO.BIN` 到 `play-movie` —— 见 `--stop-at`）；
 * 4. **把日志与结论打出来**（人读的 + `--json` 机器读的）。
 *
 * ## ★ 三条"不许"（都是本仓纪律，不是偏好）
 * 1. **不改 `process.env`**：多实例的前提。"这个实例用哪个根"只经由参数传递。
 * 2. **不把结论写进散文**：跑完只输出**可复核的事实**（停止原因 / 步数 / 帧数 / 日志条数与分类），
 *    "这条指令的语义是什么"属于知识层（`data/ledger/` + `AGENTS.md` §6）。
 * 3. **不用 `import.meta` 猜仓库根**：`--repo` 或 `process.cwd()`，显式给。
 */

import * as fs from 'node:fs';
import * as path from 'node:path';
import { createHeadlessInstance, describeAssembly } from './run.ts';
import { buildChainReport, describeChainReport } from './chain-report.ts';
import { describeStop } from '../../src/vm/machine.ts';
import { EffectLog, isKnownAction } from '../../src/host/effects.ts';
import type { EffectRecord } from '../../src/host/effects.ts';

/** 默认的停止点：`0x20F play-movie` —— 本轮目标的里程碑 */
const DEFAULT_STOP_NAME = 'play-movie';

/** 命令行里能写的助记符 → opcode（只列本入口要用的那几个；表本身在 `instruction-set.json`） */
const STOP_BY_NAME: Record<string, number> = {
  'play-movie': 0x20f,
  exit: 0x02,
  wait: 0x21c,
};

interface Cli {
  repoRoot: string;
  instanceId: string;
  install?: string;
  user?: string;
  /**
   * 直装模式：按**名字**装载一份脚本（跳过启动链，例如 `--script LOGO.BIN`）。
   * ★ 不给 ⇒ **标准启动流程**：按统一文件 id 装载根脚本（`--root-id`，缺省 0 = `SYSTEM4.BIN`）。
   */
  script?: string;
  /** 标准启动流程的根脚本 id（缺省 0；已登记观察：根脚本 = 统一文件 id 0） */
  rootId: number;
  stopAt: string;
  stopOpcode: number;
  frameMs?: number;
  maxSteps?: number;
  codecKey?: number;
  /** 随机源种子（`0x60 random`）—— ★ 必须能从命令行说出来，否则"同种子同日志"这条判据落不了地 */
  rngSeed?: number;
  indexFile?: string;
  noAlf: boolean;
  log: string;
  json: boolean;
  quiet: boolean;
  userSpecified: boolean;
  /** 只出**启动链缺口报告**（不跑）—— 把"实现启动链"变成有数字的工作清单，且可复跑 */
  chainReport: boolean;
}

function parseArgv(argv: string[]): Cli {
  const get = (name: string): string | undefined => {
    const i = argv.indexOf(name);
    return i >= 0 ? argv[i + 1] : undefined;
  };
  const has = (name: string): boolean => argv.includes(name);
  const repoRoot = path.resolve(get('--repo') ?? process.cwd());
  const stopAt = get('--stop-at') ?? DEFAULT_STOP_NAME;
  const stopOpcode = /^0x[0-9a-f]+$/i.test(stopAt)
    ? Number.parseInt(stopAt, 16)
    : (STOP_BY_NAME[stopAt] ?? Number.parseInt(stopAt, 10));
  if (!Number.isInteger(stopOpcode) || stopOpcode <= 0) {
    throw new Error(`--stop-at 认不出：${JSON.stringify(stopAt)}（给助记符、十进制或 0x 十六进制）`);
  }
  const user = get('--user');
  return {
    repoRoot,
    instanceId: get('--instance') ?? 'default',
    install: get('--install'),
    user,
    userSpecified: user !== undefined,
    script: get('--script'),
    rootId: get('--root-id') ? Number.parseInt(get('--root-id') as string, 10) : 0,
    stopAt,
    stopOpcode,
    frameMs: get('--frame-ms') ? Number.parseInt(get('--frame-ms') as string, 10) : undefined,
    maxSteps: get('--max-steps') ? Number.parseInt(get('--max-steps') as string, 10) : undefined,
    codecKey: get('--codec-key') ? Number.parseInt(get('--codec-key') as string, 10) : undefined,
    rngSeed: get('--rng-seed') ? Number.parseInt(get('--rng-seed') as string, 10) : undefined,
    indexFile: get('--index'),
    noAlf: has('--no-alf'),
    log: get('--log') ?? '-',
    json: has('--json'),
    quiet: has('--quiet'),
    chainReport: has('--chain-report'),
  };
}

function main(): number {
  const cli = parseArgv(process.argv.slice(2));

  // ★ 环境变量**只在这里读一次**，然后作为参数往下传（装配层不读它）
  const env = {
    install: process.env.AMAYUI_INSTALL_DIR ?? null,
    user: process.env.AMAYUI_USER_DIR ?? null,
  };

  const asm = createHeadlessInstance({
    repoRoot: cli.repoRoot,
    instanceId: cli.instanceId,
    cliInstall: cli.install ?? null,
    cliUser: cli.user ?? null,
    env,
    frameMs: cli.frameMs ?? null,
    maxSteps: cli.maxSteps ?? null,
    codecKey: cli.codecKey ?? null,
    rngSeed: cli.rngSeed ?? null,
    indexFile: cli.indexFile ?? null,
    useAlf: !cli.noAlf,
  });

  const out: string[] = [];
  if (!cli.quiet) {
    out.push(`[环境] ${describeAssembly(asm)}`);
    for (const p of asm.problems) out.push(`[环境·说明] ${p}`);
    if (!cli.userSpecified && !env.user) {
      out.push('[环境·注意] 用户根落在仓库内的 .tmp（**故意不**指向玩家真实存档目录）；要跑真数据请显式 --user');
    }
  }

  // —— `--chain-report`：只算启动链的指令缺口，不跑任何脚本 ——
  if (cli.chainReport) {
    // ★ 必须走**分层 fs**（而不是"只读归档"）：实测这台安装里有 107 个松散 `.BIN`，
    //   松散 `SYSTEM4.BIN` = 12012 B，而归档 TOC 声明 11992 B ⇒ 两条路径拿到的是**两份不同的输入**。
    //   报告要与**实跑**量同一份东西（否则"缺口清单"描述的是一份没人跑的文件）。
    const report = buildChainReport(asm.instance.fs, path.join(asm.env.install.label, cli.indexFile ?? 'SYS4INI.BIN'));
    for (const line of out) console.log(line);
    console.log(describeChainReport(report));
    if (cli.json) console.log(JSON.stringify(report, null, 2));
    return 0;
  }

  // —— 装载入口脚本：**直装模式**（按名字）或**标准启动流程**（按统一文件 id）——
  if (cli.script) {
    const bytes = asm.instance.fs.read(cli.script);
    if (!bytes) {
      out.push(`[失败] 读不到脚本 ${cli.script} —— 已记入 demand：`);
      for (const d of asm.instance.fs.demandsSorted()) out.push(`  · ${d.name} (${d.why} × ${d.count})`);
      if (!cli.quiet) for (const line of out) console.log(line);
      return 2;
    }
    if (!cli.quiet) out.push(`[脚本] ${cli.script} = ${bytes.length} 字节（**直装模式**：不经过启动链）`);
    asm.machine.loadScriptBytes(cli.script, bytes);
  } else {
    let rootName: string | null = null;
    try {
      const root = asm.instance.scripts?.loadById(cli.rootId) ?? null;
      rootName = root?.name ?? null;
      if (!root) {
        out.push(`[失败] 取不到根脚本：统一文件 id ${cli.rootId}（0x${cli.rootId.toString(16)}）—— 来源 ${asm.instance.scripts?.label ?? '(没有脚本来源)'}`);
        if (!cli.quiet) for (const line of out) console.log(line);
        return 2;
      }
      if (!cli.quiet) out.push(`[启动] 根脚本 id ${cli.rootId} = ${root.name}（${root.bytes.length} 字节）—— **标准启动流程**`);
      asm.machine.loadScriptById(cli.rootId);
    } catch (err) {
      out.push(`[失败] 装载根脚本时抛错：${err instanceof Error ? err.message : String(err)}`);
      if (!cli.quiet) for (const line of out) console.log(line);
      return 2;
    }
    void rootName;
  }
  const result = asm.machine.run({ stopAtOpcode: cli.stopOpcode });

  if (!cli.quiet) {
    out.push(`[停止] ${describeStop(result.reason)}`);
    out.push(`[统计] 步数 ${result.steps} · 帧数 ${result.ticks} · 虚拟时刻 ${result.atMs}ms · 副作用 ${asm.log.length} 条`);
    out.push('[副作用·按动作] ' + asm.log.countsByAction().map(([k, n]) => `${k}=${n}`).join(' '));
    out.push('[副作用·按归类] ' + asm.log.countsByDisposition().map(([k, n]) => `${k}=${n}`).join(' '));
    // ★★ **保真欠账**必须单独报出来（`engine.forward` 那些是"**跳过了**这次子系统调用、只记了一笔"）。
    //    ★ 键是**带域前缀**的（`system.engine.forward`）—— 用 endsWith 匹配，别写死前缀。
    //    口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `headless/main-fidelity-debt-and-exit-code`）。
    const forwarded = asm.log.countsByAction().find(([k]) => k.endsWith('.engine.forward'))?.[1] ?? 0;
    if (forwarded > 0) {
      out.push(`[保真欠账] 未建模的子系统调用 ${forwarded} 次（只记录、未建模）—— **这些不算已完成**，清单见 --json 的 effects`);
    }
    const oob = [...asm.machine.diag.oobByKind.entries()].sort();
    if (oob.length) out.push('[留痕] ' + oob.map(([k, n]) => `${k}=${n}`).join(' · '));
  }

  // —— 日志落点 ——
  const text = asm.log.records.map((r) => EffectLog.line(r)).join('\n');
  if (cli.log === '-') {
    if (!cli.quiet) { out.push(''); out.push('[副作用·逐条]'); out.push(text); }
  } else {
    fs.mkdirSync(path.dirname(path.resolve(cli.log)), { recursive: true });
    fs.writeFileSync(cli.log, asm.log.records.map((r) => JSON.stringify(r)).join('\n') + '\n');
    out.push(`[日志] ${asm.log.length} 条写到 ${path.resolve(cli.log)}（JSON Lines）`);
  }
  if (!cli.quiet) for (const line of out) console.log(line);

  if (cli.json) {
    console.log(JSON.stringify({
      ok: result.reason.kind === 'instruction',
      instance: asm.env.instanceId,
      environment: { install: asm.env.install.label, user: asm.env.user.label, frameMs: asm.env.frameMs, codecKey: asm.env.codecKey },
      script: cli.script,
      stop: result.reason,
      steps: result.steps,
      ticks: result.ticks,
      atMs: result.atMs,
      effects: { total: asm.log.length, byAction: asm.log.countsByAction(), byDisposition: asm.log.countsByDisposition() },
      notes: [...asm.machine.diag.oobByKind.entries()].sort(),
      fsDemands: asm.instance.fs.demandsSorted(),
      unknownEffectActions: unknownActions(asm.log.records),
    }, null, 2));
  }

  // ★ 退出码的判据是"**有没有到达停止点**"，不是"有没有抛"（跑到一半因未实现的指令停下，在 CI 里必须算失败）。
  //   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `headless/main-fidelity-debt-and-exit-code`）。
  return result.reason.kind === 'instruction' ? 0 : 1;
}

/** 有没有"记了一条不在闭集合里的动作"（`EFFECT_ACTIONS` 是产物的一部分，写错了要能被看见） */
function unknownActions(records: readonly EffectRecord[]): string[] {
  const bad = new Set<string>();
  for (const r of records) if (!isKnownAction(r.domain, r.action)) bad.add(`${r.domain}.${r.action}`);
  return [...bad].sort();
}

process.exitCode = main();
