/** @env pure @kind gate @why emu-ctl 的客户端判断（argv / 端点文件 / 判活合并 / 选中 / 退出码 / 人读行）坏了 —— 比如 nonce 不校验、多个实例自动挑一个、超时被记成成功 */
/**
 * tools/test/emu-ctl.test.mjs — `tools/lib/emu-ctl.mjs` 的**纯函数**守卫
 *
 * ★ 这个文件**不起端点、不开 socket**（`@env pure`）。传输面（`tools/emu-ctl.mjs` 的 WebSocket /
 *   TCP 探测）不在 pure 档里测；真端点的读数在 M0 的报告里（手工 e2e），不进 `pnpm test`。
 * ★ 钉住的是客户端的**判断**：
 *   ① argv（含 `wait <ms>` 是糖、`click/key` 是 M1 未实现、`--for-ms` 与 `--for frames` 互斥）；
 *   ② `ctl.<pid>.<nonce8>.json` 的解析与校验（坏文件 ⇒ `bad_file`）；
 *   ③ `hello` 与端点文件的同一性（nonce / pid / port / schema）—— 判据 9c 的核心；
 *   ④ `list` 的合并与 `why`；⑤ 选中（零 / 一 / 多 / `--session`，⛔ 不猜）；
 *   ⑥ 退出码映射（0/1/2/3/4）；⑦ 人读格式化（`TITLE.BIN:0x039C paused_at=… frame=…`）。
 */
import assert from 'node:assert/strict';
import test from 'node:test';

import * as L from '../lib/emu-ctl.mjs';

const NONCE = '9f3c0a1d5b2e4f60';
const TOKEN = '8c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f';

function ctlText(over = {}) {
  const doc = {
    schema: 'emu-ctl/2',
    url: `ws://127.0.0.1:54256/?token=${TOKEN}`,
    pid: 12345,
    port: 54256,
    nonce: NONCE,
    token: TOKEN,
    session: 'run-12345-20261116T131500',
    exe_sha256: '1937f0'.repeat(10) + 'b134',
    shape: 'split8-O2',
    started_utc: '2026-11-16T05:15:00Z',
    ...over,
  };
  return JSON.stringify(doc, null, 2);
}

const FILE = `ctl.12345.${NONCE.slice(0, 8)}.json`;

function parsedEntry(over = {}) {
  const r = L.parseCtlFile(FILE, ctlText(over));
  assert.equal(r.ok, true, `测试夹具应当有效：${JSON.stringify(r)}`);
  return r.entry;
}

function helloOf(entry, over = {}) {
  return {
    schema: 'emu-ctl/2',
    pid: entry.pid,
    session: entry.session,
    nonce: entry.nonce,
    port: entry.port,
    started_utc: '2026-11-16T05:15:00Z',
    exe_sha256: entry.exe_sha256,
    shape: entry.shape,
    capabilities: ['pause', 'resume', 'sample', 'screenshot', 'quit'],
    present: 'hidden',
    control: { clock: 'freeze', idle_timeout_s: 300, orphan_timeout_s: 10, predicate_timeout_s: 30 },
    ...over,
  };
}

// =====================================================================================
// argv
// =====================================================================================

test('argv：位置 op 与隐藏 flag 两种入口等价；wait 是 resume --for-ms 的糖', () => {
  assert.equal(L.parseArgv(['list']).op, 'list');
  assert.equal(L.parseArgv(['--list']).op, 'list');
  assert.equal(L.parseArgv(['sample']).op, 'sample');
  assert.equal(L.parseArgv(['--sample']).op, 'sample');
  assert.equal(L.parseArgv(['quit']).op, 'quit');
  assert.equal(L.parseArgv(['gc']).op, 'gc');
  assert.equal(L.parseArgv(['--gc']).op, 'gc');
  assert.equal(L.parseArgv(['gc', '--dry-run']).dryRun, true);
  assert.equal(L.parseArgv(['--gc', '--dry-run']).dryRun, true);
  assert.equal(L.parseArgv(['gc']).dryRun, false);

  const wait = L.parseArgv(['wait', '250']);
  assert.equal(wait.op, 'wait');
  assert.equal(wait.waitMs, 250);

  const resumeMs = L.parseArgv(['resume', '--for-ms', '200']);
  assert.equal(resumeMs.op, 'resume');
  assert.equal(resumeMs.forMs, 200);
  assert.equal(resumeMs.forFrames, null);

  const resumeFrames = L.parseArgv(['resume', '--for', 'frames', '3']);
  assert.equal(resumeFrames.forFrames, 3);
  assert.equal(resumeFrames.forMs, null);

  assert.equal(L.parseArgv(['resume', '--for', 'ms', '12.5']).forMs, 12.5);

  const shot = L.parseArgv(['screenshot', '/tmp/x.bmp']);
  assert.equal(shot.op, 'screenshot');
  assert.equal(shot.screenshotPath, '/tmp/x.bmp');
  assert.equal(L.parseArgv(['--screenshot', '/tmp/y.bmp']).screenshotPath, '/tmp/y.bmp');
});

