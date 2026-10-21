/**
 * opcode → 实现。★ 每条都对应 `data/spec/chapters/60-ops.md` 里的一条规则。
 * ★ 未实现的 opcode **不在表里**（不是 no-op）：`Machine.run` 会在它前面报缺口。
 * ★ 本文件不碰文件系统；外部请求只进 `m.effects`（规范不规定外部效果）。
 */
import type { DrawItem, ListRecord, Machine, Mesh, MovieObject, Operand, VoiceGroup } from './machine.ts';
import { FRAME_SLOT_COUNT, MOVIE_BIT, WAIT_BIT } from './machine.ts';
import { encodeInt, readAddress, readFloat, readInt, readIntAt, readText, writeBitsAt, writeFloat, writeInt, writeText } from './values.ts';
import { addressOfOperand, pointerSlotAddress, writeBitsToAddress, writePointerSlot } from './values.ts';
import { arrayStrideFor, resolveAddress } from './address.ts';
import type { PoolTag } from './address.ts';

export type { Machine, Operand };

/* ──────────────────────────── 空操作 ──────────────────────────── */

/** 0x1a7 / 0x1a8：只做分派协议写（由执行器完成），本身不改任何引擎数据（CORE-OPS-NOOP-*） */
function noop(): void {}

/* ──────────────────────────── 数值族 ──────────────────────────── */

/** 0x50 整数加：先读第 3 个、再读第 2 个，和写进第 1 个（CORE-OPS-NUMERIC-ADD） */
function add50(m: Machine, ops: Operand[]): void {
  const third = readInt(m, ops, 3);
  const second = readInt(m, ops, 2);
  writeInt(m, ops, 1, second + third);
}

/** 0x51 整数减：先读第 3 个、再读第 2 个，第 2 个减第 3 个写进第 1 个（CORE-OPS-NUMERIC-SUB） */
function sub51(m: Machine, ops: Operand[]): void {
  const third = readInt(m, ops, 3);
  const second = readInt(m, ops, 2);
  writeInt(m, ops, 1, second - third);
}

/** 0x2d5 浮点搬运：读第 2 个（浮点），写第 1 个（CORE-OPS-NUMERIC-FLOAT-MOV） */
function floatMov2d5(m: Machine, ops: Operand[]): void {
  const value = readFloat(m, ops, 2);
  writeFloat(m, ops, 1, value);
}

/* ──────────────────────────── 纹理族 ──────────────────────────── */

/** 引擎级每 id 对象槽：清空（纹理三条都先做这一步） */
function clearObjectSlot(m: Machine, id: number): void {
  m.scene.objectSlots.delete(id);
}

/** 0x1f7 拆离：读 op2=个数、op1=键；≤1 删单键，否则删 [键, 键+个数)（CORE-OPS-TEXTURE-DETACH） */
function detach1f7(m: Machine, ops: Operand[]): void {
  const count = readInt(m, ops, 2);
  const key = readInt(m, ops, 1);
  const tables: Map<number, unknown>[] = [m.scene.drawItems, ...m.scene.auxTables];
  let removed = 0;
  for (const table of tables) {
    if (count <= 1) {
      if (table.delete(key)) removed += 1;
    } else {
      // 左闭右开；Map 没有区间删除，按键枚举（与引擎的 lower_bound 等价）
      for (const k of [...table.keys()]) {
        if (k >= key && k < key + count) {
          table.delete(k);
          removed += 1;
        }
      }
    }
  }
  if (removed > 0) m.scene.dirty = 1;
}

/** 0x1f8 创建：读 1=id、4=种类、3=高、2=宽、1=id（CORE-OPS-TEXTURE-CREATE） */
function create1f8(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1);
  clearObjectSlot(m, id);
  const kind = readInt(m, ops, 4);
  const height = readInt(m, ops, 3);
  const width = readInt(m, ops, 2);
  readInt(m, ops, 1); // 建纹理时再读一次 id
  m.scene.textures.set(id, { file: -1, color: 0, blank: 1 });
  m.scene.textureObjects.set(id, { kind: kind === 3 ? 'divided' : 'normal', width, height });
  m.effects.push({ kind: 'texture.create.request', detail: { id, width, height, kind } });
  // 规范：底层创建失败只写错误缓冲、不抛；headless 没有后端 ⇒ 视为已受理
}

/** 0x1f9 设置：读 2=id、1=文件号、3=颜色键、2=id（CORE-OPS-TEXTURE-SET） */
function set1f9(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 2);
  clearObjectSlot(m, id);
  const fileId = readInt(m, ops, 1);
  const rawColor = readInt(m, ops, 3);
  const color = rawColor < 0 ? 0 : (0xff000000 | (rawColor & 0xffffff)) >>> 0;
  readInt(m, ops, 2); // 加载时再读一次 id
  // 外部：按文件号打开 / 解码 / 上传。headless 没有档案后端 ⇒ 视为已受理（成功路径）。
  m.effects.push({ kind: 'texture.load.request', detail: { id, fileId, color } });
  m.scene.textures.set(id, { file: fileId, color, blank: 0 });
  m.scene.textureObjects.set(id, { kind: 'normal', width: 0, height: 0 });
}

/** 0x1fa 释放：读 op1 两次（CORE-OPS-TEXTURE-RELEASE） */
function release1fa(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1);
  clearObjectSlot(m, id);
  readInt(m, ops, 1);
  // 释放守卫：规范说本构建里恒 0（全语料没有写点）⇒ 走释放路径
  const rec = m.scene.textures.get(id);
  m.scene.textures.set(id, { file: -1, color: rec?.color ?? 0, blank: rec?.blank ?? 0 });
  m.scene.textureObjects.delete(id);
}

/** 默认绘制项：规范只说"建一个默认项"，字段全 0 */
function defaultItem(): DrawItem {
  return {
    flags: 0, textureId: 0, left: 0, top: 0, right: 0, bottom: 0, aux: [0, 0, 0],
    start: 0, delay: 0, duration: 0, colorFrom: -1, colorTo: -1, unknown: new Map(),
  };
}

/** 0x1fb 绘制：读序 6,4,5,3,4,3,7,8,2,1（CORE-OPS-TEXTURE-DRAW） */
function draw1fb(m: Machine, ops: Operand[]): void {
  const bottomBase = readInt(m, ops, 6);
  const bottom = bottomBase + readInt(m, ops, 4);
  const rightBase = readInt(m, ops, 5);
  const right = rightBase + readInt(m, ops, 3);
  const top = readInt(m, ops, 4);
  const left = readInt(m, ops, 3);
  const fx = Math.fround(readInt(m, ops, 7));
  const fy = Math.fround(readInt(m, ops, 8));
  const textureId = readInt(m, ops, 2);
  const key = readInt(m, ops, 1);
  const item = m.scene.drawItems.get(key) ?? defaultItem();
  item.flags |= 1;
  item.textureId = textureId;
  item.left = left;
  item.top = top;
  item.right = right;
  item.bottom = bottom;
  item.aux = [fx, fy, 0];
  m.scene.drawItems.set(key, item);
  m.scene.dirty = 1;
}

/* ──────────────────────────── 场景 / 颜色族 ──────────────────────────── */

/** ARGB 颜色归一化：透明度 >255 ⇒ 255；<0 ⇒ 取现值的高字节；颜色 <0 ⇒ 取现值 */
function normalizeColor(m: Machine, current: number, alpha: number, color: number): number {
  let a = alpha;
  if (a > 255) a = 255;
  else if (a < 0) a = current >> 24;
  const c = color < 0 ? current : color;
  return (((c & 0xffffff) | (a << 24)) >>> 0);
}

function drawItemOf(m: Machine, id: number): DrawItem {
  const found = m.scene.drawItems.get(id);
  if (found) return found;
  const item = defaultItem();
  m.scene.drawItems.set(id, item);
  return item;
}

function defaultMesh(): Mesh {
  return {
    flags: 0, count: 0, op10: 0, copies: [[], []], colors: [], vbo: [],
    start: 0, delay: 0, duration: 0, colorFrom: -1, colorTo: -1, unknown: new Map(),
  };
}

function meshOf(m: Machine, id: number): Mesh {
  const found = m.scene.meshes.get(id);
  if (found) return found;
  const mesh = defaultMesh();
  m.scene.meshes.set(id, mesh);
  return mesh;
}

/** 0x202 绘制项：排颜色渐变窗（CORE-OPS-SCENE-202） */
function scene202(m: Machine, ops: Operand[]): void {
  const alpha = readInt(m, ops, 4);
  const color = readInt(m, ops, 5);
  let current = -1;
  if (alpha < 0 || color < 0) current = drawItemOf(m, readInt(m, ops, 1)).colorFrom;
  const value = normalizeColor(m, current, alpha, color);
  const id = readInt(m, ops, 1);
  const item = drawItemOf(m, id);
  const duration = readInt(m, ops, 3);
  const delay = readInt(m, ops, 2);
  readInt(m, ops, 1);
  if ((item.flags & 1) !== 0) {
    item.flags |= 2;
    item.start = 0;
    item.delay = delay;
    item.duration = duration;
    item.colorTo = value;
    m.scene.dirty = 1;
    m.scene.animating = 1;
  }
}

/** 0x203 绘制项：直接设当前颜色（CORE-OPS-SCENE-203） */
function scene203(m: Machine, ops: Operand[]): void {
  const alpha = readInt(m, ops, 3);
  const color = readInt(m, ops, 4);
  let current = -1;
  if (alpha < 0 || color < 0) current = drawItemOf(m, readInt(m, ops, 1)).colorFrom;
  const value = normalizeColor(m, current, alpha, color);
  const unknown = readInt(m, ops, 2);
  const item = drawItemOf(m, readInt(m, ops, 1));
  item.unknown.set(2, unknown);
  item.colorFrom = value;
  m.scene.dirty = 1;
}

