/** @env pure @kind gate @why 技能（`.agents/skills/<名字>/SKILL.md`）的体例破了，或它引用的 `pnpm tools <域> <动作>` 已经不存在（真源指针指空） */
/**
 * tools/test/skills.test.mjs —— **技能的机械契约**
 *
 * 为什么需要它：技能是 agent 的"入口文档"，它里面写的命令就是**真源指针**。
 * 工具改名 / 动作删掉之后，散文不会有任何反应 —— 指针指空了，读技能的人（agent）照着敲才发现。
 * ⇒ 把两件事变成可执行的：① 体例（front matter / 标题 / 有分节）；② **引用的域与动作必须真实存在**。
 *
 * ⛔ 不去断言技能正文的写法（那是散文，会变成"不许出现某字符串"式的脆弱守卫）——
 *    只断言**结构性**的东西：能解析、能指到真东西。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { loadDomains } from '../cli.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';

const SKILLS_DIR = path.join(REPO_ROOT, '.agents', 'skills');

const skillDirs = () => fs.readdirSync(SKILLS_DIR, { withFileTypes: true }).filter((x) => x.isDirectory()).map((x) => x.name);
const readSkill = (name) => fs.readFileSync(path.join(SKILLS_DIR, name, 'SKILL.md'), 'utf8');

test('每个技能目录都有 SKILL.md，且 front matter 的 name == 目录名（DSH 按固定路径发现 ⇒ 名字不一致就找不到）', () => {
  const dirs = skillDirs();
  assert.ok(dirs.length > 0, '至少要有一个技能');
  for (const d of dirs) {
    const f = path.join(SKILLS_DIR, d, 'SKILL.md');
    assert.ok(fs.existsSync(f), `${d}/ 里没有 SKILL.md`);
    const t = fs.readFileSync(f, 'utf8');
    const fm = /^---\r?\n([\s\S]*?)\r?\n---\r?\n/.exec(t);
    assert.ok(fm, `${d}/SKILL.md 缺 front matter`);
    assert.equal((/^name:\s*(\S+)\s*$/m.exec(fm[1]) ?? [])[1], d, `${d}/SKILL.md 的 name 与目录名不一致`);
    const desc = (/^description:\s*(.+)$/m.exec(fm[1]) ?? [])[1] ?? '';
    assert.ok(desc.length >= 40, `${d}/SKILL.md 的 description 太短（agent 靠它决定用不用）`);
    assert.ok(!/:\s/.test(desc), `${d}/SKILL.md 的 description 里含 ASCII ": " ⇒ YAML 纯量会被截断`);
    assert.match(t.slice(fm[0].length), /^# /m, `${d}/SKILL.md 正文要有 # 标题`);
    assert.match(t.slice(fm[0].length), /^## /m, `${d}/SKILL.md 正文要有 ## 分节`);
  }
});

test('★ 技能里引用的 `pnpm tools <域> [<动作>]` 必须真的存在（防"真源指针指空"）', async () => {
  const domains = await loadDomains();
  const ops = new Map(domains.map((d) => [d.id, new Set(d.operations.map((o) => o.name))]));
  const bad = [];
  for (const d of skillDirs()) {
    const t = readSkill(d);
    for (const m of t.matchAll(/pnpm tools ([a-z][a-z0-9-]*)(?: ([a-z][a-z0-9-]*))?/g)) {
      const [, dom, op] = m;
      if (!ops.has(dom)) { bad.push(`${d}: 域不存在 → ${dom}`); continue; }
      if (op && !ops.get(dom).has(op)) bad.push(`${d}: 动作不存在 → ${dom} ${op}`);
    }
  }
  assert.deepEqual(bad, [], `技能里指空了的命令：\n  - ${bad.join('\n  - ')}`);
});

/**
 * ⛔ **有意不立的一条**："技能里不许出现会写盘的动作"。
 * 试过，立刻红在 `amayui-translate` 上 —— 而它教的 `patch set --edits` / `patch index` 正是那份技能的**本职**
 * （翻译作业就是写译文）。⇒ "写盘副作用与技能定位是否相称"是**判断**，不是机械判据；
 * 把它写成"不许出现某命令"就是 AGENTS.md §10 点名的**脆弱守卫**（红得没有意义）。见 `docs/00-origin/decisions.md` §6。
 */