test('argv：--session / --timeout / --control-root / --json', () => {
  const a = L.parseArgv(['sample', '--session', 'run-1', '--timeout', '2.5', '--control-root', '/x', '--json']);
  assert.equal(a.session, 'run-1');
  assert.equal(a.timeoutSec, 2.5);
  assert.equal(a.root, '/x');
  assert.equal(a.json, true);
  assert.equal(L.parseArgv(['list']).root, L.DEFAULT_CONTROL_ROOT);
  assert.equal(L.parseArgv(['list']).timeoutSec, L.COMMAND_TIMEOUT_SEC);
});

test('argv：click 已解锁（合法坐标/按钮），只有 key 仍是 M1 未实现', () => {
  const left = L.parseArgv(['click', '1070', '480']);
  assert.equal(left.op, 'click');
  assert.deepEqual(left.click, { x: 1070, y: 480, button: 'left' });
  assert.deepEqual(L.parseArgv(['--click', '0', '0', 'right']).click, { x: 0, y: 0, button: 'right' });
  assert.deepEqual(L.parseArgv(['click', '1279', '719', 'left']).click, { x: 1279, y: 719, button: 'left' });

  assert.throws(
    () => L.parseArgv(['key', '65']),
    (err) => err instanceof L.UsageError && err.exitCode === 2 && /M1/.test(err.message) && /未实现/.test(err.message),
    'key 必须回 M1 未实现',
  );
});

test('argv：click 的坐标/按钮在本地校验（缺参 / 越界 / 非整数 / 非法按钮 ⇒ 退出码 2）', () => {
  const bad = [
    () => L.parseArgv(['click']),
    () => L.parseArgv(['click', '10']),
    () => L.parseArgv(['click', '1280', '0']),
    () => L.parseArgv(['click', '0', '720']),
    () => L.parseArgv(['click', '-1', '0']),
    () => L.parseArgv(['click', '1.5', '0']),
    () => L.parseArgv(['click', '1e3', '0']),
    () => L.parseArgv(['click', '10', '20', 'middle']),
    () => L.parseArgv(['click', '10', '20', 'left', 'extra']),
  ];
  for (const fn of bad) {
    assert.throws(fn, (err) => err instanceof L.UsageError && err.exitCode === 2);
  }
  // 边界是闭区间：1279 / 719 合法
  assert.deepEqual(L.parseArgv(['click', '1279', '719']).click, { x: 1279, y: 719, button: 'left' });
});

test('clickFrame：请求帧是顶层 x/y/button（⛔ 不包 args）', () => {
  assert.deepEqual(L.clickFrame(1070, 480), { op: 'click', x: 1070, y: 480, button: 'left' });
  assert.deepEqual(L.clickFrame(0, 719, 'right'), { op: 'click', x: 0, y: 719, button: 'right' });
  assert.equal(Object.hasOwn(L.clickFrame(1, 2), 'args'), false);
  assert.equal(L.REFERENCE_W, 1280);
  assert.equal(L.REFERENCE_H, 720);
});

// =====================================================================================
// typed global（父级 2026-11-19 追加裁决：index 与 int/uint value **只收 0x 十六进制**）
// =====================================================================================

