/** @env pure @kind gate @why 音效槽的两个数量在 shape / spec / 实现之间对不上时（10 格的引擎表被当成 15、或规则的表指针指向别的成员），"槽号 10..14 也是合法引擎格"这类**假状态**会再次悄悄进来 —— 而它看起来完全正常：读得到值、写得进去 */
/**
 * `tools/test/emulator-cxx-audio-slots.test.mjs` —— 音效槽量：shape ↔ spec ↔ 实现
 *
 * ## 为什么需要它
 * 声音后端（`T_Sound`）有 **15** 个槽，引擎自己那张「槽 → 当前资源号」表（`T_SE+0x4BC`）只有 **10** 格。
 * 实现里曾经只有一个 `15`（`std::array<…, 15> se_slot_resource_`，判据 `slot > 14`）⇒
 * 槽号 10..14 被当成合法的引擎格写进去：**凭空多出 5 个引擎格**，读得到、看起来正常，
 * 而引擎里根本没有那 5 格（越界写进下一块嵌入对象）。这类错**不会**被"跑得通"发现。
 *
 * ## 它守什么（三处必须说同一件事）
 * ① **shape**：`data/shape/types.json` 的 `T_SE+0x4bc` 是 count 10 的有界数组；`T_Sound` 的七张槽表是 count 15；
 * ② **实现**：`apps/emulator-cxx/src/core/vm.h` 的两个常量等于上面两个数，且表声明用常量（不是又写死一个数）；
 * ③ **spec**：`CORE-OPS-AUDIO-WAV` 的 `storage` 指向 `shape:T_SE@0x4bc` —— 规则的"那张表"有机器锚。
 *
 * ★ 它**不**判语义（谁写谁清在 selftest `check_audio_slot_bounds` 里咬）。
 * ★ 最后一条测试是**判别力自检**：把三处各自改坏一份内存副本，`problems()` 必须报红 ——
 *   否则"三处一致"可能只是"三处都被同一个常量的抄本喂着"。
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = (rel) => readFileSync(path.join(ROOT, rel), 'utf8');
const readJson = (rel) => JSON.parse(read(rel));

const RULE = 'CORE-OPS-AUDIO-WAV';
/** `T_Sound` 的槽数组（后端槽量）—— 七张同量表的起点，逐个都要对得上。 */
const SOUND_ARRAYS = ['0x51c', '0x564', '0x5a0', '0x5dc', '0x618', '0x690', '0x7f8'];

/** 某类型的某个成员起点是不是**有界数组**；返回 count（不是整数就返回 null）。 */
function memberCount(types, type, off) {
  const kind = types?.[type];
  if (!kind) return null;
  for (const member of kind.members ?? []) {
    if (String(member.off).toLowerCase() !== off.toLowerCase()) continue;
    return Number.isInteger(member.count) ? member.count : null;
  }
  return null;
}

/** 实现侧（两条实现线各一份常量）：`vm.h` 的 C++ 常量 + 表声明是否用常量；`machine.ts` 的 TS 常量。 */
function implementation(vmh, machineTs) {
  const table = /kSeSlotTableSize\s*=\s*(\d+)\s*;/.exec(vmh);
  const sound = /kSoundSlotCount\s*=\s*(\d+)\s*;/.exec(vmh);
  // emulator-new（参考实现）：`export const SE_SLOT_TABLE_SIZE = 10;` / `export const SOUND_SLOT_COUNT = 15;`
  const tsTable = /export\s+const\s+SE_SLOT_TABLE_SIZE\s*=\s*(\d+)\s*;/.exec(machineTs);
  const tsSound = /export\s+const\s+SOUND_SLOT_COUNT\s*=\s*(\d+)\s*;/.exec(machineTs);
  return {
    table: table ? Number(table[1]) : null,
    sound: sound ? Number(sound[1]) : null,
    tsTable: tsTable ? Number(tsTable[1]) : null,
    tsSound: tsSound ? Number(tsSound[1]) : null,
    declaredWithConstant:
      /std::array<std::int32_t,\s*kSeSlotTableSize>\s+se_slot_resource_/.test(vmh),
  };
}

