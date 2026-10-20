/**
 * 安装目录的文件系统入口（**本目录唯一读游戏文件的地方**）。
 *
 * 真实入口：索引 `SYS4INI.BIN`（ALF 索引）→ 统一文件 id == TOC 条目下标 → 根脚本 id 0。
 * 取脚本的顺序（本机安装的实测）：**松散文件优先**（安装根下的同名 `.BIN`），否则按 TOC 的
 * `(archive, offset, length)` 从 `DATA*.ALF` 切片。
 * ★ 安装目录只读；载荷按需切片（归档有 GB 级，⛔ 不整份读）。
 * ★ 玩家配置只写**调用方显式给出的路径**（`--init-config`）；装载不写安装目录。
 */
import fs from 'node:fs';
import path from 'node:path';

import { parseAlfIndex } from './alf.ts';
import type { AlfFile, AlfIndex } from './alf.ts';

export interface ScriptSource {
  id: number;
  name: string;
  side: 'loose' | 'archive';
  bytes: Buffer;
  /** TOC 里登记的长度（与松散文件的实际长度可能不同；判据见 README） */
  entryLength: number;
  archive?: string;
}

/** 只是"文件名"，不许带目录分隔符 / 盘符 / `..` */
function isBareName(name: string): boolean {
  return name.length > 0
    && name !== '.' && name !== '..'
    && !name.includes('\0')
    && !name.includes('/') && !name.includes('\\')
    && !/^[A-Za-z]:/.test(name);
}

export class Install {
  readonly dir: string;
  readonly indexFile: string;
  readonly index: AlfIndex;

  private constructor(dir: string, indexFile: string, index: AlfIndex) {
    this.dir = dir;
    this.indexFile = indexFile;
    this.index = index;
  }

  /** 打开安装目录：只要求 `SYS4INI.BIN`（索引）在场；归档按需打开 */
  static open(dir: string): Install {
    const indexFile = path.join(dir, 'SYS4INI.BIN');
    const index = parseAlfIndex(fs.readFileSync(indexFile));
    return new Install(dir, indexFile, index);
  }

  get entryCount(): number {
    return this.index.files.length;
  }

  entry(id: number): AlfFile {
    if (!Number.isInteger(id) || id < 0 || id >= this.index.files.length) {
      throw new Error(`script-id-out-of-range:${id}/${this.index.files.length}`);
    }
    return this.index.files[id];
  }

  /** 松散文件是否存在（只按裸文件名） */
  private loosePath(name: string): string | null {
    if (!isBareName(name)) return null;
    const p = path.join(this.dir, name);
    try {
      return fs.statSync(p).isFile() ? p : null;
    } catch {
      return null;
    }
  }

  locate(name: string): 'loose' | 'archive' | 'missing' {
    if (this.loosePath(name)) return 'loose';
    const hit = this.index.files.find((f) => f.name === name);
    return hit ? 'archive' : 'missing';
  }

  private readArchiveSlice(entry: AlfFile): Buffer {
    const archive = this.index.archives[entry.archiveIndex];
    if (!archive || !isBareName(archive)) {
      throw new Error(`alf-bad-archive:${entry.archiveIndex}/${entry.name}`);
    }
    if (entry.length === 0) return Buffer.alloc(0);
    const file = path.join(this.dir, archive);
    const fd = fs.openSync(file, 'r');
    try {
      const out = Buffer.alloc(entry.length);
      let done = 0;
      while (done < entry.length) {
        const n = fs.readSync(fd, out, done, entry.length - done, entry.offset + done);
        if (n <= 0) throw new Error(`alf-short-read:${entry.name} need=${entry.length} got=${done}`);
        done += n;
      }
      return out;
    } finally {
      fs.closeSync(fd);
    }
  }

  /** 只读归档（诊断 / 对照松散覆盖用） */
  readArchiveById(id: number): ScriptSource {
    const entry = this.entry(id);
    return {
      id,
      name: entry.name,
      side: 'archive',
      bytes: this.readArchiveSlice(entry),
      entryLength: entry.length,
      archive: this.index.archives[entry.archiveIndex],
    };
  }

  /** 真实入口：松散优先，否则归档切片 */
  readById(id: number): ScriptSource {
    const entry = this.entry(id);
    const loose = this.loosePath(entry.name);
    if (loose) {
      return { id, name: entry.name, side: 'loose', bytes: fs.readFileSync(loose), entryLength: entry.length };
    }
    return this.readArchiveById(id);
  }

  /** 按名字查（名字只用于日志 / 诊断；引擎侧引用的是 id） */
  readByName(name: string): ScriptSource | null {
    const id = this.index.files.findIndex((f) => f.name === name);
    return id < 0 ? null : this.readById(id);
  }
}

/** 读一份调试用的脚本文件（`--script`；不是游戏安装的取件路径） */
export function readFile(file: string): Buffer {
  return fs.readFileSync(file);
}

/** 读玩家配置文件；不存在返回 null（“全部用发行默认”是正常状态） */
export function readConfigFile(file: string): Buffer | null {
  try {
    return fs.readFileSync(file);
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code === 'ENOENT') return null;
    throw err;
  }
}

/** 写玩家配置文件（只有显式 `--init-config` / 保存才调用） */
export function writeConfigFile(file: string, data: Buffer): void {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, data);
}