/** 0x322 网格：直接设当前颜色并烘进顶点色（CORE-OPS-SCENE-322） */
function scene322(m: Machine, ops: Operand[]): void {
  const alpha = readInt(m, ops, 3);
  const color = readInt(m, ops, 4);
  let current = -1;
  if (alpha < 0 || color < 0) current = meshOf(m, readInt(m, ops, 1)).colorFrom;
  const value = normalizeColor(m, current, alpha, color);
  const unknown = readInt(m, ops, 2);
  const mesh = meshOf(m, readInt(m, ops, 1));
  mesh.unknown.set(2, unknown);
  mesh.colorFrom = value;
  mesh.colors = mesh.colors.map(() => value >>> 0);
  m.scene.dirty = 1;
}

/** 0x323 网格：排颜色渐变窗（无"已存在"门）（CORE-OPS-SCENE-323） */
function scene323(m: Machine, ops: Operand[]): void {
  const alpha = readInt(m, ops, 4);
  const color = readInt(m, ops, 5);
  let current = -1;
  if (alpha < 0 || color < 0) current = meshOf(m, readInt(m, ops, 1)).colorFrom;
  const value = normalizeColor(m, current, alpha, color);
  const duration = readInt(m, ops, 3);
  const delay = readInt(m, ops, 2);
  const mesh = meshOf(m, readInt(m, ops, 1));
  mesh.flags |= 2;
  mesh.start = 0;
  mesh.delay = delay;
  mesh.duration = duration;
  mesh.colorTo = value;
  m.scene.dirty = 1;
}

/** 0x320 网格：建/换三个缓冲（CORE-OPS-SCENE-320） */
function scene320(m: Machine, ops: Operand[]): void {
  const count = readInt(m, ops, 9);
  if (count <= 0) {
    m.scene.errorBuffer = `頂点数${count}は不正です．`;
    return;
  }
  const a5 = readAddress(m, ops, 5);
  const a6 = readAddress(m, ops, 6);
  const a7 = readAddress(m, ops, 7);
  const a8 = readAddress(m, ops, 8);
  // 第三段：逐元素合成（解码后的低 8 位 << 24 | 解码后的低 24 位）
  const colors: number[] = [];
  for (let i = 0; i < count; i += 1) {
    const hi = readIntAt(m, a5, i);
    const lo = readIntAt(m, a6, i);
    colors.push(((((hi & 0xff) << 24) | (lo & 0xffffff)) >>> 0));
  }
  const op10 = readInt(m, ops, 10);
  const a4 = readAddress(m, ops, 4);
  const a3 = readAddress(m, ops, 3);
  const a2 = readAddress(m, ops, 2);
  const id = readInt(m, ops, 1);
  // 前两段：逐字原样拷贝
  const copies: [number[], number[]] = [[], []];
  for (let i = 0; i < count; i += 1) {
    copies[0].push(readIntAt(m, a2, i));
    copies[1].push(readIntAt(m, a3, i));
  }
  const mesh = meshOf(m, id);
  mesh.flags |= 1;
  mesh.count = count;
  mesh.op10 = op10;
  mesh.copies = copies;
  mesh.colors = colors;
  mesh.vbo = [a4, a7, a8];
  if (mesh.colorFrom !== -1) mesh.colors = colors.map(() => mesh.colorFrom >>> 0);
  m.scene.dirty = 1;
}

/* ──────────────────────────── 帧与调用 ──────────────────────────── */

/**
 * 0x8f 同脚本调用（CORE-FRAME-CALL）。
 *
 * 逐字（`sub_420560`）：压返回点 `(PC − 基址) >> 2 + 3` 进本帧返回栈、SP+1；读第 1 个操作数：
 * 等于 −1 ⇒ SP−1（撤销压栈）、**不跳**（长度槽仍是 3 ⇒ 分派器照常推进）；
 * 否则 `PC ← 基址 + 4 × 操作数`、长度槽 ← 0（分派器不再推进）。
 * ★ `+3` 就是"这条指令之后的那条"的位置（`1 + 2×argc`，`0x8f` 的 argc 恒 1）。
 */
function call8f(m: Machine, ops: Operand[]): void {
  const target = readInt(m, ops, 1);
  if (target === -1) return; // 哨兵：不压、不跳、照常推进
  m.frame.retStack.push(m.ip / 4 + 1 + 2 * ops.length);
  m.ip = 4 * target;
  m.operandCount = 0;
}

/**
 * 0x5 同脚本返回（CORE-FRAME-RET）。
 *
 * 逐字（`sub_41A9B0`）：长度槽 ← 1、本帧返回栈 SP−1、弹出位置；
 * 位置 = −1 ⇒ **不跳**（长度槽留 1 ⇒ 分派器只前进 4 字节）；
 * 否则 `PC ← 基址 + 4 × 位置`、长度槽 ← 0；随后它还会从**另一个引擎级栈**弹一个值写进一个引擎级槽
 * （用途未定）—— 本层不建模，只记一条诊断（⛔ 不猜）。
 * ★ 空栈时的引擎后续没有观察 ⇒ 本实现**响亮失败**，不静默退出。
 */
function ret05(m: Machine): void {
  const pos = m.frame.retStack.pop();
  if (pos === undefined) throw new Error('ret-with-empty-stack');
  if (pos === -1) return; // 哨兵：不跳，长度槽留 1 ⇒ 前进 4 字节
  m.ip = 4 * pos;
  m.operandCount = 0;
  m.diagnostics.push({ kind: 'engine-return-pop-unmodeled' });
}

/* ---------------------------- 引擎级标量（薄写） ---------------------------- */

/** 状态选择字的第 27 位（0x88 的「为 0」支清它；置它的是另一条指令） */
const STATE_BIT_27 = 0x08000000;

/** 0x149：把第 1 个操作数原样写进一个固定格（CORE-OPS-SCALAR-THIN） */
function thin149(m: Machine, ops: Operand[]): void {
  m.scalars.set('Engine.d97058', readInt(m, ops, 1));
}

/** 0x21b：同族，但把值**归一成 0 / 1**（0 ⇒ 0，任何非 0 ⇒ 1） */
function thin21b(m: Machine, ops: Operand[]): void {
  m.scalars.set('Engine.d166965', readInt(m, ops, 1) !== 0 ? 1 : 0);
}

/** 0x252：同族，另一个固定格 */
function thin252(m: Machine, ops: Operand[]): void {
  m.scalars.set('Engine.d92323', readInt(m, ops, 1));
}

/**
 * 0x88 成对写（CORE-OPS-SCALAR-PAIR-88）：同一个值写两个格；非 0 ⇒ 第三格写常量 1；
 * 为 0 ⇒ 清状态选择字的第 27 位（读-改-写，其它位不动）。
 */
function pair88(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 1);
  m.scalars.set('Engine.d1415', value);
  m.scalars.set('Engine.d97050', value);
  if (value !== 0) {
    m.scalars.set('Engine.d122368', 1);
  } else {
    m.flags &= ~STATE_BIT_27;
  }
}

/* ---------------------------- 转发型指令 ---------------------------- */

/**
 * 0x1ca：把操作数交给**消息子系统**的「读文本跳过」入口（CORE-OPS-FORWARD-MESSAGE）。
 * 逐字：`(*((vtable+12)))(&Engine+0xAA514, "message:ReadTextSkip", op1)` —— 返回值交分派器（被丢弃）。
 * ★ 子系统的内部状态没有任何观察 ⇒ 本层只记请求 + 一条"未建模"标记；
 *   ⛔ 不假造它改了什么（真有人读那块状态时会**响亮失败**，不会静默读到 0）。
 */
function message1ca(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 1);
  m.effects.push({ kind: 'message.request', detail: { entry: 'ReadTextSkip', value } });
  m.diagnostics.push({ kind: 'message-subsystem-unmodeled' });
}

/**
 * 0x324：**没有操作数**。它把引擎里某个字段存着的对象交给一个入口（CORE-OPS-FORWARD-FIELD）。
 * 逐字：`mov ecx,[ecx+5B320h]` + `jmp sub_453530`（尾跳；那个 thunk 自己又是一条 `jmp`）。
 * ★ 入口之后做什么没有观察 ⇒ 只记请求 + 一条"未建模"标记。
 */
