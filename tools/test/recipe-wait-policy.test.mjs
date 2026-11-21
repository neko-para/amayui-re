/** @env pure @kind gate @why 配方里出现了白名单之外的 `wait`（又在拿时间当等待条件），或白名单条目对应的 wait 已经不在（棘轮松了） */
/**
 * tools/test/recipe-wait-policy.test.mjs —— `scripts/recipes/*.plan` 的**等待策略**守卫
 *
 * 用户口径：「**除非有明确原因，否则都不应该用时间作为等待条件**」。
 *   `wait <ms>` 是**宿主墙钟**、不推进 guest、**没有守卫**（`recipe.h:37-39` / `:157-159`）⇒
 *   能用 `await script` / `await position` 表达的一律该换成门（`input-plan.md` §2.6）。
 *
 * 红了意味着什么（三件都是"变了就是 bug、而且变了是静默的"）：
 *   ① 某份 `.plan` 里出现了 `ALLOWED` 之外的 `wait` ⇒ 又有一个配方在用时间当等待条件，
 *      而它会静默地让该配方**不可复现**（`wait` 的时刻是墙钟 ⇒ trace 一律 `NON-DIFFABLE`）；
 *   ② `ALLOWED` 里某条对应的 wait 已经不在（被删 / 改了毫秒数）⇒ 白名单没跟着收，
 *      下次它就会成为"新 wait 的挡箭牌"（棘轮只许降，不许涨）；
 *   ③ `ALLOWED` 里有条目的 `why` 是空的 / 文件不存在 / 条目重复 ⇒ 白名单本身坏掉。
 *
 * ★ 定位用**序号**（该文件里第几个 `wait`，1 起），⛔ 不用行号：行号会随注释 / 空行漂，
 *   而序号只在"增删一条 wait"时变 —— 那正是我们要变的时候。
 * ★ 这里**不断言"一共有几条 wait"**（那是读数）：判据是"实际集合 == 白名单集合"。
 *
 * ## 怎么加一条（换不掉的时候）
 * 白名单条目 = `{ file, ordinal, ms, why }`。`why` 必须**诚实**：
 *   · 能换门但还没换 ⇒ `TODO(gate): 候选门 = <脚本> [<lo>,<hi>)`（⛔ 不许编一个 structural 理由）；
 *   · 真的换不掉（拖拽采样间隔 / 音频尾声 / profiler 窗内时长 / 收尾窗）⇒ 写清**为什么**它是时间语义。
 *   换掉一条 wait ⇒ **同时删掉它对应的白名单条目**（序号的后续条目会整体前移，所以通常要连带改序号）。
 *
 * 现状快照 / 逐条判定（`gate-able` / `structural` / `unknown` 的分类依据）：`.tmp/recipes-audit/waits.md`。
 *
 * 运行：`node --test tools/test/recipe-wait-policy.test.mjs`（`@env pure` ⇒ 也在 `pnpm test` 里）
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

const ROOT = new URL('../../', import.meta.url);
const RECIPES = new URL('apps/emulator-lift/scripts/recipes/', ROOT);

// =====================================================================================
// 白名单（现状快照；**只许降**）。`ordinal` = 该文件里第几个 `wait`（1 起）。
// =====================================================================================
const ALLOWED = [
  // ---- save04-fault.plan（10）------------------------------------------------------
  { file: 'save04-fault.plan', ordinal: 1, ms: 300, why: 'TODO(gate): 候选门 = 把现有 SBUNKI [0x0AF8,0x0B18) 门挪到 click 之前再点（三段回调 shape，SC0000.md 第3轮）；需一次复跑确认' },
  { file: 'save04-fault.plan', ordinal: 2, ms: 8000, why: 'TODO(gate): 候选门 = SC0000 wipe 之后的停点（SC0000.md 只给了候选块 [0x7201C,0x725DC)，没有门命中读数）' },
  { file: 'save04-fault.plan', ordinal: 3, ms: 3000, why: 'TODO(gate): 候选门 = 同一处的 SC4000 [0x5800,0x5860)（0x5828 = 停在等输入，defects#BUG-32）；wait 应可直接删，需复跑确认' },
  { file: 'save04-fault.plan', ordinal: 4, ms: 2000, why: 'TODO(gate): 候选门 = 同一行门 SC4000 [0x5920,0x5960) 本身；需确认该区间也是等输入停点' },
  { file: 'save04-fault.plan', ordinal: 5, ms: 2000, why: 'TODO(gate): 候选门 = BUNKI 的输入读段（未取证，形状参照 TITLE [0x0460,0x0520)）；本条正是 BUG-32 里"等 2 s 才成"的时序敏感点' },
  { file: 'save04-fault.plan', ordinal: 6, ms: 3000, why: 'TODO(gate): 候选门 = 同一行门 SC4000 [0x6B00,0x6B80)；需确认该区间是等输入停点' },
  { file: 'save04-fault.plan', ordinal: 7, ms: 1500, why: 'TODO(gate): 候选门 = 同一行门 SC4000 [0x6B80,0x6C00)；需确认该区间是等输入停点' },
  { file: 'save04-fault.plan', ordinal: 8, ms: 1500, why: 'TODO(gate): 候选门 = 同一行门 SC4000 [0x6C00,0x6D00)；需确认该区间是等输入停点' },
  { file: 'save04-fault.plan', ordinal: 9, ms: 1000, why: 'TODO(gate): 候选门 = 同一行门 SC4000 [0x10D00,0x10E00)；需确认该区间是等输入停点' },
  { file: 'save04-fault.plan', ordinal: 10, ms: 15000, why: '收尾窗：fault 发生在 click 之后的 wipe 里，配方已没有"再往前走一步"的动作可等；等的是 guest mem fault 自己爆出来（判据在 fault 文本，不在门）' },

  // ---- save05-move.plan（26）-------------------------------------------------------
  { file: 'save05-move.plan', ordinal: 1, ms: 2000, why: 'TODO(audit): 看不出这 2 s 等的是什么（门已是 SAVE 回调体 [0x1104,0x11C4)，其后只有 click 300 388）；要一次采样才知道是不是"列表画完"' },
  { file: 'save05-move.plan', ordinal: 2, ms: 2000, why: 'TODO(gate): 候选门 = SBUNKI 内"对话框已落位"的区间（save07-battle.plan:36-37 有实测：门入口当场点会被吞）；需先量' },
  { file: 'save05-move.plan', ordinal: 3, ms: 20000, why: 'TODO(gate): 候选门 = FIELD 主轮询体 [0x579C,0x5860)（save07-battle.plan:13 的读数）' },
  { file: 'save05-move.plan', ordinal: 4, ms: 1500, why: 'TODO(gate): 候选门 = FIELD 内"覆盖图已消失"的位置（未取证）；现在等的是那次 click 的效果落地' },
  { file: 'save05-move.plan', ordinal: 5, ms: 300, why: '拖拽采样：down 之后的第一步位移必须跨过 guest 的读点 / 帧，否则整串位移在同一读点内被读完（本文件 :7-11 的实测：纯 mouse_move 镜头不动）' },
  { file: 'save05-move.plan', ordinal: 6, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 7, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 8, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 9, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 10, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 11, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 12, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 13, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 14, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 15, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 16, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 17, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 18, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 19, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 20, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 21, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 22, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 23, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 24, ms: 120, why: '拖拽采样间隔：每步之间必须有时间推进，否则鼠标位移在同一读点内被合并（本文件 :7-11 实测）' },
  { file: 'save05-move.plan', ordinal: 25, ms: 300, why: '拖拽最后一步的驻留（松手前要让这一步被看到）：与序 5..24 是同一族，不是门' },
  { file: 'save05-move.plan', ordinal: 26, ms: 3000, why: 'profiler 窗的收尾：本文件 :27 明写"窗口 = click + 拖拽 + 收尾 3 s"，窗内时长本身就是判据' },

  // ---- save07-battle.plan：**0 条**（2026-11-21 两处都换成了门）--------------------------
  //   `wait 1000` → `await position SBUNKI [0x0B18,0x0C58)`（读段之后，click 的 down→up 才能
  //   跨过下一次 `read-mouse-button` 被 guest 看到边沿）；`wait 25000` → `await position BTL
  //   [0x4A84,0x4AE4)`（战斗主循环）。⇒ 本文件不再出现在白名单里。

  // ---- sc0000-wipe.plan（4）--------------------------------------------------------
  { file: 'sc0000-wipe.plan', ordinal: 1, ms: 300, why: 'TODO(gate): 候选门 = 把 SBUNKI [0x0AF8,0x0B18) 门挪到 click 之前（三段回调 shape，SC0000.md 第3轮）；需一次复跑确认' },
  { file: 'sc0000-wipe.plan', ordinal: 2, ms: 4000, why: 'TODO(gate): 候选门 = SC0000 wipe 之后的停点（SC0000.md 只给候选块 [0x7201C,0x725DC)，无命中读数）；现在等的是 "pre" 窗开始前的稳态' },
  { file: 'sc0000-wipe.plan', ordinal: 3, ms: 1500, why: 'profiler 窗 pre 的窗内时长（量宿主墙钟区间，位置门表达不了 — 本文件 :7）' },
  { file: 'sc0000-wipe.plan', ordinal: 4, ms: 1800, why: 'profiler 窗 wipe 的窗内时长（同上；本文件 :7-9 明写"1800 ms 里只有 ~10 帧是转场"）' },

  // ---- se-eof.plan（1）-------------------------------------------------------------
  { file: 'se-eof.plan', ordinal: 1, ms: 12000, why: '音频尾声：等 SE 播完 + 尾声重复（本文件 :21-23；位置门表达不了音频喂完）' },

  // ---- sn0000-quit.plan（5）--------------------------------------------------------
  { file: 'sn0000-quit.plan', ordinal: 1, ms: 10000, why: 'TODO(gate): 候选门 = FELLOW [0x1684,0x16A4)（_recipe-map ③-4，4/4 命中）；紧随其后的 await script FELLOW 已在等同一件事' },
  { file: 'sn0000-quit.plan', ordinal: 2, ms: 2000, why: 'TODO(gate): 候选门 = FELLOW [0x1684,0x16A4)（_recipe-map ③-4，4/4 命中 + 负对照）' },
  { file: 'sn0000-quit.plan', ordinal: 3, ms: 1000, why: 'TODO(gate): 候选门 = SAVE [0x1104,0x11C4)（_recipe-map ③-5，2/2 命中）' },
  { file: 'sn0000-quit.plan', ordinal: 4, ms: 5000, why: 'TODO(gate): 候选门 = TITLE [0x0460,0x0520)（input-plan.md §2.6，7/7 命中）' },
  { file: 'sn0000-quit.plan', ordinal: 5, ms: 10000, why: '收尾窗：click 1180 640（Quit）之后没有可断言的门，等的是退出路径走完' },

  // ---- sn0000-save-profile.plan（6）------------------------------------------------
  { file: 'sn0000-save-profile.plan', ordinal: 1, ms: 12000, why: 'profiler 窗 fellow-enter 的窗内时长（本文件 :23-24 明写窗内 wait 是真·时间语义）' },
  { file: 'sn0000-save-profile.plan', ordinal: 2, ms: 2000, why: 'TODO(gate): 候选门 = FELLOW [0x1684,0x16A4)（_recipe-map ③-4，4/4 命中）' },
  { file: 'sn0000-save-profile.plan', ordinal: 3, ms: 2000, why: 'TODO(gate): 候选门 = SAVE [0x1104,0x11C4)（_recipe-map ③-5，2/2 命中）' },
  { file: 'sn0000-save-profile.plan', ordinal: 4, ms: 1000, why: 'TODO(gate): 候选门 = await script SBUNKI（选槽后确认框在另一个脚本，_recipe-map ③-7）；更细的位置门需先量' },
  { file: 'sn0000-save-profile.plan', ordinal: 5, ms: 15000, why: 'profiler 窗 save-confirm 的窗内时长（本文件 :56-57 明写留 15 s 量缩略图 + DAT 写盘）' },
  { file: 'sn0000-save-profile.plan', ordinal: 6, ms: 2000, why: '收尾：profile_end 之后最后一条，等写盘收尾；若"保存完成回到哪一屏"取证了可换成门' },

  // ---- sn0000-save.plan（5）--------------------------------------------------------
  { file: 'sn0000-save.plan', ordinal: 1, ms: 10000, why: 'TODO(gate): 候选门 = FELLOW [0x1684,0x16A4)（_recipe-map ③-4，4/4 命中）；紧随其后的 await script FELLOW 已在等同一件事' },
  { file: 'sn0000-save.plan', ordinal: 2, ms: 2000, why: 'TODO(gate): 候选门 = FELLOW [0x1684,0x16A4)（_recipe-map ③-4，4/4 命中）' },
  { file: 'sn0000-save.plan', ordinal: 3, ms: 2000, why: 'TODO(gate): 候选门 = SAVE [0x1104,0x11C4)（_recipe-map ③-5，2/2 命中）' },
  { file: 'sn0000-save.plan', ordinal: 4, ms: 1000, why: 'TODO(gate): 候选门 = await script SBUNKI（确认框换脚本，_recipe-map ③-7）；更细的位置门需先量' },
  { file: 'sn0000-save.plan', ordinal: 5, ms: 8000, why: '收尾窗：本文件最后一条（之后没有指令）；等的是同步写盘 + 缩略图落地，没有可断言的门' },

  // ---- sn0000.plan（3）-------------------------------------------------------------
  { file: 'sn0000.plan', ordinal: 1, ms: 1000, why: 'profiler 窗 idle 的窗内时长（本文件 :12-13 明写留下的 wait 就是三片窗内时长）' },
  { file: 'sn0000.plan', ordinal: 2, ms: 3000, why: 'profiler 窗 click1 的窗内时长（同上）' },
  { file: 'sn0000.plan', ordinal: 3, ms: 3000, why: 'profiler 窗 click2 的窗内时长（同上）' },

  // ---- title-loaddata.plan（2）-----------------------------------------------------
  { file: 'title-loaddata.plan', ordinal: 1, ms: 5000, why: 'TODO(gate): 候选门 = TITLE [0x0460,0x0520)（input-plan.md §2.6，7/7 命中；同一个 wait 2000 的对照是 12 跑 4 成）' },
  { file: 'title-loaddata.plan', ordinal: 2, ms: 20000, why: '收尾窗：这份是"看结局"的复现件（进加载界面 / 干净退出），判据在日志三行；没有可断言的门' },
];

// =====================================================================================
// 解析：一份 `.plan` 里的全部 `wait`（按文件顺序；ordinal 1 起）
// =====================================================================================
function planFiles() {
  return fs
    .readdirSync(RECIPES)
    .filter((name) => name.endsWith('.plan'))
    .sort();
}

/** 返回 `[{ ordinal, line, ms }]`；形态不认得 ⇒ `[{ ordinal, line, bad: true }]`（判据见下）。 */
function waitsOf(file) {
  const text = fs.readFileSync(new URL(file, RECIPES), 'utf8');
  const lines = text.split('\n');
  const out = [];
  for (let i = 0; i < lines.length; i += 1) {
    const code = lines[i].replace(/\r$/, '').split('#')[0].trim();  // `#` 起注释（行尾也吃）
    if (code === '') continue;
    if (!/^wait\b/.test(code)) continue;
    const ordinal = out.length + 1;
    const m = /^wait\s+(\d+)$/.exec(code);
    if (!m) {
      // `wait` 起头但不是 `wait <ms>` ⇒ 解析器那边会硬错；这里必须红，⛔ 不许跳过（否则守卫静默变绿）。
      out.push({ ordinal, line: i + 1, bad: true, text: code });
      continue;
    }
    out.push({ ordinal, line: i + 1, ms: Number(m[1]), text: code });
  }
  return out;
}

