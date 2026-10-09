/**
 * apps/emulator/src/host/effects.ts —— **副作用日志**（★ 核心层：零 Node 依赖）
 *
 * headless 前端的"渲染 / 音频 / 输入"**不产生像素与声音，只产生记录** ⇒ 这一层就是它的**产物本身**，
 * 不是"没有实现的桩"。
 *
 * ★★ 每条记录必须自带 `disposition`，且三态**不许合并**：`modeled` = 引擎态真的按语义改了；
 *   `logged-only` = 只是记下来了（headless 没有对应子系统）；`not-provided` = 宿主根本没提供这能力。
 *   合并就分不清"我知道我跳过了什么"与"我连跳过了什么都不确定"，也分不清"没有输入源"与"输入源坏了"。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/effects-disposition-three-state`）。
 * ★ 域与动作名是**闭集合**、由守卫逐条核：开放字符串会让打错的动作名变成"新增一类记录"，
 *   而那类错不报错、只让统计悄悄少一块（动作名是产物的一部分）⇒ 新增动作必须同时改表与守卫。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/effects-action-closed-set`）。
 *
 * `seq` 单调递增、由日志自己发号；`atMs` 由机器在 emit 时从**注入的时钟**取样 ⇒
 * "同一脚本 + 同一初始时钟 ⇒ 逐字节相同的日志"，这是拿日志当回归基准的前提。
 */

/** 副作用所属的域（闭集合） */
export const EFFECT_DOMAINS = ['render', 'audio', 'input', 'resource', 'system'] as const;
export type EffectDomain = (typeof EFFECT_DOMAINS)[number];

/**
 * 每个域的动作名**闭集合**。新增动作必须同时改这里与守卫（有意的摩擦）。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/effects-action-closed-set`）。
 */
export const EFFECT_ACTIONS: Record<EffectDomain, readonly string[]> = {
  render: [
    'texture.bind',        // set-texture：把某个 imgid 绑到槽
    'texture.release',     // release-texture：释放槽
    'texture.create',      // create-texture：建离屏表面
    'draw-item.detach',    // detach-texture：摘掉绘制项
    'draw-item.draw',      // draw-texture：建/更新一个绘制项
    'draw-item.color',     // set-draw-color / set-draw-color-alpha
    'mesh.create',         // create-mesh
    'mesh.vertex-color',   // set-vertex-color / set-vertex-color-alpha
    'anim.window',         // 建立了一个计时窗（颜色动画）
    'anim.done',           // 计时窗跑完
  ],
  audio: [
    'movie.play',          // play-movie
    'movie.stop',
  ],
  input: [
    'poll',                // poll-input：问了一次输入状态
  ],
  resource: [
    /**
     * ★ `read.request` 与 `read.miss` / `read.hit` 是**两件事**：
     * 前者是"引擎会在这里读一份资源"（由**指令**发出，headless 不去真读 ⇒ 记 `logged-only`），
     * 后者是"核心真的去读了、结果读到/没读到"（由**文件系统层**的事实决定）。
     * 混在一起会让"这张图没读"与"这张图读了但没找到"永远分不清。
     */
    'read.request',
    'read.miss',           // 要一份资源但没读到（demand）
    'read.hit',
    'archive.mount',       // 挂载了一个归档/索引
  ],
  system: [
    'gate.open',           // 等待门（0x400）开了
    'gate.block',          // 等待门挡住了派发
    'frame.tick',          // 一帧结束、时钟前进
    'script.load',
    'script.exit',
    /**
     * ★ `0x60 random` 取了一次随机数：**取数次数本身就是复现性判据**（同种子却给出不同次数 ⇒ 控制流更早分叉）。
     *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/random-seeded-injection`）。
     */
    'random.draw',
  ],
};

/** `domains.action` 的**点分全名** */
export type EffectAction = `${EffectDomain}.${string}`;

/** 这条记录属于哪一类（★ 见文件头：三者不许合并） */
export type EffectDisposition = 'modeled' | 'logged-only' | 'not-provided';

/** 一条副作用记录（**纯数据**：可 `JSON.stringify` 往返、可 diff、可当回归基准） */
export interface EffectRecord {
  seq: number;
  /** 帧号（机器维护；从 0 起） */
  frame: number;
  /** 虚拟时钟（毫秒，由注入的时钟取样） */
  atMs: number;
  domain: EffectDomain;
  action: string;
  disposition: EffectDisposition;
  /** 结构化载荷（纯数据；**不许**放 `Uint8Array` 之类不可 JSON 往返的东西） */
  detail: Record<string, unknown>;
}