function field324(m: Machine): void {
  m.effects.push({ kind: 'engine.request', detail: { callee: 'sub_453530', holder: 'Engine.d93384' } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_453530' });
}

/**
 * 0x32f：把一个索交给容器入口（CORE-OPS-FORWARD-CONTAINER）。
 * 逐字：`sub_49A150(Engine+0x4ED10, op1)` —— callee 里**已知的数据效果**是按 op1 清一格
 * （`[ecx+edx*4+0D5B4h] = 0`，绝对落点 = `Engine+0x5C2C4` 起）；随后还会走一次 `vtable+0xD4`
 * 通知（内部行为未建模）。
 */
function container32f(m: Machine, ops: Operand[]): void {
  const slot = readInt(m, ops, 1);
  m.scalars.set(`Engine.d94385+${slot}`, 0);
  m.effects.push({ kind: 'container.request', detail: { callee: 'sub_49A150', holder: 'Engine.d95428', slot } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_49A150' });
}

/**
 * 0x70 设置一个列表项（CORE-OPS-LIST-SET）。
 *
 * 逐字（`sub_41ED20` → `sub_45D660(this = Engine+0x14D30, a2..a6, 0)`）：
 * 读序是 **op5 → op4 → op3 → op2 → op1**（倒序），另有一个写死的第 6 实参 0；组号为 0 时换成
 * 引擎里的默认组（默认组是谁未定）。写进该组记录的四对字段：
 * 三对 ← (op2, op3)，一对 ← (op4, op5)；并把该组标记为"已登记"。
 * 之后的列表登记与绘制请求（`sub_45D240` / `sub_43C8D0` / `sub_43B070`，以及浮点路径）**未建模**。
 */
function listSet70(m: Machine, ops: Operand[]): void {
  const slot = readInt(m, ops, 1);
  if (slot === 0) {
    // 引擎：组号 0 ⇒ 换成引擎里的默认组（哪个组未定）⇒ 本层不猜，留诊断
    m.diagnostics.push({ kind: 'ui-default-slot-unmodeled' });
    return;
  }
  const a3 = readInt(m, ops, 2);
  const a4 = readInt(m, ops, 3);
  const a5 = readInt(m, ops, 4);
  const a6 = readInt(m, ops, 5);
  const rec = listRecordOf(m, slot);
  rec.f0c = a5;
  rec.f10 = a6;
  rec.f14 = a3;
  rec.f18 = a4;
  rec.f24 = a3;
  rec.f28 = a4;
  rec.f7c = a3;
  rec.f80 = a4;
  rec.registered = 1;
  m.effects.push({ kind: 'ui.list.request', detail: { slot, a3, a4, a5, a6 } });
  m.diagnostics.push({ kind: 'ui-list-internals-unmodeled' });
}

/** 按组号取记录（缺则建，字段全 0：引擎构造时把这张表清过） */
function listRecordOf(m: Machine, slot: number): ListRecord {
  const found = m.ui.records.get(slot);
  if (found) return found;
  const created: ListRecord = {
    f0c: 0, f10: 0, f14: 0, f18: 0, f1c: 0, f20: 0, f24: 0, f28: 0, f64: 0, f68: 0, f6c: 0, f114: 0, f118: 0, f120: 0, f124: 0, f7c: 0, f80: 0, registered: 0,
  };
  m.ui.records.set(slot, created);
  return created;
}

/**
 * 0x71 文本转发（CORE-OPS-TEXT-REQUEST）。
 *
 * 逐字（`sub_41ED80`，argc 1）：
 * ① 把本条指令的**位置**（`(PC − 代码区基址) >> 2`）写进当前帧的一个字段（`帧+0x6C`，用途未定）；
 * ② `sub_45EC60(Engine+0x14D30, op1, Engine[97055])` —— 第 3 个实参是**引擎槽**，不是操作数；
 * ③ 两处整块拷贝（`0x4B0` 字节 / `20000` 字节）、一次 `sub_48FFB0`、一次 `vtable+4` 调用
 *    （入口键 `message:ReadTextSkip`），以及一次**有条件**的状态位写 —— 这些**都不在本版范围**。
 * ★ 状态位那一条尤其不许"顺手置上"：条件未取证（见 `REQ-01M5CWFZV…`），置错会让门立刻关上。
 */
function text71(m: Machine, ops: Operand[]): void {
  const slot = readInt(m, ops, 1);
  m.frame.position = m.ip / 4;
  m.effects.push({
    kind: 'text.request',
    detail: { slot, engineSlot: 'Engine.d97055', engineSlotModeled: false },
  });
  m.diagnostics.push({ kind: 'text-subsystem-unmodeled' });
}

/**
 * 0x73 子系统转发（CORE-OPS-SUBSYSTEM-SET73，argc 10 —— 前段最长的一条）。
 *
 * 逐字（`sub_41F250`）：
 * ① **先读第 10 个操作数**，写进一个固定格：值为 0 时写成 **1**（`sub_453AD0` 的 `a2 ?: 1`）；
 * ② 再**倒序**读第 9…1 个操作数，交给子系统（`sub_456430(Engine+0x14D30, op1..op9)`）；
 * ③ 由引擎级 16 位槽 `Engine[166964]` 决定是否再发一次 `vtable+4` 请求（实参 0x21 / 2×op7 / op8 / 2）。
 *    ★ 那个槽的当前值未建模（本层没人写它）⇒ 请求照发、但记 `gateModeled: false`，不假装知道门的状态。
 */
function forward73(m: Machine, ops: Operand[]): void {
  const op10 = readInt(m, ops, 10);
  m.scalars.set('Engine.d107528', op10 === 0 ? 1 : op10);
  const args: number[] = [];
  for (const k of [9, 8, 7, 6, 5, 4, 3, 2, 1]) args.push(readInt(m, ops, k));
  args.reverse();
  m.effects.push({ kind: 'subsystem.request', detail: { callee: 'sub_456430', args } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_456430' });
  const gate = m.scalars.get('Engine.d166964') ?? 0;
  if (gate === 0) {
    m.effects.push({
      kind: 'vtable.request',
      detail: { callee: 'vtable+4', message: 0x21, a: 2 * args[6], b: args[7], c: 2, gateSlot: 'Engine.d166964', gateModeled: false },
    });
  }
}

/**
 * 0x78 / 0x2db：把第 1 个操作数写进**子对象的一个固定字段**，然后**尾跳**进子系统
 * （CORE-OPS-SUBSYSTEM-SET-TAIL）。
 *
 * 逐字：两条同形 —— `0x78` 写 `Engine+0x14D30+0x55C`；`0x2db` 写同一个子对象的 `+0x313D0`；
 * 然后 `jmp sub_459F40`（尾跳：子系统的返回值直接回给分派器）。
 * ★ 字段的领域用途未定；子系统内部行为未建模（只记请求）。
 */
function setFieldTail(m: Machine, ops: Operand[], field: string): void {
  m.scalars.set(field, readInt(m, ops, 1));
  m.effects.push({ kind: 'subsystem.request', detail: { callee: 'sub_459F40', field } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_459F40' });
}

/** 0x78：写子对象 +0x55C 后尾跳 */
function field78(m: Machine, ops: Operand[]): void {
  setFieldTail(m, ops, 'Engine.d21683');
}

/** 0x2db：写同一子对象 +0x313D0 后尾跳（与 0x78 同形，只差落点） */
function field2db(m: Machine, ops: Operand[]): void {
  setFieldTail(m, ops, 'Engine.d71760');
}

/**
 * 0x79 / 0x1c1：把两个操作数写进**列表项记录**的一对字段（CORE-OPS-LIST-PAIR）。
 *
 * 逐字：两条同形，argc 3 —— 读序 **op3 → op2 → op1**；op1 是组号（0 ⇒ 换成引擎里的默认组），
 * 记录 = `[子对象 + 4×组号 + 0x414]`；`0x79` 写 `+0x1C ← op2` / `+0x20 ← op3`；
 * `0x1c1` 写 `+0x24 ← op2` / `+0x28 ← op3`。两条都不产生脚本可观察的返回值。
 */
function listPair(m: Machine, ops: Operand[], first: 'f1c' | 'f24'): void {
  const third = readInt(m, ops, 3);
  const second = readInt(m, ops, 2);
  const slot = readInt(m, ops, 1);
  if (slot === 0) {
    m.diagnostics.push({ kind: 'ui-default-slot-unmodeled' });
    return;
  }
  const rec = listRecordOf(m, slot);
  if (first === 'f1c') {
    rec.f1c = second;
    rec.f20 = third;
  } else {
    rec.f24 = second;
    rec.f28 = third;
  }
}

/** 0x79：写 +0x1C / +0x20（转发 sub_4563A0） */
function pair79(m: Machine, ops: Operand[]): void {
  listPair(m, ops, 'f1c');
}

/** 0x1c1：写 +0x24 / +0x28（转发 sub_4563D0，与 0x79 同形、只差落点） */
function pair1c1(m: Machine, ops: Operand[]): void {
  listPair(m, ops, 'f24');
}

/**
 * 0x212 / 0x213 / 0x25d：写**已存在的**列表项记录的字段（CORE-OPS-LIST-FIELD）。
 *
 * 逐字：三条都按第 1 个操作数索引记录表（`[Engine + 4×组号 + 0x15144]`），
 * 记录为空 ⇒ **什么都不做（不创建记录）**；
 * `0x212`（argc 2）写 `+0x64 ← op2`；`0x213`（argc 3）写 `+0x68 ← op2` / `+0x6C ← op3`；
 * `0x25d`（argc 3，读序 op3→op2→op1）写 `+0x114 ← op2` / `+0x118 ← op3`。
 */
function listField212(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 2);
  const slot = readInt(m, ops, 1);
  const rec = m.ui.records.get(slot);
  if (!rec) return;
  rec.f64 = value;
}

function listPair213(m: Machine, ops: Operand[]): void {
  const third = readInt(m, ops, 3);
  const second = readInt(m, ops, 2);
  const rec = m.ui.records.get(readInt(m, ops, 1));
  if (!rec) return;
  rec.f68 = second;
  rec.f6c = third;
}

function listPair25d(m: Machine, ops: Operand[]): void {
  const third = readInt(m, ops, 3);
  const second = readInt(m, ops, 2);
  const rec = m.ui.records.get(readInt(m, ops, 1));
  if (!rec) return;
  rec.f114 = second;
  rec.f118 = third;
}

/**
 * 0x303：把一对值写进**已存在的**列表项记录（CORE-OPS-LIST-PAIR2，argc 3）。
 * 逐字：读序 op3 → op2 → op1；记录 = [子对象 + 4×组号 + 0x414]；写 +0x120 ← op2、+0x124 ← op3。
 * ★ 与 0x212/0x213/0x25d **不同**：这一条**不检查记录是否为空**（记录为空时引擎会解引用空指针）
 *   ⇒ 本层对缺失记录响亮失败，不静默跳过。
 */
function pair303(m: Machine, ops: Operand[]): void {
  const third = readInt(m, ops, 3);
  const second = readInt(m, ops, 2);
  const slot = readInt(m, ops, 1);
  if (slot === 0) {
    m.diagnostics.push({ kind: 'ui-default-slot-unmodeled' });
    return;
  }
  const rec = m.ui.records.get(slot);
  if (!rec) throw new Error(`list-record-missing:${slot}`);
  rec.f120 = second;
  rec.f124 = third;
}

/**
 * 0xa0 条件跳转（CORE-FRAME-JUMP-A0）。
 *
 * 逐字（sub_4209B0，argc 3）：读 op1 作条件；非 0 ⇒ 读 op2 作目标，等于 0 ⇒ 读 op3 作目标；
 * 目标为 −1（全 1 位模式）⇒ 不跳（长度槽留着 ⇒ 分派器照常推进）；否则
 * PC ← 代码区起点 + 4 × 目标、长度槽清 0（不自动推进）。
 * ★ 只读"该走的那一支"的那个操作数 —— 另一支的操作数不被读。
 */
function jumpA0(m: Machine, ops: Operand[]): void {
  const condition = readInt(m, ops, 1);
  const target = condition !== 0 ? readInt(m, ops, 2) : readInt(m, ops, 3);
  if (target === -1) return;
  m.ip = 4 * target;
  m.operandCount = 0;
}

/**
 * 0x3 换脚本（CORE-FRAME-CALL-SCRIPT）：操作数是**统一文件 id**；在「当前帧号 + 1」那个槽上建子帧
 * 并切过去，从子脚本 dword 0 开始；子帧的局部池**新分配**（不继承父池）。
 * ★ 深度按**槽号**判（`cur + 1 >= 40` 才超限）；装载失败 ⇒ 整个执行中止（本层响亮失败）。
 */
function callScript3(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1);
  if (!m.loadScript) throw new Error('call-script-loader-missing');
  const child = m.loadScript(id);
  if (!child) throw new Error('call-script-load-failed:' + id);
  m.enterScript(child, id);
}

/**
 * 0x2 结束当前脚本（CORE-FRAME-EXIT）：有父帧 ⇒ 回到**父帧号**那一帧；根帧 ⇒ 整个执行结束。
 * ★ 它**不写 ip**：父帧 ip 仍停在调用点，靠父帧自己的长度槽推进到"下一条"（引擎的算术结果）。
 * ★ 释放是**有条件**的（只有「父号 + 1 == 当前号」才清槽）：`0x8 换帧` 切出来的返回可以跨槽。
 */
function exit2(m: Machine): void {
  if (!m.returnToCaller()) m.halted = true;
}

/**
 * 0x6 装载帧（CORE-OPS-FRAME-LOAD-FRAME）：操作数 1 = 统一文件 id、操作数 2 = 目标帧号。
 * 把脚本装进**目标槽**，**当前帧号一动都不动**、也不开始执行它；目标槽原有帧被替换。
 * ★ 帧深上限按**帧号**判（≥ 40 ⇒ 报错，文本照抄引擎那句日文）。
 * ★ 装载失败 ⇒ 当前帧号已恢复，整个执行中止（本层响亮失败）。
 */
function loadFrame6(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1);
  const slot = readInt(m, ops, 2);
  if (!m.loadScript) throw new Error('load-frame-loader-missing');
  // ★ 引擎的装载前置里有一条"解码密钥可用"：本层的 key 是宿主给的显式参数（`--script` 调试口可以给 0）
  //   ⇒ 给 0 时记一笔、继续走（⛔ 照抄成抛会让调试口停在"我的占位值不合法"上 —— 那是模型的问题，不是脚本的问题）
  if (m.key === 0) m.diagnostics.push({ kind: 'codec-key-placeholder:load-frame' });
  if (slot >= FRAME_SLOT_COUNT) {
    throw new Error(`load-frame：目标帧深 ${slot} ≥ ${FRAME_SLOT_COUNT} —— 引擎抛「ファイルの階層が深すぎます．最大は%dです．」`);
  }
  const body = m.loadScript(id);
  if (!body) throw new Error('load-frame-load-failed:' + id);
  m.loadFrameAt(slot, body, id);
}

/**
 * 0x8 换帧（CORE-OPS-FRAME-CALL-FRAME）：操作数 = 目标帧号（必须是**已装载**的那一槽）。
 * 切当前帧过去、从那一帧的开头执行；父帧号 ← 调用者。目标帧未装载 ⇒ 报错（文本照抄引擎）。
 */
function callFrame8(m: Machine, ops: Operand[]): void {
  const slot = readInt(m, ops, 1);
  m.callFrameAt(slot);
}

/**
 * 0x9 全局重置并重装根脚本（CORE-FRAME-EXIT-SCRIPT）：丢全部帧、以统一文件 id 0 重装进 0 号帧。
 * ★ 它**不是返回**：调用者永远不会拿回控制权（本层用"重置后从根脚本 dword 0 继续"表达）。
 */
function exitScript9(m: Machine): void {
  if (!m.loadScript) throw new Error('call-script-loader-missing');
  const root = m.loadScript(0);
  if (!root) throw new Error('call-script-load-failed:0');
  m.slots = [{ body: root, ip: 0, frame: m.freshFrame(), parent: -1, length: 0 }];
  m.cur = 0;
  m.operandCount = 0;
  // 复位：状态选择字归 0；三个全局池清回初值（整型池 decode 后为 0、浮点池 0、字符串池空）
  m.flags = 0;
  m.globalInt.clear();
  m.globalFloat.clear();
  m.globalString.clear();
}
/**
 * 0x55 整数搬运（CORE-OPS-NUMERIC-MOV-INT）：把第 2 个操作数按整型读出，按整型写进第 1 个。
 * 逐字：`sub_42C720` —— 先读 op2（`sub_41BF50`），再 `sub_42B4B0(this, 1, 值)` 写回 op1；第 1 个只写不读。
 */
function move55(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 2);
  writeInt(m, ops, 1, value);
}
/* ---------------------------- Voice 族 ---------------------------- */

/** 组号 → 声音通道号的固定偏移（`0x2f6` 的机器细节，见绑定） */
const VOICE_CHANNEL_BASE = 12;
/** 组数：引擎里是 3 格、步长 4 的数组（组号越界会踩相邻数组 ⇒ 本层对越界响亮失败） */
const VOICE_GROUP_COUNT = 3;
/** 声音后端的通道数（0..14） */
const VOICE_BUFFER_COUNT = 15;

/** 按组号取记录；缺则建（字段全 0，与引擎构造函数一致） */
function voiceGroupOf(m: Machine, group: number): VoiceGroup {
  const found = m.voice.groups.get(group);
  if (found) return found;
  const created: VoiceGroup = { subsystem: [0, 0, 0, 0], engine: [0, 0, 0, 0] };
  m.voice.groups.set(group, created);
  return created;
}

/**
 * 0x2f8 设置一个语音组的音量（CORE-OPS-VOICE-VOLUME）。
 *
 * 逐字（`sub_4268D0` → `sub_4B6940`）：**先读第 2 个操作数（音量）、再读第 1 个（组号）**；
 * 通道号 = 组号 + 固定偏移；通道号 ≥ 15 ⇒ 写错误文本 + 记日志、**不改任何状态**；
 * 否则音量**夹到 [-10000, 10000]** 后写进该通道的音量格，并向声音后端发出设置请求。
 */
function setVolume2f8(m: Machine, ops: Operand[]): void {
  const volume = readInt(m, ops, 2);
  const group = readInt(m, ops, 1);
  const channel = group + VOICE_CHANNEL_BASE;
  if (channel >= VOICE_BUFFER_COUNT) {
    // 引擎：越界只写错误文本 + 打日志，**不动音量格**（`sub_4B6940` 的 else 支）
    m.sound.errorBuffer = '関数：dsSetVolume エラー：不正なSound番号です';
    return;
  }
  const clamped = volume < -10000 ? -10000 : volume > 10000 ? 10000 : volume;
  // 音量格**先写**（`this[a2+375] = v5` 在调用后端之前）⇒ 与后端在不在无关
  m.sound.volumes.set(channel, clamped);
  if (m.sound.backend === 'present') {
    m.effects.push({ kind: 'audio.volume.request', detail: { channel, volume: clamped } });
  }
}

/**
 * 0x2f6 停掉一个语音组，并重算「是否还有语音在播」（CORE-OPS-VOICE-STOP）。
 *
 * 逐字次序（`sub_426820` / `sub_4BB9F0` / `sub_404CB0`）：
 * ① 读组号：引擎拿它索引**步长 4、3 格**的数组，且**不检查上界** ⇒ 越界会踩相邻数组（未定义）；
 *    本层对 0..2 之外响亮失败（⛔ 不静默收下）。
 * ② 停止该组：释放「组号 + 固定偏移」那条声音通道上的缓冲对象（没有对象 ⇒ 什么都不发），
 *    再清语音对象里的 4 格 —— ★ 这两步都只在**声音后端存在**时发生（`Voice+0x408` 那道门）。
 * ③ 清引擎级的 4 格（无条件：与后端在不在无关）。
 * ④ 重算「是否还有语音在播」并**无条件**写进那个共享标志：逐组看它第 4 格的**低 2 位**，
 *    等于 3 的组跳过；其余组问「这条声音通道还在播吗」，第一个在播 ⇒ 1；三组都不在播 ⇒ 0。
 *    后端不存在 ⇒ 一次都不问、直接 0。
 */
function stopVoice2f6(m: Machine, ops: Operand[]): void {
  const group = readInt(m, ops, 1);
  if (group < 0 || group >= VOICE_GROUP_COUNT) {
    throw new Error(`voice-group-out-of-domain:${group}`);
  }
  const channel = group + VOICE_CHANNEL_BASE;
  const backend = m.sound.backend === 'present';
  // ② 停止 + 清语音对象里的 4 格（门 = 后端存在；释放请求另有一道门 = 该通道有对象）
  if (backend) {
    if (m.sound.buffers.delete(channel)) {
      m.effects.push({ kind: 'audio.release.request', detail: { channel } });
    }
    voiceGroupOf(m, group).subsystem = [0, 0, 0, 0];
  }
  // ③
  voiceGroupOf(m, group).engine = [0, 0, 0, 0];
  // ④
  let stillPlaying = 0;
  if (backend) {
    for (let index = 0; index < VOICE_GROUP_COUNT; index += 1) {
      if ((voiceGroupOf(m, index).subsystem[3] & 3) === 3) continue;
      const probe = index + VOICE_CHANNEL_BASE;
      m.effects.push({ kind: 'audio.playing.query', detail: { channel: probe } });
      if ((m.sound.buffers.get(probe)?.playing ?? 0) !== 0) {
        stillPlaying = 1;
        break;
      }
    }
  }
  m.scalars.set('voice.stillPlaying', stillPlaying);
}

/* ──────────────────────────── 等待 / 输入 ──────────────────────────── */

/** 0x21c wait：置等待位（阻塞在门里，不在 handler 内）（CORE-OPS-WAIT） */
function wait21c(m: Machine): void {
  m.flags |= WAIT_BIT;
}

/** 0x101 poll-input：采样并丢弃（CORE-OPS-POLL-INPUT） */
function pollInput101(m: Machine): void {
  m.effects.push({ kind: 'input.poll', detail: {} });
  m.flags &= ~0x08000000;
  m.inputWord = 0;
  m.scalars.set('pollInput.a', 1);
  m.scalars.set('pollInput.b', 0);
}

/** 0x20f 播影片：建/配置槽上的影片对象、置影片位与"有活动对象"（CORE-OPS-MOVIE） */
function movie20f(m: Machine, ops: Operand[]): void {
  const slot = readInt(m, ops, 2);
  let obj = m.scene.objectSlots.get(slot) as MovieObject | undefined;
  if (!obj || obj.kind !== 'movie') {
    obj = { kind: 'movie', slot, resourceId: 0, opt: 0, started: false };
    m.scene.objectSlots.set(slot, obj);
  }
  obj.resourceId = readInt(m, ops, 1);
  // 外部：按资源 id 取名、打开影片（headless 视为已受理，只记请求）
  m.effects.push({ kind: 'audio.movie.play', detail: { slot, resourceId: obj.resourceId } });
  readInt(m, ops, 1); // 绑定步骤再读一次资源 id
  if (!m.scene.textureObjects.has(slot)) {
    // 引擎：该槽没有纹理绑定 ⇒ 抛"显示消息"异常（本层用响亮失败表示）
    throw new Error(`movie-binding-missing:${slot}`);
  }
  obj.opt = readInt(m, ops, 3);
  obj.started = true;
  m.flags |= MOVIE_BIT;
  m.scene.hasObjects = 1;
}

/* ──────────────────────────── 配置访问（55-config.md） ──────────────────────────── */

/** 配置键 = 类型码 + 8 位十六进制下标；本层只支持全局值族（type 3 / 5） */
/**
 * 配置键：`类型码 + 8 位十六进制下标`（`55-config.md` 的「脚本访问」）。
 * ★ 下标怎么来分两类：
 *   - 值族（3 / 5）⇒ 操作数自带的池下标；
 *   - 引用 / 指针族（6 / 12 走整型族；8 / 14 走字符串族）⇒ **取槽里的地址**，
 *     减去**该族全局池起点**再除以元素宽度（4 / 28）。键里没有池标识 ⇒
 *     "全局值池第 i 槽"与"指向它的指针"生成同一个键（这是规范明写的可观察耦合）。
 */
function configKey(m: Machine, op: Operand, code: 3 | 5): string {
  if (op.type === code) return String.fromCharCode(code) + op.raw.toString(16).padStart(8, '0');
  const family: number[] = code === 3 ? [6, 12] : [8, 14];
  if (!family.includes(op.type)) {
    throw new Error(`config-key-unsupported-type:${code}:0x${op.type.toString(16)}`);
  }
  const addr = pointerSlotAddress(m, op.type as PoolTag, op.raw);
  // ★ 只有"地址落在该族**全局**池内"时键下标才有定义（= 该全局池的槽下标）。
  //   其它情形（局部池、浮点池、窗口外）在规范里是 **UB**（55-config 的「脚本访问」）：
  //   引擎只做 (地址 − 全局池起点)/元素宽、没有范围检查 ⇒ 那里得到的是"取决于分配布局的数"。
  //   本层对 UB 一律**响亮失败**（⛔ 不拿它去命中一个有定义的键）。
  const wantTag: PoolTag = code === 3 ? 3 : 5;
  const r = resolveAddress(addr);
  if (r.tag !== wantTag) {
    throw new Error(
      `config-key：引用地址 0x${addr.toString(16)} 落在池 0x${r.tag.toString(16)}，` +
      `不是该族全局池 0x${wantTag.toString(16)} ⇒ 未定义行为（55-config 的「脚本访问」）`,
    );
  }
  return String.fromCharCode(code) + r.index.toString(16).padStart(8, '0');
}

function requireConfig(m: Machine) {
  if (!m.config) throw new Error('config-not-initialized');
  return m.config;
}

/** 0x1a3 load-int：查整数键；未命中 ⇒ 0；**无条件**写回第 1 个操作数 */
function loadInt1a3(m: Machine, ops: Operand[]): void {
  const key = configKey(m, operandAt(ops, 1), 3);
  writeInt(m, ops, 1, requireConfig(m).getInt(key));
}

/** 0x1a2 save-int：把第 1 个操作数的整数值写进整数键 */
function saveInt1a2(m: Machine, ops: Operand[]): void {
  const key = configKey(m, operandAt(ops, 1), 3);
  requireConfig(m).setInt(key, readInt(m, ops, 1));
}

/** 0x1aa load-string：查字符串键；未命中 ⇒ 空串；写回第 1 个操作数 */
function loadString1aa(m: Machine, ops: Operand[]): void {
  const key = configKey(m, operandAt(ops, 1), 5);
  writeText(m, ops, 1, requireConfig(m).getString(key));
}

/** 0x1a9 save-string：把第 1 个操作数的文本写进字符串键 */
function saveString1a9(m: Machine, ops: Operand[]): void {
  const key = configKey(m, operandAt(ops, 1), 5);
  requireConfig(m).setString(key, readText(m, ops, 1));
}

/**
 * 0x192 set-string（CORE-OPS-STRING-SET）：把**第 2 个操作数**的文本写进**第 1 个操作数**的串池格。
 * ★ 源侧既可以是串池（type 5）、也可以是**内联字符串**（type 2，偏移见 `values.ts` 的 `inlineStringAt`）；
 *   落点按 op1 的 type 分派（本层只建模 type 5 全局串池；其余响亮失败）。
 * ★ 它是启动链里出现最多的一条（取证见台账 `01M4FECQ…` 的同类记录：`sub_433660`，argc 2）。
 */
function setString192(m: Machine, ops: Operand[]): void {
  const text = readText(m, ops, 2);
  writeText(m, ops, 1, text);
  m.effects.push({
    kind: 'string.set',
    detail: { opcode: '0x192', text, target: ops[0].raw, targetType: ops[0].type, sourceType: ops[1].type },
  });
}

/**
 * 0x6c fill-zero（数组填充族）：从 op1 的**地址**起，把 op2 个 dword 填成 **`ENC(key, 0)`**。
 * ★ 助记名骗人：填的**不是字面 0** —— 整型族里存的是编码后的位模式，DEC 之后才是 0；
 *   浮点族里存的是那个 dword 的**裸 32 位**（读的时候按 float32 解释）。
 * ★ op2 ≤ 0 ⇒ 一格都不写（引擎的循环不进入）。
 */
function fillZero6c(m: Machine, ops: Operand[]): void {
  const addr = readAddress(m, ops, 1);
  const count = readInt(m, ops, 2);
  const bits = encodeInt(0, m.key) >>> 0;
  if (count > 0) for (let i = 0; i < count; i += 1) writeBitsAt(m, addr, i, bits);
  m.effects.push({
    kind: 'array.fill',
    detail: { opcode: '0x6c', pool: addr.pool, from: addr.index, count: Math.max(0, count), bits: `0x${bits.toString(16)}` },
  });
}

/**
 * 0x2d8 set-array-to（数组填充族）：从 op1 的**地址**起，把 op3 个 dword 填成 **`ENC(key, op2)`**。
 * ★ 与 `0x6c` 的差别只有"填什么值"：`0x6c` 的值恒为 `ENC(key,0)`。
 */
function setArrayTo2d8(m: Machine, ops: Operand[]): void {
  const addr = readAddress(m, ops, 1);
  const value = readInt(m, ops, 2);
  const count = readInt(m, ops, 3);
  const bits = encodeInt(value, m.key) >>> 0;
  if (count > 0) for (let i = 0; i < count; i += 1) writeBitsAt(m, addr, i, bits);
  m.effects.push({
    kind: 'array.fill',
    detail: { opcode: '0x2d8', pool: addr.pool, from: addr.index, count: Math.max(0, count), value, bits: `0x${bits.toString(16)}` },
  });
}

/** 0x8c jmp：op1 是**位置**（代码字下标）；−1（哨兵）⇒ 落下；否则 ip ← 4×位置、长度槽清 0。 */
function jump8c(m: Machine, ops: Operand[]): void {
  const target = readInt(m, ops, 1);
  if (target === -1) return; // 哨兵 = "没有目标" ⇒ 本条原地落下（不写长度槽）
  m.ip = 4 * target;
  m.operandCount = 0;
}

/** 0x5c lt：op1 ← (int32(op2) < int32(op3)) ? 1 : 0（有符号比较，结果按整型写回）。 */
function lt5c(m: Machine, ops: Operand[]): void {
  const a = readInt(m, ops, 2) | 0;
  const b = readInt(m, ops, 3) | 0;
  writeInt(m, ops, 1, a < b ? 1 : 0);
}

/**
 * 0x61 lookup-array：`op1`（引用 / 指针族）← `取址(op2) + 元素宽度 × 读整(op3)`。
 *
 * 逐字（`sub_418CC0`，`0x61` 与 `0x12c` 共用这个写入器）：按 **op1 的类型标签**分派 case：
 * 6/7/12/13 ⇒ `base + 4×idx`；8/14 ⇒ `base + 28×idx`；其余（值族 9/10/11 等）⇒ 抛类型异常。
 * ★ 写进去的是**地址本身**（不是那一格的值），且指针槽不参与编解码。
 * ★ 越界**不检查**（引擎直接算地址）；地址落在窗口外 / 没对齐时，**解引用**那一步才响亮失败。
 */
function lookupArray61(m: Machine, ops: Operand[]): void {
  const dest = ops[0];
  const stride = arrayStrideFor(dest.type);
  const base = addressOfOperand(m, ops, 2);
  const index = readInt(m, ops, 3) | 0;
  const addr = (base + stride * index) >>> 0;
  writePointerSlot(m, ops, 1, addr);
  m.effects.push({
    kind: 'array.lookup',
    detail: { opcode: '0x61', pool: `0x${dest.type.toString(16)}`, from: base, index, stride, address: addr },
  });
}

/** 0x12c lookup-array-2d：下标 = `读整(op3) × 读整(op4) + 读整(op5)`（乘在前、加在后），其余同 0x61 */
function lookupArray2d12c(m: Machine, ops: Operand[]): void {
  const dest = ops[0];
  const stride = arrayStrideFor(dest.type);
  const base = addressOfOperand(m, ops, 2);
  const index = ((readInt(m, ops, 3) | 0) * (readInt(m, ops, 4) | 0) + (readInt(m, ops, 5) | 0)) | 0;
  const addr = (base + stride * index) >>> 0;
  writePointerSlot(m, ops, 1, addr);
  m.effects.push({
    kind: 'array.lookup',
    detail: { opcode: '0x12c', pool: `0x${dest.type.toString(16)}`, from: base, index, stride, address: addr },
  });
}

/** `0x64` 一次最多拷多少格（元素个数来自**脚本字节**，坏 raw 不许把内存吃光） */
const COPY_ARRAY_MAX = 1 << 20;

/**
 * 0x64 copy-local-array：把脚本里一块**解码后的整数**逐格编码后写进 op1 的地址。
 * 逐字（`sub_42CBE0`）：`dest = 取址(op1)`；块首 = `代码区起点 + 4×raw`，首 u32 = 元素个数，数据从 +4 起；
 * 逐格写 `ENC(源 dword)`（**无条件**编码，不按目标池分派）。
 * ★ `body` 已经切掉头 ⇒ 脚本内偏移 = `4 × raw`（与内联字符串同一个算式）。
 * ★ 个数为 0 ⇒ 什么都不做（引擎的 `if (count > 0)`）；越出脚本 ⇒ 响亮失败。
 */
function copyLocalArray64(m: Machine, ops: Operand[]): void {
  const dest = addressOfOperand(m, ops, 1);
  const raw = readInt(m, ops, 2) >>> 0;
  const off = 4 * raw;
  if (off + 4 > m.body.length) {
    throw new Error(`copy-local-array 的块首越出脚本：raw=${raw} ⇒ body 偏移 ${off}（body ${m.body.length} B）`);
  }
  const count = m.body.readUInt32LE(off) >>> 0;
  if (count === 0) {
    m.effects.push({ kind: 'array.copy', detail: { opcode: '0x64', raw, count: 0, note: '空块 ⇒ 引擎什么都不做' } });
    return;
  }
  if (count > COPY_ARRAY_MAX) {
    throw new Error(`copy-local-array 的块声明了 ${count} 个元素（> ${COPY_ARRAY_MAX}）—— raw=${raw} 口径错了？`);
  }
  if (off + 4 + 4 * count > m.body.length) {
    throw new Error(`copy-local-array 的块越出脚本：${count} 格从 body ${off + 4} 起（body ${m.body.length} B）`);
  }
  for (let i = 0; i < count; i += 1) {
    const src = m.body.readUInt32LE(off + 4 + 4 * i) >>> 0;
    writeBitsToAddress(m, (dest + 4 * i) >>> 0, encodeInt(src | 0, m.key));
  }
  m.effects.push({ kind: 'array.copy', detail: { opcode: '0x64', dest: `0x${dest.toString(16)}`, raw, count } });
}

/**
 * 0x2de 字体名查表：在**宿主字体名表**里线性查第 2 个操作数的文本 ⇒ 0 基下标 / 未命中 **−1**，写回第 1 个。
 * 逐字（`sub_430DF0`，台账 `01M4B4HE3Z6T6K7M114D0V5J2T`）：
 * 文本原语读 op2 → `sub_428990` 线性查表 → `sub_42B4B0(this,1,结果)` 写回。
 * 判定规则：**长度相等 + 逐字节相等**（`memcmp` 且先比 size）；键首字节 `'@'` 时**跳过它**再比；无副作用。
 * ★ 宿主表是**外部输入**（`Machine.fontNames`）；空表 ⇒ 一律未命中（引擎会走回退分支）。
 */
function fontIndex2de(m: Machine, ops: Operand[]): void {
  const raw = readText(m, ops, 2);
  const key = raw.startsWith('@') ? raw.slice(1) : raw;
  let hit = -1;
  for (let i = 0; i < m.fontNames.length; i += 1) {
    const cand = m.fontNames[i]!;
    if (cand.length === key.length && cand === key) { hit = i; break; }
  }
  writeInt(m, ops, 1, hit);
  m.effects.push({
    kind: 'font.lookup',
    detail: { opcode: '0x2de', key, table: m.fontNames.length, hit },
  });
}

/** 0x56 and：op1 ← op2 & op3（按 u32 位与） */
function and56(m: Machine, ops: Operand[]): void {
  writeInt(m, ops, 1, ((readInt(m, ops, 2) & readInt(m, ops, 3)) >>> 0) | 0);
}

/** 0x5f gre：op1 ← (int32(op2) >= int32(op3)) ? 1 : 0（有符号） */
function gre5f(m: Machine, ops: Operand[]): void {
  const a = readInt(m, ops, 2) | 0;
  const b = readInt(m, ops, 3) | 0;
  writeInt(m, ops, 1, a >= b ? 1 : 0);
}

/** 24 位内倒字节序（0x76/0x77 的形态；像 BGR↔RGB，⛔ 不是原值） */
function bswap24(v: number): number {
  const x = v >>> 0;
  return (((x & 0xff) << 16) | (x & 0xff00) | ((x >>> 16) & 0xff)) >>> 0;
}

/** 0x76：`Engine.d21664 ← bswap24(op1)`，随后转发进子系统（未建模，记 `call-target-unmodeled`） */
function bswap76(m: Machine, ops: Operand[]): void {
  const value = bswap24(readInt(m, ops, 1));
  m.scalars.set('Engine.d21664', value);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x76', slot: 'Engine.d21664', form: 'bswap24(op1)', value } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_459F40' });
}

/** 0x77：与 0x76 同形，只差落点（`Engine.d21665`） */
function bswap77(m: Machine, ops: Operand[]): void {
  const value = bswap24(readInt(m, ops, 1));
  m.scalars.set('Engine.d21665', value);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x77', slot: 'Engine.d21665', form: 'bswap24(op1)', value } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_459F40' });
}

