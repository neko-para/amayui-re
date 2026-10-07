/**
 * apps/emulator/src/host/environment.ts —— **环境初始化**（★ 核心层：零 Node 依赖）
 *
 * ## "环境"是什么，为什么它值得一个模块
 * 引擎要跑起来，需要的**不是一堆文件路径**，而是**一份说得清、可注入、可复现的配置**：
 * 从哪里读、往哪里写、一帧走多久、编解码用什么 key、单次跑多少步上限。
 * 这些东西有三个共同点，正是它们要住在一起的理由：
 * 1. **它们在"执行第一条指令"之前就必须全部确定** —— 跑到一半再决定"往哪写"是事故的形状；
 * 2. **它们决定复现性** —— 同一份环境 + 同一份脚本 ⇒ 同样的日志；环境不同就没有可比性；
 * 3. **它们必须可注入** —— 同一个引擎，测试要注入内存文件系统与假时钟，
 *    Electron 前端要注入 IPC 文件系统与真实时钟。
 *
 * ## ★★ 三个级别的"定不下来"，处理方式**不同**（这是本模块的核心口径）
 * | 情况 | 处理 | 为什么 |
 * |---|---|---|
 * | **非法**（id 含 `/`、帧步长 ≤ 0、memory 根为空…） | **抛** | 它不是"可以用默认值补"的东西；补了就是把错误配置伪装成合法配置 |
 * | **没给但可默认**（帧步长 / 编解码 key / 步数上限） | **用显式常量兜 + 记一条 problem** | 默认值要**看得见**（谁默认的、默认成什么），否则"我配了 vs 它默认了"分不清 |
 * | **必须给**（安装根 / 用户根） | 缺 ⇒ **抛** | 核心**不可能**猜出宿主路径：它没有 `node:os`、读不到环境变量、也不知道谁是"安装目录"。猜就是伪造 |
 *
 * ★ 为什么"必须给"而不是"给个默认的相对路径"：旧仓的教训是**资源根本曾经散落在 9 处**
 *   （4 个模块 + 5 个测试各自硬编码），结果是"换一套资源要改 9 个地方，
 *   而且测试用的语料和产品读的可以是两份"。⇒ 本仓把它收成**一个注入点**：
 *   `apps/emulator/frontends/headless/paths.ts` 是唯一的解析地，核心只接受解析结果。
 *
 * ## ★ 不用 `import.meta` / 不读环境变量 / 不改 `process.env`
 * 前两条的理由是"这一层要能在浏览器里跑"；第三条是多实例的前提 ——
 * 一份进程级的 `process.env` 无法同时表达两个实例的不同根，
 * 而"两个实例并发跑"是本仓明确要求的用法（旧仓为此专门立过守卫）。
 */

/** 一个根（只读或可写）的**身份**：人类可读标签 + 用于"同不同一块地方"判定的稳定标记 */
export interface RootRef {
  /** 人类可读标签（Node 前端给绝对路径；插件前端给插件 id）—— 只出现在日志与错误里 */
  label: string;
  /** 身份标记：**可写区与只读源同身份即拒绝**（大小写不敏感） */
  identity: string;
}

/** 环境的输入（前端解析后的结果；核心不关心它从哪来） */
export interface EnvironmentInputs {
  instanceId?: string | null;
  /** 安装根（只读来源）—— **必须给** */
  installRoot?: RootRef | null;
  /** 用户根（存档/配置的可写落点）—— **必须给** */
  userRoot?: RootRef | null;
  /** 虚拟时钟每帧前进多少毫秒 */
  frameMs?: number | null;
  /** int 族 DEC/ENC 的 key */
  codecKey?: number | null;
  /** 单次 `run()` 的步数上限（**不是**帧边界阈值，两者混用会静默改掉统计口径） */
  maxSteps?: number | null;
}

/** 一份**冻结**的环境（初始化完成后不再可变） */
export interface EngineEnvironment {
  readonly instanceId: string;
  readonly install: RootRef;
  readonly user: RootRef;
  readonly frameMs: number;
  readonly codecKey: number;
  readonly maxSteps: number;
  /** 环境初始化期间记下的**非致命**说明（用了哪个默认值之类）—— 必须能看见 */
  readonly notes: readonly string[];
}

/** 环境默认值（**显式常量**：默认值本身是产物的一部分，藏起来就没法解释"为什么这次是这样"） */
export const ENV_DEFAULTS = {
  instanceId: 'default',
  /**
   * 帧步长（毫秒）。★ 这是**宿主策略**，不是引擎事实：
   * 引擎真实帧率未观测 ⇒ 不许把它当成"引擎的帧率"写进任何结论。
   * 取 16 ms ≈ 62.5 fps：够贴近常见刷新率，而且**保持整数毫秒**（日志与窗口边界可比）。
   */
  frameMs: 16,
  /**
   * int 族编解码的 key。引擎里它是**运行期赋值**的（`Engine+0x5EC8C` 由启动代码写入一个非立即数），
   * 从不出现在脚本里 ⇒ 对脚本的**可观测行为没有影响**（DEC/ENC 互逆，key 只是把位模式重排）。
   * ⇒ 它是一个**自由参数**，这里固定成 0 以便两次运行逐字节可比；要换 key 就显式注入。
   */
  codecKey: 0,
  maxSteps: 1_000_000,
} as const;

/** 实例 id 的合法形态（★ 它会参与路径构造，所以是一条**安全**规则，不是命名品味） */
export const INSTANCE_ID_RE = /^[A-Za-z0-9._-]{1,64}$/;

/** 保留 id：会与将来的 HTTP 路由 `/api/…` 歧义（旧仓踩过） */
export const RESERVED_INSTANCE_IDS = ['api'];

