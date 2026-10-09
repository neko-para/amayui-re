/** @env assets @kind product @why "能跑到 play-movie"这条里程碑塌了：headless 前端/环境初始化/LOGO 路径上的指令语义有一处不对 */
/**
 * tools/test/emulator-headless-logo.assets.test.mjs —— **跑到 `play-movie` 的端到端判据**
 *
 * ## 为什么这条守卫值得存在
 * 它是本轮里程碑的**唯一**客观凭据：从安装目录（ALF 归档里）取出 `LOGO.BIN`、
 * 装载、按主循环跑到 `0x20F play-movie`。它一次钉住四件事：
 * 1. **环境初始化 + fs 抽象**真的能把归档里的脚本取出来（不是从磁盘摊开的文件）；
 * 2. **等待门**真的按场景里的计时窗把执行挡住了 5 秒（虚拟时间），而不是被当成 no-op 跳过；
 * 3. **副作用记账**的条数与归类是稳定的（`logged-only` / `modeled` / `not-provided` 各自可数）；
 * 4. **统一文件 id == ALF 条目下标**这条观察（启动链里 `call-script 5262` 就是 `LOGO.BIN`）。
 *
 * ★ 判据是**具体数字**（步数/帧数/虚拟时刻）而不是"跑完了" ——
 *   数字变了就说明某条指令的语义或某一处记账动了，那正是要被看见的事。
 * ★ 样本缺席（fresh clone / 没装游戏）⇒ **skip**，不是红。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { readAlf } from '@amayui/age-format/src/alf.mts';
import { createHeadlessInstance } from '../../apps/emulator/frontends/headless/run.ts';
import { rootFromAssetsJson } from '../../apps/emulator/frontends/headless/paths.ts';
import { buildChainReport } from '../../apps/emulator/frontends/headless/chain-report.ts';
import { describeStop } from '../../apps/emulator/src/vm/machine.ts';
// ★ 语料普查要用装载器 + 指令表（普查那条测的是"哪些操作数 type 真被用到" ⇒ 它决定实现范围）
import { loadScript } from '../../apps/emulator/src/vm/script.ts';
import { OPCODE_TABLE } from '@amayui/age-format/src/asm/runtime.mts';

const PLAY_MOVIE = 0x20f;

const installDir = rootFromAssetsJson(REPO_ROOT, 'gameInstall');
const haveInstall = Boolean(installDir) && fs.existsSync(installDir);
const skip = haveInstall ? false : '游戏安装不在场（corpus/assets.json 的 roots.gameInstall 指向的目录不存在）';

test('★ 统一文件 id == ALF 条目下标（启动链的 `call-script 5262` 就是 LOGO.BIN）', { skip }, () => {
  const index = path.join(installDir, 'SYS4INI.BIN');
  assert.ok(fs.existsSync(index), `索引不在场：${index}`);
  const alf = readAlf(index);
  // ★ 根脚本按定义是**统一文件 id 0**；启动链里 LOGO 是 `call-script 5262`
  assert.equal(alf.entries[0].filename.toUpperCase(), 'SYSTEM4.BIN', 'id 0 必须是根脚本（引擎装载根脚本用的就是 id 0）');
  assert.equal(alf.entries[0x5262].filename.toUpperCase(), 'LOGO.BIN', 'id 0x5262 必须是 LOGO.BIN');
  assert.equal(alf.entries[0x5263].filename.toUpperCase(), 'INIT.BIN');
  assert.equal(alf.entries[0x5264].filename.toUpperCase(), 'TITLE.BIN');
  // SYSTEM4.BIN 里没有名字字面量这条判据的**反面**：名字来自数据文件（索引条目）
  assert.ok(alf.entries.length > 0x5262, '索引必须长过 0x5262（否则上面的下标是巧合）');
});

test('★ 标准启动流程：按统一文件 id 装载根脚本（不是按名字），并且能派发指令', { skip }, () => {
  const asm = createHeadlessInstance({
    repoRoot: REPO_ROOT, instanceId: 'guard-boot', cliInstall: installDir, cliUser: null, env: {},
  });
  // ★ 前端必须提供"按 id 取脚本"这条能力（`call-script` 在**执行期**才要字节）
  assert.ok(asm.instance.scripts, 'headless 前端必须提供脚本来源（否则 call-script 会响亮失败）');
  // ★★ 地址空间：`Machine` 持有它，而且**增长钩子是接通的**（决策 `REQ-01M4B969TBWVERFCB1MXS2Q2E1`：
  //    "初值 0 + 按需增长、**每次增长留痕**"）—— ⛔ 光有钩子类型而没有装配 = 死代码。
  assert.ok(asm.machine.space, 'Machine 必须持有地址空间');
  const probe = asm.machine.space.alloc({ tag: 'guard-probe', elemBytes: 4, capacity: 0 });
  assert.equal(asm.machine.space.ensureCapacity(probe, 4, 'assets guard'), true, '显式增长应当成功');
  assert.equal(probe.capacity, 5, '涨到"容得下第 4 格"= 5');
  assert.equal(asm.log.countsByAction().find(([k]) => k.endsWith('.region.grow'))?.[1] ?? 0, 1,
    '★ 增长必须进副作用日志（region.grow）—— 否则"按需增长"会退化成"悄悄把越界放过去"');

  const root = asm.instance.scripts.loadById(0);
  assert.ok(root, '统一文件 id 0 必须取得到');
  assert.equal(root.name.toUpperCase(), 'SYSTEM4.BIN', '根脚本 = 统一文件 id 0（已登记进台账的观察）');

  asm.machine.loadScriptById(0);
  assert.equal(asm.machine.frame.scriptName.toUpperCase(), 'SYSTEM4.BIN');
  assert.equal(asm.machine.scriptOf().instructions.length, 545, 'SYSTEM4.BIN 的指令数（结构基准）');

  // ★ 第 0 条是 `comment`（no-op），第 1 条是 `0x1a8`（no-op）⇒ 它们必须被正常执行掉。
  const r = asm.machine.run({ stopAtOpcode: 0xffff });
  // ★★ `0x6 load-frame`：**帧记录没建**，必须每次留痕（不许静默）—— 这是本轮唯一"故意欠着"的那一步
  const lf = asm.log.records.filter((e) => e.action === 'engine.load-frame');
  assert.ok(lf.length >= 4, `0x6 load-frame 在这次运行里至少 4 次（实测 ${lf.length}）—— 每次都要进保真欠账`);
  for (const e of lf) {
    assert.equal(e.disposition, 'logged-only', '未建模 ⇒ logged-only（会进「保真欠账」）');
    assert.ok(e.detail.cur < 40, '目标帧深必须在引擎的上限 40 之内');
    assert.match(String(e.detail.note), /帧记录没建/, '欠账信息里要写明"帧记录没建"与原因');
    assert.ok(typeof e.detail.script === 'string' && e.detail.script.length > 0, '要记下装载到的是哪份脚本');
  }
  // ★★ **前沿棘轮**：启动链能执行的**指令数**与**停在哪**都只许往前走。
  //    数字由实跑复算（不是手写）；实现新 handler 会让它涨 ⇒ 那时**要同步抬高这里**。
  //    它防的是"某个 handler 悄悄坏了 ⇒ 前沿倒退"（那种倒退否则只会表现成日志变短）。
  // ★★ 前沿棘轮（数字由实跑复算；往前走了就抬高它）
  //    走过的路：根脚本 → `call` 子程序 → `jcc` 配置门 → `call-script` → `INITCONFIG.BIN`
  //    → `INITCONFIG0..5`（**全部跑完**，含 `INITCONFIG4` 那个 1000 次循环）
  //    → `call-script INITCHARM.BIN`（**跑完**）→ `SYSTEM4` 后半段逐条推
  //    `0x107/0x10b/0xfe/0x10c/0x10f/0x308/0x30a/0x25b/0x248` 那条链 → `0x6 load-frame` ×4
  //    → 进入 `INIT2.BIN`，它依次 `call-script` 一整套 init：`WDINIT` / `LKINIT` / `DPINIT` /
  //    `ALINIT`(3993) / `IMINIT`(6070) / `VIINIT`(643) / `CIINIT`(470) / `BIINIT`(961) / `BTANINIT2`(1063)
  //    → 现在停在 `INIT2.BIN#134 = 0x143`。
  assert.ok(r.steps >= 85994, `启动链至少能执行 85994 步（实际 ${r.steps}）—— ${describeStop(r.reason)}`);
  if (r.reason.kind === 'error') {
    assert.equal(r.reason.script.toUpperCase(), 'INIT2.BIN', '当前在 `INIT2.BIN`（各 INIT 子脚本的调用者）');
    assert.equal(r.reason.opcode, 0x143, '当前停点 = `0x143`（指令表没命名的那一族）');
    assert.equal(r.reason.index, 134, '当前停点的指令下标（INIT2#134）');
  } else {
    assert.fail(`启动链应当仍停在某个明确缺口上，实际：${describeStop(r.reason)}`);
  }
});

test('★ 语料的操作数 type 普查：指针族里**只有 0xc/0xe 用得上**（0x6/0x7/0x8/0xd 一次都没出现）', { skip }, () => {
  const files = fs.readdirSync(installDir).filter((f) => f.toUpperCase().endsWith('.BIN'));
  assert.ok(files.length >= 100, `安装目录里的散装脚本数（实测 ${files.length}）`);
  const byType = new Map();
  let scripts = 0;
  for (const f of files) {
    let s;
    try {
      s = loadScript(f, new Uint8Array(fs.readFileSync(path.join(installDir, f))), { table: OPCODE_TABLE });
    } catch { continue; }
    scripts += 1;
    for (const ins of s.instructions) for (const a of ins.args) byType.set(a.type, (byType.get(a.type) ?? 0) + 1);
  }
  assert.ok(scripts >= 100, `能装载的脚本数（实测 ${scripts}）`);
  // ★ 用了的：0xc（local-ptr）与 0xe（local string-ptr）—— 后者的量决定了"字符串也要能发地址"
  assert.ok((byType.get(0xc) ?? 0) > 10000, `type 0xc 的出现次数（实测 ${byType.get(0xc) ?? 0}）`);
  assert.ok((byType.get(0xe) ?? 0) > 1000, `type 0xe 的出现次数（实测 ${byType.get(0xe) ?? 0}）`);
  // ★ 一次都没出现的：0x6（全局指针）/ 0x7（全局浮点指针）/ 0x8（全局字符串指针）/ 0xd（局部浮点指针）
  //   ⇒ 本层不实现它们（**写**那两支顺手实现了，代价为零）；真出现时会响亮失败（不是静默）
  for (const t of [0x6, 0x7, 0x8, 0xd]) {
    assert.equal(byType.get(t) ?? 0, 0, `type 0x${t.toString(16)} 在本语料里应当一次都不出现（实测 ${byType.get(t) ?? 0}）`);
  }
});

test('★ 启动链的规模与缺口可复算（到 LOGO 为止：脚本数 / 指令数 / 缺 handler 的 opcode 数）', { skip }, () => {
  const asm = createHeadlessInstance({
    repoRoot: REPO_ROOT, instanceId: 'guard-chain', cliInstall: installDir, cliUser: null, env: {},
  });
  // ★ 与实跑走同一条路径（分层 fs ⇒ **松散文件优先**）。⛔ 不要只读归档源：
  //   实测这台安装有 107 个松散 `.BIN`，松散 `SYSTEM4.BIN` 12012 B ≠ 归档 TOC 的 11992 B
  //   ⇒ 只读归档会量到**另一份没人跑的文件**，报告的缺口清单就是假的。
  const report = buildChainReport(asm.instance.fs, path.join(installDir, 'SYS4INI.BIN'));

  // ★ 截断点：闭包必须停在 SYSTEM4 里 `call-script LOGO.BIN` 那一条（否则它会把整个游戏拉进来）
  assert.ok(report.cut, '必须报出闭包截断点');
  assert.equal(report.cut.targetName.toUpperCase(), 'LOGO.BIN');
  assert.equal(report.cut.targetId, 0x5262, 'call-script 5262（已登记：id == ALF 条目下标）');
  assert.equal(report.cut.fromScript.toUpperCase(), 'SYSTEM4.BIN');

  // —— 这三个数字是"实现启动链"的工作量基准（它们变了就说明链本身或口径变了）——
  assert.equal(report.scripts.length, 65, '到 LOGO 为止的脚本数');
  const totalIns = report.scripts.reduce((n, s) => n + s.instructions, 0);
  assert.equal(totalIns, 74031, '到 LOGO 为止的指令总数（★ 走**分层 fs**：松散脚本优先，与实跑同一条输入）');
  assert.equal(report.cut.at, 'SYSTEM4.BIN#134', '截断点的位置（松散的 SYSTEM4 比归档多一条指令）');
  // ★ 缺口种数是**棘轮**：只许下降（实现 handler 会让它变小；变小不用改这条，
  //   而"某个 handler 没了"会让它变大 ⇒ 当场红）。数字由上面的报告复算得出，不手写。
  assert.ok(report.missing.length <= 62, `缺 handler 的 opcode 种数只许下降，现在 ${report.missing.length}（上限 62）`);
  // 缺的那几个必须是**真缺**（不能因为有 handler 而被算进来）
  for (const m of report.missing) assert.ok(m.sites > 0 && m.scripts > 0, `${m.name} 的出现/脚本数必须是正的`);
  // ★ 报告的**契约**（不依赖当前实现了哪些 handler，所以实现推进时它不会假红）：
  //   ① 按出现次数降序 —— 那条顺序就是实现顺序；② 每条 `sites ≥ scripts`（同一份脚本可出现多次）。
  for (let i = 1; i < report.missing.length; i += 1) {
    assert.ok(report.missing[i - 1].sites >= report.missing[i].sites,
      `缺口列表必须按出现次数降序：${report.missing[i - 1].name}(${report.missing[i - 1].sites}) 排在 ${report.missing[i].name}(${report.missing[i].sites}) 前面`);
  }
  for (const m of report.missing) {
    assert.ok(m.sites >= m.scripts, `${m.name}：sites(${m.sites}) 必须 ≥ scripts(${m.scripts})`);
    assert.ok(m.sample.script.length > 0 && Number.isInteger(m.sample.index), `${m.name} 必须给出可去取证的一处实例`);
  }
  // ★ 闭包里**存在动态 `call-script` 目标**（实测 `SETSTAGE.BIN#70`：操作数 0 不是立即数，
  //   目标在运行时才算出来）⇒ 静态报告算不出它，**但必须显式列出来**（不许静默丢）。
  //   棘轮：这个数不许涨（涨 = 出现了新的"静态算不出"形态，那要单独看）。
  assert.ok(report.unresolved.length <= 2, `静态算不出的目标只许是已知那几处，现在 ${report.unresolved.length} 处：\n${report.unresolved.join('\n')}`);
  for (const u of report.unresolved) assert.ok(u.length > 0 && u.includes('call-script'), `算不出的项必须说清是哪条 call-script：${u}`);
});

test('★ 三条子脚本各自跑到"下一个明确缺口"（`LOADCONFIG` / `CHECKCONFIG` 直跑量出来）', { skip }, () => {
  // ★ 为什么"直跑"而不是走启动链：`SYSTEM4#56` 的 `jcc (global-int 5)` 由**引擎侧**驱动
  //   （配置存在 ⇒ 置某个 global），**不是脚本自己读配置** —— 那是已登记的能力缺口
  //   （实测：同一实例连跑两次，配置确实落盘了 14 条，但两次走的是同一条支）。
  //   ⇒ 在补上那个能力之前，**以子脚本为根**是唯一诚实且可复算的量法。
  const cases = /** @type {const} */ ([
    // ★ `LOADCONFIG` 现在**跑完**（`exit`）—— 停点不再是"缺口"，所以它的 opcode/index 用 `null` 表示"跑到尾"
    [21080, 'LOADCONFIG.BIN', null, null, 10032],
    // ★ CHECKCONFIG 现在也**跑完**了（x2ee 实现之后）
    [20955, 'CHECKCONFIG.BIN', null, null, 41],
  ]);
  for (const [id, name, opcode, index, steps] of cases) {
    const asm = createHeadlessInstance({
      repoRoot: REPO_ROOT, instanceId: `guard-sub-${id}`, cliInstall: installDir, cliUser: null, env: {},
    });
    asm.machine.loadScriptById(id);
    assert.equal(asm.machine.frame.scriptName.toUpperCase(), name, `id ${id} 应当是 ${name}`);
    const r = asm.machine.run({ stopAtOpcode: 0xffff });
    assert.equal(r.steps, steps, `${name} 的步数（实跑复算；前沿往前走了就抬高这条）`);
    if (opcode === null) {
      assert.equal(r.reason.kind, 'exit', `${name} 应当**跑完**：${describeStop(r.reason)}`);
    } else {
      assert.equal(r.reason.kind, 'error', `${name} 应当停在一个明确缺口上：${describeStop(r.reason)}`);
      if (r.reason.kind === 'error') {
        assert.equal(r.reason.opcode, opcode, `${name} 的停点 opcode`);
        assert.equal(r.reason.index, index, `${name} 的停点下标`);
      }
    }
  }
});