/** 0x1a4：成对写 —— `Engine.d21671 ← op2`、`Engine.d21670 ← op1`（读/写都是先 op2 后 op1） */
function pair1a4(m: Machine, ops: Operand[]): void {
  const op2 = readInt(m, ops, 2);
  const op1 = readInt(m, ops, 1);
  m.scalars.set('Engine.d21671', op2);
  m.scalars.set('Engine.d21670', op1);
  m.effects.push({ kind: 'engine.scalar.pair', detail: { opcode: '0x1a4', a: 'Engine.d21670', b: 'Engine.d21671' } });
}

/**
 * 0x2ee：`Engine.d80106 ← op1`，随后经 `Engine+0xAA514` 对象的 `vtable+12` 转发
 * （静态串 `message:MessageFade`，并把 **op1 再读一次**作为实参）—— 转发未建模，记诊断。
 */
function message2ee(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 1);
  m.scalars.set('Engine.d80106', value);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x2ee', slot: 'Engine.d80106', form: 'op1', value } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:message:MessageFade' });
}

/** 薄标量写的公共体：写一个具名格并留痕 */
function scalarWrite(m: Machine, opcode: string, slot: string, value: number): void {
  m.scalars.set(slot, value >>> 0);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode, slot, value } });
}

/** 0xfe：`Engine.d517 ← op1`；引擎自带范围检查（值 > 0x1f ⇒ 抛） */
function thinFe(m: Machine, ops: Operand[]): void {
  const v = readInt(m, ops, 1);
  if (v > 0x1f) throw new Error(`opcode 0xfe 的操作数 ${v} 超出引擎的范围检查（> 0x1f）—— 引擎这里抛异常`);
  scalarWrite(m, '0xfe', 'Engine.d517', v);
}