test('argv：global-* 的 --index / int-uint --value 只收 0x 十六进制；纯十进制 ⇒ 退出码 2', () => {
  const g = L.parseArgv(['global-get', '--kind', 'int', '--index', '0xb1b6']);
  assert.equal(g.op, 'global-get');
  assert.equal(g.globalKind, 'int');
  assert.equal(g.globalIndex, '0xb1b6');
  assert.equal(g.globalValue, null);
  // 隐藏 flag 形式 + 大小写 / 前导零归一化
  assert.equal(L.parseArgv(['--global-get', '--kind', 'uint', '--index', '0x0000B1B6']).globalIndex, '0xb1b6');

  // int / uint：value 是 0x 字符串
  assert.equal(
    L.parseArgv(['global-set', '--kind', 'int', '--index', '0xb1b6', '--value', '0x80000010']).globalValue,
    '0x80000010',
  );
  // float：value 是十进制 number（**唯一例外**）；-0.0 的符号要留住
  assert.equal(L.parseArgv(['global-set', '--kind', 'float', '--index', '0x1', '--value', '1.5']).globalValue, 1.5);
  assert.equal(
    Object.is(L.parseArgv(['global-set', '--kind', 'float', '--index', '0x1', '--value', '-0.0']).globalValue, -0),
    true,
  );
  // string：本地放行（由服务端回"未实现"的 type_mismatch），不做第二份裁决
  assert.equal(L.parseArgv(['global-get', '--kind', 'string', '--index', '0xb1b6']).globalKind, 'string');

  // ★ 错误信息要**教人怎么写**（0x 前缀 + 例值）
  assert.throws(
    () => L.parseArgv(['global-get', '--kind', 'int', '--index', '45494']),
    /0x 前缀.*0xb1b6/,
    '纯十进制 index 必须点名"要 0x 前缀"',
  );

  const bad = [
    ['global-get', '--kind', 'int', '--index', '45494'], // ★ 纯十进制（追加裁决）
    ['global-get', '--kind', 'int', '--index', 'b1b6'], // 缺 0x
    ['global-set', '--kind', 'int', '--index', '0x1', '--value', '16'], // ★ 纯十进制 value
    ['global-set', '--kind', 'uint', '--index', '0x1', '--value', '-1'],
    ['global-set', '--kind', 'float', '--index', '0x1', '--value', '0x10'], // float 不收 hex
    ['global-set', '--kind', 'float', '--index', '0x1', '--value', '1e40'], // 超 float32
    ['global-get', '--kind', 'int'], // 缺 index
    ['global-get', '--kind', 'int', '--index', '0x1', '--value', '0x1'], // get 不收 value
    ['global-set', '--kind', 'int', '--index', '0x1'], // 缺 value
    ['global-get', '--kind', 'int32', '--index', '0x1'], // 未知 kind
    ['global-get', '--index', '0x1'], // 缺 kind
    ['global-get', '--kind', 'int', '--index', '0x100000000'], // > 32 位
    ['pause', '--index', '0x1'], // global 参数不能给别的 op
  ];
  for (const argv of bad) {
    assert.throws(
      () => L.parseArgv(argv),
      (err) => err instanceof L.UsageError && err.exitCode === 2,
      `应当拒：${argv.join(' ')}`,
    );
  }
});

test('globalFrame：顶层 {op,kind,index,value}；index / int-uint value 是字符串 0x，float 是 number', () => {
  assert.deepEqual(L.globalFrame('global-get', 'int', '0xb1b6'), {
    op: 'global-get',
    kind: 'int',
    index: '0xb1b6',
  });
  assert.deepEqual(L.globalFrame('global-set', 'int', '0xb1b6', '0x80000010'), {
    op: 'global-set',
    kind: 'int',
    index: '0xb1b6',
    value: '0x80000010',
  });
  assert.deepEqual(L.globalFrame('global-set', 'float', '0x1', 1.5), {
    op: 'global-set',
    kind: 'float',
    index: '0x1',
    value: 1.5,
  });
  assert.equal(Object.hasOwn(L.globalFrame('global-get', 'int', '0x1'), 'value'), false);
  assert.equal(Object.hasOwn(L.globalFrame('global-get', 'int', '0x1'), 'args'), false);
});

test('formatGlobalLine：kind/index/value/raw 按服务端 0x 位模式打印；float 的 null 不冒充 0', () => {
  assert.equal(
    L.formatGlobalLine('global-get', {
      ok: true,
      data: { global: { kind: 'int', index: '0x0000B1B6', value: '0x80000010', raw: '0x79A66695' } },
    }),
    'global-get ok kind=int index=0x0000B1B6 value=0x80000010 raw=0x79A66695',
  );
  assert.equal(
    L.formatGlobalLine('global-get', {
      ok: true,
      data: { global: { kind: 'float', index: '0x00000002', value: null, raw: '0x7FC00001' } },
    }),
    'global-get ok kind=float index=0x00000002 value=null raw=0x7FC00001',
  );
});

// =====================================================================================
// move（纯 move：只把光标放到参考坐标，⛔ 不按键）
// =====================================================================================

test('moveFrame：请求帧是顶层 x/y（⛔ 不带 button、⛔ 不包 args）', () => {
  assert.deepEqual(L.moveFrame(740, 355), { op: 'move', x: 740, y: 355 });
  assert.equal(Object.hasOwn(L.moveFrame(1, 2), 'button'), false);
  assert.equal(Object.hasOwn(L.moveFrame(1, 2), 'args'), false);
});

