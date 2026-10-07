/**
 * apps/emulator/src/host/random.ts —— **随机源**（★ 核心层：零 Node 依赖）
 *
 * ## 为什么它必须是一个注入的服务（用户口径：需要）
 * `0x60 random` 是**纯数值族里唯一的非确定源**（模型里那 39 条的例外之一，见 `model/numeric-ops.ts`），
 * 语料里出现 **22 次** ⇒ 游戏**确实用到**它。而"非确定性"与另外两条硬口径直接冲突：
 * * **快照要逐字节可比**（"恢复后重跑到第 N 帧 == 自然跑到第 N 帧"）；
 * * **日志要能当回归基准**（同一脚本 + 同一环境 ⇒ 逐字节相同的日志）。
 * ⇒ 若裸用 `Math.random()`，上面两条**同时失效**，而且失效方式很隐蔽：
 *   日志里一切正常，只是两次跑不一样。
 *
 * ## ★ 所以：**种子是前端输入的一部分，而且必须能说出来**
 * 引擎自己也是这么做的 —— 取证：Engine 构造函数里 `timeGetTime()/100 → srand` 再若干次 `rand()`
 * （锚见需求单）。也就是"种子来自时刻"。我们把那一刻的输入**提到前端的参数里**：
 * 同一个种子 ⇒ 同一串随机数 ⇒ 上面两条口径重新成立；换种子 = 模拟"另一次启动"。
 * ★ 它**不**放进 `Environment`：环境管的是"读哪里 / 写哪里 / 一帧多久"这类**结构**，
 *   而种子是**这一次运行的身份**（`--rng-seed` / `AMAYUI_RNG_SEED`，前端解析后注入）。
 *   ⛔ 不许在核心内部取一次当前时刻当种子 —— 那等于把"不可复现"藏进库里。
 *
 * ## ★ 为什么**不是** `Math.random()` 的包装
 * 包装 `Math.random()` 只能做到"看起来随机"，做不到"可复现" —— 而本层存在的**唯一**理由就是可复现。
 * 所以这里自带一个**纯函数** PRNG（算法写成不可变状态推进，不依赖任何宿主 API）。
 *
 * ## ★ 取数次数也是产物
 * `draws` 是单调计数：它能回答"这一趟跑依赖了多少次随机"。这对复现性问题很关键 ——
 * 若两次跑同种子却给出不同**取数次数**，说明控制流在那之前就分叉了（而这比"结果不同"早得多、
 * 也好查得多）。⇒ 每次取数都会发一条 `system.random.draw` 副作用记录。
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
 * `state` 是引擎态（同一状态 ⇒ 同一后续序列）；`count` 是诊断（它随取数增长，混进快照
 * 会让"恢复后重跑"与"自然跑"在它上面必然分叉 ⇒ 红得没意义）。
 */
export const STATE_PARTITION: Record<string, Record<string, string>> = {
  SeededRandom: { state: 'engine', count: 'diagnostic', label: 'derived' },
};