/** 0x10f：`Engine.d122369 ← op1` */
function thin10f(m: Machine, ops: Operand[]): void {
  scalarWrite(m, '0x10f', 'Engine.d122369', readInt(m, ops, 1));
}

/** 0x248：`Engine.d55052 ← op1` */
function thin248(m: Machine, ops: Operand[]): void {
  scalarWrite(m, '0x248', 'Engine.d55052', readInt(m, ops, 1));
}

/**
 * 0x25b：`Engine.d92379 ← 2`（常量）、`Engine.d92381 ← op1`；**条件**转发 `sub_408440(this, op1)`
 * （逐字 `if (!this[167990]) sub_408440(...)`）—— 转发未建模，记诊断；条件成立与否取决于 `Engine.d167990`（未取证来源，缺省 0 ⇒ 成立）。
 */
function message25b(m: Machine, ops: Operand[]): void {
  const v = readInt(m, ops, 1);
  scalarWrite(m, '0x25b', 'Engine.d92379', 2);
  scalarWrite(m, '0x25b', 'Engine.d92381', v);
  if ((m.scalars.get('Engine.d167990') ?? 0) === 0) m.diagnostics.push({ kind: 'call-target-unmodeled:sub_408440' });
}

/** 标量数组：`Engine.<base>+索引 ← 值`；越界口径照抄知识层（skip / throw） */
function scalarArray(m: Machine, opcode: string, base: string, index: number, value: number,
  maxIndex: number, outOfRange: 'skip' | 'throw', maxValue?: number): void {
  const bad = index > maxIndex || (maxValue !== undefined && value > maxValue);
  if (bad) {
    if (outOfRange === 'skip') {
      m.diagnostics.push({ kind: `scalar-array-skip:${opcode}` });
      return;
    }
    throw new Error(`opcode ${opcode} 越界：索引 ${index}（上界 0x${maxIndex.toString(16)}）${maxValue !== undefined ? ` / 值 ${value}（上界 0x${maxValue.toString(16)}）` : ''} —— 引擎这里抛异常`);
  }
  scalarWrite(m, opcode, `${base}+${index}`, value);
}