test('argv：move 已解锁（合法坐标），且与 click 共用同一条坐标边界', () => {
  const m = L.parseArgv(['move', '740', '355']);
  assert.equal(m.op, 'move');
  assert.deepEqual(m.move, { x: 740, y: 355 });
  assert.deepEqual(L.parseArgv(['--move', '1279', '719']).move, { x: 1279, y: 719 });
  assert.deepEqual(L.parseArgv(['move', '0', '0']).move, { x: 0, y: 0 });
  for (const fn of [
    () => L.parseArgv(['move']),
    () => L.parseArgv(['move', '10']),
    () => L.parseArgv(['move', '1280', '0']),
    () => L.parseArgv(['move', '0', '720']),
    () => L.parseArgv(['move', '-1', '0']),
    () => L.parseArgv(['move', '1.5', '0']),
    () => L.parseArgv(['move', '10', '20', 'left']), // move 没有按钮参数
  ]) {
    assert.throws(fn, (err) => err instanceof L.UsageError && err.exitCode === 2);
  }
});
test('argv：非法组合与非法数值都在客户端挡住（退出码 2）', () => {
  const bad = [
    () => L.parseArgv([]),
    () => L.parseArgv(['nope']),
    () => L.parseArgv(['--nope']),
    () => L.parseArgv(['resume', '--for-ms', '200', '--for', 'frames', '1']),
    () => L.parseArgv(['pause', '--for-ms', '200']),
    () => L.parseArgv(['wait', '0']),
    () => L.parseArgv(['wait', '-5']),
    () => L.parseArgv(['wait', 'abc']),
    () => L.parseArgv(['resume', '--for', 'frames', '0']),
    () => L.parseArgv(['resume', '--for', 'frames', '1.5']),
    () => L.parseArgv(['resume', '--for', 'seconds', '1']),
    () => L.parseArgv(['screenshot']),
    () => L.parseArgv(['sample', '--timeout', '0']),
    () => L.parseArgv(['sample', 'extra']),
    () => L.parseArgv(['list', '--dry-run']),
    () => L.parseArgv(['click', '1280', '0']),
    () => L.parseArgv(['click', '10', '20', 'middle']),
  ];
  for (const fn of bad) {
    assert.throws(fn, (err) => err instanceof L.UsageError && err.exitCode === 2);
  }
});

// =====================================================================================
// 端点文件
// =====================================================================================

test('parseCtlFile：合法文件 ⇒ 连接要的字段齐全（url / token / session）', () => {
  const e = parsedEntry();
  assert.equal(e.pid, 12345);
  assert.equal(e.port, 54256);
  assert.equal(e.nonce, NONCE);
  assert.equal(e.token, TOKEN);
  assert.equal(e.session, 'run-12345-20261116T131500');
  assert.equal(e.url, `ws://127.0.0.1:54256/?token=${TOKEN}`);
  assert.equal(e.shape, 'split8-O2');
});

test('parseCtlFile：坏文件一律 bad_file（JSON / schema / pid / port / nonce / token / 文件名不一致）', () => {
  const cases = [
    ['ctl.12345.9f3c0a1d.json', '{ not json'],
    [FILE, ctlText({ schema: 'emu-ctl/1' })],
    [FILE, ctlText({ pid: 0 })],
    [FILE, ctlText({ port: 0 })],
    [FILE, ctlText({ port: 70000 })],
    [FILE, ctlText({ nonce: '' })],
    [FILE, ctlText({ nonce: 'zzzzzzzzzzzzzzzz' })],
    [FILE, ctlText({ token: '' })],
    ['ctl.99999.9f3c0a1d.json', ctlText()], // 文件名 pid 与内容 pid 不一致
    ['ctl.12345.deadbeef.json', ctlText()], // 文件名 nonce8 与内容不一致
    ['not-a-ctl.json', ctlText()],
  ];
  for (const [name, text] of cases) {
    const r = L.parseCtlFile(name, text);
    assert.equal(r.ok, false, `${name} / ${text.slice(0, 40)} 应当被判坏`);
    assert.equal(r.why, 'bad_file');
    assert.ok(r.detail.length > 0);
  }
});

test('listCtlFiles：只收 ctl.<pid>.<nonce8>.json，.tmp 与别的文件不碰', () => {
  const names = [
    `ctl.12345.${NONCE.slice(0, 8)}.json`,
    `ctl.12345.${NONCE.slice(0, 8)}.json.tmp`,
    'keep.txt',
    'ctl.json',
    `ctl.2.${NONCE.slice(0, 8)}.json`,
    `ctl.12345.${NONCE.slice(0, 8)}.JSON`,
  ];
  assert.deepEqual(L.listCtlFiles(names), [
    `ctl.12345.${NONCE.slice(0, 8)}.json`,
    `ctl.2.${NONCE.slice(0, 8)}.json`,
  ]);
});

