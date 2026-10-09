/**
 * apps/emulator/src/host/random.ts —— **随机源**（★ 核心层：零 Node 依赖）
 *
 * `0x60 random` 是**纯数值族里唯一的非确定源**（模型里那 39 条的例外之一，见 `model/numeric-ops.ts`），
 * 语料里出现 **22 次** ⇒ 游戏**确实用到**它。而它必须**注入**（种子来自前端参数、默认值是显式常量）：
 * 裸用 `Math.random()` 会让"快照逐字节可比"与"日志能当回归基准"两条口径**同时失效**，而且失效方式
 * 很隐蔽（日志里一切正常，只是两次跑不一样）；种子**不许**在核心内部取当前时刻 —— 那等于把
 * "不可复现"藏进库里。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/random-seeded-injection`）。
 *
 * ★ 引擎自己也是这么做的（种子来自时刻）：台账 subject `Engine-ctor/timeGetTime-to-srand`
 *   —— `.lst:33783-33789`：`timeGetTime` → `÷100` → `srand`（→ 若干次 `rand()`）。我们把那一刻的
 *   输入**提到前端的参数里**（`--rng-seed` / `AMAYUI_RNG_SEED`）：同种子 ⇒ 同一串随机数；换种子 = "另一次启动"。
 *   （种子**不**放进 `Environment`：环境管"读哪里 / 写哪里 / 一帧多久"这类**结构**，而种子是**这一次运行的身份**。）
 * ★ 取数次数也是产物：每次取数发一条 `system.random.draw`（同种子却给出不同次数 ⇒ 控制流更早分叉）。
 */

/** 一个随机源。**同步**（引擎的一条指令内就要拿到数）。 */
export interface RandomSource {
  readonly label: string;
  /** 取下一个 u32 */
  nextU32(): number;
  /** 取过多少次（单调；诊断 + 复现性判据） */
  draws(): number;
}

/**
 * **mulberry32**：32 位状态的纯 PRNG。
 *
 * 选它的理由（不是为了"质量好"）：状态只有 32 位 ⇒ **能整个进快照**，且推进是纯算术
 * （`>>>` / `Math.imul` / 加法），在任何宿主上逐位一致。需要密码学质量的地方不该用它，
 * 而这里要的是"同种子同序列"。
 */
export class SeededRandom implements RandomSource {
  readonly label: string;
  /** 当前状态（**进快照**：随机源的状态就是引擎态的一部分） */
  state: number;
  /** 取数次数（诊断，不进快照） */
  count = 0;

  constructor(seed: number, label = `seeded(0x${(seed >>> 0).toString(16)})`) {
    this.state = seed >>> 0;
    this.label = label;
  }

  nextU32(): number {
    this.count += 1;
    // mulberry32：state += 0x6D2B79F5；然后两轮 xor-shift-multiply
    this.state = (this.state + 0x6d2b79f5) >>> 0;
    let t = this.state;
    t = Math.imul(t ^ (t >>> 15), t | 1) >>> 0;
    t = (t ^ (t + Math.imul(t ^ (t >>> 7), t | 61))) >>> 0;
    return (t ^ (t >>> 14)) >>> 0;
  }

  draws(): number {
    return this.count;
  }

  /** 快照（只记状态；`count` 是诊断） */
  snapshot(): { state: number } {
    return { state: this.state >>> 0 };
  }

  /** 从快照重建（**新实例**：`count` 归零是有意的 —— 它只描述"这一趟"） */
  static restore(snap: { state: number }, label?: string): SeededRandom {
    if (!Number.isInteger(snap?.state)) throw new Error(`随机源快照缺 state：${JSON.stringify(snap?.state)}`);
    return new SeededRandom(snap.state, label ?? `seeded(restored)`);
  }
}

/**
 * ★ 状态分区（口径同 `model/pools.ts`）：`engine` 类的字段恰好是快照的顶层键。
 * `state` 是引擎态；`count` 是诊断（它随取数增长，混进快照会让"恢复后重跑"与"自然跑"必然分叉）。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/state-partition-engine-is-snapshot-keys`）。
 */
export const STATE_PARTITION: Record<string, Record<string, string>> = {
  SeededRandom: { state: 'engine', count: 'diagnostic', label: 'derived' },
};
