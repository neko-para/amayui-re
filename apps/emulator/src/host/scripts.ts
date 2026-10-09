/**
 * apps/emulator/src/host/scripts.ts —— **脚本的取用**（★ 核心层：零 Node 依赖）
 *
 * 引擎在**执行期**按**统一文件 id** 装载脚本（`call-script <id>` 会去请求那一份字节），而"id → 字节"
 * 只有前端做得了（它知道 ALF 索引在哪、归档在哪，甚至可能从网络取）⇒ 核心只认这个接口。
 *
 * ★ 核心按 **id** 取、不按名字（**根脚本 = 统一文件 id 0**（`SYSTEM4.BIN`），`LOGO.BIN` 是
 *   `call-script 5262`）：按名字装载会逼前端维护一份会漂的 id→名字反查表（扩展包、改名、大小写），
 *   而名字只用于**日志与报错**。
 * ★ 取不到 ⇒ `null`，由调用方响亮失败 —— 统一成"空脚本"会让 `call-script` 变成静默 no-op，
 *   而后面每条指令都在错的上下文里跑。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/scripts-by-file-id`）。
 */

/** 一份按 id 取到的脚本 */
export interface LoadedBytes {
  /** 人类可读的名字（**只用于日志与报错**；核心的引用一律用 id） */
  name: string;
  bytes: Uint8Array;
}

/** 脚本来源：核心在执行期按**统一文件 id** 要字节 */
export interface ScriptLoader {
  readonly label: string;
  /** 取一份脚本；取不到 ⇒ `null`（**不抛**：调用方要能给出"哪个 id 取不到"这种信息） */
  loadById(id: number): LoadedBytes | null;
}

/**
 * 一份**纯内存**的脚本来源（测试与守卫的基准；也用于"只跑一份脚本"的直装模式）。
 * ★ 它按 **id** 建表（不是按名字）：这样"直装模式"与"启动链模式"走的是**同一条**取用路径，
 *   于是 `call-script` 在两种模式下行为一致。
 */
export class MemoryScriptLoader implements ScriptLoader {
  readonly label: string;
  readonly byId: Map<number, LoadedBytes>;

  constructor(label = 'memory-scripts', entries?: Iterable<[number, LoadedBytes]>) {
    this.label = label;
    this.byId = new Map(entries ?? []);
  }

  /** 登记一份（直装模式：把入口脚本放在某个 id 上） */
  put(id: number, name: string, bytes: Uint8Array): void {
    this.byId.set(id >>> 0, { name, bytes });
  }

  loadById(id: number): LoadedBytes | null {
    return this.byId.get(id >>> 0) ?? null;
  }

  get size(): number {
    return this.byId.size;
  }
}