// =====================================================================================
// hello 同一性（判据 9c 的核心）
// =====================================================================================

test('helloMismatch：同一份身份 ⇒ null；nonce / pid / port / schema 任一不符 ⇒ 点名', () => {
  const e = parsedEntry();
  assert.equal(L.helloMismatch(helloOf(e), e), null);
  assert.match(L.helloMismatch(helloOf(e, { nonce: 'deadbeefdeadbeef' }), e), /nonce/);
  assert.match(L.helloMismatch(helloOf(e, { pid: 999 }), e), /pid/);
  assert.match(L.helloMismatch(helloOf(e, { port: 1 }), e), /port/);
  assert.match(L.helloMismatch(helloOf(e, { schema: 'emu-ctl/1' }), e), /schema/);
  assert.match(L.helloMismatch(null, e), /对象/);
});

// =====================================================================================
// list 的合并 / why
// =====================================================================================

test('listRow：alive 用 hello 的 present/session；dead 的 present 必须是 null', () => {
  const e = parsedEntry();
  const alive = L.listRow(e, { alive: true, hello: helloOf(e, { present: 'visible', session: 'hello-session' }) });
  assert.deepEqual(alive, {
    session: 'hello-session',
    pid: 12345,
    port: 54256,
    alive: true,
    why: 'ok',
    present: 'visible',
  });

  for (const why of ['refused', 'timeout', 'nonce_mismatch', 'bad_file']) {
    const dead = L.listRow(e, { alive: false, why });
    assert.equal(dead.alive, false);
    assert.equal(dead.why, why);
    assert.equal(dead.present, null);
    assert.equal(dead.session, e.session); // 文件里的 session 仍可读
  }
  assert.deepEqual(L.WHY, ['ok', 'refused', 'timeout', 'nonce_mismatch', 'bad_file']);
});

// =====================================================================================
// 选中（⛔ 不猜）
// =====================================================================================

function row(over = {}) {
  return { session: 's1', pid: 1, port: 1001, alive: true, why: 'ok', present: 'hidden', ...over };
}

test('selectEntry：恰好一个 alive 才自动选中；零 / 多 ⇒ 退出码 4', () => {
  assert.equal(L.selectEntry([row()]).ok, true);
  assert.equal(L.selectEntry([row(), row({ pid: 2, port: 1002, alive: false, why: 'refused', present: null })]).ok, true);

  const none = L.selectEntry([row({ alive: false, why: 'refused', present: null })]);
  assert.equal(none.ok, false);
  assert.equal(none.code, L.EXIT.instance);
  assert.equal(none.reason, 'none');

  const two = L.selectEntry([row(), row({ session: 's2', pid: 2, port: 1002 })]);
  assert.equal(two.ok, false);
  assert.equal(two.code, 4);
  assert.equal(two.reason, 'multiple');
  assert.equal(two.entries.length, 2, '多个 alive 必须把全部实例交回来');
  assert.equal(L.selectEntry([]).reason, 'none');
});

test('selectEntry：--session 精确选中（多个 alive 时唯一合法路径），匹配不到 ⇒ 4', () => {
  const rows = [row(), row({ session: 's2', pid: 2, port: 1002 })];
  const hit = L.selectEntry(rows, 's2');
  assert.equal(hit.ok, true);
  assert.equal(hit.entry.pid, 2);

  const miss = L.selectEntry(rows, 'nope');
  assert.equal(miss.ok, false);
  assert.equal(miss.code, 4);
  assert.equal(miss.reason, 'not_found');

  // 死实例不能靠 --session 选中（存活是事实）
  const dead = L.selectEntry([row({ alive: false, why: 'refused', present: null })], 's1');
  assert.equal(dead.reason, 'not_found');
});

// =====================================================================================
// gc 判定表（只删残留端点文件；⛔ 不碰 keep.txt / 目录 / 别的名字）
// =====================================================================================

