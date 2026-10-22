/**
 * 安装目录的取件（**核心层**）+ 平台接口 `Host`。
 *
 * 真实入口：索引 `SYS4INI.BIN`（ALF 索引）→ 统一文件 id == TOC 条目下标 → 根脚本 id 0。
 * 取脚本的顺序（本机安装的实测）：**松散文件优先**（安装根下的同名 `.BIN`），否则按 TOC 的
 * `(archive, offset, length)` 从 `DATA*.ALF` 切片。
 * ★ 安装目录只读；载荷按需切片（归档有 GB 级，⛔ 不整份读）。
 * ★ 玩家配置只写**调用方显式给出的路径**（`--init-config`）；装载不写安装目录。
 *
 * ★ 本文件不 import 任何 `node:*`：文件系统 / 路径 / 环境变量全部经 `Host`（平台层实现）。
 *   所有 IO 都是 `Promise`：核心在浏览器 / worker 里与在 Node 里走同一条代码路径。
 */
import { parseAlfIndex } from './alf.ts';
import type { AlfFile, AlfIndex } from './alf.ts';

/**
 * 平台接口：核心需要、但只有宿主才知道的那几件事（路径风格 / 文件系统 / 环境变量）。
 *
 * ★ 契约（实现方必须遵守，⛔ 不许"方便就吞掉"）：
 *   - `readBytes`：整份读；不存在 / 打不开 ⇒ **抛**（错误照原样冒泡）。
 *   - `readBytesMaybe`：**只有"文件不存在"** 返回 `null`；其它错误（EACCES / EISDIR…）照抛。
 *   - `readSlice(path, offset, length)`：读 `[offset, offset+length)`，**长度必须精确**；
 *     短读 / 越界 ⇒ 抛（归档切片不许退化成整份读）。
 *   - `isFile`：不是文件 / 不存在 ⇒ `false`；其它 IO 错误照抛（不许静默当成"不存在"）。
 *   - `ensureReadDir`：只读路径上的建目录；权限 / 只读盘一类失败**视为正常**（读不到 ⇒ 走默认值）。
 *   - `writeBytes`：建父目录 + 写；任何失败都**硬失败**。
 */
export interface Host {
  join(...parts: string[]): string;
  dirname(file: string): string;
  readBytes(file: string): Promise<Uint8Array>;
  readBytesMaybe(file: string): Promise<Uint8Array | null>;
  readSlice(file: string, offset: number, length: number): Promise<Uint8Array>;
  readText(file: string): Promise<string>;
  isFile(file: string): Promise<boolean>;
  ensureReadDir(dir: string): Promise<void>;
  writeBytes(file: string, data: Uint8Array): Promise<void>;
  /**
   * **引擎路径字节串 → 宿主路径**。
   * ★ 输入是核心的 latin1 字节串（`55-config` / ALF 名字的口径）：游戏的路径值是
   *   **CP932 / ANSI 字节**，不是 Unicode。平台必须按本地代码页解码，并把 `\\` 规范成
   *   宿主分隔符（POSIX 上 `\\` 不是分隔符，直接拼会造出一个名字里带反斜杠的目录）。
   * ★ 只用于**引擎给的路径值**（`REGFILEPATH` / ALF 条目名）；调用方给的普通路径不要过它。
   */
  pathFromEngine(text: string): string;
  /** 环境变量；没有 ⇒ `null`（不折叠大小写，平台决定） */
  env(name: string): string | null;
  /** 用户主目录（配置根的兜底） */
  homeDir(): string;
}

export interface ScriptSource {
  id: number;
  name: string;
  side: 'loose' | 'archive';
  bytes: Uint8Array;
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
  readonly host: Host;
  readonly dir: string;
  readonly indexFile: string;
  readonly index: AlfIndex;

  private constructor(host: Host, dir: string, indexFile: string, index: AlfIndex) {
    this.host = host;
    this.dir = dir;
    this.indexFile = indexFile;
    this.index = index;
  }