/** 纯函数：返回问题清单（守卫断言与判别力自检共用同一份判据）。 */
function problems({ shape, impl, bindings }) {
  const out = [];
  const engineTable = memberCount(shape?.types, 'T_SE', '0x4bc');
  if (engineTable === null) {
    out.push('shape 的 T_SE+0x4bc 不是条数有界的数组（count 缺失 ⇒ 表量无从判定）');
  } else if (impl.table !== engineTable) {
    out.push(`实现的引擎槽表量 ${impl.table} != shape 的 T_SE+0x4bc count ${engineTable}`);
  }
  for (const off of SOUND_ARRAYS) {
    const count = memberCount(shape?.types, 'T_Sound', off);
    if (count !== impl.sound) {
      out.push(`实现的后端槽量 ${impl.sound} != shape 的 T_Sound+${off} count ${count}`);
    }
  }
  if (engineTable !== null && impl.tsTable !== engineTable) {
    out.push(`emulator-new 的引擎槽表量 ${impl.tsTable} != shape 的 T_SE+0x4bc count ${engineTable}`);
  }
  if (impl.sound !== null && impl.tsSound !== impl.sound) {
    out.push(`emulator-new 的后端槽量 ${impl.tsSound} != C++ 侧的 ${impl.sound}`);
  }
  if (!impl.declaredWithConstant) {
    out.push('se_slot_resource_ 的数组长度没有用 kSeSlotTableSize 声明（数字又写死了一处）');
  }
  const rule = (bindings?.bindings ?? []).find((entry) => entry.rule === RULE);
  if (!rule) {
    out.push(`bindings 里没有 ${RULE}`);
  } else if (!(rule.storage ?? []).includes('shape:T_SE@0x4bc')) {
    out.push(`${RULE} 的 storage 没有指向 shape:T_SE@0x4bc（规则的"那张表"没有机器锚）`);
  }
  return out;
}

const shape = readJson('data/shape/types.json');
const bindings = readJson('data/spec/bindings.json');
const impl = implementation(
  read('apps/emulator-cxx/src/core/vm.h'),
  read('apps/emulator-new/src/core/machine.ts'),
);

test('音效槽量：shape ↔ 实现 ↔ spec 三处说同一件事', () => {
  assert.deepEqual(problems({ shape, impl, bindings }), []);
});

test('判别力：三处各自改坏一份副本，problems() 必须报红', () => {
  const broken = (mutate) => {
    const copy = {
      shape: structuredClone(shape),
      impl: { ...impl },
      bindings: structuredClone(bindings),
    };
    mutate(copy);
    return problems(copy);
  };

  // ① 实现把引擎表量写成后端的 15（历史上就是这个错）
  assert.notDeepEqual(
    broken((copy) => {
      copy.impl.table = impl.sound;
    }),
    [],
    '把引擎表量改成 15 必须报红',
  );

  // ② shape 里那张表不再是 count 10
  assert.notDeepEqual(
    broken((copy) => {
      const member = copy.shape.types.T_SE.members.find((m) => m.off === '0x4bc');
      member.count = 15;
    }),
    [],
    'shape 的 T_SE+0x4bc 改成 15 必须报红（实现的那 5 个格就没有依据了）',
  );

  // ③ 参考实现（emulator-new）把引擎表量写成 15
  assert.notDeepEqual(
    broken((copy) => {
      copy.impl.tsTable = copy.impl.sound;
    }),
    [],
    'emulator-new 把引擎表量改成 15 必须报红',
  );

  // ④ 表声明又写死一个数字（常量成了摆设）
  assert.notDeepEqual(
    broken((copy) => {
      copy.impl.declaredWithConstant = false;
    }),
    [],
    '表声明不用常量必须报红',
  );

  // ⑤ 规则的表指针被拿掉
  assert.notDeepEqual(
    broken((copy) => {
      const rule = copy.bindings.bindings.find((entry) => entry.rule === RULE);
      rule.storage = [];
    }),
    [],
    '规则的表指针被拿掉必须报红',
  );
});