test('gcDecision：删除必须同时满足 connect 判死 + pid 不存活 + 文件名严格匹配', () => {
  const dead = { isFile: true, alive: false, why: 'refused', pid: 123, port: 456, pidAlive: false };
  const cases = [
    // ★ 判据 9b 的 keep.txt：哪怕 connect 失败 + pid 死了，也绝不删（文件名不匹配）
    [{ name: 'keep.txt', isFile: true, alive: false, why: 'refused', pid: 123, port: 456, pidAlive: false }, 'keep', 'name_mismatch'],
    [{ name: 'ctl.123.abcdef12.json', ...dead, alive: true, why: 'ok' }, 'keep', 'alive'],
    [{ name: 'ctl.123.abcdef12.json', ...dead, pidAlive: true }, 'keep', 'pid_alive'],
    [{ name: 'ctl.123.abcdef12.json', ...dead, why: 'nonce_mismatch' }, 'keep', 'nonce_mismatch'],
    [{ name: 'ctl.123.abcdef12.json', ...dead, why: 'bad_file' }, 'keep', 'bad_file'],
    [{ name: 'ctl.123.abcdef12.json', ...dead }, 'remove', 'dead'],
    [{ name: 'ctl.123.abcdef12.json', ...dead, why: 'timeout' }, 'remove', 'dead'],
    // 文件名不合法的四类：非 ctl / 非 8hex / 目录 / .tmp
    [{ name: 'ctl.123.zzzzzzzz.json', ...dead }, 'keep', 'name_mismatch'],
    [{ name: 'ctl.123.abcdef12.json', ...dead, isFile: false }, 'keep', 'not_regular_file'],
    [{ name: 'ctl.123.abcdef12.json.tmp', ...dead }, 'keep', 'name_mismatch'],
    [{ name: 'not-a-ctl.json', ...dead }, 'keep', 'name_mismatch'],
    // 没有 alive/why 的目录项（非 ctl）也只能 keep
    [{ name: 'keep.txt', isFile: true }, 'keep', 'name_mismatch'],
  ];
  for (const [item, action, why] of cases) {
    const d = L.gcDecision(item);
    assert.equal(d.action, action, `${item.name} 的 action 应为 ${action}（实际 ${d.action}）`);
    assert.equal(d.why, why, `${item.name} 的 why 应为 ${why}（实际 ${d.why}）`);
  }
});

test('gcPlan / gcJson / formatGcText：removed/kept/why 是契约字段，keep.txt 一定在 kept', () => {
  const items = [
    { name: 'ctl.1.aaaaaaaa.json', isFile: true, pid: 1, port: 10, alive: false, why: 'refused', pidAlive: false },
    { name: 'keep.txt', isFile: true },
    { name: 'ctl.2.bbbbbbbb.json', isFile: true, pid: 2, port: 20, alive: true, why: 'ok' },
    { name: 'ctl.3.cccccccc.json', isFile: true, pid: 3, port: 30, alive: false, why: 'refused', pidAlive: true },
  ];
  const plan = L.gcPlan(items);
  assert.deepEqual(plan.removed.map((r) => r.name), ['ctl.1.aaaaaaaa.json']);
  assert.deepEqual(plan.kept.map((r) => r.name), ['keep.txt', 'ctl.2.bbbbbbbb.json', 'ctl.3.cccccccc.json']);
  assert.deepEqual(plan.why, {
    'ctl.1.aaaaaaaa.json': 'dead',
    'keep.txt': 'name_mismatch',
    'ctl.2.bbbbbbbb.json': 'alive',
    'ctl.3.cccccccc.json': 'pid_alive',
  });
  assert.equal(plan.removed[0].action, 'remove');
  assert.equal(plan.removed[0].pid, 1);
  assert.equal(plan.removed[0].port, 10);

  const j = L.gcJson('/root', plan, true);
  assert.equal(j.op, 'gc');
  assert.equal(j.root, '/root');
  assert.equal(j.dry_run, true);
  assert.equal(j.ok, true);
  assert.deepEqual(j.removed, plan.removed);
  assert.deepEqual(j.kept, plan.kept);
  assert.deepEqual(j.why, plan.why);

  const text = L.formatGcText('/root', plan, true);
  assert.equal(text[0], 'root /root');
  assert.ok(text.includes('remove ctl.1.aaaaaaaa.json pid=1 port=10 why=dead'));
  assert.ok(text.includes('keep   keep.txt why=name_mismatch'));
  assert.ok(text.includes('keep   ctl.2.bbbbbbbb.json pid=2 port=20 why=alive'));
  assert.ok(text.includes('(--dry-run：只报告，未删任何文件)'));
  assert.equal(text.at(-1), 'removed 1 · kept 3');
});

// =====================================================================================
// 退出码
// =====================================================================================