/** 收记录的地方。核心只认这一个方法 —— 于是"写到哪"完全是前端的自由（内存 / stdout / 文件 / 网络）。 */
export interface EffectSink {
  emit(record: EffectRecord): void;
}

/**
 * 一份**纯内存**的副作用日志。
 *
 * 它是 headless 前端的默认 sink，也是守卫的判据来源。发号（`seq`）在这里做，
 * **不由调用方传** —— 否则"谁先谁后"就取决于调用点写没写对。
 */
export class EffectLog implements EffectSink {
  readonly records: EffectRecord[] = [];
  /** 上一条的序号（**只用来验单调**，不参与内容）—— 见 `emit` 的注释 */
  lastSeq = -1;

  emit(record: EffectRecord): void {
    // ★ 只验**严格递增**，不验"等于当前条数"：后者会被 `drain()` 打乱
    //   （序号是"这件事在整个运行里第几个发生"，与"现在手上存着几条"是两件事）。
    if (!(record.seq > this.lastSeq)) {
      throw new Error(`副作用记录的 seq 必须严格递增（上一条 ${this.lastSeq}，收到 ${record.seq}）`);
    }
    this.lastSeq = record.seq;
    this.records.push(record);
  }

  /** 取走全部记录（清空）—— 流式消费用 */
  drain(): EffectRecord[] {
    return this.records.splice(0, this.records.length);
  }

  get length(): number {
    return this.records.length;
  }

  /** 按 `domain.action` 计数（**键升序** ⇒ 同样记录给出同样摘要） */
  countsByAction(): [string, number][] {
    const m = new Map<string, number>();
    for (const r of this.records) {
      const k = `${r.domain}.${r.action}`;
      m.set(k, (m.get(k) ?? 0) + 1);
    }
    return [...m.entries()].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  }

  /** 按 `disposition` 计数（键升序） */
  countsByDisposition(): [EffectDisposition, number][] {
    const m = new Map<EffectDisposition, number>();
    for (const r of this.records) m.set(r.disposition, (m.get(r.disposition) ?? 0) + 1);
    return [...m.entries()].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  }

  /** 一条记录的单行渲染（给 stdout 用；**不是** JSON —— JSON 由前端自己 `JSON.stringify`） */
  static line(r: EffectRecord): string {
    const detail = Object.keys(r.detail).length ? ` ${JSON.stringify(r.detail)}` : '';
    return `[${String(r.atMs).padStart(6)}ms f${String(r.frame).padStart(4)}] ${r.domain}.${r.action}${detail} (${r.disposition})`;
  }
}

/** 一条"动作名是不是这个域的合法动作"的判据（校验器与守卫共用**同一份**） */
export function isKnownAction(domain: string, action: string): boolean {
  if (!(EFFECT_DOMAINS as readonly string[]).includes(domain)) return false;
  return (EFFECT_ACTIONS[domain as EffectDomain] as readonly string[]).includes(action);
}

/**
 * 一个**记账型** sink：只数不存。给"高步数长跑"用（LOGO 那种几百帧的跑法不需要，
 * 但真链路一帧上万条记录时，存全量会把内存吃光）。
 * ★ 它与 `EffectLog` 的差别只在"存不存"，**发号与校验同源**（都走 `makeRecord`）。
 */
export class CountingSink implements EffectSink {
  readonly byAction: Map<string, number> = new Map();
  readonly byDisposition: Map<EffectDisposition, number> = new Map();
  total = 0;
  /** 上一条的序号（只用来验单调） */
  lastSeq = -1;

  emit(record: EffectRecord): void {
    if (!(record.seq > this.lastSeq)) {
      throw new Error(`副作用记录的 seq 必须严格递增（上一条 ${this.lastSeq}，收到 ${record.seq}）`);
    }
    this.lastSeq = record.seq;
    this.total += 1;
    const k = `${record.domain}.${record.action}`;
    this.byAction.set(k, (this.byAction.get(k) ?? 0) + 1);
    this.byDisposition.set(record.disposition, (this.byDisposition.get(record.disposition) ?? 0) + 1);
  }
}
