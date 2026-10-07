/**
 * apps/emulator/frontends/headless/run.ts —— **headless 前端的装配**（Node 侧）
 *
 * ## 它把哪些东西接在一起（这张表就是"前端"的定义）
 * | 核心要什么（接口） | Node 前端给什么 |
 * |---|---|
 * | `ReadSource`（只读） | 安装根上的**松散目录**（优先）+ **ALF 索引**（归档里那些） |
 * | `WriteArea`（可写） | 用户根（存档 / 配置） |
 * | `EffectSink` | `EffectLog`（内存）+ 由入口决定怎么落盘 |
 * | `Clock` | `VirtualClock`（虚拟时间：5 秒的窗在几毫秒的墙上时间里跑完） |
 * | `ConfigStore` | `MemoryConfig`（本批还没有 INI 解析 —— 见文件末的缺口） |
 * | `AnimationGate` / `Presenter` / `InputSource` | **都不给** —— headless 的语义就是"没有这些能力"， |
 * | | 而且"没有"会被**记账**（`not-provided`），不是静默空操作 |
 *
 * ## ★ 为什么"不给能力"要写成显式的一行（而不是什么都不写）
 * 因为"没给"与"给了个空实现"在日志里必须分得开。前者是 `not-provided`（我知道我跳过了什么），
 * 后者会让"没有输入"与"输入源坏了"永远分不清 —— 而那正是旧仓那些
 * "opcode 标着已实现、画面上什么都没发生"的来历。
 *
 * ## ★ 装配是**纯函数式**的：所有路径来自参数
 * 本文件**不读** `process.env`、**不读** `import.meta`、**不** `chdir`。
 * 环境变量的读取只在 `main.ts`（入口）做一次，然后作为参数传进来 ——
 * 于是"同一份参数 ⇒ 同一份装配"，而多实例只是"多调用一次 `createHeadlessInstance`"。
 */

import * as path from 'node:path';
import { AlfIndexSource } from './alf.ts';
import { NodeDirSource, NodeWriteArea, identityOf } from './node-fs.ts';
import { ensureDir, resolveRoots } from './paths.ts';
import type { RootResolution, RootInputs } from './paths.ts';
import { describeEnvironment, resolveEnvironment } from '../../src/host/environment.ts';
import type { EngineEnvironment } from '../../src/host/environment.ts';
import { EffectLog } from '../../src/host/effects.ts';
import { LayeredFilesystem } from '../../src/host/fs.ts';
import type { ReadSource } from '../../src/host/fs.ts';
import { Instance } from '../../src/host/instance.ts';
import type { ScriptLoader } from '../../src/host/scripts.ts';
import { MemoryConfig } from '../../src/host/config.ts';
import { VirtualClock } from '../../src/host/clock.ts';
import { SeededRandom } from '../../src/host/random.ts';
import { Machine } from '../../src/vm/machine.ts';

/** 装配选项（**全部显式**；环境变量由入口读好后放进 `env`） */
export interface HeadlessOptions extends RootInputs {
  /** ALF 索引文件名（缺省 `SYS4INI.BIN`）；给 `null` = 只用松散目录 */
  indexFile?: string | null;
  /** 是否挂 ALF 归档（缺省 true） */
  useAlf?: boolean;
  frameMs?: number | null;
  codecKey?: number | null;
  maxSteps?: number | null;
  /** 随机源种子（`0x60 random`）。缺省用 `DEFAULT_RNG_SEED` —— ★ 缺省值是常量，不是时刻 */
  rngSeed?: number | null;
}

/**
 * 默认随机种子。★ 它是一个**显式常量**，理由是"同种子 ⇒ 同日志"这条判据：
 * 取当前时刻当默认值会让两次跑不同，而那**不会报错**（只会让回归基准失效）。
 */
export const DEFAULT_RNG_SEED = 0x5eed_1234;

/** 装配出来的东西（一个 headless 实例 + 它的日志） */
export interface HeadlessAssembly {
  instance: Instance;
  machine: Machine;
  /** 内存里的副作用日志（入口决定它怎么落盘） */
  log: EffectLog;
  /** 环境 + 两个根 */
  env: EngineEnvironment;
  roots: RootResolution;
  /** 环境初始化期间的非致命说明（入口必须打出来） */
  problems: string[];
  /** 安装根上前端挂上的只读来源（诊断用） */
  sources: { label: string; kind: string }[];
}