test('退出码：ok ⇒ 0（含 script.available:false）；timeout ⇒ 3；其余 ok:false ⇒ 1', () => {
  assert.equal(L.exitCodeForReply({ ok: true, data: { sample: { script: { available: false } } } }), 0);
  assert.equal(L.exitCodeForReply({ ok: false, error: { code: 'bad_request' } }), 1);
  assert.equal(L.exitCodeForReply({ ok: false, error: { code: 'not_paused' } }), 1);
  assert.equal(L.exitCodeForReply({ ok: false, error: { code: 'timeout' } }), 3);
  // click 的服务端错误：not_paused / bad_request（队列满）都是端点回 ok:false ⇒ 1
  assert.equal(L.exitCodeForReply({ ok: false, error: { code: 'not_paused' } }), 1);
  assert.equal(L.exitCodeForReply({ ok: false, error: { code: 'bad_request' } }), 1);
  assert.equal(L.exitCodeForReply(null), 1);
  assert.equal(L.exitCodeForReply(undefined), 1);

  assert.equal(L.exitCodeForError('usage'), 2);
  assert.equal(L.exitCodeForError('protocol'), 2);
  assert.equal(L.exitCodeForError('timeout'), 3);
  assert.equal(L.exitCodeForError('instance'), 4);
  assert.equal(L.exitCodeForError('endpoint'), 1);
  assert.equal(L.errorKindOf({ ok: false, error: { code: 'timeout' } }), 'timeout');
  assert.equal(L.errorKindOf({ ok: false, error: { code: 'io' } }), 'endpoint');
});

// =====================================================================================
// 人读 / JSON
// =====================================================================================

test('formatSampleLine：TITLE.BIN:0x039C paused_at=… frame=…（不可用时也不装作有身份）', () => {
  const sample = {
    paused: true,
    paused_at: 'block',
    state_seq: 1,
    screen: { frame: 129, last_hash: '0x3AF490E99A387689' },
    script: { available: true, script: 'TITLE.BIN', pos_hex: '0x0000039C' },
  };
  assert.equal(L.formatSampleLine(sample), 'TITLE.BIN:0x0000039C paused_at=block frame=129');

  const unavailable = {
    paused_at: 'block',
    screen: { frame: 0 },
    script: { available: false, why: 'identity-invalid' },
  };
  assert.equal(L.formatSampleLine(unavailable), 'script:unavailable(identity-invalid) paused_at=block frame=0');
  assert.equal(L.formatSampleLine(null), 'script:unavailable(no-sample) paused_at=- frame=-');
});

test('formatScreenshotLine / formatStateLine / formatQuitLine：读数里要有判据字段', () => {
  assert.equal(
    L.formatScreenshotLine({
      path: '/tmp/x.bmp',
      bytes: 3686454,
      frame: 129,
      hash_still_matches: true,
      source: 'guest-backbuffer',
    }),
    '/tmp/x.bmp bytes=3686454 frame=129 hash_still_matches=true source=guest-backbuffer',
  );
  assert.equal(L.formatScreenshotLine(undefined), 'screenshot:missing');

  assert.equal(
    L.formatStateLine('pause', { ok: true, data: { paused: true, paused_at: 'block', state_seq: 2, guest_ms: 42 } }),
    'pause ok paused=true paused_at=block state_seq=2 guest_ms=42',
  );
  assert.equal(
    L.formatQuitLine({ ok: true, data: { state_seq: 3 } }, { event: 'bye', reason: 'stopped', requested_by: 'client' }),
    'quit ok reason=stopped requested_by=client state_seq=3',
  );
  assert.equal(
    L.formatQuitLine({ ok: true, data: { state_seq: 3 } }, null),
    'quit ok bye=connection-closed state_seq=3',
  );
  assert.equal(
    L.formatClickLine({ ok: true, data: { state_seq: 4, click: { x: 1070, y: 480, button: 'left', queued: 1 } } }),
    'click ok x=1070 y=480 button=left queued=1 state_seq=4',
  );
});

test('list 的人读行：alive / dead 都带 why 与 present；死条目不改退出码（格式层）', () => {
  const alive = row({ session: 'run-a', pid: 1, port: 1001 });
  const dead = row({ session: 'run-b', pid: 2, port: 1002, alive: false, why: 'refused', present: null });
  assert.equal(L.formatListLine(alive), 'alive pid=1 port=1001 why=ok present=hidden session=run-a');
  assert.equal(L.formatListLine(dead), 'dead  pid=2 port=1002 why=refused present=- session=run-b');
  const text = L.formatListText('/root', [alive, dead]);
  assert.equal(text[0], 'root /root');
  assert.match(text.at(-1), /共 2 条 · alive 1 · dead 1/);
  assert.equal(L.formatListText('/root', [])[0], 'root /root');
});

