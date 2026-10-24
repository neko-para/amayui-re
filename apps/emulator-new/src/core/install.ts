/**
 * 安装目录的取件（**核心层**）+ 平台接口 `Host`。
 *
 * 真实入口：索引 `SYS4INI.BIN`（ALF 索引）→ 统一文件 id == TOC 条目下标 → 根脚本 id 0。
 * 取脚本的顺序（本机安装的实测）：**松散文件优先**（安装根下的同名 `.BIN`），否则按 TOC 的
 * `(archive, offset, length)` 从 `DATA*.ALF` 切片。
 * ★ 装载栈（`CORE-FS-INDEX-STACK`）：基础索引恰好一份；成功之后枚举当前目录的 `*.AAI`
 *   （扩展索引，`CORE-FS-EXT-INDEX-KEY`），每份按**头内区号**写槽，id 的高 8 位选出槽。
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
 *   - `listDir`：列**一层**条目名，顺序 = **宿主顺序**（核心不排序）；目录项可以出现在结果里；
 *     枚举失败 ⇒ **抛**（"当成没有扩展索引"是调用方的决定，见 `Install.open`）。
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
  /**
   * **列目录**：返回 `dir` 下一层的**条目名字**（不含路径），顺序 = **宿主顺序**。
   * ★ 核心**不排序**、不比较名字 / 时间：枚举顺序就是合同的一部分（`CORE-FS-INDEX-STACK`）。
   * ★ 只列一层（不递归）；目录项**可以**出现在结果里 —— 调用方自己用 `isFile` 过滤。
   * ★ 枚举失败（目录不存在 / 没权限）⇒ **抛**；⛔ 不许静默返回空数组来冒充"目录是空的"。
   */
  listDir(dir: string): Promise<string[]>;
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

/** 扩展索引槽表容量（引擎固定 256 格；高 8 位按有符号字节解释 ⇒ 可用区号只有 1..127） */
const AAI_SLOT_COUNT = 256;

/** `offset` 字段上的"没有归档可回退"哨兵（`CORE-FS-ARCHIVE-FALLBACK` 未定名的那个值） */
const OFFSET_NO_ARCHIVE = 0xffffffff;

export class Install {
  readonly host: Host;
  readonly dir: string;
  readonly indexFile: string;
  readonly index: AlfIndex;
  /**
   * 扩展索引槽表（`CORE-FS-INDEX-STACK`）：下标 = **头内区号**，值 = 那份扩展索引。
   * ★ 长度固定 256（引擎的槽表容量），不是"有几份 AAI 就多长"。
   * ★ 同区号**后装载者胜**（无条件覆盖，见 `loadExtensionIndexes`）。
   */
  readonly slots: (AlfIndex | null)[];
  /** 扩展索引装载失败的诊断（每份一条）；★ 装载失败不中止启动，也不写槽 */
  readonly aaiDiagnostics: string[];

  private constructor(host: Host, dir: string, indexFile: string, index: AlfIndex) {
    this.host = host;
    this.dir = dir;
    this.indexFile = indexFile;
    this.index = index;
    this.slots = new Array<AlfIndex | null>(AAI_SLOT_COUNT).fill(null);
    this.aaiDiagnostics = [];
  }

  /**
   * 打开安装目录：只要求 `SYS4INI.BIN`（基础索引）在场；归档按需打开。
   * ★ 基础索引成功之后才枚举 `*.AAI`（`CORE-FS-INDEX-STACK`）；基础失败时一个也不扫 ——
   *   `readBytes` 抛出去，调用方看到"打不开"，与扩展索引无关。
   */
  static async open(host: Host, dir: string): Promise<Install> {
    const indexFile = host.join(dir, 'SYS4INI.BIN');
    const index = parseAlfIndex(await host.readBytes(indexFile));
    const install = new Install(host, dir, indexFile, index);
    await install.loadExtensionIndexes();
    return install;
  }

  get entryCount(): number {
    return this.index.files.length;
  }

  /**
   * 枚举当前目录里的 `*.AAI` 并按**头内区号**写槽（`CORE-FS-EXT-INDEX-KEY`）。
   * - 顺序 = `Host.listDir` 给的顺序（核心不排序、不比较名字 / 时间）；
   * - 不递归；`*.AAI` 里若是目录 ⇒ 跳过；
   * - 每份独立解析 → 头标题与**基础索引**头标题**逐字节相同** → 用头内区号写槽；
   * - 任何一份失败 ⇒ 一条诊断 + 跳过（**不中止**，也**不写槽**：失败不顶掉同区号旧档）；
   * - 目录枚举失败 ⇒ 视为"没有扩展索引"（引擎里枚举失败也只是没扫到）。
   */
  private async loadExtensionIndexes(): Promise<void> {
    let names: string[];
    try {
      names = await this.host.listDir(this.dir);
    } catch {
      return;
    }
    for (const name of names) {
      // `*.AAI`：大小写不敏感（Windows 上文件名本来就不区分大小写）
      if (!/\.aai$/i.test(name)) continue;
      try {
        const file = this.host.join(this.dir, name);
        if (!(await this.host.isFile(file))) continue; // 目录项：跳过
        const index = parseAlfIndex(await this.host.readBytes(file));
        if (index.title !== this.index.title) {
          throw new Error(`aai-title-mismatch:${index.title}`);
        }
        if (!Number.isInteger(index.zone) || index.zone < 0 || index.zone >= AAI_SLOT_COUNT) {
          // 规范不规定 ≥ 槽表容量的区号（`CORE-FS-EXT-INDEX-KEY` 的成立条件）
          throw new Error(`aai-zone-out-of-slot:${index.zone}`);
        }
        this.slots[index.zone] = index;
      } catch (err) {
        this.aaiDiagnostics.push(`${name}: ${err instanceof Error ? err.message : String(err)}`);
      }
    }
  }

