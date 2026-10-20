/**
 * 配置库（`data/spec/chapters/55-config.md` 的参考实现）。
 *
 * ★ 纯函数 / 纯内存：不碰文件系统。键词表由 `data/spec/format/config-keys.json` 提供，
 *   经 `spec.ts` 读入后注入（`ConfigKeySchema`）—— 本文件不抄第二份键表。
 * ★ 值的字符串是**字节串**（latin1 一字节一字符）：CP932 值在往返中不会被转码。
 */
import type { ConfigKeySchema, DefaultKeyRow, IniKeyRow, PersistKeyRow } from './spec.ts';

export interface ConfigPair {
  key: string;
  value: string;
}

export interface ConfigStats {
  ints: number;
  strings: number;
  lists: number;
}

/** C `atoi` 的前缀语义：跳前导空白、可选符号、连续十进制数字；无数字 ⇒ 0 */
export function atoiPrefix(s: string): number {
  let i = 0;
  while (i < s.length && s.charCodeAt(i) <= 0x20) i += 1;
  let sign = 1;
  if (s[i] === '+' || s[i] === '-') {
    if (s[i] === '-') sign = -1;
    i += 1;
  }
  let n = 0;
  let any = false;
  while (i < s.length) {
    const c = s.charCodeAt(i);
    if (c < 0x30 || c > 0x39) break;
    n = n * 10 + (c - 0x30);
    any = true;
    i += 1;
  }
  return any ? sign * n : 0;
}

/**
 * 一行里的“字段”：去首尾 0x00–0x20 字节，再剥一层成对双引号。
 * ★ 不去 CP932 双字节内部（前导字节 ≥ 0x80，不会被 0x00–0x20 的边界命中）。
 */
function trimField(s: string): string {
  let a = 0;
  let b = s.length;
  while (a < b && s.charCodeAt(a) <= 0x20) a += 1;
  while (b > a && s.charCodeAt(b - 1) <= 0x20) b -= 1;
  let out = s.slice(a, b);
  if (out.length >= 2 && out.startsWith('"') && out.endsWith('"')) out = out.slice(1, -1);
  return out;
}

/**
 * 逐行解析玩家文件。返回 `段名\键名 → 值`（**字节精确**，不折叠大小写）。
 * - 只按 LF 分行；行尾 CR 由字段裁剪处理。
 * - 原始首字节 `[` ⇒ 段头；取到随后的 `]` 为止。
 * - 其余行按第一个 `=` 切；没有 `=` 的行登记成空键/空值（不是注释）。
 * - 重复键：最后一次覆盖。
 */
export function parseIni(buf: Buffer): Map<string, string> {
  const text = buf.toString('latin1');
  const out = new Map<string, string>();
  let section = '';
  for (const raw of text.split('\n')) {
    if (raw === '') continue; // EOF 后的空段 / 空行：不产生可匹配条目
    if (raw.startsWith('[')) {
      const close = raw.indexOf(']');
      section = trimField(close < 0 ? raw.slice(1) : raw.slice(1, close));
      continue;
    }
    const eq = raw.indexOf('=');
    if (eq < 0) {
      out.set(`${section}\\`, '');
      continue;
    }
    out.set(`${section}\\${trimField(raw.slice(0, eq))}`, trimField(raw.slice(eq + 1)));
  }
  return out;
}

export class ConfigStore {
  readonly schema: ConfigKeySchema;
  readonly ints = new Map<string, number>();
  readonly strings = new Map<string, string>();
  readonly lists = new Map<string, string[]>();
  private readonly defaultByKey: Map<string, DefaultKeyRow>;

  constructor(schema: ConfigKeySchema) {
    this.schema = schema;
    this.defaultByKey = new Map();
    for (const row of schema.defaults) this.defaultByKey.set(row.indexKey.toUpperCase(), row);
  }

  getInt(key: string): number {
    return this.ints.get(key) ?? 0;
  }

  setInt(key: string, value: number): void {
    this.strings.delete(key);
    this.lists.delete(key);
    this.ints.set(key, value | 0);
  }

  getString(key: string): string {
    return this.strings.get(key) ?? '';
  }

  setString(key: string, value: string): void {
    this.ints.delete(key);
    this.lists.delete(key);
    this.strings.set(key, value);
  }

