/** @env pure @kind gate @why 代码注释里引用的规范条目 id 在 spec 里不存在时，读者会把"注释说有这条规范"当成证据 —— 本仓已经因此把一条**不存在**的规范当成了判据（`CORE-OPS-AUDIO-SE-STOP`），并据此写下了与取证相反的行为断言 */
/**
 * `tools/test/spec-rule-refs.test.mjs` —— 代码里引用的**规范条目 id** 必须真的存在
 *
 * ## 为什么需要它
 * 注释里的 `CORE-…` 是最容易被当成证据的东西：它读起来像"这条已经定案了"。实测两次踩到：
 * ① `0xb6` 的实现注释引 `CORE-OPS-AUDIO-SE-STOP`，而 spec 里**没有**这条（该 opcode 在未成篇 gap 里）；
 *    更糟的是自检的断言字符串写着"（规范：不清）"—— 依据是一个不存在的规范；
 * ② `apps/emulator-new` 的帧族注释把 `CORE-OPS-FRAME-*` 写成 `CORE-FRAME-*`（少一段 `OPS`）。
 * 两者都**不影响编译、不影响运行**，只影响"人相信什么"。
 *
 * ## 判据（机械、不认语义）
 * 代码（`apps/<应用>/src` 下的 `.ts` / `.mts` / `.cpp` / `.h`）里出现的每个 `CORE-…` 记号，
 * 必须是**某个真实规范条目 id 的前缀**（允许用族前缀 `CORE-OPS-SCENE` 指代整族）。
 * ★ 为什么允许前缀：注释里写族名是合理的（`CORE-INPUT` 指输入族）；不允许的是**指向不存在的条目**。
 *
 * ★ 最后一条测试是判别力自检：喂两条假 id（真踩过的那两个）⇒ 判据必须报红。
 */
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
/** 扫描面：**代码**（`src`）。文档 / 台账不在这里 —— 它们各有自己的守卫。 */
const CODE_DIRS = ['apps/emulator-cxx/src', 'apps/emulator-new/src', 'apps/emulator/src'];
const CODE_EXT = new Set(['.ts', '.mts', '.mjs', '.cpp', '.h', '.hpp', '.cc']);
const REF_RE = /CORE-[A-Z0-9]+(?:-[A-Z0-9]+)*/g;

/** 全部规范条目 id（`<!-- rule: ID -->`）。 */
function ruleIds() {
  const dir = path.join(ROOT, 'data/spec/chapters');
  const ids = new Set();
  for (const file of readdirSync(dir)) {
    if (!file.endsWith('.md')) continue;
    const text = readFileSync(path.join(dir, file), 'utf8');
    for (const match of text.matchAll(/<!--\s*rule:\s*([A-Z0-9-]+)\s*-->/g)) ids.add(match[1]);
  }
  return ids;
}

/** 递归收集代码文件（跳过 `node_modules` / 构建目录）。 */
function codeFiles(dir, out = []) {
  let entries;
  try {
    entries = readdirSync(dir, { withFileTypes: true });
  } catch {
    return out; // 目录不存在（例如某条实现线被移走）⇒ 不扫，不报红
  }
  for (const entry of entries) {
    if (entry.name === 'node_modules' || entry.name === 'out' || entry.name === 'dist') continue;
    const abs = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      codeFiles(abs, out);
    } else if (CODE_EXT.has(path.extname(entry.name))) {
      out.push(abs);
    }
  }
  return out;
}

/** 代码里出现过的 `CORE-…` 记号 → 出现位置（相对仓库根）。 */
function codeRefs() {
  const refs = new Map();
  for (const dir of CODE_DIRS) {
    for (const file of codeFiles(path.join(ROOT, dir))) {
      const text = readFileSync(file, 'utf8');
      for (const match of text.matchAll(REF_RE)) {
        const rel = path.relative(ROOT, file).split(path.sep).join('/');
        if (!refs.has(match[0])) refs.set(match[0], []);
        refs.get(match[0]).push(rel);
      }
    }
  }
  return refs;
}

/** 纯函数：返回"指向不存在条目"的引用（守卫断言与判别力自检共用）。 */
function danglingRefs(refs, rules) {
  const list = [...rules];
  const out = [];
  for (const [ref, files] of refs) {
    // 前缀判据：族前缀（`CORE-OPS-SCENE`）算合法；`CORE-OPS-SCENEX` 这种不算。
    const known = list.some((id) => id === ref || id.startsWith(`${ref}-`));
    if (!known) out.push(`${ref}  ← ${[...new Set(files)].join(', ')}`);
  }
  return out;
}

test('代码注释引用的规范条目 id 都存在（允许族前缀）', () => {
  const refs = codeRefs();
  assert.ok(refs.size > 20, `扫描面可疑：只找到 ${refs.size} 个 CORE-… 记号`);
  assert.deepEqual(danglingRefs(refs, ruleIds()), []);
});

test('判别力：指向不存在条目的引用必须报红', () => {
  const rules = ruleIds();
  // ① 真实踩过的那条：0xb6 被写成有规范条目，其实它在未成篇 gap 里
  assert.notDeepEqual(
    danglingRefs(new Map([['CORE-OPS-AUDIO-SE-STOP', ['apps/emulator-cxx/src/core/vm_ops_engine.cpp']]]), rules),
    [],
  );
  // ② 少一段 `OPS` 的帧族 id（真实踩过）
  assert.notDeepEqual(
    danglingRefs(new Map([['CORE-FRAME-CALL', ['apps/emulator-new/src/core/handlers.ts']]]), rules),
    [],
  );
  // ③ 族前缀是合法的（否则守卫会把 `CORE-OPS-SCENE` 这类合理写法判红）
  assert.deepEqual(danglingRefs(new Map([['CORE-OPS-SCENE', ['x.cpp']]]), rules), []);
});
