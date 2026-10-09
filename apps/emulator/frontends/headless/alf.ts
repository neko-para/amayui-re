/**
 * apps/emulator/frontends/headless/alf.ts —— **安装根上的资源来源**（Node 侧）
 *
 * ## 它解决的问题
 * 引擎的脚本与资源**不在磁盘上摊开**：它们在一个索引文件（`SYS4INI.BIN`）描述的
 * ALF 归档里（`DATA1.ALF`…）。索引给的是 `(归档, 偏移, 长度)`，载荷在归档文件里
 * **裸拼**（无头、无目录区）。
 *
 * ## ★ 为什么"索引"与"归档"要分开注入（而不是合成一个"文件系统"）
 * 索引只有几百 KB、归档有 6.7 GB ⇒ 分开之后**看目录**只读索引、**取载荷**才按 `(偏移,长度)` 打开归档；
 * `readPrefix` 让"只要头几十字节"的调用方**不必把整份文件读进来**。
 * 口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `headless/alf-lazy-payload`）。
 *
 * ## ★ 松散文件优先
 * 安装根上可能有同名的松散文件（打补丁/改过的件），这一条**不在本文件里**做 ——
 * 它由核心的 `LayeredFilesystem` 用"源的顺序"表达（松散目录在前、本来源在后）。
 * 口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/fs-layer-order-and-demand`）。
 */

import * as fs from 'node:fs';
import * as path from 'node:path';
import { readAlf } from '@amayui/age-format/src/alf.mts';
import type { AlfEntry, AlfIndex } from '@amayui/age-format/src/alf.mts';
import { identityOf } from './node-fs.ts';
import type { ReadSource } from '../../src/host/fs.ts';

/** 索引（`SYS4INI.BIN` 一族）里的一个条目 + 它所在归档的绝对路径（惰性解析） */
interface IndexedFile {
  name: string;
  archiveIndex: number;
  offset: number;
  length: number;
}

/** `AlfIndexSource` 的构造选项 */
export interface AlfIndexSourceOptions {
  /** 人类可读标签（日志用） */
  label?: string;
  /** 索引文件路径（`SYS4INI.BIN`） */
  indexPath: string;
  /** 归档所在目录（缺省 = 索引文件所在目录） */
  archiveDir?: string;
}

/**
 * 一个建立在 ALF 索引上的**只读来源**。
 *
 * ★ 索引只解析一次（`readAlf` 会把目录区 LZSS 解开）；载荷**按需**从归档里读。
 * ★ 找不到的名字 ⇒ `null`（核心会记一条 demand）—— 本层不抛：
 *   "这份安装里没有这个文件"是**正常情况**（补丁包、扩展包会让名字集合不同）。
 */
export class AlfIndexSource implements ReadSource {
  readonly label: string;
  readonly identity: string;
  readonly indexPath: string;
  readonly archiveDir: string;
  readonly index: AlfIndex;
  /** 归一化（小写）名字 → 条目；**重复名字保留第一个**（与旧仓"base 先入表、append 只补空"同口径） */
  readonly byKey: Map<string, IndexedFile>;
  /** 归档绝对路径缓存（`archiveIndex` → 路径） */
  readonly archivePaths: Map<number, string>;
  /** 载荷读次数（诊断：能回答"这次跑到底读了几份资源"） */
  readCount = 0;

  constructor(opts: AlfIndexSourceOptions) {
    this.indexPath = path.resolve(opts.indexPath);
    this.label = opts.label ?? `ALF 索引 ${this.indexPath}`;
    this.identity = `alf:${identityOf(this.indexPath)}`;
    this.archiveDir = path.resolve(opts.archiveDir ?? path.dirname(this.indexPath));
    this.index = readAlf(this.indexPath);
    this.byKey = new Map();
    for (const e of this.index.entries) {
      const key = normalizeKey(e.filename);
      if (!this.byKey.has(key)) this.byKey.set(key, entryOf(e));
    }
    this.archivePaths = new Map();
  }

  /** 目录里的条目数（索引自身的事实；不读归档） */
  get entryCount(): number {
    return this.index.entries.length;
  }

  /** 归档文件名列表（索引自身的事实） */
  archiveNames(): string[] {
    return this.index.archives.map((a) => a.filename);
  }

