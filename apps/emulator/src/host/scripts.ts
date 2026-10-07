/**
 * apps/emulator/src/host/scripts.ts —— **脚本的取用**（★ 核心层：零 Node 依赖）
 *
 * ## 为什么这是一个**宿主能力**（而不是核心自己去读文件）
 * 引擎在**执行期**按**统一文件 id** 装载脚本：`call-script <id>` 会去请求那一份字节。
 * 而"id → 字节"这件事只有前端做得了（它知道 ALF 索引在哪、归档在哪、甚至可能从网络取）。
 * ⇒ 核心只认这个接口；`call-script` 拿到字节之后一切照旧。
 *
 * ## ★ 为什么按 **id** 而不是按名字
 * 引擎侧就是这么做的，而且已登记进台账的观察说：**根脚本 = 统一文件 id 0**
 * （`SYSTEM4.BIN`），`LOGO.BIN` 是 `call-script 5262`。脚本之间的引用**全是 id**
 * ⇒ 如果核心按名字装载，前端就得自己维护一份 id→名字的反查，而那份表**会漂**
 * （扩展包、改名、大小写）。⇒ 核心按 id 要走，名字只用于**日志与报错**。
 *
 * ## ★ 取不到 ⇒ `null`（由调用方响亮失败），不是"给一份空的"
 * 取不到脚本的原因很多（id 越界、扩展包没装、归档缺文件）。把它们统一成"空脚本"会让
 * `call-script` 变成一个静默的 no-op —— 而那正是最难查的一类（后面每条指令都在错的上下文里跑）。
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