/** 0x107：`Engine.d551+op1 ← op2`（下标 > 0x1f ⇒ 引擎静默跳过） */
function array107(m: Machine, ops: Operand[]): void {
  scalarArray(m, '0x107', 'Engine.d551', readInt(m, ops, 1), readInt(m, ops, 2), 0x1f, 'skip');
}

/** 0x10b：`Engine.d1383+op2 ← op1`（下标由 **op2** 给出；越界静默跳过） */
function array10b(m: Machine, ops: Operand[]): void {
  scalarArray(m, '0x10b', 'Engine.d1383', readInt(m, ops, 2), readInt(m, ops, 1), 0x1f, 'skip');
}

/** 0x30a：`Engine.d1969+op2 ← op1`（8 格；**索引与值都检查**，越界抛） */
function array30a(m: Machine, ops: Operand[]): void {
  scalarArray(m, '0x30a', 'Engine.d1969', readInt(m, ops, 2), readInt(m, ops, 1), 0x7, 'throw', 0x1f);
}

/**
 * 0x10c：带范围检查的**间接**引擎态写 —— `value = op1`、`slot = op2`；`value > 0x1f ⇒ 抛`。
 * 写目标是 `Engine[1434 + Engine[1690 + op2]]`：**索引表内容未取证**（台账 `01M4E510JP06185M2S58313T12`）
 * ⇒ 只记 `logged-only`，⛔ 不猜它是 0。
 */