  #archivePath(archiveIndex: number): string {
    const cached = this.archivePaths.get(archiveIndex);
    if (cached) return cached;
    const a = this.index.archives[archiveIndex];
    if (!a) throw new Error(`索引里的归档下标越界：${archiveIndex}（只有 ${this.index.archives.length} 个）`);
    const abs = path.resolve(this.archiveDir, a.filename);
    this.archivePaths.set(archiveIndex, abs);
    return abs;
  }

  /** 索引里有没有这个名字（**不读归档**） */
  has(name: string): boolean {
    return this.byKey.has(normalizeKey(name));
  }

  /**
   * **统一文件 id → 名字**（id == 目录条目下标，根脚本 = id 0）；名字只用于日志与报错，核心一律走 id。
   * 口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/scripts-by-file-id`）。
   */
  nameOfId(id: number): string | null {
    return this.index.entries[id >>> 0]?.filename ?? null;
  }

  /** id 声明的长度（`null` = 那个 id 不存在）—— 只看索引，不读归档 */
  declaredLengthOfId(id: number): number | null {
    return this.index.entries[id >>> 0]?.length ?? null;
  }

  /** 某条目的声明长度（`null` = 索引里没有）——**诊断用**：能回答"这个名字该有多大"而不读载荷 */
  declaredLength(name: string): number | null {
    return this.byKey.get(normalizeKey(name))?.length ?? null;
  }

  read(name: string): Uint8Array | null {
    const e = this.byKey.get(normalizeKey(name));
    if (!e) return null;
    this.readCount += 1;
    const abs = this.#archivePath(e.archiveIndex);
    let fd: number | null = null;
    try {
      fd = fs.openSync(abs, 'r');
      const buf = new Uint8Array(e.length);
      const got = fs.readSync(fd, buf, 0, e.length, e.offset);
      this.#assertFull(e, name, got);
      return got === e.length ? buf : buf.subarray(0, got);
    } finally {
      if (fd !== null) fs.closeSync(fd);
    }
  }

  readPrefix(name: string, maxBytes: number): Uint8Array | null {
    const e = this.byKey.get(normalizeKey(name));
    if (!e) return null;
    this.readCount += 1;
    const want = Math.max(0, Math.min(maxBytes, e.length)); // ★ 不补齐
    const abs = this.#archivePath(e.archiveIndex);
    let fd: number | null = null;
    try {
      fd = fs.openSync(abs, 'r');
      const buf = new Uint8Array(want);
      const got = fs.readSync(fd, buf, 0, want, e.offset);
      this.#assertFull(e, name, got);
      return got === want ? buf : buf.subarray(0, got);
    } finally {
      if (fd !== null) fs.closeSync(fd);
    }
  }

  /**
   * ★ 短读**必须响亮失败**（索引声明 N 字节而归档读不满 N ⇒ 索引与归档不是一对、或归档被截断）。
   * 口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `headless/alf-lazy-payload`）。
   */
  #assertFull(e: IndexedFile, name: string, got: number): void {
    if (got === e.length) return;
    const abs = this.#archivePath(e.archiveIndex);
    throw new Error(
      `ALF 载荷短读：${name} 声明 ${e.length} 字节，实际只读到 ${got} 字节 ` +
      `（归档 ${path.basename(abs)} 偏移 ${e.offset}）—— 索引与归档不是一对，或归档被截断`,
    );
  }

  /** 列**一层**虚拟目录（名字集合上的目录，不是盘上的目录） */
  list(dir = ''): string[] | null {
    const prefix = dir === '' || dir === '.' ? '' : `${dir.replace(/\\/g, '/')}/`;
    const out = new Set<string>();
    for (const e of this.byKey.values()) {
      if (prefix && !e.name.toLowerCase().startsWith(prefix.toLowerCase())) continue;
      const rest = e.name.slice(prefix.length);
      if (rest === '') continue;
      const slash = rest.indexOf('\\') >= 0 ? rest.indexOf('\\') : rest.indexOf('/');
      out.add(slash < 0 ? rest : `${rest.slice(0, slash)}/`);
    }
    return [...out].sort();
  }
}

const normalizeKey = (name: string): string => name.replace(/\\/g, '/').toLowerCase();

const entryOf = (e: AlfEntry): IndexedFile => ({
  name: e.filename.replace(/\\/g, '/'),
  archiveIndex: e.archiveIndex,
  offset: e.offset,
  length: e.length,
});