/**
 * 装配一个 headless 实例（**不跑脚本** —— 跑什么由调用方决定）。
 *
 * ★ 顺序刻意如此：**先解析根 → 再建来源 → 再建环境 → 最后建实例**。
 *   反过来的话，"环境里写着的根"与"实际挂上的来源"可能不是一回事，
 *   而那不会报错，只会让日志里的路径与实际读的地方不一致。
 */
export function createHeadlessInstance(opts: HeadlessOptions): HeadlessAssembly {
  const roots = resolveRoots(opts);
  const problems = [...roots.problems];

  // —— 只读来源：松散目录优先，其次 ALF 归档 ——
  // ★ 顺序就是**语义**：同名的松散文件（打补丁的件）先命中，归档里的那份才轮到。
  const dirSource = new NodeDirSource(`安装根（松散文件）${roots.installRoot.label}`, roots.installRoot.label);
  const sources: ReadSource[] = [dirSource];
  const sourceKinds: { label: string; kind: string }[] = [{ label: dirSource.label, kind: 'dir' }];
  if (opts.useAlf ?? true) {
    const indexFile = opts.indexFile ?? 'SYS4INI.BIN';
    const indexPath = path.join(roots.installRoot.label, indexFile);
    const alf = new AlfIndexSource({ indexPath });
    sources.push(alf);
    sourceKinds.push({ label: `${alf.label}（${alf.entryCount} 条目 / ${alf.archiveNames().length} 归档）`, kind: 'alf' });
    problems.push(`ALF 索引已挂：${indexPath}（${alf.entryCount} 条目，${alf.archiveNames().length} 个归档）`);
  }

  // —— 可写区：用户根（**先建目录**：写的时候才建会把失败推到很远的地方）——
  ensureDir(roots.userRoot.label);
  const writable = new NodeWriteArea(`用户根${roots.userRoot.label}`, roots.userRoot.label);

  // ★ 身份标记必须**两侧同源**（都用 `identityOf`）：核心靠它判"可写区与只读源是不是同一块地方"，
  //   两边算法不同的话这条判据就永远判否 —— 那是**静默失效**，比判错更糟。
  const installIdentity = identityOf(roots.installRoot.label);
  const userIdentity = identityOf(roots.userRoot.label);

  const fs = new LayeredFilesystem({ label: `headless:${opts.instanceId}`, sources, writable });

  const { env, problems: envProblems } = resolveEnvironment({
    instanceId: opts.instanceId,
    installRoot: { label: roots.installRoot.label, identity: installIdentity },
    userRoot: { label: roots.userRoot.label, identity: userIdentity },
    frameMs: opts.frameMs,
    codecKey: opts.codecKey,
    maxSteps: opts.maxSteps,
  });
  problems.push(...envProblems);

  const log = new EffectLog();
  // ★ 脚本来源：**id → 名字**（ALF 索引）→ **字节**（分层 fs，松散文件优先）。
  //   为什么两步：引擎按 id 引用脚本（`call-script 5262`），而"名字"只用于日志；
  //   而"松散文件优先于归档"这条口径已经在分层 fs 里了 —— 不必在这里再来一遍。
  const alfSource = sources.find((s) => s instanceof AlfIndexSource) as AlfIndexSource | undefined;
  const scripts: ScriptLoader | null = alfSource
    ? {
        label: `统一文件 id → 分层 fs（${alfSource.label}）`,
        loadById(id: number) {
          const name = alfSource.nameOfId(id);
          if (!name) return null;
          const b = fs.read(name);
          return b ? { name, bytes: b } : null;
        },
      }
    : null;

  const instance = new Instance({
    env,
    fs,
    effects: log,
    clock: new VirtualClock(0),
    config: new MemoryConfig(),
    // ★ 随机源**必须给**（`0x60` 语料里 22 处；不给就该条指令响亮失败，而不是偷偷用 Math.random）。
    //   种子来自前端输入（`--rng-seed` / `AMAYUI_RNG_SEED`），**默认值是一个显式常量** ——
    //   "同种子 ⇒ 同日志"这条判据依赖它；换成时刻就等于把不可复现藏进库里。
    random: new SeededRandom(opts.rngSeed ?? DEFAULT_RNG_SEED),
    scripts,
    // ★ gate / present / input **都不给** —— 见文件头那张表
  });
  const machine = new Machine(instance);

  return { instance, machine, log, env, roots, problems, sources: sourceKinds };
}

/** 一份装配的单行描述（入口与日志都用它，避免两处各写一遍措辞） */
export function describeAssembly(a: HeadlessAssembly): string {
  return `${describeEnvironment(a.env)} · 只读来源 ${a.sources.length} 个（${a.sources.map((s) => s.kind).join('+')}）`;
}
