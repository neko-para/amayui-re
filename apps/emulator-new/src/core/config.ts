/**
 * 配置库（`data/spec/chapters/55-config.md` 的参考实现）。
 *
 * ★ 纯函数 / 纯内存：不碰文件系统。键词表由 `data/spec/format/config-keys.json` 提供，
 *   经 `spec.ts` 读入后注入（`ConfigKeySchema`）—— 本文件不抄第二份键表。
 * ★ 值的字符串是**字节串**（latin1 一字节一字符）：CP932 值在往返中不会被转码。
 */
import { decodeLatin1, encodeLatin1 } from './bytes.ts';
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

/** C `atoi` 的前缀语义：跳前导空白（isspace：0x09–0x0D / 0x20）、可选符号、连续十进制数字；无数字 ⇒ 0 */
export function atoiPrefix(s: string): number {
  let i = 0;
  while (i < s.length) {
    const c = s.charCodeAt(i);
    if (c !== 0x20 && (c < 0x09 || c > 0x0d)) break;
    i += 1;
  }
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

/** `_atoi64` 的十进制前缀，再按引擎的 32 位槽截断（REGKEY 只存低 32 位） */
function atoi64Low32(s: string): number {
  let i = 0;
  while (i < s.length) {
    const c = s.charCodeAt(i);
    if (c !== 0x20 && (c < 0x09 || c > 0x0d)) break;
    i += 1;
  }
  let sign = 1n;
  if (s[i] === '+' || s[i] === '-') {
    if (s[i] === '-') sign = -1n;
    i += 1;
  }
  let n = 0n;
  let any = false;
  while (i < s.length) {
    const c = s.charCodeAt(i);
    if (c < 0x30 || c > 0x39) break;
    n = n * 10n + BigInt(c - 0x30);
    any = true;
    i += 1;
  }
  return Number((any ? sign * n : 0n) & 0xffffffffn) | 0;
}

/**
 * 一行里的“字段”：去首尾连续的 0x00–0x20 字节，再剥**一层**成对双引号。
 * ★ 剥引号有门槛（与引擎逐字节一致）：只有字段**起始下标 > 0**时才剥 ——
 *   即开引号之前必须至少有一个被裁掉的 0x00–0x20 字节；第 0 字节就是 `"` 时引号原样留下。
 * ★ 只剥一层：剥完直接返回，不循环（`""A""` ⇒ `"A"`）。
 * ★ 只按**两个端点**判定，没有“长度 ≥ 2”的前置条件：` "` 这种单字符字段也会被剥成空串
 *   （引擎的 substr 在 pos == size 时返回空串，不抛）。
 * ★ 0x21 以上不裁；0x80–0xFF（DBCS 前导/后继字节）也不会被 0x00–0x20 的边界命中。
 */
function trimField(s: string): string {
  let a = 0;
  let b = s.length;
  while (a < b && s.charCodeAt(a) <= 0x20) a += 1;
  while (b > a && s.charCodeAt(b - 1) <= 0x20) b -= 1;
  if (a > 0 && b > 0 && s.charCodeAt(a) === 0x22 && s.charCodeAt(b - 1) === 0x22) {
    return s.slice(a + 1, Math.max(a + 1, b - 1));
  }
  return s.slice(a, b);
}

/**
 * 取一行的第 `index` 个**顶层字段**：`0` = 键（`=` 之前）、`1` = 值（第 1 与第 2 个 `=` 之间）。
 * - `=` 只在「不在双引号之内（`"` 出现偶数次）且括号深度为 0」时才算分隔符；
 * - 没有该字段的分隔符时：`index` = 0 ⇒ 整行；`index` > 0 ⇒ 空串。
 * ★ 因此值在**第二个**顶层 `=` 处截断（`k=a=b` ⇒ 值 `a`），不是“第一个 `=` 之后的全部”。
 * ★ 引擎在这里还做一次多字节边界判定（`_mbbtype`），但它对本函数用到的字节值**恒真**：
 *   DBCS 的后继字节一律 ≥ 0x40，而这里只关心 `"`(0x22) / `(`(0x28) / `)`(0x29) / `=`(0x3d)。
 */
function topLevelField(line: string, index: number): string {
  const n = line.length;
  let start = index === 0 ? 0 : -1;
  let end = n;
  let seen = 0;
  let quote = 0;
  let paren = 0;
  for (let pos = 0; pos < n; ) {
    const c = line.charCodeAt(pos);
    if (c === 0x22) {
      quote += 1;
      pos += 1;
      continue;
    }
    if (c === 0x28) {
      paren += 1;
      pos += 1;
      continue;
    }
    if (c === 0x29) {
      paren -= 1;
      pos += 1;
      continue;
    }
    if (paren === 0 && quote % 2 === 0 && c === 0x3d) {
      seen += 1;
      if (seen === index + 1) {
        end = pos;
        break;
      }
      pos += 1;
      if (seen === index) start = pos;
      continue;
    }
    pos += 1;
  }
  if (start < 0) return '';
  return line.slice(start, end);
}

/** 引擎的行缓冲是 256 字节（末字节放 NUL）⇒ 一次最多交出 255 字节 */
const LINE_MAX = 255;

/**
 * CP932 的单字节 / 双字节判定。
 * 前导字节 0x81–0x9F / 0xE0–0xFC；后继字节 0x40–0x7E / 0x80–0xFC。
 * ★ `[`(0x5B) / `]`(0x5D) 的字节值都落在**后继字节**区间内 ⇒
 *   某个双字节字符的第二个字节就可能恰好是它们。
 */
function isCp932LeadByte(b: number): boolean {
  return (b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xfc);
}

function isCp932TrailByte(b: number): boolean {
  return (b >= 0x40 && b <= 0x7e) || (b >= 0x80 && b <= 0xfc);
}

/**
 * 位置 `pos` 是否落在双字节字符内部。
 * ★ 判据是**局部**的（引擎只比较 `pos` 与相邻字节）：当前字节是前导字节且下一字节是后继字节，
 *   或当前字节是后继字节且前一字节是前导字节 —— 不按行首重新配对。
 */
function insideDoubleByte(s: string, pos: number): boolean {
  const cur = s.charCodeAt(pos);
  if (isCp932LeadByte(cur) && pos + 1 < s.length && isCp932TrailByte(s.charCodeAt(pos + 1))) return true;
  return pos > 0 && isCp932LeadByte(s.charCodeAt(pos - 1)) && isCp932TrailByte(cur);
}

/**
 * 段头的结束括号：**最后一个**不落在双字节字符内部的 `]`（没有 ⇒ -1）。
 * ★ 直接 `lastIndexOf(']')` 会把「双字节字符的第二个字节恰为 0x5D」当成结束括号、截掉半个字符。
 */
function findSectionClose(line: string): number {
  for (let pos = line.length - 1; pos >= 0; pos -= 1) {
    if (line.charCodeAt(pos) === 0x5d && !insideDoubleByte(line, pos)) return pos;
  }
  return -1;
}

/**
 * 逐行解析玩家文件。返回 `段名\键名 → 值`（**字节精确**，不折叠大小写）。
 * - 只按 LF 分行：读到 LF 即交出一行（LF 留着，随后由字段裁剪去掉）；读满 255 字节未见 LF ⇒
 *   这 255 字节先作为一行交出，其余字节继续按下一行解析。
 * - ★ 文件末尾**没有 LF 结尾的残行被丢弃**（引擎的读行器在 EOF 返回失败）。
 * - 行按 C 字符串度量：行内第一个 0x00 之后不参与解析。
 * - 原始首字节 `[` ⇒ 段头：取**第一个** `[` 与**最后一个不落在双字节字符内部的** `]` 之间的文本
 *   为当前段名（不产生条目）。★ 首字节 `[` 之前没有字节 ⇒ 它不可能落在字符内部；
 *   需要判双字节边界的只有后向的 `]`。
 * - 其余行按顶层 `=` 切：键 = 第 0 字段、值 = 第 1 字段；两者各自裁剪 + 剥一层引号。
 * - 没有注释符：`;` / `#` 没有特殊含义；不带 `=` 的非空行 ⇒ 键 = 整行、值 = 空串。
 * - 重复键：最后一次覆盖。
 */
export function parseIni(buf: Uint8Array): Map<string, string> {
  const out = new Map<string, string>();
  let section = '';
  let i = 0;
  while (i < buf.length) {
    const chunk: number[] = [];
    let lf = false;
    while (i < buf.length && chunk.length < LINE_MAX) {
      const b = buf[i];
      i += 1;
      chunk.push(b);
      if (b === 0x0a) {
        lf = true;
        break;
      }
    }
    if (!lf && chunk.length < LINE_MAX) break; // EOF 前的残行：整行丢弃
    const zero = chunk.indexOf(0);
    const raw = decodeLatin1(Uint8Array.from(zero < 0 ? chunk : chunk.slice(0, zero)));
    if (raw.charCodeAt(0) === 0x5b) {
      const close = findSectionClose(raw);
      section = trimField(close < 0 ? raw.slice(1) : raw.slice(1, close));
      continue;
    }
    out.set(`${section}\\${trimField(topLevelField(raw, 0))}`, trimField(topLevelField(raw, 1)));
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
      if (row.special === 'int64') {
        this.setInt(internal, atoi64Low32(raw));
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
      if (row.indexKey.toUpperCase() === 'ENABLEANTIFONT' && opts.antifontUnavailable && v === 1) v = 0;
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
  serializeIni(): Uint8Array {
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
    return encodeLatin1(parts.join(''));
  }
}
