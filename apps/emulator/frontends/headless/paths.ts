/**
 * apps/emulator/frontends/headless/paths.ts —— **宿主路径的唯一解析点**（Node 侧）
 *
 * ## 为什么值得单独一个文件
 * 核心（`src/host/environment.ts`）**故意**不接受路径、也猜不出路径
 * （它没有 `node:os`、读不到环境变量、也不知道谁是"安装目录"）。于是"路径从哪来"
 * 必然落在前端身上 —— 那就必须**只有一个地方**回答它。
 * ★ 旧仓为此付过代价：资源根一度散落在 **9 处**（4 个模块 + 5 个测试各自硬编码），
 *   结果是"换一套资源要改 9 个地方"，而且**测试用的语料和产品读的可以是两份**。
 *
 * ## 优先级（**显式**，不猜）
 * ```
 * --install  >  AMAYUI_INSTALL_DIR  >  corpus/assets.json 的 roots.gameInstall  >  报错
 * --user     >  AMAYUI_USER_DIR     >  <repo>/.tmp/emulator-headless/<id>/user
 * ```
 * ★ 解析顺序里有 `corpus/assets.json` 的 `roots`：那是本仓**已经存在**的"这些路径是什么"的
 *   单一真源（`oldRepo` / `gameInstall` / `gameSaves`），没必要再立一份。
 *   本文件只按名字读那几个根，**不解释**它的其它字段（schema 的真源是 `pnpm tools corpus describe`）。
 *
 * ## ★★ 默认用户根**故意不**指向玩家的真实存档目录
 * 旧仓的默认是 `%LOCALAPPDATA%\Eushully\天結いキャッスルマイスター`（那是玩家真在用的地方）。
 * 对**开发/回归用**的 headless 前端来说，这个默认是个陷阱：
 * 一次回归就会往真存档目录里写东西，而且"这次跑的结果"从此取决于那一堆残留。
 * ⇒ 本前端的默认落点是**仓库内**的 `.tmp/emulator-headless/<实例 id>/user`（gitignore 区），
 *   要跑真实数据必须**显式** `--user`。★ 这不是"忘了实现"，是一条有意的取舍，写在这里以免被"顺手修正"。
 *
 * ## 不用 `import.meta`
 * 旧仓实测：同时被 Electron 主进程（esbuild 打成 CJS）与 Node 工具引用的模块里写 `import.meta`
 * 会直接报错。⇒ `repoRoot` 由**调用方**传（CLI 入口传 `process.cwd()` 或 `--repo`），
 * 本文件不自己找。
 */

import * as fs from 'node:fs';
import * as path from 'node:path';
import { identityOf } from './node-fs.ts';
import type { RootRef } from '../../src/host/environment.ts';

/** 解析的输入（全是显式参数 —— 环境变量的读取在**入口**做，见 `main.ts`） */
export interface RootInputs {
  /** 仓库根（调用方传；本模块不自己找） */
  repoRoot: string;
  /** 实例 id（决定默认用户根） */
  instanceId: string;
  /** `--install` */
  cliInstall?: string | null;
  /** `--user` */
  cliUser?: string | null;
  /** `--repo` 之外的资源根覆盖（`AMAYUI_INSTALL_DIR` / `AMAYUI_USER_DIR`） */
  env?: { install?: string | null; user?: string | null };
}

/** 解析结果 + **非致命**说明（用了哪个来源必须能看见） */
export interface RootResolution {
  installRoot: RootRef;
  userRoot: RootRef;
  problems: string[];
}

/** `.tmp` 下的默认落点（生成物区，gitignore；**不入库**） */
export const HEADLESS_TMP_REL = path.join('.tmp', 'emulator-headless');

/** 从 `corpus/assets.json` 读一个根（读不到 ⇒ `null`，**不抛**：这只是一条回落路径） */
export function rootFromAssetsJson(repoRoot: string, name: string): string | null {
  const file = path.join(repoRoot, 'corpus', 'assets.json');
  try {
    const j = JSON.parse(fs.readFileSync(file, 'utf8')) as { roots?: Record<string, string> };
    const v = j.roots?.[name];
    return typeof v === 'string' && v !== '' ? v : null;
  } catch {
    return null;
  }
}

/**
 * 解析两个根。**这一层必须能说出"我从哪拿到的"** —— 于是返回值里带 problems，
 * 入口会把它们逐行打出来（旧仓的教训："路径名不可靠，不做猜测" ⇒ 那就把它打出来）。
 */
export function resolveRoots(input: RootInputs): RootResolution {
  const problems: string[] = [];

  // —— 安装根（只读）——
  let installRaw: string | null = null;
  let installFrom = '';
  if (input.cliInstall) { installRaw = input.cliInstall; installFrom = '--install'; }
  else if (input.env?.install) { installRaw = input.env.install; installFrom = 'AMAYUI_INSTALL_DIR'; }
  else {
    installRaw = rootFromAssetsJson(input.repoRoot, 'gameInstall');
    if (installRaw) installFrom = 'corpus/assets.json 的 roots.gameInstall';
  }
  if (!installRaw) {
    throw new Error(
      '解析不出安装根：请给 --install <目录>（或设 AMAYUI_INSTALL_DIR），' +
      `或在 corpus/assets.json 的 roots 里登记 gameInstall。★ 本层不猜路径（见 paths.ts 头注）`,
    );
  }
  const installAbs = path.resolve(installRaw);
  if (!fs.existsSync(installAbs)) throw new Error(`安装根不存在：${installAbs}（来源：${installFrom}）`);
  problems.push(`安装根 = ${installAbs}（来源：${installFrom}）`);

  // —— 用户根（可写：存档 / 配置）——
  let userRaw: string | null = null;
  let userFrom = '';
  if (input.cliUser) { userRaw = input.cliUser; userFrom = '--user'; }
  else if (input.env?.user) { userRaw = input.env.user; userFrom = 'AMAYUI_USER_DIR'; }
  else {
    userRaw = path.join(input.repoRoot, HEADLESS_TMP_REL, input.instanceId, 'user');
    userFrom = '默认（仓库内 .tmp；**故意不**指向玩家真实存档目录）';
  }
  const userAbs = path.resolve(userRaw);
  problems.push(`用户根 = ${userAbs}（来源：${userFrom}）`);

  // ★ 身份标记与 `NodeDirSource` / `NodeWriteArea` **同源**（都用 `identityOf`）。
  //   两边算法不同的话，"可写区与只读源是不是同一块地方"这条判据会**永远判否** ——
  //   那是静默失效，比判错更糟。
  return {
    installRoot: { label: installAbs, identity: identityOf(installAbs) },
    userRoot: { label: userAbs, identity: identityOf(userAbs) },
    problems,
  };
}

/** 确保目录存在（可写区要先有目录；`NodeWriteArea` 也能自己建，但显式更利于排错） */
export function ensureDir(dir: string): string {
  fs.mkdirSync(dir, { recursive: true });
  return dir;
}
