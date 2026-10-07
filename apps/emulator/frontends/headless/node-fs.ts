/**
 * apps/emulator/frontends/headless/node-fs.ts —— **Node 侧的文件系统适配**（★ 本文件**允许**用 Node）
 *
 * ## 为什么它在 `frontends/` 而不是 `src/`
 * `src/` 里是**平台无关**的核心，由 `apps/emulator/tsconfig.json` 的 `"types": []` 机械地挡住
 * `node:*` 与 `Buffer`（写一行就 `pnpm typecheck` 红）。这一层恰恰相反：它的**全部意义**
 * 就是"把 Node 的 `node:fs` 接上核心定义的那几个接口"。
 * ⇒ 它归 `apps/emulator/tsconfig.frontends.json`（`types: ["node"]`）管。
 *
 * ## ★ 三条必须在这里做对、而且**只能**在这里做的事
 * 1. **大小写不敏感的名字解析**：核心只给"名字"，而名字的**大小写可能与盘上不一致**
 *    （引擎是 Windows 程序；同一份安装被 Linux/macOS 上跑时大小写敏感就成了 bug）。
 *    ⇒ 逐段 `readdir` + 不敏感匹配（带缓存）。★ 直接在 `fs.existsSync(join(root,name))` 上赌
 *    是错的：那在 win32 上"看起来能跑"，在别的平台上静默找不到文件。
 * 2. **写要原子**：先写 `<名字>.$$tmp` 再 `rename`。半截文件比"没写"更难查
 *    （读的人会把它当成一份完整的、内容错的存档）。
 * 3. **`identity` 用解析后的绝对路径**（win32 下先小写）：核心靠它判"可写区与只读源是不是同一块地方"，
 *    而这判定只有在**两边都归一化**之后才有意义。
 *
 * ## ★ 不在这里做的事（免得又散出第二处实现）
 * 名字的合法性与归一化（`..` / 绝对路径 / 控制字符）**在核心里**（`src/host/fs.ts`）。
 * 本文件假定收到的名字已经归一化 —— 万一没有，`split('/')` 也分不出盘符，
 * 但那不是"再检查一遍"的理由（两处检查必然漂）。
 */

import * as fs from 'node:fs';
import * as path from 'node:path';
// ★ 同包内的相对引用（跨**包**才必须走包名 —— 见 `AGENTS.md` §3）。
//   这里相对引用还额外避开一个坑：以包名引用**自己**要求包有 `exports` 自引用，
//   而本 app 的 `package.json` 没有（它只声明依赖，不是给别人 import 的库）。
import type { ReadSource, WriteArea } from '../../src/host/fs.ts';

/** 把绝对路径归一化成"身份"标记（win32 大小写不敏感 ⇒ 比较前统一小写） */
export function identityOf(absPath: string): string {
  return process.platform === 'win32' ? path.resolve(absPath).toLowerCase() : path.resolve(absPath);
}

/**
 * 一个**目录**上的只读来源。名字按 `\` 与 `/` 分隔的**相对**路径给（核心已归一化成 `/`）。
 */
export class NodeDirSource implements ReadSource {
  readonly label: string;
  readonly identity: string;
  readonly root: string;
  /** 目录 → （小写段名 → 盘上的真实段名）。★ 缓存的是"盘上真名"，不是内容 */
  readonly dirCache: Map<string, Map<string, string>>;

  constructor(label: string, root: string) {
    this.label = label;
    this.root = path.resolve(root);
    this.identity = identityOf(this.root);
    this.dirCache = new Map();
  }

  /** 逐段做**大小写不敏感**解析；任一段找不到 ⇒ `null`（不是抛：找不到是常态） */
  resolve(name: string): string | null {
    if (name === '' || name === '.') return this.root;
    let cur = this.root;
    for (const seg of name.split('/')) {
      if (seg === '') continue;
      const table = this.#childrenOf(cur);
      if (!table) return null;
      const actual = table.get(seg.toLowerCase());
      if (actual === undefined) return null;
      cur = path.join(cur, actual);
    }
    return cur;
  }

