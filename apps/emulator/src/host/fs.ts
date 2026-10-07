/**
 * apps/emulator/src/host/fs.ts —— **宿主文件系统抽象**（★ 核心层：本文件零 Node 依赖）
 *
 * ## 为什么需要这一层（而不是直接 `fs.readFileSync`）
 * 引擎的一切都要经文件：脚本（`*.BIN`）、配置（`SYS4REG.INI`）、存档（`SAVE\SAVE.DAT`）、
 * 资源（ALF 归档 / AGF 图片）。而**同一份核心将来要跑在 Node、Electron 渲染进程、
 * 以及第三方插件宿主**里 —— 那三者的"文件"根本不是一回事：
 * * Node：真磁盘；
 * * Electron 渲染进程：**没有**本地文件访问，一切经 IPC 到主进程；
 * * 插件宿主：可能是网络、可能是只读快照。
 * ⇒ 核心**只认名字**，不认宿主路径；"名字 → 字节"由前端注入。
 *
 * ## ★★ 同步接口，理由是**引擎本身同步**（这不是偷懒）
 * 引擎在**一条指令内**完成"读文件 + 解码"：`set-texture` 之后同一条不可分割的指令序列里
 * 就敢问纹理尺寸。旧仓把这一层做成 `async`（为了适应 Electron IPC），代价是**必须在指令中间
 * `await`** —— 那正好把"一条指令是原子的"这个前提打破，实测症状是纹理尺寸读到 `0×0` 并写进
 * 绘制项的源矩形（图元永远画不出来，日志里却有"图已载入"）。
 * ⇒ 本层是**同步**的。**前端若无法同步读**（Electron 渲染进程），它必须
 * **异步预取进一份缓存，再用缓存满足同步读**；而"这次没读到"不是静默的 0 —— 见下一条。
 *
 * ## ★★ 读不到 ⇒ `null` **并且记一条 demand**（不许静默补 0 / 不许伪造）
 * 读失败有两种：`not-found`（有源但没这个名字）与 `no-source`（没有任何源认得它）。
 * 两者都返回 `null` 并进 `demands()`（按名字计数）—— 于是"前端该预取什么"是**可查询的**，
 * 而不是靠猜；同时"读不到"这件事不会被伪装成"读到了一份空文件"。
 * ★ 旧仓那条教训同源：`readHeaderSync` 未就绪时返回 `null` 而**不伪造尺寸**。
 *
 * ## ★ 路径安全是**核心的**职责，不是前端的
 * 引擎传进来的名字是**相对名**（`LOGO.BIN`、`SAVE\SAVE.DAT`）。凡是绝对路径、
 * 盘符、UNC、`..` 段、NUL 与控制字符，一律**在归一化时就判非法并抛**。
 * 理由：这类越界一旦漏到前端，前端就得各自再实现一遍（而"各自实现一遍"正是它会被漏掉的原因）。
 * ⇒ 归一化是**纯函数**（`normalizeEngineName`），可以脱离任何宿主单测。
 *
 * ## ★ 可写区与只读源必须**不同身份**（构造期不变式）
 * "写进只读语料"是最贵的一类事故（原始语料逐字节忠实是 `AGENTS.md` §1 的第 5 条纪律）。
 * 光靠"调用方小心"不够 ⇒ 每个源与可写区都带一个 `identity` 标签，
 * **可写区的 identity 一旦与任何一个只读源相同（大小写不敏感）就拒绝构造** ——
 * 于是"把 overlay 指到 base"这条路根本走不通（旧仓是在写的那一刻才拒，本层更早）。
 */

// ───────────────────────────────────────────────────────── 名字规则（纯函数）

/** 名字归一化的结果：合法 ⇒ `ok`，非法 ⇒ `reason`（**不抛**，便于守卫逐条列反例） */
export type NameCheck =
  | { ok: true; /** 归一化后的名字（`/` 分隔、无重复分隔符、无首尾分隔符） */ normalized: string; /** 大小写不敏感的查表键 */ key: string; /** 路径段（已归一化） */ segments: string[] }
  | { ok: false; reason: string; input: string };

/** Windows 保留字符之外还要挡的：控制字符与 NUL */
const CONTROL_RE = /[\u0000-\u001f\u007f]/;

