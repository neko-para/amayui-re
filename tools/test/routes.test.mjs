/** @env pure @kind gate @why 界面路径表坏了、或入库的播放器输入脚本没跟着数据重生 */
/**
 * tools/test/routes.test.mjs — 界面路径登记表（`data/routes/screens.json`）的守卫
 *
 * 五条**独立**的红线（守的是"表还能不能被信"与"数据 / 派生件有没有走散"）：
 *   ① schema 形态 + `_doc` 指针 + id 唯一（kebab-case）
 *   ② `from` 可解析（boot 或表内 id）且不成环
 *   ③ `steps` 每行能解析、时间戳非递减
 *   ④ `emit` 的输出与入库的 `apps/emulator-cxx/tests/input/route-*.txt` **逐字节相同**
 *      （+ 目录里没有孤儿 `route-*.txt`）—— 防"数据改了、脚本没重生"
 *   ⑤ `arrive.script` ↔ `arrive.bin` 在表内自洽（同一个 script 不许对应两个 bin，反向亦然）
 *
 * ★ 只读仓库内文本：不联网、不读游戏安装目录、不跑播放器（`@env pure`）。
 * ★ 不在这里断言数据"现在有哪些 route / 哪个坐标"（那是 `pnpm tools routes validate` 的事）；
 *   这里只守**结构与派生关系**。
 *
 * 运行：`node --test tools/test/routes.test.mjs`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import {
  DEFAULT_INPUT_DIR,
  DOC_POINTER,
  SCHEMA,
  emitScript,
  inputFileName,
  loadRoutes,
  parseStep,
  scriptBinProblems,
  validateRoutes,
} from '../routes.mjs';

const data = loadRoutes();
const routes = Array.isArray(data.routes) ? data.routes : [];
const byId = new Map(routes.filter((r) => r && typeof r.id === 'string').map((r) => [r.id, r]));
const rel = (p) => path.relative(REPO_ROOT, p).split(path.sep).join('/');

test('① schema 形态：schema / _doc / routes[] 齐全，id 是 kebab-case 且唯一', () => {
  assert.equal(data.schema, SCHEMA, 'schema 版本不对（改 schema 要同步工具与守卫）');
  assert.equal(data._doc, DOC_POINTER, '_doc 必须逐字等于工具给出的自描述指针');
  assert.ok(Array.isArray(data.routes), 'routes 必须是数组');
  assert.ok(routes.length > 0, 'routes 不能为空');

  const seen = new Set();
  for (const route of routes) {
    assert.equal(typeof route.id, 'string', '每条 route 都要有字符串 id');
    assert.match(route.id, /^[a-z0-9]+(?:-[a-z0-9]+)*$/, `id 必须是 kebab-case：${route.id}`);
    assert.ok(!seen.has(route.id), `id 重复：${route.id}`);
    seen.add(route.id);
    assert.ok(typeof route.title === 'string' && route.title.trim() !== '', `route ${route.id} 缺 title`);
    assert.ok(Array.isArray(route.steps), `route ${route.id} 的 steps 必须是数组`);
    assert.ok(route.arrive && typeof route.arrive === 'object', `route ${route.id} 缺 arrive`);
    assert.ok(route.evidence && typeof route.evidence.command === 'string', `route ${route.id} 缺 evidence.command`);
  }
});

test('② from 可解析：boot 或表内 id，且沿 from 上溯不会成环', () => {
  for (const route of routes) {
    assert.equal(typeof route.from, 'string', `route ${route.id} 的 from 必须是字符串`);
    if (route.from === 'boot') continue;
    assert.ok(byId.has(route.from), `route ${route.id} 的 from=${JSON.stringify(route.from)} 不在表内`);

    const chain = [route.id];
    let cur = route;
    while (cur.from !== 'boot') {
      assert.ok(!chain.includes(cur.from), `from 链成环：${[...chain, cur.from].join(' → ')}`);
      chain.push(cur.from);
      cur = byId.get(cur.from);
      assert.ok(cur, `from 链断在 ${JSON.stringify(cur)}`);
    }
  }
});

test('③ steps 可解析且时间戳非递减', () => {
  for (const route of routes) {
    let prev = -1;
    route.steps.forEach((line, index) => {
      const step = parseStep(line); // 语法错了这里就抛
      assert.ok(step.at >= prev, `route ${route.id} 的 steps[${index}] 时间戳递减：${prev} → ${step.at}`);
      prev = step.at;
    });
  }
});

test('④ emit 与入库的 route-*.txt 逐字节相同（防"数据改了、脚本没重生"）', () => {
  for (const route of routes) {
    const file = path.join(DEFAULT_INPUT_DIR, inputFileName(route.id));
    assert.ok(fs.existsSync(file), `缺派生脚本 ${route.id}：先跑 \`pnpm tools routes emit --id ${route.id} --out ${rel(file)}\``);
    const onDisk = fs.readFileSync(file);
    const expected = Buffer.from(emitScript(route), 'utf8');
    assert.ok(
      onDisk.equals(expected),
      `route-${route.id}.txt 与 emit 的输出不一致（数据改了但脚本没重生）：` +
        `\`pnpm tools routes emit --id ${route.id} --out ${rel(file)}\``,
    );
  }
  // 孤儿：目录里每个 route-*.txt 都必须有对应数据（否则它是上一版数据的幽灵）
  const expectedNames = new Set(routes.map((r) => inputFileName(r.id)));
  const strays = fs
    .readdirSync(DEFAULT_INPUT_DIR)
    .filter((name) => /^route-.*\.txt$/.test(name) && !expectedNames.has(name));
  assert.deepEqual(strays, [], `这些 route-*.txt 在表里没有对应 route：${strays.join(' / ')}`);
});

test('⑤ arrive.script ↔ arrive.bin 在表内自洽（同一 script 不许对应两个 bin）', () => {
  assert.deepEqual(scriptBinProblems(routes), [], '同一个 script / bin 在表内对应了多个值');
  for (const route of routes) {
    assert.ok(Number.isInteger(route.arrive.script) && route.arrive.script > 0, `route ${route.id} 的 arrive.script 必须是正整数`);
    assert.match(route.arrive.bin, /^[A-Z0-9_]+\.BIN$/, `route ${route.id} 的 arrive.bin 必须是 NAME.BIN 形态`);
  }
});

test('validateRoutes：真源通过全部不变量', () => {
  assert.deepEqual(validateRoutes(data), [], '`pnpm tools routes validate` 有红项（这里汇总看一眼）');
});

test('⑥ evidence.command 写机器绝对路径 ⇒ 必须红（换机器照抄跑不了）', () => {
  const bad = JSON.parse(JSON.stringify(data));
  bad.routes[0].evidence.command = 'emulator --install "E:\\Games\\Eushully\\天結いキャッスルマイスター"';
  const problems = validateRoutes(bad);
  assert.ok(
    problems.some((p) => typeof p?.message === 'string' && p.message.includes('机器绝对路径')),
    `机器绝对路径没有被 evidence-command-portable 抓住：${problems.join(' / ')}`,
  );
});