test('recipe-wait-policy/whitelist-shape：白名单条目形如 `{ file, ordinal, ms, why }`，`why` 非空且不重复', () => {
  const files = new Set(planFiles());
  const seen = new Set();
  const bad = [];
  for (const e of ALLOWED) {
    const label = `${e.file}#${e.ordinal}`;
    if (!files.has(e.file)) bad.push(`${label}：文件不在 scripts/recipes/ 里`);
    if (!Number.isInteger(e.ordinal) || e.ordinal < 1) bad.push(`${label}：ordinal 必须是正整数`);
    if (!Number.isInteger(e.ms) || e.ms < 1) bad.push(`${label}：ms 必须是正整数`);
    if (typeof e.why !== 'string' || e.why.trim() === '') bad.push(`${label}：why 不许空（诚实写清为什么换不掉）`);
    if (seen.has(label)) bad.push(`${label}：白名单里重复了（同一条 wait 两条 why）`);
    seen.add(label);
  }
  assert.deepEqual(bad, [], `白名单本身坏了：\n  - ${bad.join('\n  - ')}`);
});

test('recipe-wait-policy/wait-set-equal：白名单之外的 wait 立刻红，白名单里过期的条目也红（棘轮只降）', () => {
  const files = planFiles();
  // 防"解析器整体失配 ⇒ 0 条 ⇒ 恒绿"的地板（不是条数断言：只要求目录里确实有配方）。
  assert.ok(files.length > 0, `scripts/recipes/ 里一份 .plan 都没解析到（路径 / 后缀变了？）`);

  const actual = new Map();  // key = `${file}#${ordinal}` → { ms, line, text }
  const malformed = [];
  for (const file of files) {
    for (const w of waitsOf(file)) {
      if (w.bad) {
        malformed.push(
          `${file}:${w.line}：这一行以 wait 起头但不是 \`wait <ms>\` —— 形态变了（${JSON.stringify(w.text)}）`,
        );
        continue;
      }
      actual.set(`${file}#${w.ordinal}`, { file, ordinal: w.ordinal, line: w.line, ms: w.ms });
    }
  }
  assert.deepEqual(malformed, [], `这些 wait 行的形态认不得（守卫会静默漏过它们）：\n  - ${malformed.join('\n  - ')}`);

  const allowed = new Map(ALLOWED.map((e) => [`${e.file}#${e.ordinal}`, e]));

  // ① 实际有、白名单没有（含"序号撞上但毫秒数变了"⇒ 当成新 wait + 过期条目，两条都报）。
  const unexpected = [];
  for (const [key, w] of actual) {
    const entry = allowed.get(key);
    if (entry === undefined) {
      unexpected.push(`${w.file}:${w.line}（该文件第 ${w.ordinal} 个 wait）\`wait ${w.ms}\``);
    } else if (entry.ms !== w.ms) {
      unexpected.push(
        `${w.file}:${w.line}（该文件第 ${w.ordinal} 个 wait）\`wait ${w.ms}\` —— 白名单记的是 ${entry.ms}`,
      );
    }
  }

  // ② 白名单有、实际没有（被删 / 被改；序号前移也会落在这里）。
  const stale = [];
  for (const [key, e] of allowed) {
    const w = actual.get(key);
    if (w === undefined) {
      stale.push(`${e.file}#${e.ordinal}（白名单记 ${e.ms} ms）—— 该文件里已经没有第 ${e.ordinal} 个 wait`);
    } else if (w.ms !== e.ms) {
      stale.push(`${e.file}#${e.ordinal}（白名单记 ${e.ms} ms）—— 现在是 \`wait ${w.ms}\`（${e.file}:${w.line}）`);
    }
  }

  assert.deepEqual(
    unexpected,
    [],
    '配方里出现了白名单之外的 `wait`（用户口径：除非有明确原因，否则不该用时间作为等待条件）：\n  ' +
      `${unexpected.join('\n  ')}\n` +
      '要么换成 `await` 门（`await script <名>` / `await position <名> <lo> <hi>`），' +
      '要么在 `tools/test/recipe-wait-policy.test.mjs` 的白名单里写 why（只许 structural —— 拖拽采样间隔 / ' +
      '音频尾声 / profiler 窗内时长 / 收尾窗 这几类；⛔ 不要为了让它绿而编一个 structural 理由）。',
  );
  assert.deepEqual(
    stale,
    [],
    '白名单里有条目对应的 wait 已经不在（棘轮只许降：换掉一条就删一条）：\n  ' +
      `${stale.join('\n  ')}\n` +
      '删掉它，并把同一文件里**靠后**条目的 ordinal 各减一（序号是该文件里第几个 wait，不是行号）。',
  );
});
