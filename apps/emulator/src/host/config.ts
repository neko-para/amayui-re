/**
 * apps/emulator/src/host/config.ts —— **配置存储抽象**（★ 核心层：零 Node 依赖）
 *
 * 引擎有一份**键 → 值**的配置（`SYS4REG.INI` 那一层：`set:SaveVersion1` 这类键），脚本通过 `0x1A0`
 * 一族指令读写它。它是**跨实例不该共享**的东西之一（另一个是存档）⇒ 每个实例**各持一份**，落点由环境注入。
 *
 * ★★ 回写是**防丢键棘轮**：`replaceAll()` 若会**丢掉一个当前在场的键**就直接抛 —— 除非调用方把它
 *   显式列进 `drop`。旧仓的事故是回写路径**先清空再写它知道的那几项**，于是一次写回就把整份 INI 抹成两行。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/config-drop-key-ratchet`）。
 * ★ 值统一是 `string`，而且**不给默认值**：提前转 number/bool 会把"引擎认不认这个写法"糊掉；
 *   静默给默认值等于把"这个键没配"伪装成"配了默认值"。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/config-values-opaque`）。
 */

/** 一条配置项（写出时的顺序由调用方给；读 API 一律**键升序**以保可比） */
export interface ConfigEntry {
  key: string;
  value: string;
}

/** 一次拒绝丢键时给出的说明 */
export interface ConfigDrop {
  key: string;
  value: string;
}

/** 键 → 值的存储。**没有默认值**：不存在的键读出来就是 `undefined`。 */
export interface ConfigStore {
  get(key: string): string | undefined;
  set(key: string, value: string): void;
  /** 已知的键（**升序**；顺序不影响语义，只影响可比性） */
  keys(): string[];
  /** 一次性替换全部内容；**会丢键且没列进 `drop` ⇒ 抛** */
  replaceAll(entries: readonly ConfigEntry[], opts?: { drop?: readonly string[] }): void;
  /** 当前的键值对（**键升序**）—— 前端拿它去落盘 */
  entries(): ConfigEntry[];
}

/** 一份**纯内存**的配置存储（headless 默认；守卫的基准） */
export class MemoryConfig implements ConfigStore {
  readonly values: Map<string, string>;

  constructor(initial?: Readonly<Record<string, string>> | readonly ConfigEntry[]) {
    this.values = new Map();
    if (Array.isArray(initial)) for (const e of initial) this.values.set(e.key, e.value);
    else if (initial) for (const [k, v] of Object.entries(initial as Record<string, string>)) this.values.set(k, v);
  }

  get(key: string): string | undefined {
    return this.values.get(key);
  }

  set(key: string, value: string): void {
    if (typeof key !== 'string' || key === '') throw new Error('配置键不许是空串');
    this.values.set(key, value);
  }

  keys(): string[] {
    return [...this.values.keys()].sort();
  }

  entries(): ConfigEntry[] {
    return this.keys().map((key) => ({ key, value: this.values.get(key) as string }));
  }

  replaceAll(entries: readonly ConfigEntry[], opts: { drop?: readonly string[] } = {}): void {
    const drop = new Set(opts.drop ?? []);
    const next = new Map<string, string>();
    for (const e of entries) {
      if (typeof e?.key !== 'string' || e.key === '') throw new Error(`非法的配置项：${JSON.stringify(e)}`);
      if (next.has(e.key)) throw new Error(`配置项重复：${e.key}（后写覆盖先写 ⇒ 会静静丢一项）`);
      next.set(e.key, e.value);
    }
    const lost: ConfigDrop[] = [];
    for (const [k, v] of this.values) if (!next.has(k) && !drop.has(k)) lost.push({ key: k, value: v });
    if (lost.length) {
      throw new Error(
        `★ 这次替换会丢掉 ${lost.length} 个在场配置键（${lost.map((l) => l.key).join(' / ')}）—— ` +
        `拒绝写回。若确实要丢，把键列进 drop（"我以为我知道全部键"必须是一次响亮的失败，不是一份静静变短的配置）`,
      );
    }
    this.values.clear();
    for (const [k, v] of next) this.values.set(k, v);
  }
}