/**
 * 把引擎侧的名字归一化并判合法性。**纯函数**。
 *
 * 规则（每条都有具体事故在背后）：
 * 1. **`\` 与 `/` 等价**：引擎在 Windows 上用 `\`，脚本/配置里两种都出现过。
 * 2. **大小写不敏感**：Windows 语义；`key` 一律小写 ⇒ 查表不用两个候选（旧仓的 `#nameIndex`
 *    也是 `toLowerCase()`）。
 * 3. **拒绝绝对路径**：`\foo`、`/foo`、`C:...`、`\\server\share`。引擎的名字永远是相对的。
 * 4. **拒绝 `..` 与 `.` 段**：`..` 是逃逸面；`.` 段会让"同一个东西两个名字"（查表会分叉）。
 * 5. **重复的分隔符折叠掉**（`a//b` ⇒ `a/b`）：Windows 语义就是折叠，而且折叠后
 *    "同一个东西两个名字"这件事**不存在** ⇒ 不需要（也不该）为它写一条永远不成立的检查。
 *    ⇒ 所以本函数**没有**"空路径段"这条分支：`/[\\/]+/` 切分保证段不可能为空。
 *    （★ 一条永远不触发的检查 = 恒真守卫，本仓明确反对：见 `docs/00-origin/decisions.md` §6。）
 * 6. **拒绝 NUL 与控制字符**：它们会让下游（原生 API）截断名字，于是"你检查的"和"它打开的"
 *    是两个不同的文件。
 */
export function normalizeEngineName(input: string): NameCheck {
  if (typeof input !== 'string' || input.length === 0) return { ok: false, reason: '空名字', input: String(input) };
  if (CONTROL_RE.test(input)) return { ok: false, reason: '含控制字符或 NUL（下游原生 API 会在此截断名字）', input };
  // 绝对 / 盘符 / UNC：先看原始形态（归一化会把 `\` 变成 `/`，两种都要挡）
  if (/^[\\/]/.test(input)) return { ok: false, reason: '绝对路径（引擎的名字永远是相对的）', input };
  if (/^[A-Za-z]:/.test(input)) return { ok: false, reason: '带盘符（引擎的名字永远是相对的）', input };
  const rawSegments = input.split(/[\\/]+/); // ★ `+` ⇒ 重复分隔符被折叠，段不可能为空
  const segments: string[] = [];
  for (let i = 0; i < rawSegments.length; i += 1) {
    const s = rawSegments[i];
    if (s === '') continue; // 只可能是末尾分隔符造成的
    if (s === '..') return { ok: false, reason: '含 `..` 段（越界逃逸面）', input };
    if (s === '.') return { ok: false, reason: '含 `.` 段（同一对象两个名字会让查表分叉）', input };
    segments.push(s);
  }
  if (segments.length === 0) return { ok: false, reason: '没有有效路径段', input };
  const normalized = segments.join('/');
  return { ok: true, normalized, key: normalized.toLowerCase(), segments };
}

/** 归一化并**直接拿结果**（非法即抛）—— 给"名字来自可信调用方"的内部路径用 */
export function requireEngineName(input: string): { normalized: string; key: string; segments: string[] } {
  const r = normalizeEngineName(input);
  if (!r.ok) throw new Error(`非法的引擎侧名字：${JSON.stringify(input)} —— ${r.reason}`);
  return { normalized: r.normalized, key: r.key, segments: r.segments };
}

// ───────────────────────────────────────────────────────── 读源 / 可写区 / 文件系统

/** 一次"读不到"的记录（按名字聚合；`count` = 被问了几次） */
export interface FsDemand {
  name: string;
  why: 'not-found' | 'no-source';
  count: number;
}

/**
 * 一个**只读**来源。前端注入的每一份都实现它（真磁盘目录 / ALF 归档 / IPC 缓存 / 内存）。
 *
 * `identity` 是"这是哪块地方"的**稳定标记**（Node 前端用解析后的绝对路径，
 * 插件前端用插件 id）。它只用于一条判据：**可写区不许与任何只读源同身份**。
 */
export interface ReadSource {
  readonly label: string;
  readonly identity: string;
  /** 读整个文件；**读不到返回 `null`，不抛**（"没有"与"坏了"是两件事） */
  read(name: string): Uint8Array | null;
  /**
   * 读前 `maxBytes` 字节（文件更短就返回多少算多少，**不补齐** —— 补齐会把坏文件伪装成好文件）。
   * ★ 为什么需要：存档这类 1~1.5 MB 的文件会被**每帧问上百次**（旧仓实测整份读 84~117 ms/次
   *   ⇒ 一帧 4.8 s）；只读前缀回到 ~5 ms。
   */
  readPrefix(name: string, maxBytes: number): Uint8Array | null;
  /** 列一层目录（**不递归**）；不支持 ⇒ `null`（"不支持"与"空目录"必须可区分） */
  list(dir: string): string[] | null;
}

