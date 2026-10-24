/** @env assets @kind gate @why 第四批新增/改写的绑定站点若有一处不对着 .lst 指令行首，规则正文的机器锚就失效了（锚只能是 EA，EA 必须真是指令） */
/**
 * tools/test/spec-sites-batch4.assets.test.mjs
 *
 * 第四批三条规则的站点：场景命令记录族 / 图形设备初始化 / 绘制项的消费者。
 * ★ 只钉"锚是机器站点"，不证明合同语义；语义证据由各规则的残差与实现守卫承担。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const SKIP = fs.existsSync(LST) ? false : '语料 .lst 不在场';

const SCENE_COMMANDS = [
  0x4adf14, 0x4adf30, 0x4adf45, 0x4adf5a, 0x4adf6f, 0x4adf84, 0x4adf96, 0x4adf9c,
  0x4adfa2, 0x4adfa8, 0x4adfbd, 0x4b0862, 0x4b0876, 0x4b08db, 0x4b0d48, 0x4b105c,
  0x4b107a, 0x4af8e9, 0x4af93e, 0x4afa99, 0x4afaee, 0x4af996, 0x4af9c0, 0x4afb46,
  0x4afb5b, 0x4afb70, 0x4b31a6, 0x4b326a, 0x4b359b, 0x4b351a, 0x4b0c50, 0x4b0c67,
  0x4b0c8c, 0x4b0ca3, 0x4b0cc3, 0x4b0cda, 0x4b4029, 0x4b3f8e, 0x4b3f99, 0x4addfa,
  0x4addff, 0x4ade05, 0x4adec5, 0x4a2d19, 0x4a2cdf, 0x4adde0,
];

const BOOT_GRAPHICS = [
  0x499027, 0x49902e, 0x499032, 0x499037, 0x49903e, 0x499054, 0x499055, 0x499070,
  0x499071, 0x499052, 0x4990a9, 0x498e22, 0x498ed1, 0x498bf8, 0x498c15, 0x498c31,
  0x498c53, 0x49865e, 0x498687, 0x498680, 0x498682, 0x4067a0, 0x4067d9, 0x4067bd,
  0x4067f8, 0x4067ec, 0x4067f2, 0x406805, 0x40682a, 0x406852, 0x40676c, 0x406812,
  0x40686b, 0x406ae3, 0x406af5, 0x406b3e, 0x4065f0, 0x498d1e, 0x498d4a, 0x4990ce,
  0x4990f5, 0x4991e5, 0x4991a2, 0x4991d2, 0x499202,
];

const TEX_CONSUMER = [
  0x4aef36, 0x4aef7a, 0x4aef8f, 0x4aef95, 0x4aef9c, 0x4aefa3, 0x4aefb8, 0x4aefdd,
  0x4af1a5, 0x4a2e59, 0x4a2e77, 0x4a2e9d, 0x4a2ec5, 0x4a2ffb, 0x4a3004, 0x4a3023,
  0x4a3048, 0x4a30af, 0x4a3219, 0x4a3243, 0x4a326a, 0x4a3292, 0x4a32b2, 0x4a32cb,
  0x4a331c, 0x4a332f, 0x4a343b, 0x4a3474, 0x4a34ab, 0x4a34eb, 0x49aa89, 0x49aaa8,
  0x49aad3, 0x49aaef, 0x49ab14, 0x49ab43, 0x49aba0, 0x49ad80, 0x49ad83, 0x49ad72,
  0x49ad75, 0x49ad8a, 0x49ad59, 0x49bfcc, 0x49bfd4, 0x49c933, 0x49c935, 0x49c93b,
  0x49c93e, 0x4ab13f, 0x4ab15e, 0x499c51, 0x405317, 0x4a50d7, 0x4a51b7, 0x4a5247,
  0x4ace87,
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

test('★ CORE-OPS-SCENE-COMMAND-RECORDS 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(SCENE_COMMANDS);
});

test('★ CORE-BOOT-GRAPHICS-INIT 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(BOOT_GRAPHICS);
});

test('★ CORE-OPS-TEXTURE-DRAW-CONSUMER 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(TEX_CONSUMER);
});