  #childrenOf(dir: string): Map<string, string> | null {
    const cached = this.dirCache.get(dir);
    if (cached) return cached;
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return null;
    }
    const table = new Map<string, string>();
    for (const e of entries) table.set(e.name.toLowerCase(), e.name);
    this.dirCache.set(dir, table);
    return table;
  }

  read(name: string): Uint8Array | null {
    const abs = this.resolve(name);
    if (abs === null) return null;
    try {
      const st = fs.statSync(abs);
      if (!st.isFile()) return null;
      return new Uint8Array(fs.readFileSync(abs));
    } catch {
      return null;
    }
  }

  readPrefix(name: string, maxBytes: number): Uint8Array | null {
    const abs = this.resolve(name);
    if (abs === null) return null;
    let fd: number | null = null;
    try {
      const st = fs.statSync(abs);
      if (!st.isFile()) return null;
      const want = Math.max(0, Math.min(maxBytes, st.size)); // ★ 文件更短就返回短的，**不补齐**
      fd = fs.openSync(abs, 'r');
      const buf = new Uint8Array(want);
      const got = fs.readSync(fd, buf, 0, want, 0);
      return got === want ? buf : buf.subarray(0, got);
    } catch {
      return null;
    } finally {
      if (fd !== null) fs.closeSync(fd);
    }
  }

  /** 列**一层**（不递归）：目录名带尾 `/`。★ 缓存的目录表要跟着失效，否则新建的文件看不见 */
  list(dir = ''): string[] | null {
    const abs = this.resolve(dir === '.' ? '' : dir);
    if (abs === null) return null;
    this.dirCache.delete(abs);
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(abs, { withFileTypes: true });
    } catch {
      return null;
    }
    return entries.map((e) => (e.isDirectory() ? `${e.name}/` : e.name)).sort();
  }
}

/**
 * 一个**目录**上的可写区。
 *
 * ★ 写是**原子**的（`<名字>.$$tmp` + `rename`）：崩溃/中断留下的半截文件会被下一次读
 * 当成"一份完整的、内容错的"东西 —— 那比"没写"难查得多。
 * ★ `identity` 必须与只读源不同（核心在构造期就判），所以这里只负责如实报告自己的身份。
 */
export class NodeWriteArea implements WriteArea {
  readonly label: string;
  readonly identity: string;
  readonly root: string;
  /** 是否强制建中间目录（缺省 true：存档目录 `SAVE\` 常常还不存在） */
  readonly mkdirs: boolean;

  constructor(label: string, root: string, opts: { mkdirs?: boolean } = {}) {
    this.label = label;
    this.root = path.resolve(root);
    this.identity = identityOf(this.root);
    this.mkdirs = opts.mkdirs ?? true;
  }

  /** 名字 → 根内的绝对路径（核心已挡住 `..` 与绝对路径；这里再兜一层是**成本为零**的保险） */
  #abs(name: string): string {
    const abs = path.resolve(this.root, ...name.split('/'));
    const rootWithSep = this.root.endsWith(path.sep) ? this.root : this.root + path.sep;
    if (abs !== this.root && !abs.startsWith(rootWithSep)) {
      throw new Error(`可写区越界：${JSON.stringify(name)} ⇒ ${abs} 不在 ${this.root} 之内`);
    }
    return abs;
  }

  write(name: string, bytes: Uint8Array): void {
    const abs = this.#abs(name);
    const dir = path.dirname(abs);
    if (this.mkdirs) fs.mkdirSync(dir, { recursive: true });
    else if (!fs.existsSync(dir)) throw new Error(`可写区的目标目录不存在：${dir}`);
    const tmp = `${abs}.$$tmp`;
    try {
      fs.writeFileSync(tmp, bytes);
      fs.renameSync(tmp, abs); // ★ 同目录内改名：读者要么看到旧的、要么看到新的，不会看到半截
    } catch (err) {
      try { fs.unlinkSync(tmp); } catch { /* 清不掉就算了：报原始错误更有用 */ }
      throw err;
    }
  }

  read(name: string): Uint8Array | null {
    const abs = this.#abs(name);
    try {
      const st = fs.statSync(abs);
      if (!st.isFile()) return null;
      return new Uint8Array(fs.readFileSync(abs));
    } catch {
      return null;
    }
  }

  readPrefix(name: string, maxBytes: number): Uint8Array | null {
    const abs = this.#abs(name);
    let fd: number | null = null;
    try {
      const st = fs.statSync(abs);
      if (!st.isFile()) return null;
      const want = Math.max(0, Math.min(maxBytes, st.size));
      fd = fs.openSync(abs, 'r');
      const buf = new Uint8Array(want);
      const got = fs.readSync(fd, buf, 0, want, 0);
      return got === want ? buf : buf.subarray(0, got);
    } catch {
      return null;
    } finally {
      if (fd !== null) fs.closeSync(fd);
    }
  }

  list(dir = ''): string[] | null {
    const abs = this.#abs(dir === '.' ? '' : dir);
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(abs, { withFileTypes: true });
    } catch {
      return null;
    }
    return entries.map((e) => (e.isDirectory() ? `${e.name}/` : e.name)).sort();
  }

  remove(name: string): boolean {
    const abs = this.#abs(name);
    try {
      fs.unlinkSync(abs);
      return true;
    } catch {
      return false;
    }
  }
}