/**
 * 实例 id 的校验（非法即抛）。
 * 挡三类：① 形态不符；② `.` / `..`（**路径语义会变**：`<root>/instances/..` 就是 `<root>`）；
 * ③ 保留段。★ 这三条都不是"命名品味"——漏一条，"实例根"就不再是一个可控的隔离单位。
 */
export function assertInstanceId(id: string): string {
  if (typeof id !== 'string' || !INSTANCE_ID_RE.test(id)) {
    throw new Error(`非法的实例 id：${JSON.stringify(id)} —— 只许 [A-Za-z0-9._-]{1,64}（它会参与路径构造）`);
  }
  if (id === '.' || id === '..') throw new Error(`非法的实例 id：${JSON.stringify(id)} —— 路径语义会变成上一层`);
  if (RESERVED_INSTANCE_IDS.includes(id)) throw new Error(`保留的实例 id：${JSON.stringify(id)}（会与 /api/… 路由歧义）`);
  return id;
}

/** 校验一个根：标签与身份都必须是非空串（空身份会让"同不同一块地方"永远判 false = 静默放过） */
function requireRoot(what: string, r: RootRef | null | undefined): RootRef {
  if (!r) {
    throw new Error(
      `环境缺少 ${what} —— 核心**不可能**猜出宿主路径（它没有 node:os、读不到环境变量、也不知道谁是安装目录）。` +
      `解析这一步属于前端，见 apps/emulator/frontends/`,
    );
  }
  if (typeof r.label !== 'string' || r.label === '') throw new Error(`${what} 的 label 必须是非空串`);
  if (typeof r.identity !== 'string' || r.identity === '') {
    throw new Error(`${what} 的 identity 必须是非空串（空身份会让"可写区与只读源是不是同一块地方"永远判为否）`);
  }
  return { label: r.label, identity: r.identity };
}

/**
 * **环境初始化**：注入的输入 → 一份冻结的环境（纯函数，不读盘、不读环境变量、不用 `import.meta`）。
 *
 * @returns `env` + `problems`（**非致命**的说明：哪个键用了默认值）。非法输入**直接抛**，不混进 problems ——
 *   "可以带伤跑"与"根本跑不了"必须分开。
 */
export function resolveEnvironment(inputs: EnvironmentInputs): { env: EngineEnvironment; problems: string[] } {
  const problems: string[] = [];
  const notes: string[] = [];

  // —— 实例 id：没给就用默认（记一条），给了就校验（非法 ⇒ 抛）
  let instanceId: string;
  if (inputs.instanceId === undefined || inputs.instanceId === null || inputs.instanceId === '') {
    instanceId = ENV_DEFAULTS.instanceId;
    problems.push(`instanceId 未给 ⇒ 用默认 ${JSON.stringify(ENV_DEFAULTS.instanceId)}（多实例时必须显式给，否则两个实例会指向同一份落点）`);
  } else {
    instanceId = assertInstanceId(inputs.instanceId);
  }

  // —— 两个根：必须给
  const install = requireRoot('installRoot（安装根，只读来源）', inputs.installRoot);
  const user = requireRoot('userRoot（用户根，存档/配置的可写落点）', inputs.userRoot);

  // —— 帧步长：正整数（0 会让"窗是否跑完"永远停在起点 ⇒ 死循环）
  let frameMs: number;
  if (inputs.frameMs === undefined || inputs.frameMs === null) {
    frameMs = ENV_DEFAULTS.frameMs;
    problems.push(`frameMs 未给 ⇒ 用默认 ${ENV_DEFAULTS.frameMs} ms（宿主的帧步长策略，不是引擎事实）`);
  } else {
    frameMs = inputs.frameMs;
    if (!Number.isInteger(frameMs) || frameMs <= 0) throw new Error(`frameMs 必须是正整数：${frameMs}（0 会让计时窗永远跑不完）`);
  }

  // —— 编解码 key：u32（自由参数，见 ENV_DEFAULTS.codecKey）
  let codecKey: number;
  if (inputs.codecKey === undefined || inputs.codecKey === null) {
    codecKey = ENV_DEFAULTS.codecKey;
    notes.push(`codecKey 未给 ⇒ 用 ${ENV_DEFAULTS.codecKey}（自由参数：它对脚本的可观测行为没有影响，只影响盘上位模式）`);
  } else {
    codecKey = inputs.codecKey;
    if (!Number.isInteger(codecKey) || codecKey < 0 || codecKey > 0xffffffff) throw new Error(`codecKey 必须是 u32：${codecKey}`);
  }

  // —— 步数上限
  let maxSteps: number;
  if (inputs.maxSteps === undefined || inputs.maxSteps === null) {
    maxSteps = ENV_DEFAULTS.maxSteps;
    problems.push(`maxSteps 未给 ⇒ 用默认 ${ENV_DEFAULTS.maxSteps}`);
  } else {
    maxSteps = inputs.maxSteps;
    if (!Number.isInteger(maxSteps) || maxSteps <= 0) throw new Error(`maxSteps 必须是正整数：${maxSteps}`);
  }

  return {
    env: { instanceId, install, user, frameMs, codecKey, maxSteps, notes },
    problems,
  };
}

/** 一份环境的单行描述（★ 入口必须打印它：旧仓的教训是"路径名不可靠，不做猜测"） */
export function describeEnvironment(env: EngineEnvironment): string {
  return (
    `实例 ${env.instanceId} · 安装根 ${env.install.label} · 用户根 ${env.user.label} · ` +
    `帧步长 ${env.frameMs}ms · codecKey ${env.codecKey} · maxSteps ${env.maxSteps}`
  );
}
