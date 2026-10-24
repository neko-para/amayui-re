/** @env assets @kind gate @why 第七批新增规则的绑定站点若有一处不对着 .lst 指令行首，规则正文的机器锚就失效了 */
/**
 * tools/test/spec-sites-batch7.assets.test.mjs
 *
 * 第七批新增规则（按住派发按键族）的站点。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const SKIP = fs.existsSync(LST) ? false : '语料 .lst 不在场';

const CORE_INPUT_102_SLOT = [
  0x421d00,
  0x421d18,
  0x421d2e,
  0x421d40,
  0x421d4d,
  0x421d5b,
  0x421d75,
  0x421d7c,
  0x421d7e,
  0x421d91,
  0x421d9e,
  0x421dbc,
];

const CORE_INPUT_103_MODULUS = [
  0x421de0,
  0x421df0,
  0x421dff,
  0x421e0a,
  0x421e10,
];

const CORE_INPUT_105_MASK = [
  0x421e20,
  0x421e30,
  0x421e3f,
  0x421e44,
  0x419e48,
  0x40e2a5,
  0x436464,
];

const CORE_OPS_INPUT_104 = [
  0x419d20,
  0x419d30,
  0x419d41,
  0x419d49,
  0x419d4b,
  0x419d60,
  0x419d76,
  0x419d97,
  0x419db9,
  0x419df4,
  0x419e29,
  0x419e48,
  0x419e50,
  0x419e75,
  0x419e95,
  0x419ee9,
  0x419f0d,
  0x419f4e,
  0x419f61,
  0x419f82,
  0x419fda,
  0x419ff6,
  0x421d91,
  0x421e0a,
  0x421e10,
  0x421e44,
  0x419ae0,
  0x419bc9,
  0x419be2,
  0x419b3a,
  0x419c24,
];

const CORE_INPUT_RESET_FC = [
  0x419a70,
  0x419a7d,
  0x419a88,
  0x4056f0,
  0x405704,
  0x40571a,
  0x405722,
];

function check(ea, lines) {
  const prefix = `.text:${ea.toString(16).toUpperCase().padStart(8, '0')}`;
  const idx = lines.findIndex((l) => l.startsWith(prefix));
  if (idx < 0) return '没有这一行';
  for (let k = idx; k < Math.min(idx + 12, lines.length); k += 1) {
    if (!lines[k].startsWith(prefix)) break;
    const body = lines[k].slice(prefix.length).trim();
    if (body === '' || body.startsWith(';')) continue;
    return null;
  }
  return '标号后没有指令';
}

function assertSites(sites) {
  const lines = fs.readFileSync(LST, 'utf8').split('\n');
  const bad = [];
  for (const ea of sites) {
    const why = check(ea, lines);
    if (why) bad.push(`0x${ea.toString(16)}：${why}`);
  }
  assert.deepEqual(bad, []);
}

test('★ CORE-INPUT-102-SLOT 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(CORE_INPUT_102_SLOT);
});

test('★ CORE-INPUT-103-MODULUS 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(CORE_INPUT_103_MODULUS);
});

test('★ CORE-INPUT-105-MASK 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(CORE_INPUT_105_MASK);
});

test('★ CORE-OPS-INPUT-104 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(CORE_OPS_INPUT_104);
});

test('★ CORE-INPUT-RESET-FC 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(CORE_INPUT_RESET_FC);
});