/** 一个**可写**落点（每个实例一份；通常就是"这个实例自己的存档/配置目录"） */
export interface WriteArea {
  readonly label: string;
  readonly identity: string;
  /** 写入（**要么全写成、要么抛**；不许"写了一半再报错"） */
  write(name: string, bytes: Uint8Array): void;
  /** 读回自己写的东西（先只读源、后本区；见 `LayeredFilesystem.read`） */
  read(name: string): Uint8Array | null;
  readPrefix(name: string, maxBytes: number): Uint8Array | null;
  list(dir: string): string[] | null;
  /** 删掉；返回"是否真的删了一个" */
  remove(name: string): boolean;
}

/** 读的结果里带"命中哪一层"（诊断用：**每次读都能说出命中了谁**，旧仓同口径） */
export interface FsHit {
  bytes: Uint8Array;
  side: string;
}

/**
 * 核心看到的文件系统。**只有"名字"这一个维度** —— 没有宿主路径、没有 `node:fs`。
 *
 * 读的顺序：`sources[0] → sources[1] → … → writable`（**先命中先用**）。
 * 为什么可写区排在最后：它代表"玩家/本实例自己的改动"，而只读源是"更权威的来源"？
 * ★ 恰恰相反，这里的顺序是**改动优先**：`writable` 放最后只是为了让"只读源先说话"这条默认
 * 行为不至于让**本实例刚写的东西读不回来** —— 所以 `read` 会先问 sources，再问 writable；
 * 如果你要"改动覆盖来源"（overlay 语义），把 overlay 作为 **source[0]** 注入即可。
 * ⇒ **顺序是注入方的显式选择**，核心不替它猜。（这就是"分层"与"overlay"的区别：
 *   前者是"找得到就用"，后者是"我的改动优先"。两种都由同一个接口表达。）
 */
export class LayeredFilesystem {
  readonly label: string;
  readonly sources: ReadSource[];
  readonly writable: WriteArea | null;
  /** 读不到的记录（按名字聚合）—— 诊断，不是引擎态 */
  readonly demands: Map<string, FsDemand>;
  /** 读命中的来源计数（label → 次数）：**每次读都能说出命中了谁** */
  readonly hits: Map<string, number>;

  constructor(opts: { label: string; sources?: ReadSource[]; writable?: WriteArea | null }) {
    this.label = opts.label;
    this.sources = [...(opts.sources ?? [])];
    this.writable = opts.writable ?? null;
    this.demands = new Map();
    this.hits = new Map();
    // ★ 构造期不变式：可写区与任一**只读源**同身份 ⇒ 拒绝。
    //   判据用 identity（大小写不敏感，Windows 语义），而不是"路径字符串长得像"。
    if (this.writable) {
      const w = this.writable.identity.toLowerCase();
      const clash = this.sources.find((s) => s.identity.toLowerCase() === w);
      if (clash) {
        throw new Error(
          `可写区 ${this.writable.label}(${this.writable.identity}) 与只读源 ${clash.label} 是同一块地方 ` +
          `⇒ 拒绝构造：写下去就是覆盖只读来源（原始语料/真游戏数据）`,
        );
      }
    }
  }

  /** 读整个文件（找不到 ⇒ `null` + 记 demand） */
  read(name: string): Uint8Array | null {
    const hit = this.locate(name);
    return hit ? hit.bytes : null;
  }

  /** 读前 `maxBytes` 字节（找不到 ⇒ `null` + 记 demand；文件更短 ⇒ **不补齐**） */
  readPrefix(name: string, maxBytes: number): Uint8Array | null {
    const hit = this.locatePrefix(name, maxBytes);
    return hit ? hit.bytes : null;
  }

  /** 读 + 说出命中哪一层 */
  locate(name: string): FsHit | null {
    const { key, normalized } = requireEngineName(name);
    for (const s of this.sources) {
      const b = s.read(normalized);
      if (b) return this.#hit(s.label, key, b);
    }
    if (this.writable) {
      const b = this.writable.read(normalized);
      if (b) return this.#hit(this.writable.label, key, b);
    }
    this.#demand(key, this.sources.length === 0 && !this.writable ? 'no-source' : 'not-found', normalized);
    return null;
  }

  locatePrefix(name: string, maxBytes: number): FsHit | null {
    const { key, normalized } = requireEngineName(name);
    for (const s of this.sources) {
      const b = s.readPrefix(normalized, maxBytes);
      if (b) return this.#hit(s.label, key, b);
    }
    if (this.writable) {
      const b = this.writable.readPrefix(normalized, maxBytes);
      if (b) return this.#hit(this.writable.label, key, b);
    }
    this.#demand(key, this.sources.length === 0 && !this.writable ? 'no-source' : 'not-found', normalized);
    return null;
  }

  /** 存在吗（不记 demand：这不是"要读它"，只是问一句） */
  exists(name: string): boolean {
    const { normalized } = requireEngineName(name);
    for (const s of this.sources) if (s.readPrefix(normalized, 1) !== null) return true;
    return this.writable ? this.writable.readPrefix(normalized, 1) !== null : false;
  }