function keyMulti10c(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 1);
  const slotIndex = readInt(m, ops, 2);
  if (value > 0x1f) throw new Error(`0x10c 的操作数 ${value} 超出引擎的范围检查（> 0x1f）—— 引擎这里抛 aSetkeymulti 异常`);
  m.effects.push({
    kind: 'engine.indirect-write',
    detail: { opcode: '0x10c', slotIndex, value, note: '写目标 = Engine[1434 + Engine[1690+op2]]，索引表未取证 ⇒ 只记录' },
  });
}

/** 0x308：转发 `sub_407B20(dword_55E1BC, Engine[96981], op1)` —— 被调方未建模（只记一条 `engine.forward`） */
function forward308(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 1);
  m.effects.push({ kind: 'engine.forward', detail: { opcode: '0x308', callee: 'sub_407B20', args: ['dword_55E1BC', 'Engine.d96981', value] } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_407B20' });
}

/** 0x135 bit-set：`op1 ← op1 | (1 << op2)`；位号 > 0x1f ⇒ 引擎打错误串后**继续**、不写 op1 */
function bitSet135(m: Machine, ops: Operand[]): void {
  const bit = readInt(m, ops, 2);
  if (bit > 0x1f) { m.diagnostics.push({ kind: 'bit-index-oob:0x135' }); return; }
  writeInt(m, ops, 1, ((readInt(m, ops, 1) | (1 << bit)) >>> 0) | 0);
}

/** 0x136 bit-reset：`op1 ← op1 & ~(1 << op2)`（位号越界口径同 0x135） */
function bitReset136(m: Machine, ops: Operand[]): void {
  const bit = readInt(m, ops, 2);
  if (bit > 0x1f) { m.diagnostics.push({ kind: 'bit-index-oob:0x136' }); return; }
  writeInt(m, ops, 1, (readInt(m, ops, 1) & ~(1 << bit)) | 0);
}

/** 0x13f check-bit：**位号在 op3** —— `op1 ← ((1 << op3) & op2) != 0 ? 1 : 0`；越界写 0 并记一笔 */
function checkBit13f(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 2);
  const bit = readInt(m, ops, 3);
  if (bit > 0x1f) { m.diagnostics.push({ kind: 'bit-index-oob:0x13f' }); writeInt(m, ops, 1, 0); return; }
  writeInt(m, ops, 1, ((1 << bit) & value) !== 0 ? 1 : 0);
}

/* —— 数值双目：比较/乘除/位（与 0x50/0x51 同族；旧实现 `binInt`/`cmpInt` 的同形落地） —— */

const binInt = (m: Machine, ops: Operand[], f: (a: number, b: number) => number): void => {
  writeInt(m, ops, 1, f(readInt(m, ops, 2), readInt(m, ops, 3)) | 0);
};
const cmpInt = (m: Machine, ops: Operand[], f: (a: number, b: number) => boolean): void => {
  const a = readInt(m, ops, 2) | 0;
  const b = readInt(m, ops, 3) | 0;
  writeInt(m, ops, 1, f(a, b) ? 1 : 0);
};

function mul52(m: Machine, ops: Operand[]): void { binInt(m, ops, (a, b) => Math.imul(a | 0, b | 0)); }
function div53(m: Machine, ops: Operand[]): void {
  const a = readInt(m, ops, 2) | 0;
  const b = readInt(m, ops, 3) | 0;
  if (b === 0) throw new Error('div: 除数为 0（引擎在这一点上会抛）');
  writeInt(m, ops, 1, Math.trunc(a / b) | 0);
}
function mod54(m: Machine, ops: Operand[]): void {
  const a = readInt(m, ops, 2) | 0;
  const b = readInt(m, ops, 3) | 0;
  if (b === 0) throw new Error('mod: 除数为 0（引擎在这一点上会抛）');
  writeInt(m, ops, 1, (a % b) | 0);
}
function or57(m: Machine, ops: Operand[]): void { binInt(m, ops, (a, b) => (a | b) >>> 0); }
function sar58(m: Machine, ops: Operand[]): void { binInt(m, ops, (a, b) => (a | 0) >> (b & 31)); }
function shl59(m: Machine, ops: Operand[]): void { binInt(m, ops, (a, b) => (a << (b & 31)) >>> 0); }
function eq5a(m: Machine, ops: Operand[]): void { cmpInt(m, ops, (a, b) => a === b); }
function ne5b(m: Machine, ops: Operand[]): void { cmpInt(m, ops, (a, b) => a !== b); }
function lte5d(m: Machine, ops: Operand[]): void { cmpInt(m, ops, (a, b) => a <= b); }
function gr5e(m: Machine, ops: Operand[]): void { cmpInt(m, ops, (a, b) => a > b); }

/**
 * 0x143（`sub_41A000`，argc 0）：把"待处理事件"走一遍，再交回主循环。
 *
 * 逐字：长度槽 1 → 扫 `Engine+0xA90C8` 起 0x100 格（非 0 ⇒ `sub_40FC90(this, i<<24)`）→ 清 `Engine[0x796F8]` →
 * 长度槽 0 → **ip += 4** → 尾调 `sub_40FB60(&ip)`。
 *
 * ★ `sub_40FB60` 是**事件队列的弹出**：队列空且 `[68EB8]==0` ⇒ 直接 `retn`（不改 cur / ip，逐字 `loc_40FC8B`）；
 *   队列非空则可能改 cur / ip（甚至 `cur = 0x25` 那条装载路径）。本层**没有事件生产者**（通道表与队列从未被写）
 *   ⇒ 走"空队列"分支，并把两条未建模项记进诊断（⛔ 不是静默）。
 */