  appendList(key: string, value: string): void {
    this.ints.delete(key);
    this.strings.delete(key);
    let list = this.lists.get(key);
    if (!list) {
      list = [];
      this.lists.set(key, list);
    }
    list.push(value);
  }

  getList(key: string): string[] {
    return this.lists.get(key) ?? [];
  }

  stats(): ConfigStats {
    return { ints: this.ints.size, strings: this.strings.size, lists: this.lists.size };
  }

  /**
   * 发行默认表：按文件顺序写入；未知键在**写之前**就响亮失败（不建立半份配置）。
   * `antifontUnavailable` 是宿主能力（旧平台字体平滑不可用），只影响 `ENABLEANTIFONT`。
   */
  applyDefaults(pairs: readonly ConfigPair[], opts: { antifontUnavailable?: boolean } = {}): void {
    const rows: DefaultKeyRow[] = [];
    for (const p of pairs) {
      const row = this.defaultByKey.get(p.key.toUpperCase());
      if (!row) throw new Error(`config-unknown-key:${p.key}`);
      rows.push(row);
    }
    // 引擎在解析前清空两个列表容器（词表里 kind=list 的键）。
    for (const row of this.schema.defaults) if (row.kind === 'list' && row.internal) this.lists.delete(row.internal);
    for (let i = 0; i < pairs.length; i += 1) {
      const row = rows[i];
      const raw = pairs[i].value;
      if (row.kind === 'ignore' || row.special === 'ignore') continue;
      const internal = row.internal;
      if (!internal) throw new Error(`config-key-without-internal:${row.indexKey}`);
      if (row.special === 'saveVersionSplit') {
        const v = atoiPrefix(raw);
        this.setInt('set:SaveVersion1', Math.trunc(v / 100));
        this.setInt('set:SaveVersion2', v % 100);
        continue;
      }
      if (row.kind === 'list') {
        this.appendList(internal, raw);
        continue;
      }
      if (row.kind === 'string') {
        this.setString(internal, raw);
        continue;
      }
      let v = atoiPrefix(raw);
      if (row.special === 'bool') v = v !== 0 ? 1 : 0;
      if (row.indexKey.toUpperCase() === 'ENABLEANTIFONT' && opts.antifontUnavailable) v = 0;
      this.setInt(internal, v);
      for (const extra of row.extra ?? []) {
        if (extra.if === 'value!=0' && v === 0) continue;
        this.setInt(extra.internal, extra.from === 'value-1' ? v - 1 : v);
      }
    }
    // 收尾修补：菜单保存关 + 版本 1.00 ⇒ 1.10（引擎的内建迁移）。
    if (
      this.getInt('set:Menu_Save') === 0
      && this.getInt('set:SaveVersion1') === 1
      && this.getInt('set:SaveVersion2') === 0
    ) {
      this.setInt('set:SaveVersion1', 1);
      this.setInt('set:SaveVersion2', 10);
    }
  }

  /** 玩家文件覆盖：按词表顺序应用；缺键保留默认；未知条目忽略。 */
  applyIni(map: ReadonlyMap<string, string>): void {
    for (const row of this.schema.ini) {
      if (!this.conditionOpen(row)) continue;
      const got = map.get(`${row.section}\\${row.key}`);
      if (got === undefined) continue;
      if (row.kind === 'string') this.setString(row.internal, got);
      else this.setInt(row.internal, atoiPrefix(got));
    }
  }

  private conditionOpen(row: IniKeyRow | PersistKeyRow): boolean {
    return !row.if || this.getInt(row.if) !== 0;
  }

  /** 保存：按 `persist` 的固定段/键顺序写出；缺失值按 0 / 空串。 */
  serializeIni(): Buffer {
    const parts: string[] = [];
    let section = '';
    for (const row of this.schema.persist) {
      if (!this.conditionOpen(row)) continue;
      if (row.section !== section) {
        section = row.section;
        parts.push(`[${section}]\r\n`);
      }
      const value = row.kind === 'string' ? this.getString(row.internal) : String(this.getInt(row.internal));
      parts.push(`${row.key}=${value}\r\n`);
    }
    return Buffer.from(parts.join(''), 'latin1');
  }
}