  /** 写进可写区；**没有可写区 ⇒ 抛**（"只读运行"这件事不许表现成"写成功了"） */
  write(name: string, bytes: Uint8Array): void {
    if (!this.writable) {
      throw new Error(`本文件系统 ${this.label} 没有可写区 ⇒ 拒绝写 ${JSON.stringify(name)}（只读运行不是"写成功了"）`);
    }
    const { normalized } = requireEngineName(name);
    this.writable.write(normalized, bytes);
  }

  /** 列一层目录：把每个源的列表并起来（**去重、升序**；顺序不影响语义，但影响可比性） */
  list(dir = ''): string[] {
    const { normalized } = requireEngineName(dir === '' ? '.' : dir);
    const out = new Set<string>();
    for (const s of this.sources) for (const n of s.list(normalized) ?? []) out.add(n);
    if (this.writable) for (const n of this.writable.list(normalized) ?? []) out.add(n);
    return [...out].sort();
  }

  /** demand 摘要（名字升序 ⇒ 同样状态给出同样输出） */
  demandsSorted(): FsDemand[] {
    return [...this.demands.values()].sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  }

  #hit(side: string, key: string, bytes: Uint8Array): FsHit {
    this.hits.set(side, (this.hits.get(side) ?? 0) + 1);
    return { bytes, side };
  }

  #demand(key: string, why: FsDemand['why'], name: string): void {
    const prev = this.demands.get(key);
    if (prev) { prev.count += 1; return; }
    this.demands.set(key, { name, why, count: 1 });
  }
}

// ───────────────────────────────────────────────────────── 纯内存实现（测试与守卫的基准）

/**
 * 一份**纯内存**的文件系统（可当只读源、也可当可写区）。**零 Node 依赖**。
 *
 * 它有两个用途，两者都不是"为了测试而测试"：
 * 1. **守卫的基准**：多实例隔离、路径逃逸、写只读源这三条判据要能在 `pnpm test`（`@env pure`）
 *    里跑，而那档**不许碰真实语料**；
 * 2. **协议的可执行定义**：`ReadSource` / `WriteArea` 的语义（"读不到返回 null"、
 *    "list 不递归"、"写要么全成要么抛"）在这里有一份**最直白的**实现，
 *    前端适配器照着它对齐即可。
 */
export class MemoryStore implements ReadSource, WriteArea {
  readonly label: string;
  readonly identity: string;
  /** key（小写归一化名）→ 字节（**存副本**：调用方改自己手上那份不该影响这里） */
  readonly files: Map<string, Uint8Array>;
  /** key → 归一化后的展示名（`list()` 要还回原始大小写） */
  readonly names: Map<string, string>;

  constructor(label: string, identity = label) {
    this.label = label;
    this.identity = identity;
    this.files = new Map();
    this.names = new Map();
  }

  /** 放一份文件进去（构造测试夹具用；也允许当可写区被 `write` 调） */
  put(name: string, bytes: Uint8Array): void {
    const { key, normalized } = requireEngineName(name);
    this.files.set(key, copyBytes(bytes));
    this.names.set(key, normalized);
  }

  write(name: string, bytes: Uint8Array): void {
    this.put(name, bytes);
  }

  read(name: string): Uint8Array | null {
    const { key } = requireEngineName(name);
    const b = this.files.get(key);
    return b ? copyBytes(b) : null;
  }

  readPrefix(name: string, maxBytes: number): Uint8Array | null {
    const { key } = requireEngineName(name);
    const b = this.files.get(key);
    if (!b) return null;
    const n = Math.max(0, Math.min(maxBytes, b.length));
    return b.slice(0, n); // ★ 文件更短就返回短的，**不补齐**
  }

  /** 列**一层**目录（不递归）：`dir/` 的直接子项（目录名带尾 `/`） */
  list(dir = ''): string[] {
    const prefix = dir === '' || dir === '.' ? '' : `${requireEngineName(dir).normalized}/`;
    const out = new Set<string>();
    for (const [key, shown] of this.names) {
      if (!key.startsWith(prefix)) continue;
      const rest = shown.slice(prefix.length);
      if (rest === '') continue;
      const slash = rest.indexOf('/');
      out.add(slash < 0 ? rest : `${rest.slice(0, slash)}/`);
    }
    return [...out].sort();
  }

  remove(name: string): boolean {
    const { key } = requireEngineName(name);
    this.names.delete(key);
    return this.files.delete(key);
  }
}

/** 复制一份字节（`slice()` 在 `Uint8Array` 上是**拷贝**，不是视图 —— 这里正需要拷贝） */
export function copyBytes(b: Uint8Array): Uint8Array {
  return b.slice(0);
}