test('★ 从归档里取 LOGO.BIN 并跑到 `play-movie`（步数 / 帧数 / 虚拟时刻 / 副作用归类都要对）', { skip }, () => {
  const asm = createHeadlessInstance({
    repoRoot: REPO_ROOT,
    instanceId: 'guard-logo',
    cliInstall: installDir,
    cliUser: null,
    env: {},
  });

  const bytes = asm.instance.fs.read('LOGO.BIN');
  assert.ok(bytes, 'LOGO.BIN 必须能从"松散目录 + ALF 归档"这两层里取到');
  // ★ 随机源必须由**前端**注入（`0x60` 语料里 22 处 ⇒ 游戏确实用到它）。
  //   "前端忘了注入"这条路不会报错 —— 它只会在跑到那条指令时才炸 ⇒ 用守卫把它提前。
  assert.ok(asm.instance.random, 'headless 前端必须注入随机源（否则 `0x60 random` 会响亮失败）');
  assert.equal(asm.instance.random.label.startsWith('seeded(0x'), true, '随机源必须是**带种子的**（可复现），不是裸 Math.random');
  assert.equal(bytes.length, 1508, 'LOGO.BIN 的字节数');
  // ★ 取到的必须是**归档里那一份**（松散目录里没有它）—— 命中来源要能说出来
  assert.equal(asm.instance.fs.locate('LOGO.BIN').side.startsWith('ALF'), true, '应当命中 ALF 索引那一层');
  assert.deepEqual(asm.instance.fs.demandsSorted(), [], '这一次跑不该有读不到的demand');

  asm.machine.loadScriptBytes('LOGO.BIN', bytes);
  const r = asm.machine.run({ stopAtOpcode: PLAY_MOVIE });

  assert.equal(r.reason.kind, 'instruction', `应当命中停止条件而不是别的：${describeStop(r.reason)}`);
  assert.equal(r.reason.opcode, PLAY_MOVIE);
  assert.equal(r.reason.index, 46, 'play-movie 必须是第 46 条（0 起）');
  assert.equal(r.steps, 47, '执行了 47 条指令');
  // ★ 这一对数字是"等待门真的等了"的判据：版权页 4500+500 ms 的窗 + 两帧锁存
  assert.equal(r.atMs, 5024, `虚拟时刻应当是 5024ms（窗 5000ms + 锁存帧），实际 ${r.atMs}`);
  assert.equal(r.ticks, 314, `过了 314 帧（5024 / 16），实际 ${r.ticks}`);
  assert.equal(asm.machine.frameNo, 314);

  const byAction = new Map(asm.log.countsByAction());
  assert.equal(byAction.get('system.script.load'), 1);
  assert.equal(byAction.get('system.gate.block'), 1, '等待门挡住过一次（且只记一条，不按帧刷屏）');
  assert.equal(byAction.get('system.gate.open'), 1);
  assert.equal(byAction.get('render.texture.bind'), 2);
  assert.equal(byAction.get('render.mesh.create'), 2);
  assert.equal(byAction.get('audio.movie.play'), 1);
  assert.equal(byAction.get('input.poll'), 1);

  // 门是被**场景里的计时窗**放行的（不是"没有判据来源"那条 not-provided 路径）
  const gate = asm.log.records.find((x) => x.action === 'gate.open');
  assert.equal(gate.detail.waitingOn, 'scene-window');
  assert.equal(gate.detail.waitedMs, 5024);
  assert.equal(gate.disposition, 'modeled');
  assert.equal(asm.log.records.find((x) => x.action === 'gate.block').detail.waitingOn, 'scene-window');

  // —— 副作用三类各自可数，而且 headless 的缺口**被记成 not-provided** ——
  const byDisp = new Map(asm.log.countsByDisposition());
  assert.equal(byDisp.get('not-provided'), 1, '`input.poll`：headless 没有输入源 ⇒ 记 not-provided，而不是"读到了没有按键"');
  assert.ok(byDisp.get('logged-only') >= 3, '`resource.read.request` / `audio.movie.play` 都是 logged-only');
  assert.ok(byDisp.get('modeled') > 0);

  // —— 三条计时窗（`set-draw-color` 1 条 + `set-vertex-color-alpha` 2 条）都建起并跑完 ——
  assert.equal(byAction.get('render.anim.window'), 3);
  assert.equal(byAction.get('render.anim.done'), 3);

  // —— 这条路径不该碰到"未初始化的池"：本仓没做装载期池初始化 ⇒ 一旦碰到就该显形 ——
  const notes = [...asm.machine.diag.oobByKind.keys()];
  assert.deepEqual(notes.filter((k) => k.startsWith('uninitialized-read')), [],
    `LOGO 到 play-movie 的路径不该依赖未初始化的池槽，实际留痕：${JSON.stringify(notes)}`);

  // —— play-movie 自己的记账：异步 + 资源请求 + 影片对象表未建模 ——
  const movie = asm.log.records.find((x) => x.action === 'movie.play');
  assert.equal(movie.disposition, 'logged-only', 'headless 没有视频播放器 ⇒ 记 logged-only');
  assert.equal(movie.detail.async, true, '取证结论：play-movie 是异步的（播放在主循环的后续帧里）');
  assert.equal(movie.detail.slot, 42, 'op2 = 槽号（与 set-texture 的槽是同一维度：LOGO 全程用的是 0x2a）');
});

// 注：原先这里挂过 `REPRO(ret-0x05)` 锚点（当时 ret 被实现成弹帧）。该缺陷已收口
// （`REQ-01M4AT41GQBN6D7QE0G349NH1R` 的 verify = `tools/test/emulator-frames.test.mjs`）
// ⇒ 锚点撤掉：判据改由那份**纯函数**守卫承担（它不依赖启动链推进到第 56 条）。
