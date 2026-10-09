/**
 * apps/emulator/frontends/headless/file-config.ts —— **实例隔离的文件配置**（Node 侧）
 *
 * ## 为什么需要它（不是"顺手加个持久化"）
 * 启动链的 `SYSTEM4` 在 `#56` 有一个**配置存在性门**：
 * ```
 *   jcc (global-int 5) ffffffff label    ; global5 != 0 ⇒ 落下执行 call-script LOADCONFIG
 *                                        ; global5 == 0 ⇒ 跳走，走 INITCONFIG（建默认配置）
 * ```
 * 而 `global5` 是 `load-int` **从配置里读**出来的 ⇒ 配置若每次都是空的，`LOADCONFIG`
 * （目标里点名要跑的三条子脚本之一）**永远不会被走到**，我们也就永远量不到它。
 * ⇒ 配置必须能**跨进程留下**，而且**按实例隔离**（不同实例不许互相看见存档/配置）。
 *
 * ## 格式：**全十六进制、逐行**（不是 JSON）
 * 键里带控制字节（`\x03`/`\x05` 打头，见 `ops.ts` 的 `configKeyInt`），值可以是任意的日文文本、
 * 也可能含换行/制表符 ⇒ 两个字段都**按 UTF-8 逐字节十六进制编码**：
 * ```
 *   # amayui-emulator-config/1        ← 版本行（不认识就不读，宁可报错也不猜）
 *   03 000004d2|e383a1e382a4e383aa...  ← <hex(键)>|<hex(值 utf8)>
 * ```
 * 口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `headless/file-config-hex-encoding`）。
 * ★ 不用 JSON 的另一个理由：仓库里有"自有 JSON 必须有同名 `.md`"的守卫，而这是**运行时产物**、
 *   不是自有数据文件 —— 用一个没有 schema 争议的纯文本格式最省事。
 *
 * ## 纯函数与 IO 分开
 * `serializeConfig` / `parseConfig` 是**纯函数**（守卫直接测它们，包括"值里有换行"这种）。
 */

import fs from 'node:fs';
import path from 'node:path';
import type { ConfigEntry, ConfigStore } from '../../src/host/config.ts';

/** 版本行：格式一变就换它（读到不认识的行 ⇒ 抛，不猜） */
export const CONFIG_HEADER = '# amayui-emulator-config/1';

/** 一条配置项 */
export type ConfigEntryTuple = readonly [key: string, value: string];

const toHex = (s: string): string => Buffer.from(s, 'utf8').toString('hex');
const fromHex = (h: string): string => Buffer.from(h, 'hex').toString('utf8');

/**
 * 序列化（纯函数）。★ 键**排序** ⇒ 同样内容同样字节（便于 diff 与快照比对）。
 * 口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `headless/file-config-header-guard`）。
 */
export function serializeConfig(entries: readonly ConfigEntryTuple[]): string {
  const lines = [...entries]
    .map(([k, v]) => [k, v] as const)
    .sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0))
    .map(([k, v]) => `${toHex(k)}|${toHex(v)}`);
  return [CONFIG_HEADER, ...lines].join('\n') + '\n';
}

/** 反序列化（纯函数）。★ 认不出头部 ⇒ **抛**（旧格式静默当空配置会表现成"配置丢了"） */
export function parseConfig(text: string): ConfigEntryTuple[] {
  const lines = text.split('\n').filter((l) => l.length > 0);
  if (lines.length === 0) return [];
  if (lines[0] !== CONFIG_HEADER) {
    throw new Error(`配置文件头不对：期望 ${JSON.stringify(CONFIG_HEADER)}，实际 ${JSON.stringify(lines[0])}（不猜旧格式）`);
  }
  const out: ConfigEntryTuple[] = [];
  for (const line of lines.slice(1)) {
    const i = line.indexOf('|');
    if (i < 0) throw new Error(`配置行没有分隔符：${JSON.stringify(line)}`);
    out.push([fromHex(line.slice(0, i)), fromHex(line.slice(i + 1))]);
  }
  return out;
}

/**
 * 文件配置：构造时读盘（文件不在 ⇒ 空配置，**这不是错误**：第一次运行本来就没有），
 * `set` **写穿**（立刻落盘 ⇒ 进程被打断也留得住）。
 */
export class FileConfig implements ConfigStore {
  readonly file: string;
  private readonly map: Map<string, string>;

  constructor(file: string) {
    this.file = file;
    this.map = new Map();
    if (fs.existsSync(file)) {
      for (const [k, v] of parseConfig(fs.readFileSync(file, 'utf8'))) this.map.set(k, v);
    }
  }

  get(key: string): string | undefined {
    return this.map.get(key);
  }

  set(key: string, value: string): void {
    this.map.set(key, value);
    this.flush();
  }

  keys(): string[] {
    return [...this.map.keys()].sort();
  }

  /** 当前的键值对（**键升序** —— 与 `ConfigStore` 的口径一致；前端拿它去落盘） */
  entries(): ConfigEntry[] {
    return this.keys().map((key) => ({ key, value: this.map.get(key) as string }));
  }

  replaceAll(entries: readonly ConfigEntry[], opts: { drop?: readonly string[] } = {}): void {
    const next = new Map(this.map);
    for (const d of opts.drop ?? []) next.delete(d);
    for (const e of entries) next.set(e.key, e.value);
    this.map.clear();
    for (const [k, v] of next) this.map.set(k, v);
    this.flush();
  }

  /** 落盘（原子性够用：先写同目录临时文件再改名 —— 半个文件比没有文件更糟） */
  private flush(): void {
    fs.mkdirSync(path.dirname(this.file), { recursive: true });
    const tmp = `${this.file}.tmp`;
    fs.writeFileSync(tmp, serializeConfig([...this.map.entries()]), 'utf8');
    fs.renameSync(tmp, this.file);
  }
}
