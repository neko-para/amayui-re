/** @env pure @kind contract @why 宿主隔离 / 路径安全 / 副作用归类 / 场景计时窗 这些口径塌了：多实例互相覆盖、或"没做的事"被记成"做了" */
/**
 * tools/test/emulator-host.test.mjs —— **headless 前端的宿主层与模型契约**
 *
 * ## 它守的是什么（每条都对应一类**不会报错**的坏结果）
 * 1. **路径安全**：`..` / 绝对路径 / 盘符 / 控制字符漏过去 ⇒ 引擎侧的名字能逃出根。
 * 2. **"只读运行"不许表现成"写成功"**：没有可写区时 `write` 必须抛。
 * 3. **可写区与只读源同身份 = 拒绝构造**：写下去就是覆盖原始语料 / 真游戏数据。
 * 4. **多实例不许共享可变状态**：共用 sink / 时钟 / 配置 / 用户根 ⇒
 *    "这次跑的结果"取决于跑的顺序（而**不会报错**）。
 * 5. **配置回写不许静默丢键**（旧仓为此把整份 INI 抹成两行）。
 * 6. **副作用的 `disposition` 三态不许合并**："没做" / "没这张能力" / "做了" 必须分得开。
 * 7. **场景计时窗**：`set-draw-color` 建窗、`set-draw-color-alpha` **不**建窗（家族不对称）；
 *    窗跑完要把工作色提交成目标色。
 * 8. **操作数 type**：指针族（6/7/8/c/d/e）**必须抛** —— 当普通池读会"类型对、值错"且不报错。
 *
 * 运行：`pnpm test`（纯反射 + 内存实现，不需要语料、不碰真游戏安装）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { LayeredFilesystem, MemoryStore, normalizeEngineName, requireEngineName } from '../../apps/emulator/src/host/fs.ts';
import { EffectLog, isKnownAction, EFFECT_DOMAINS, EFFECT_ACTIONS } from '../../apps/emulator/src/host/effects.ts';
import { VirtualClock } from '../../apps/emulator/src/host/clock.ts';
import { MemoryConfig } from '../../apps/emulator/src/host/config.ts';
import { assertInstanceId, resolveEnvironment } from '../../apps/emulator/src/host/environment.ts';
import { Instance, InstanceRegistry, sharedMutableState } from '../../apps/emulator/src/host/instance.ts';
import { SceneModel } from '../../apps/emulator/src/model/scene.ts';
import { LocalPools, GlobalPools } from '../../apps/emulator/src/model/pools.ts';
import { GLOBAL_POOL_BY_TYPE_TAG, POINTER_OPERAND_TYPES, floatFromBits, asFloat32, readOperand, writeOperand } from '../../apps/emulator/src/vm/operand.ts';

const bytes = (...v) => new Uint8Array(v);

// ─────────────────────────────────────────────────── 1. 名字规则

test('★ 名字归一化：`\\`≡`/`、大小写不敏感，且**六类越界一律拒**', () => {
  const ok = normalizeEngineName('SAVE\\Sub\\File.DAT');
  assert.equal(ok.ok, true);
  assert.equal(ok.normalized, 'SAVE/Sub/File.DAT');
  assert.equal(ok.key, 'save/sub/file.dat', '查表键必须小写（Windows 语义）');
  // 末尾分隔符允许（`SAVE\\` 这种写法在配置里出现过）
  assert.equal(normalizeEngineName('SAVE\\').ok, true);

  const cases = [
    ['/etc/passwd', '绝对路径'],
    ['\\foo', '绝对路径'],
    ['C:file', '盘符'],
    ['../x', '`..`'],
    ['a/../b', '`..`'],
    ['./a', '`.` 段'],
    ['a\u0000b', '控制字符'],
    ['', '空名字'],
  ];
  for (const [input, why] of cases) {
    const r = normalizeEngineName(input);
    assert.equal(r.ok, false, `${JSON.stringify(input)} 应当被拒（${why}）`);
  }
  assert.throws(() => requireEngineName('../x'), /非法的引擎侧名字/);
  // ★ 反例的另一半：重复分隔符是**折叠**、不是拒绝（折叠后不存在"同一对象两个名字"）
  assert.equal(normalizeEngineName('a//b').normalized, 'a/b');
  assert.equal(normalizeEngineName('a//b').key, normalizeEngineName('a\\b').key);
});

// ─────────────────────────────────────────────────── 2. 只读源 / 可写区

test('★ 可写区与只读源**同身份** ⇒ 拒绝构造（写下去就是覆盖原始语料）', () => {
  const src = new MemoryStore('安装根', 'C:\\game');
  const same = new MemoryStore('用户根', 'c:\\game'); // 大小写不同但同身份
  assert.throws(
    () => new LayeredFilesystem({ label: 't', sources: [src], writable: same }),
    /同一块地方/,
  );
  const other = new MemoryStore('用户根', 'C:\\user');
  assert.ok(new LayeredFilesystem({ label: 't', sources: [src], writable: other }), '不同身份应当构造成功');
});

test('★ 没有可写区时 `write` 必须抛（"只读运行"不许表现成"写成功"）', () => {
  const fs = new LayeredFilesystem({ label: 'ro', sources: [new MemoryStore('src', 's')] });
  assert.throws(() => fs.write('a.bin', bytes(1)), /没有可写区/);
});

test('★ 读：只读源优先，然后才轮到可写区；读不到 ⇒ null + 一条 demand', () => {
  const src = new MemoryStore('src', 's');
  src.put('same.bin', bytes(1));
  src.put('only-src.bin', bytes(2));
  const w = new MemoryStore('w', 'w');
  w.put('same.bin', bytes(9));
  w.put('only-w.bin', bytes(3));
  const fs = new LayeredFilesystem({ label: 't', sources: [src], writable: w });

  assert.deepEqual([...fs.read('same.bin')], [1], '同名的先命中只读源');
  assert.deepEqual([...fs.read('only-src.bin')], [2]);
  assert.deepEqual([...fs.read('only-w.bin')], [3], '只读源没有才轮到可写区');
  assert.equal(fs.read('nope.bin'), null);
  assert.deepEqual(fs.demandsSorted(), [{ name: 'nope.bin', why: 'not-found', count: 1 }]);
  // 命中来源可被说出（每次读都要能回答"命中了谁"）
  assert.equal(fs.locate('same.bin').side, 'src');
  assert.equal(fs.locate('only-w.bin').side, 'w');
});

test('★ `readPrefix` 不许补齐（短文件就是短的，不伪造长度）', () => {
  const s = new MemoryStore('s', 's');
  s.put('short.bin', bytes(1, 2, 3));
  assert.deepEqual([...s.readPrefix('short.bin', 8)], [1, 2, 3], '文件更短就返回短的');
  assert.deepEqual([...s.readPrefix('short.bin', 2)], [1, 2]);
  assert.equal(s.readPrefix('missing.bin', 4), null);
});

test('★ 每次 `write` 都经归一化：`..` 之类的名字在**进可写区之前**就被挡下', () => {
  const w = new MemoryStore('w', 'w');
  const fs = new LayeredFilesystem({ label: 't', sources: [], writable: w });
  fs.write('SAVE\\a.dat', bytes(7));
  assert.deepEqual([...w.read('save/a.dat')], [7], '归一化后才落盘');
  assert.throws(() => fs.write('../../escape.dat', bytes(1)), /非法的引擎侧名字/);
  assert.equal(w.read('escape.dat'), null);
});

// ─────────────────────────────────────────────────── 3. 多实例隔离

function makeInstance(id, userIdentity) {
  const { env } = resolveEnvironment({
    instanceId: id,
    installRoot: { label: 'C:\\game', identity: 'c:\\game' },
    userRoot: { label: `C:\\u\\${id}`, identity: userIdentity },
  });
  return new Instance({
    env,
    fs: new LayeredFilesystem({ label: id, sources: [new MemoryStore('src', 'c:\\game')], writable: new MemoryStore('w', userIdentity) }),
    effects: new EffectLog(),
    clock: new VirtualClock(0),
    config: new MemoryConfig(),
  });
}

test('★ 两个实例**没有任何一处共享可变对象**（共享了不会报错，只会让结果取决于跑的顺序）', () => {
  const a = makeInstance('iso-a', 'c:\\u\\a');
  const b = makeInstance('iso-b', 'c:\\u\\b');
  assert.deepEqual(sharedMutableState(a, b), [], '两个实例不该共享 fs/effects/clock/config/门/输入/用户根');
  // 反证：把用户根指成同一个 ⇒ 判据必须点名
  const c = makeInstance('iso-c', 'c:\\u\\a');
  assert.ok(sharedMutableState(a, c).includes('env.user.identity'), '同用户根必须被点名');
  // 安装根**允许**相同（只读来源），这条不该被误判
  assert.equal(a.env.install.identity, b.env.install.identity);
});

test('★ 注册表：同一个 id 不许注册两次（重合的落点/日志/存档不会报错）', () => {
  const reg = new InstanceRegistry();
  reg.register(makeInstance('x', 'c:\\u\\x'));
  assert.throws(() => reg.register(makeInstance('x', 'c:\\u\\y')), /实例 id 已被占用/);
  assert.equal(reg.size, 1);
  assert.equal(reg.unregister('x'), true);
  assert.equal(reg.unregister('x'), false);
});

test('★ 实例 id 会参与路径构造 ⇒ 非法 id 一律抛（含 `.` / `..` / 保留段）', () => {
  for (const bad of ['../evil', 'a/b', 'a\\b', '.', '..', 'api', '', 'x'.repeat(65)]) {
    assert.throws(() => assertInstanceId(bad), /非法的实例 id|保留的实例 id/, `${JSON.stringify(bad)} 应当被拒`);
  }
  assert.equal(assertInstanceId('iso-a.1'), 'iso-a.1');
});

test('★ 环境初始化：缺根 ⇒ **抛**（核心不许猜宿主路径）；可默认的那几项 ⇒ 记 problem', () => {
  assert.throws(
    () => resolveEnvironment({ instanceId: 'a', installRoot: { label: 'x', identity: 'x' } }),
    /环境缺少 userRoot/,
  );
  const { env, problems } = resolveEnvironment({
    instanceId: 'a',
    installRoot: { label: 'x', identity: 'x' },
    userRoot: { label: 'y', identity: 'y' },
  });
  assert.equal(env.frameMs, 16);
  assert.equal(env.codecKey, 0);
  assert.ok(problems.length >= 2, '用了默认值必须记 problem（"我配了"与"它默认了"要分得开）');
  assert.ok(env.notes.length >= 1, 'codecKey 是自由参数这件事必须写在 notes 里');
  assert.throws(
    () => resolveEnvironment({ instanceId: 'a', installRoot: { label: 'x', identity: 'x' }, userRoot: { label: 'y', identity: 'y' }, frameMs: 0 }),
    /frameMs 必须是正整数/,
  );
});

// ─────────────────────────────────────────────────── 4. 配置

test('★ 配置回写：会丢键就**抛**（旧仓为此把整份 INI 抹成两行）', () => {
  const c = new MemoryConfig({ 'set:A': '1', 'set:B': '2' });
  assert.equal(c.get('set:A'), '1');
  assert.deepEqual(c.keys(), ['set:A', 'set:B']);
  assert.throws(() => c.replaceAll([{ key: 'set:A', value: '9' }]), /会丢掉 1 个在场配置键/);
  c.replaceAll([{ key: 'set:A', value: '9' }], { drop: ['set:B'] });
  assert.equal(c.get('set:A'), '9');
  assert.equal(c.get('set:B'), undefined);
  // 不存在的键读出来是 undefined（**没有默认值** —— 有默认值就分不清"没配"与"配了默认值"）
  assert.equal(c.get('set:ZZZ'), undefined);
});

// ─────────────────────────────────────────────────── 5. 副作用日志

test('★ 副作用：序号严格递增 · 归类三态 · 动作名必须在**闭集合**里', () => {
  const log = new EffectLog();
  log.emit({ seq: 0, frame: 0, atMs: 0, domain: 'render', action: 'texture.bind', disposition: 'modeled', detail: {} });
  log.emit({ seq: 1, frame: 0, atMs: 0, domain: 'audio', action: 'movie.play', disposition: 'logged-only', detail: {} });
  log.emit({ seq: 2, frame: 1, atMs: 16, domain: 'input', action: 'poll', disposition: 'not-provided', detail: {} });
  assert.throws(() => log.emit({ seq: 2, frame: 1, atMs: 16, domain: 'input', action: 'poll', disposition: 'modeled', detail: {} }), /严格递增/);
  assert.deepEqual(log.countsByDisposition(), [['logged-only', 1], ['modeled', 1], ['not-provided', 1]]);
  // 三态**不许合并**：条数必须各自可数
  assert.equal(log.countsByDisposition().length, 3);
  for (const r of log.records) assert.ok(isKnownAction(r.domain, r.action), `${r.domain}.${r.action} 应当已在闭集合里`);
  assert.equal(isKnownAction('render', 'nope.nope'), false);
  // 闭集合本身要非空且每个域都有动作（空域 = 写下一条时会漏）
  for (const d of EFFECT_DOMAINS) assert.ok(EFFECT_ACTIONS[d].length > 0, `${d} 域没有动作`);
});

// ─────────────────────────────────────────────────── 6. 场景计时窗

test('★ 计时窗：`set-draw-color` **建窗**、`set-draw-color-alpha` **不建窗**（家族不对称）', () => {
  const s = new SceneModel();
  s.drawTexture(200000, 42, { x: 0, y: 0, w: 1280, h: 720 }, { x: 0, y: 0 });
  // 203：只改工作色 + 那个语义未定的 param，**不**建窗
  s.setDrawColorAlpha(200000, 0, 0x00ffffff);
  assert.equal(s.items.get(200000).window, null, '0x203 不许建窗');
  assert.equal(s.items.get(200000).param, 0);
  assert.equal(s.items.get(200000).workingArgb, 0x00ffffff);
  // 202：建窗（delay/dur/TO）
  s.setDrawColorWindow(200000, 300, 300, 0xffffffff);
  assert.ok(s.items.get(200000).window, '0x202 必须建窗');

  const at = 1000;
  assert.ok(s.hasPendingWindows(at), '未锁存的窗 = pending（"还没开始"不是"已经做完"）');
  s.latchWindows(at);
  assert.ok(s.hasPendingWindows(at + 599), '窗内应当 pending');
  assert.equal(s.hasPendingWindows(at + 600), false, 'delay+dur 走完就不该 pending');
  const done = s.completeWindows(at + 600);
  assert.equal(done.length, 1);
  assert.equal(s.items.get(200000).window, null, '跑完 ⇒ 窗清空');
  assert.equal(s.items.get(200000).workingArgb, 0xffffffff, '跑完 ⇒ 工作色提交成目标色');
});

test('★ `detach` 的区间是**左闭右开** `[handle, handle+count)`，且 `count ≤ 1` 是单键', () => {
  const s = new SceneModel();
  for (const h of [200000, 200001, 200002, 200003, 200004]) s.drawTexture(h, 1, { x: 0, y: 0, w: 1, h: 1 }, { x: 0, y: 0 });
  const removed = s.detach(200000, 4);
  assert.deepEqual(removed, [200000, 200001, 200002, 200003], 'count=4 ⇒ 摘 4 个（200004 留着）');
  assert.equal(s.items.has(200004), true);
  assert.deepEqual(s.detach(200004, 1), [200004], 'count ≤ 1 ⇒ 单键');
  assert.deepEqual(s.detach(999999, 1), []);
});

test('★ 场景快照是**规范化**的（与写入历史无关）：handle / 槽号升序', () => {
  const s = new SceneModel();
  s.drawTexture(9, 1, { x: 0, y: 0, w: 1, h: 1 }, { x: 0, y: 0 });
  s.drawTexture(3, 1, { x: 0, y: 0, w: 1, h: 1 }, { x: 0, y: 0 });
  s.bindTexture(7, 1, null);
  s.bindTexture(2, 1, null);
  const snap = s.snapshot();
  assert.deepEqual(snap.items.map(([h]) => h), [3, 9]);
  assert.deepEqual(snap.slots.map(([n]) => n), [2, 7]);
  assert.deepEqual(SceneModel.restore(snap).snapshot(), snap, '快照往返必须逐值相同');
});

// ─────────────────────────────────────────────────── 7. 操作数 type

test('★ `type` 表：只把"池里那一格就是值"的几个当普通池（3/4/5 + 9..14 的非指针）', () => {
  assert.deepEqual(Object.keys(GLOBAL_POOL_BY_TYPE_TAG).map(Number).sort((a, b) => a - b), [3, 4, 5]);
  assert.deepEqual([...POINTER_OPERAND_TYPES].sort((a, b) => a - b), [0x6, 0x7, 0x8, 0xc, 0xd, 0xe]);
});

test('★ 指针族 type **必须抛**（当普通池读会"类型对、值错"且不报错）', () => {
  const ctx = { locals: new LocalPools(0), globals: new GlobalPools(0), script: {} };
  for (const t of POINTER_OPERAND_TYPES) {
    assert.throws(() => readOperand(ctx, { type: t, rawData: 0 }, 0), /需要\*\*地址空间\*\*才能实现/, `type 0x${t.toString(16)} 读应当抛`);
    assert.throws(() => writeOperand(ctx, { type: t, rawData: 0 }, 0, 1), /需要\*\*地址空间\*\*才能实现/, `type 0x${t.toString(16)} 写应当抛`);
  }
});

test('★ 立即数不是 lvalue；type 1 是**浮点立即数**（raw 就是 float32 位模式）', () => {
  const ctx = { locals: new LocalPools(0), globals: new GlobalPools(0), script: {} };
  assert.throws(() => writeOperand(ctx, { type: 0, rawData: 5 }, 0, 1), /不是 lvalue/);
  assert.throws(() => writeOperand(ctx, { type: 1, rawData: 5 }, 0, 1), /不是 lvalue/);
  // 0x2D5 float-mov (global-float 9) 500 —— 操作数是 int 立即数 500
  assert.equal(readOperand(ctx, { type: 0, rawData: 500 }, 1).value, 500);
  // type 1 的 raw 直接是位模式：1.5f = 0x3FC00000
  assert.equal(readOperand(ctx, { type: 1, rawData: 0x3fc00000 }, 1).value, 1.5);
  assert.equal(floatFromBits(0x3fc00000), 1.5);
});

test('★ 落池按**目标池的类型**收敛：浮点池存 4 字节单精度（取证 `fstp dword ptr`）', () => {
  assert.equal(asFloat32(1 / 3), Math.fround(1 / 3));
  assert.notEqual(asFloat32(1 / 3), 1 / 3, '单精度 ≠ 双精度（这条正是"落池是对是错"的判据）');
  const ctx = { locals: new LocalPools(0), globals: new GlobalPools(0), script: {} };
  writeOperand(ctx, { type: 4, rawData: 9 }, 0, 1 / 3);
  assert.equal(ctx.globals.read('float', 9), Math.fround(1 / 3), 'global-float 落的是 float32');
  // int 池：向零截断（`encInt` 的 `>>> 0`）
  writeOperand(ctx, { type: 9, rawData: 0 }, 0, 7.9);
  assert.equal(ctx.locals.read(9, 0), 7, 'int 池落的是截断后的整数');
});