function eventFlush143(m: Machine): void {
  m.effects.push({
    kind: 'engine.event-flush',
    detail: { opcode: '0x143', channels: 'unmodeled(empty)', queue: 'unmodeled(empty)' },
  });
  m.diagnostics.push({ kind: 'event-queue-unmodeled:0x143' });
  m.operandCount = 0;
  m.ip += 4; // 引擎自己加的那 4 字节（队列空 ⇒ 尾调不改 cur/ip）
}

/** 0x75：转发 `sub_4185F0(Engine+0x14D30, op1)`（字体/文本子系统；被调方未建模，只记一条 `engine.forward`） */
function forward75(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 1);
  m.effects.push({ kind: 'engine.forward', detail: { opcode: '0x75', callee: 'sub_4185F0', holder: 'Engine.d21324', arg: value } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_4185F0' });
}

/**
 * 0x1a5 set-font：`sub_4328F0(Engine+0x14D30, 文本(op1))`（文本原语 `sub_41B640`）—— 字体/文本子系统，未建模。
 * ★ 它与 `0x2de` 的那张"字体名表"是否同一张表**未取证**（若是，`0x2de` 的宿主表就该由这些 set-font 填充）⇒ 只记转发 + 诊断。
 */
function setFont1a5(m: Machine, ops: Operand[]): void {
  const name = readText(m, ops, 1);
  m.effects.push({ kind: 'engine.forward', detail: { opcode: '0x1a5', callee: 'sub_4328F0', holder: 'Engine.d21324', name } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_4328F0' });
}

/**
 * 0x2bd：`v = 读整(op1)` ⇒ 子系统两格写 `v ? 0x2BC : 0`；`v != 0` 时再**尾跳** `sub_459F40(Engine+0x14D30)`（未建模）。
 * 落点 = 子系统 `+0x35594` / `+0x4E0`（绝对 `Engine[75953]` / `Engine[21636]`）。
 */
function toggle2bd(m: Machine, ops: Operand[]): void {
  const v = readInt(m, ops, 1);
  const value = v !== 0 ? 0x2bc : 0;
  scalarWrite(m, '0x2bd', 'Engine.d75953', value);
  scalarWrite(m, '0x2bd', 'Engine.d21636', value);
  if (v !== 0) m.diagnostics.push({ kind: 'call-target-unmodeled:sub_459F40' });
}

/** 0x2fe：转发 `sub_432DD0(Engine+0x14D30, 文本(op1))`（文本原语；被调方未建模） */
function forward2fe(m: Machine, ops: Operand[]): void {
  const name = readText(m, ops, 1);
  m.effects.push({ kind: 'engine.forward', detail: { opcode: '0x2fe', callee: 'sub_432DD0', holder: 'Engine.d21324', name } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_432DD0' });
}

/** 0x197：转发 `sub_418680(Engine+0x14D30, 读整(op1))`（被调方未建模） */
function forward197(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 1);
  m.effects.push({ kind: 'engine.forward', detail: { opcode: '0x197', callee: 'sub_418680', holder: 'Engine.d21324', arg: value } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_418680' });
}

/** 0x8b：`Engine.d21669 ← 读整(op1)`（`[esi+15294h]`） */
function thin8b(m: Machine, ops: Operand[]): void {
  scalarWrite(m, '0x8b', 'Engine.d21669', readInt(m, ops, 1));
}

/**
 * 0x260：四个操作数写消息子系统的四格（读序 **op4 → op3 → op2 → op1**，写序 op4/op3/op2/op1 交错）：
 * `Engine.d80102 ← op1`、`d80103 ← op2`、`d80104 ← op3`、`d80105 ← op4`（`[esi+4E398h..4E3A4h]`）。
 */
function quad260(m: Machine, ops: Operand[]): void {
  const op4 = readInt(m, ops, 4);
  const op3 = readInt(m, ops, 3);
  const op2 = readInt(m, ops, 2);
  const op1 = readInt(m, ops, 1);
  scalarWrite(m, '0x260', 'Engine.d80102', op1);
  scalarWrite(m, '0x260', 'Engine.d80103', op2);
  scalarWrite(m, '0x260', 'Engine.d80104', op3);
  scalarWrite(m, '0x260', 'Engine.d80105', op4);
}

/** 0x261：`Engine.d80101 ← 读整(op1)`（`[esi+4E394h]`） */
function thin261(m: Machine, ops: Operand[]): void {
  scalarWrite(m, '0x261', 'Engine.d80101', readInt(m, ops, 1));
}

/**
 * 0xae（argc 0）：**存档版本检查**。逐字开头：长度槽 1 → `if (Engine[0x5D890] == 0) return;`（`loc_419684` 是 `pop esi; retn`）。
 * 本模型从不写 `Engine[0x5D890]`（引擎级标量，从未被建模的写入者赋值）⇒ 恒走**早退**分支（= 不做任何事）。
 * ★ 另一支（读 `set:SaveVersion1/2`、修存档槽）**未建模** ⇒ 若它被置非 0，本层**响亮失败**（⛔ 不猜）。
 */
function saveVersionAe(m: Machine): void {
  if ((m.scalars.get('Engine.d95780') ?? 0) !== 0) {
    throw new Error('0xae：Engine[0x5D890] != 0 ⇒ 进入"存档版本修复"分支（未建模）');
  }
  m.diagnostics.push({ kind: 'save-version-check-skipped:0xae' });
}

/** 0x130 load-show-logo：`op1 ← Engine[0x5EB5C]`（引擎初始化置 1，见 `Machine` 构造函数；0x9 的复位会清它） */
function loadShowLogo130(m: Machine, ops: Operand[]): void {
  writeInt(m, ops, 1, m.showLogo);
}

function operandAt(ops: Operand[], k: number): Operand {
  const op = ops[k - 1];
  if (!op) throw new Error(`operand-out-of-range:${k}/${ops.length}`);
  return op;
}

export const HANDLERS = new Map<number, (m: Machine, ops: Operand[]) => void>([
  [0x1a7, noop],
  [0x1a8, noop],
  [0x50, add50],
  [0x51, sub51],
  [0x2d5, floatMov2d5],
  [0x8f, call8f],
  [0x1ca, message1ca],
  [0x70, listSet70],
  [0x71, text71],
  [0x73, forward73],
  [0x78, field78],
  [0x79, pair79],
  [0x1c1, pair1c1],
  [0x212, listField212],
  [0x213, listPair213],
  [0x25d, listPair25d],
  [0x303, pair303],
  [0x2db, field2db],
  [0x324, field324],
  [0x32f, container32f],
  [0x3, callScript3],
  [0x2, exit2],
  [0x6, loadFrame6],
  [0x8, callFrame8],
  [0x9, exitScript9],
  [0xa0, jumpA0],
  [0x55, move55],
  [0x149, thin149],
  [0x21b, thin21b],
  [0x252, thin252],
  [0x88, pair88],
  [0x5, ret05],
  [0x2f6, stopVoice2f6],
  [0x2f8, setVolume2f8],
  [0x1f7, detach1f7],
  [0x1f8, create1f8],
  [0x1f9, set1f9],
  [0x1fa, release1fa],
  [0x1fb, draw1fb],
  [0x202, scene202],
  [0x203, scene203],
  [0x320, scene320],
  [0x322, scene322],
  [0x323, scene323],
  [0x101, pollInput101],
  [0x21c, (m) => wait21c(m)],
  [0x20f, movie20f],
  [0x1a2, saveInt1a2],
  [0x1a3, loadInt1a3],
  [0x1a9, saveString1a9],
  [0x192, setString192],
  [0x6c, fillZero6c],
  [0x8c, jump8c],
  [0x5c, lt5c],
  [0x56, and56],
  [0x5f, gre5f],
  [0x2de, fontIndex2de],
  [0x76, bswap76],
  [0x77, bswap77],
  [0x1a4, pair1a4],
  [0x2ee, message2ee],
  [0xfe, thinFe],
  [0x10f, thin10f],
  [0x248, thin248],
  [0x25b, message25b],
  [0x107, array107],
  [0x10b, array10b],
  [0x30a, array30a],
  [0x10c, keyMulti10c],
  [0x308, forward308],
  [0x135, bitSet135],
  [0x136, bitReset136],
  [0x13f, checkBit13f],
  [0x143, eventFlush143],
  [0x75, forward75],
  [0x1a5, setFont1a5],
  [0x2bd, toggle2bd],
  [0x2fe, forward2fe],
  [0x197, forward197],
  [0x8b, thin8b],
  [0x260, quad260],
  [0x261, thin261],
  [0xae, saveVersionAe],
  [0x130, loadShowLogo130],
  [0x52, mul52],
  [0x53, div53],
  [0x54, mod54],
  [0x57, or57],
  [0x58, sar58],
  [0x59, shl59],
  [0x5a, eq5a],
  [0x5b, ne5b],
  [0x5d, lte5d],
  [0x5e, gr5e],
  [0x61, lookupArray61],
  [0x12c, lookupArray2d12c],
  [0x64, copyLocalArray64],
  [0x2d8, setArrayTo2d8],
  [0x1aa, loadString1aa],
]);

export function unimplemented(opcode: number): never {
  throw new Error(`unimplemented opcode 0x${opcode.toString(16)}`);
}

