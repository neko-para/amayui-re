/**
 * opcode → 实现。★ 每条都对应 `data/spec/chapters/60-ops.md` 里的一条规则。
 * ★ 未实现的 opcode **不在表里**（不是 no-op）：`Machine.run` 会在它前面报缺口。
 * ★ 本文件不碰文件系统；外部请求只进 `m.effects`（规范不规定外部效果）。
 */
import type { DrawItem, Machine, Mesh, MovieObject, Operand } from './machine.ts';
import { MOVIE_BIT, WAIT_BIT } from './machine.ts';
import { readAddress, readFloat, readInt, readIntAt, readText, writeFloat, writeInt, writeText } from './values.ts';

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
    flags: 0, count: 0, op10: 0, weights: [], colors: [],
    scale: [0, 0, 0], uv: [0, 0],
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
  const f7 = readFloat(m, ops, 7);
  const f8 = readFloat(m, ops, 8);
  const weights: number[] = [];
  const colors: number[] = [];
  for (let i = 0; i < count; i += 1) {
    weights.push(1);
    const a = readIntAt(m, a5, i);
    const b = readIntAt(m, a6, i);
    colors.push((((a << 24) | (b & 0xffffff)) >>> 0));
  }
  const op10 = readInt(m, ops, 10);
  const f4 = readFloat(m, ops, 4);
  const f3 = readFloat(m, ops, 3);
  const f2 = readFloat(m, ops, 2);
  const id = readInt(m, ops, 1);
  const mesh = meshOf(m, id);
  mesh.flags |= 1;
  mesh.count = count;
  mesh.op10 = op10;
  mesh.weights = weights;
  mesh.colors = colors;
  mesh.scale = [f2, f3, f4];
  mesh.uv = [f7, f8];
  if (mesh.colorFrom !== -1) mesh.colors = colors.map(() => mesh.colorFrom >>> 0);
  m.scene.dirty = 1;
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
function configKey(op: Operand, code: 3 | 5): string {
  const want = code === 3 ? 3 : 5;
  if (op.type !== want) {
    throw new Error(`config-key-unsupported-type:${code}:0x${op.type.toString(16)}`);
  }
  return String.fromCharCode(code) + op.raw.toString(16).padStart(8, '0');
}

function requireConfig(m: Machine) {
  if (!m.config) throw new Error('config-not-initialized');
  return m.config;
}

/** 0x1a3 load-int：查整数键；未命中 ⇒ 0；**无条件**写回第 1 个操作数 */
function loadInt1a3(m: Machine, ops: Operand[]): void {
  const key = configKey(operandAt(ops, 1), 3);
  writeInt(m, ops, 1, requireConfig(m).getInt(key));
}

/** 0x1a2 save-int：把第 1 个操作数的整数值写进整数键 */
function saveInt1a2(m: Machine, ops: Operand[]): void {
  const key = configKey(operandAt(ops, 1), 3);
  requireConfig(m).setInt(key, readInt(m, ops, 1));
}

/** 0x1aa load-string：查字符串键；未命中 ⇒ 空串；写回第 1 个操作数 */
function loadString1aa(m: Machine, ops: Operand[]): void {
  const key = configKey(operandAt(ops, 1), 5);
  writeText(m, ops, 1, requireConfig(m).getString(key));
}

/** 0x1a9 save-string：把第 1 个操作数的文本写进字符串键 */
function saveString1a9(m: Machine, ops: Operand[]): void {
  const key = configKey(operandAt(ops, 1), 5);
  requireConfig(m).setString(key, readText(m, ops, 1));
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
  [0x1aa, loadString1aa],
]);

export function unimplemented(opcode: number): never {
  throw new Error(`unimplemented opcode 0x${opcode.toString(16)}`);
}