  /** 打开安装目录：只要求 `SYS4INI.BIN`（索引）在场；归档按需打开 */
  static async open(host: Host, dir: string): Promise<Install> {
    const indexFile = host.join(dir, 'SYS4INI.BIN');
    const index = parseAlfIndex(await host.readBytes(indexFile));
    return new Install(host, dir, indexFile, index);
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
  private async loosePath(name: string): Promise<string | null> {
    const hostName = this.host.pathFromEngine(name);
    if (!isBareName(hostName)) return null;
    const p = this.host.join(this.dir, hostName);
    return (await this.host.isFile(p)) ? p : null;
  }

  async locate(name: string): Promise<'loose' | 'archive' | 'missing'> {
    if (await this.loosePath(name)) return 'loose';
    const hit = this.index.files.find((f) => f.name === name);
    return hit ? 'archive' : 'missing';
  }

  private async readArchiveSlice(entry: AlfFile): Promise<Uint8Array> {
    const archive = this.index.archives[entry.archiveIndex];
    const hostArchive = archive === undefined ? null : this.host.pathFromEngine(archive);
    if (hostArchive === null || !isBareName(hostArchive)) {
      throw new Error(`alf-bad-archive:${entry.archiveIndex}/${entry.name}`);
    }
    // 空条目：不碰归档文件（引擎也不读）；否则按 (offset, length) 精确切片
    if (entry.length === 0) return new Uint8Array(0);
    const file = this.host.join(this.dir, hostArchive);
    return this.host.readSlice(file, entry.offset, entry.length);
  }

  /** 只读归档（诊断 / 对照松散覆盖用） */
  async readArchiveById(id: number): Promise<ScriptSource> {
    const entry = this.entry(id);
    return {
      id,
      name: entry.name,
      side: 'archive',
      bytes: await this.readArchiveSlice(entry),
      entryLength: entry.length,
      archive: this.index.archives[entry.archiveIndex],
    };
  }

  /** 真实入口：松散优先，否则归档切片 */
  async readById(id: number): Promise<ScriptSource> {
    const entry = this.entry(id);
    const loose = await this.loosePath(entry.name);
    if (loose) {
      return { id, name: entry.name, side: 'loose', bytes: await this.host.readBytes(loose), entryLength: entry.length };
    }
    return this.readArchiveById(id);
  }

  /** 按名字查（名字只用于日志 / 诊断；引擎侧引用的是 id） */
  async readByName(name: string): Promise<ScriptSource | null> {
    const id = this.index.files.findIndex((f) => f.name === name);
    return id < 0 ? null : this.readById(id);
  }
}

/** 读一份调试用的脚本文件（`--script`；不是游戏安装的取件路径） */
export function readFile(host: Host, file: string): Promise<Uint8Array> {
  return host.readBytes(file);
}

/** 读玩家配置文件；不存在返回 null（“全部用发行默认”是正常状态） */
export function readConfigFile(host: Host, file: string): Promise<Uint8Array | null> {
  return host.readBytesMaybe(file);
}

/**
 * 玩家配置根：`USEAPPDATAFOLDER` 为真 ⇒ 用户配置目录；否则安装目录。
 * ★ `??` 的口径照旧：**空串也算"给了"**（⛔ 不改成 falsy 判断 —— 那会在 `APPDATA=` 时换一个根）。
 */
function configBase(host: Host, installDir: string, useAppData: boolean): string {
  if (!useAppData) return installDir;
  return host.env('APPDATA') ?? host.env('XDG_CONFIG_HOME') ?? host.join(host.homeDir(), '.config');
}

/** 引擎的默认玩家文件路径：配置根 + `REGFILEPATH` + 文件名（`55-config.md`） */
export function defaultConfigPath(host: Host, installDir: string, regFilePath: string, useAppData: boolean): string {
  return host.join(configBase(host, installDir, useAppData), host.pathFromEngine(regFilePath), 'SYS4REG.INI');
}

/**
 * 读之前按需创建配置目录（引擎 `sub_4900F0` 的行为）。
 * ★ 只读场景下目录建不出来（沙箱 / 只读盘 / EPERM）不是错误：文件读不到 ⇒ 默认值。
 *   写场景（`writeConfigFile`）仍然硬失败。
 */
export async function ensureConfigDir(host: Host, file: string): Promise<void> {
  await host.ensureReadDir(host.dirname(file));
}

/** 写玩家配置文件（只有显式 `--init-config` / 保存才调用） */
export async function writeConfigFile(host: Host, file: string, data: Uint8Array): Promise<void> {
  await host.writeBytes(file, data);
}