test('resultJson / listJson：--json 是**一个**对象（list 条目就是 listRow 的形状）', () => {
  const reply = { ok: true, data: { paused: true, paused_at: 'block', state_seq: 1 } };
  const j = L.resultJson({ op: 'pause', entry: parsedEntry(), reply, bye: null });
  assert.equal(j.op, 'pause');
  assert.equal(j.ok, true);
  assert.equal(j.pid, 12345);
  assert.equal(j.port, 54256);
  assert.deepEqual(j.data, reply.data);
  assert.equal(Object.hasOwn(j, 'bye'), false);

  const q = L.resultJson({ op: 'quit', entry: parsedEntry(), reply, bye: { event: 'bye', reason: 'stopped', requested_by: 'client' } });
  assert.equal(q.bye.requested_by, 'client');

  const e = L.resultJson({ op: 'pause', entry: parsedEntry(), reply: { ok: false, error: { code: 'not_paused' } } });
  assert.equal(e.ok, false);
  assert.equal(e.error.code, 'not_paused');

  const l = L.listJson('/root', [row(), row({ alive: false, why: 'timeout', present: null })]);
  assert.equal(l.op, 'list');
  assert.equal(l.count, 2);
  assert.equal(l.alive, 1);
  assert.equal(l.entries[1].why, 'timeout');
});

// =====================================================================================
// 自描述（mutates 逐条标；M1 不在实现里）
// =====================================================================================

test('OPERATIONS 的 mutates 逐条正确；sample/click/move/global-set/gc 会写；key 不在实现里', () => {
  const byName = Object.fromEntries(L.OPERATIONS.map((o) => [o.name, o]));
  assert.deepEqual(Object.keys(byName).sort(), [
    'click',
    'gc',
    'global-get',
    'global-set',
    'list',
    'move',
    'pause',
    'quit',
    'resume',
    'sample',
    'screenshot',
    'wait',
  ]);
  assert.equal(byName.list.mutates, false);
  assert.equal(byName.click.mutates, true);
  assert.match(byName.click.summary, /入队 ≠ 施加/);
  assert.match(byName.click.summary, /1280×720/);
  assert.match(byName.click.summary, /64 条/);
  // ★ move = 纯 move（⛔ 不按键）：同一条立即队列、同样只接受暂停态
  assert.equal(byName.move.mutates, true);
  assert.match(byName.move.summary, /纯 move/);
  assert.match(byName.move.summary, /不按键/);
  assert.match(byName.move.summary, /1280×720/);
  // ★ typed global：get 只读；set 写一个 guest 数据槽；两者都必须点名"必须 0x 十六进制"
  assert.equal(byName['global-get'].mutates, false);
  assert.equal(byName['global-set'].mutates, true);
  assert.match(byName['global-get'].summary, /0x 十六进制/);
  assert.match(byName['global-set'].summary, /0x 十六进制/);
  assert.match(byName['global-set'].summary, /十进制浮点/);
  // ★ sample 会 freeze + ++state_seq（停在汇点才能采样）⇒ 契约上是"会写"
  assert.equal(byName.sample.mutates, true);
  assert.equal(byName.sample.summary.includes('停在汇点才能采样'), true);
  assert.equal(byName.pause.mutates, true);
  assert.equal(byName.resume.mutates, true);
  assert.equal(byName.wait.mutates, true);
  assert.equal(byName.quit.mutates, true);
  assert.equal(byName.screenshot.mutates, true);
  assert.equal(byName.gc.mutates, true);
  assert.ok(/keep\.txt/.test(byName.gc.summary), 'gc 的说明必须点名"不碰 keep.txt"');
  assert.equal(byName.key, undefined, 'key 仍是 M1，不该被注册成实现');
  for (const o of L.OPERATIONS) assert.match(o.argv[0], /^--[a-z-]+$/);
});

test('describeText：写明同名陷阱（控制层 wait ≠ --input-plan 的 wait）与 M1 未实现', () => {
  const text = L.describeText();
  assert.match(text, /DOMAIN/);
  assert.match(text, /OPERATIONS/);
  assert.match(text, /让 guest 跑 N ms 后\*\*重停\*\*/);
  assert.match(text, /--input-plan/);
  assert.match(text, /不是一回事/);
  assert.match(text, /click（入队 ≠ 施加；参考坐标 1280×720）/);
  assert.match(text, /move（纯 move，⛔ 不按键；参考坐标 1280×720）/);
  assert.match(text, /未实现：key/);
  assert.match(text, /gc（只删残留端点文件，⛔ 不动其它文件）/);
  assert.match(text, /keep\.txt/);
  assert.match(text, /已结算/);
  assert.match(text, /含当前冻结区间/);
  assert.match(text, /\(会写\)/);
  assert.match(text, /\(只读\)/);
  // ★ typed global 的口径必须自描述（哑原语 / 只收 0x / float 例外 / 三类 reason）
  assert.match(text, /global-get \/ global-set（哑原语/);
  assert.match(text, /纯十进制一律拒/);
  assert.match(text, /唯一例外/);
  assert.match(text, /id_out_of_range, type_mismatch, encode_failed/);
});