  /** 槽表某一格（诊断用）；越界 / 空 ⇒ `null` */
  slot(zone: number): AlfIndex | null {
    if (!Number.isInteger(zone) || zone < 0 || zone >= AAI_SLOT_COUNT) return null;
    return this.slots[zone] ?? null;
  }

  entry(id: number): AlfFile {
    return this.resolve(id).entry;
  }

  /**
   * id → （索引, 条目）。id 分两区（`CORE-FS-ID-ZONES`）：
   * 高 8 位 0 ⇒ 基础索引；非 0 ⇒ 按**有符号字节**解释成区号，低 24 位是**那份索引自己的**下标。
   * ★ 失败必须彼此可分（`CORE-FS-FETCH-FAIL`）：
   *   `script-id-out-of-range`（基础区）· `aai-zone-undefined`（128..255，无定义）·
   *   `aai-not-loaded`（槽空）· `aai-entry-out-of-range`（扩展区下标越界）。
   */
  private resolve(id: number): { index: AlfIndex; entry: AlfFile; zone: number } {
    if (!Number.isInteger(id)) throw new Error(`script-id-not-integer:${id}`);
    const zone = (id | 0) >> 24; // ★ 算术右移 = 有符号区号（128..255 得负数）
    if (zone === 0) {
      if (id < 0 || id >= this.index.files.length) {
        throw new Error(`script-id-out-of-range:${id}/${this.index.files.length}`);
      }
      return { index: this.index, entry: this.index.files[id]!, zone: 0 };
    }
    // 128..255：规范说没有定义（槽表里没有那一格）⇒ 明确失败，⛔ 不模拟"读槽表之外"
    if (zone < 0) throw new Error(`aai-zone-undefined:${zone}`);
    const index = this.slots[zone] ?? null;
    if (index === null) throw new Error(`aai-not-loaded:${zone}`);
    const idx = id & 0xffffff;
    if (idx >= index.files.length) {
      throw new Error(`aai-entry-out-of-range:${zone}:${idx}:${index.files.length}`);
    }
    return { index, entry: index.files[idx]!, zone };
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

  /** 归档切片：归档名用**该条目所属那份索引自己的**归档表（⛔ 不给扩展索引加盘符前缀） */
  private async readArchiveSlice(index: AlfIndex, entry: AlfFile): Promise<Uint8Array> {
    // ★ 哨兵在**偏移**字段上：offset == 0xFFFFFFFF ⇒ 这条记录不进入归档回退 ⇒ 取件失败
    if (entry.offset === OFFSET_NO_ARCHIVE) throw new Error(`alf-no-archive:${entry.name}`);
    const archive = index.archives[entry.archiveIndex];
    const hostArchive = archive === undefined ? null : this.host.pathFromEngine(archive);
    if (hostArchive === null || !isBareName(hostArchive)) {
      throw new Error(`alf-bad-archive:${entry.archiveIndex}/${entry.name}`);
    }
    // 空条目：不碰归档文件（引擎也不读）；否则按 (offset, length) 精确切片
    if (entry.length === 0) return new Uint8Array(0);
    const file = this.host.join(this.dir, hostArchive);
    return this.host.readSlice(file, entry.offset, entry.length);
  }

  /** 只读归档（诊断 / 对照松散覆盖用）：id 可以是基础区或扩展区 */
  async readArchiveById(id: number): Promise<ScriptSource> {
    const { index, entry } = this.resolve(id);
    return {
      id,
      name: entry.name,
      side: 'archive',
      bytes: await this.readArchiveSlice(index, entry),
      entryLength: entry.length,
      archive: index.archives[entry.archiveIndex],
    };
  }

  /** 真实入口：松散优先，否则归档切片（两个区走同一套形状） */
  async readById(id: number): Promise<ScriptSource> {
    const { index, entry } = this.resolve(id);
    const loose = await this.loosePath(entry.name);
    if (loose) {
      return { id, name: entry.name, side: 'loose', bytes: await this.host.readBytes(loose), entryLength: entry.length };
    }
    return {
      id,
      name: entry.name,
      side: 'archive',
      bytes: await this.readArchiveSlice(index, entry),
      entryLength: entry.length,
      archive: index.archives[entry.archiveIndex],
    };
  }

  /**
   * 按名字反查（`CORE-FS-INDEX-STACK` 第 4 条）：**基础索引优先**，其次按**区号升序**
   * （1..255；区号 0 恒指基础索引，不参与第二段扫描）；每份索引内部按文件表顺序取第一条。
   * ★ 不区分大小写（宿主文件系统不区分；名字只是定位手段）。
   * ★ 命中扩展索引 ⇒ 返回 `(区号 << 24) | 下标`；全不中 ⇒ `null`（不抛、不返回 0 号文件）。
   */
  async readByName(name: string): Promise<ScriptSource | null> {
    const wanted = name.toLowerCase();
    const base = this.index.files.findIndex((f) => f.name.toLowerCase() === wanted);
    if (base >= 0) return this.readById(base);
    for (let zone = 1; zone < AAI_SLOT_COUNT; zone += 1) {
      const index = this.slots[zone];
      if (index === null) continue;
      const idx = index.files.findIndex((f) => f.name.toLowerCase() === wanted);
      if (idx >= 0) return this.readById(((zone << 24) | idx) >>> 0);
    }
    return null;
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
