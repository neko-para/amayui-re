/**
 * opcode → 实现。★ 每条都对应 `data/spec/chapters/60-ops-*.md` 里的一条规则。
 * ★ 未实现的 opcode **不在表里**（不是 no-op）：`Machine.run` 会在它前面报缺口。
 * ★ 本文件不碰文件系统；外部请求只进 `m.effects`（规范不规定外部效果）。
 * ★ 取件类（`0x3` / `0x6` / `0x9`）是 `async`：核心的 IO 全部经注入的 `Machine.loadScript`
 *   回调 + `await`，所以同一份处理器在 Node / 浏览器里都能跑。
 */
import { decodeCp932, readU16LE, readU32LE } from './bytes.ts';
import type { DrawItem, Handler, ListRecord, Machine, Mesh, MovieObject, Operand, SceneCommandRecord, TextureObject, VoiceGroup } from './machine.ts';
import { FRAME_SLOT_COUNT, MOVIE_BIT, SLEEP_BIT, WAIT_BIT, indexInTable } from './machine.ts';
import { decodeInt, encodeInt, readAddress, readFloat, readInt, readIntAt, readRawAt, readText, writeBitsAt, writeFloat, writeInt, writeRawAt, writeText } from './values.ts';
import { addressOfOperand, pointerSlotAddress, writeBitsToAddress, writePointerSlot } from './values.ts';
import { arrayStrideFor, resolveAddress } from './address.ts';
import type { PoolTag } from './address.ts';
import { NO_COLOR, RENDER_TARGET_BACK_BUFFER } from './render.ts';
import { buildSaveFile } from './save-codec.ts';
import type { SaveTextureRecord } from './save-codec.ts';
import type { RenderHost } from './render.ts';
import { AgfError, decodeAgf } from './agf.ts';
import { cgDigitSlots } from './cg-digit.ts';

export type { Machine, Operand };

function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

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

/**
 * `0x1f7` / `0x1f6` / `0x23b` 共用的**四张同键表**：对象记录表、网格表、两张辅助表。
 * ★ 引擎同一组：`sub_4ABB60` 删 / `sub_41A130` 清的是 `+408` / `+428` / `+438` / `+448`；
 *   同键空间的第五张表 `+418` **不在这组里**（三条指令都不碰它）。
 */
function sameKeyTables(m: Machine): Map<number, unknown>[] {
  return [m.scene.drawItems, m.scene.meshes, ...m.scene.auxTables];
}

/** 0x1f7 拆离：读 op2=个数、op1=键；≤1 删单键，否则删 [键, 键+个数)（CORE-OPS-TEXTURE-DETACH） */
function detach1f7(m: Machine, ops: Operand[]): void {
  const count = readInt(m, ops, 2);
  const key = readInt(m, ops, 1);
  const tables = sameKeyTables(m);
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

/** 0x1f8 创建：读 1=id、4=种类、3=高、2=宽（CORE-OPS-TEXTURE-CREATE） */
async function create1f8(m: Machine, ops: Operand[]): Promise<void> {
  const id = readInt(m, ops, 1);
  clearObjectSlot(m, id);
  const kindRaw = readInt(m, ops, 4);
  const height = readInt(m, ops, 3);
  const width = readInt(m, ops, 2);
  readInt(m, ops, 1); // 同一编号再读一次（引擎在建纹理时再问 id）
  const kind = kindRaw === 3 ? 'divided' : 'normal';
  const previous = m.scene.textures.get(id);
  // ★ 只写来源与空白位；颜色键 / 释放守卫 / 其余字段保持原值
  m.scene.textures.set(id, { file: -1, color: previous?.color ?? 0, blank: 1 });
  const object: TextureObject = { kind, width: 0, height: 0 };
  m.scene.textureObjects.set(id, object);
  m.effects.push({ kind: 'texture.create.request', detail: { id, width, height, kind: kindRaw } });
  if (m.renderHost === undefined) {
    // 无后端：只做引擎数据；⛔ 不声称平台创建成功
    object.width = width;
    object.height = height;
    return;
  }
  const ok = await m.renderHost.createTexture({ textureId: id, width, height, kind });
  if (ok) {
    object.width = width;
    object.height = height;
    return;
  }
  // 失败：对象仍留在槽里，但尺寸 / 可用性未定义（本层记 0×0 + 诊断）
  m.scene.errorBuffer = 'テクスチャ作成に失敗';
  m.diagnostics.push({ kind: 'texture-create-failed', index: id });
}

/** 0x1f9 设置：读 2=id、1=文件号、3=颜色键（CORE-OPS-TEXTURE-SET / -LOAD） */
async function set1f9(m: Machine, ops: Operand[]): Promise<void> {
  const id = readInt(m, ops, 2);
  clearObjectSlot(m, id);
  const fileId = readInt(m, ops, 1);
  const previous = m.scene.textures.get(id);
  if (m.loadImage === undefined) {
    m.diagnostics.push({ kind: 'texture-image-loader-missing', index: id });
    throw new Error(`texture-image-loader-missing:${fileId}`);
  }

  // 打开 / 加锁失败发生在清纹理槽之前 ⇒ 记录与旧对象都保持原值
  let bytes: Uint8Array | null;
  try {
    bytes = await m.loadImage(fileId);
  } catch (error) {
    m.diagnostics.push({ kind: 'texture-file-open-failed', index: fileId });
    throw new Error(`画像ファイル ${fileId} の読み込みに失敗しました: ${messageOf(error)}`);
  }
  if (bytes === null) {
    m.diagnostics.push({ kind: 'texture-file-missing', index: fileId });
    throw new Error(`画像ファイル ${fileId} の読み込みに失敗しました`);
  }

  // 引擎在打开 / 加锁之后才读颜色键与第二次 id
  const rawColor = readInt(m, ops, 3);
  const color = rawColor < 0 ? 0 : (0xff000000 | (rawColor & 0xffffff)) >>> 0;
  readInt(m, ops, 2); // 加载时再读一次 id

  // 解码期失败：旧纹理对象已在解码前清掉，槽为空；记录来源 = 无
  m.scene.textureObjects.delete(id);
  let image: ReturnType<typeof decodeAgf>;
  try {
    image = decodeAgf(bytes);
  } catch (error) {
    m.scene.textures.set(id, { file: -1, color: previous?.color ?? 0, blank: previous?.blank ?? 0 });
    const code = error instanceof AgfError ? error.code : 'agf-decode-failed';
    m.diagnostics.push({ kind: `texture-${code}`, index: fileId });
    throw new Error(`画像ファイル ${fileId} の読み込みに失敗しました: ${messageOf(error)}`);
  }

  m.scene.textureObjects.set(id, { kind: 'normal', width: image.width, height: image.height });
  m.effects.push({
    kind: 'texture.load.request',
    detail: { id, fileId, color, width: image.width, height: image.height },
  });

  if (m.renderHost !== undefined) {
    const ok = await m.renderHost.uploadTexture({
      textureId: id,
      width: image.width,
      height: image.height,
      kind: 'normal',
      rgba: image.rgba,
      color,
    });
    if (!ok) {
      // 上传失败：槽里留下已创建的普通纹理对象；记录来源 = 无
      m.scene.textures.set(id, { file: -1, color: previous?.color ?? 0, blank: previous?.blank ?? 0 });
      m.diagnostics.push({ kind: 'texture-upload-failed', index: id });
      throw new Error(`画像ファイル ${fileId} の読み込みに失敗しました: upload`);
    }
  }

  m.scene.textures.set(id, { file: fileId, color, blank: 0 });
}

/** 0x1fa 释放：读 op1 两次（CORE-OPS-TEXTURE-RELEASE） */
async function release1fa(m: Machine, ops: Operand[]): Promise<void> {
  const id = readInt(m, ops, 1);
  clearObjectSlot(m, id);
  readInt(m, ops, 1);
  // 释放守卫：规范说本构建里恒 0（全语料没有写点）⇒ 走释放路径
  const rec = m.scene.textures.get(id);
  m.scene.textures.set(id, { file: -1, color: rec?.color ?? 0, blank: rec?.blank ?? 0 });
  m.scene.textureObjects.delete(id);
  if (m.renderHost !== undefined) await m.renderHost.releaseTexture(id);
}

/** 4×4 单位矩阵（列主序，与 D3D 一致）。 */
function identity4(): number[] {
  const m = new Array(16).fill(0);
  m[0] = m[5] = m[10] = m[15] = 1;
  return m;
}

/** 默认绘制项：规范只说“建一个默认项”，字段全 0；六个矩阵默认单位阵。 */
function defaultItem(): DrawItem {
  return {
    flags: 0, textureId: 0, left: 0, top: 0, right: 0, bottom: 0, aux: [0, 0, 0],
    start: 0, delay: 0, duration: 0, colorFrom: -1, colorNow: -1, colorTo: -1, unknown: new Map(),
    vec1: [0, 0, 0], vec2: [0, 0, 0],
    matrices: Array.from({ length: 6 }, () => identity4()),
    config: 0, g0: 0, g1: 0, g2: 0,
    fields: new Map(), extraMatrices: new Map(),
    sourceWindow: null,
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
    start: 0, delay: 0, duration: 0, colorFrom: -1, colorNow: -1, colorTo: -1, unknown: new Map(),
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
    item.colorNow = item.colorFrom;
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
  item.colorNow = value;
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
  mesh.colorNow = value;
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
  mesh.colorNow = mesh.colorFrom;
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
  if (mesh.colorFrom !== -1) {
    mesh.colors = colors.map(() => mesh.colorFrom >>> 0);
    mesh.colorNow = mesh.colorFrom;
  }
  m.scene.dirty = 1;
}

/* ──────────────────────────── 场景命令记录族（60-ops-scene.md 的 CORE-OPS-SCENE-COMMAND-RECORDS） ──────────────────────────── */

/**
 * 写侧的五条（`0x223` / `0x24d` / `0x24f` / `0x250` / `0x251`）只写**场景命令表**。
 *
 * ★ **命令不立即画**：这几条 handler ⛔ 不发任何绘制请求；绘制由渲染步的命令档执行
 *   （本层**未建模**命令档，见 `renderer/scene-order.ts`）。
 *   **唯一的例外**是写表**之前**的那一次建 / 换纹理请求（`0x223` / `0x24f` / `0x250` / `0x251`
 *   —— 种类 2 的抹除结果落在命令自己的纹理上，所以它也发这一次请求）。
 * ★ **同键是替换**：写前先删该键的旧记录（⛔ 不是追加两条）。
 * ★ **置脏**由 `writeCommandRecord` 统一做；`0x24d` 另外置场景「有动画」（两个不同的格）。
 * ★ **读序**：证据里这五条的**操作数读序**是倒序（种类 0 / 1 / 3；种类 1 还重复读若干次）。
 *   可观察差异只有"读到非法类型时最先抛哪一个" ⇒ 本层按参数顺序读，**不把读序写成合同**
 *   （⛔ 不声称已对齐证据）。
 */

/** 诊断去重：同类**且同编号**只记一条（命令族可能每帧都被写，别让诊断刷满） */
function noteOnce(m: Machine, kind: string, index?: number): void {
  if (m.diagnostics.some((d) => d.kind === kind && d.index === index)) return;
  m.diagnostics.push(index === undefined ? { kind } : { kind, index });
}

/**
 * 默认命令记录：首格 = 种类、变体格 = 变体、时间戳 = 0；其余格保持默认
 * （spec：「记录里没有被本条写到的格保持默认值」）。
 */
function defaultCommandRecord(kind: number, variant: number): SceneCommandRecord {
  return {
    kind,
    variant,
    timestamp: 0,
    textureId: 0,
    ownerKey: 0,
    rangeStart: 0,
    rangeCount: 0,
    segmentA: [0, 0],
    segmentB: [0, 0],
    rect: { left: 0, top: 0, right: 0, bottom: 0 },
    delay: 0,
    duration: 0,
    flag: 0,
    mode: 0,
    split: 0,
    rampLength: [0, 0],
    rampAngle: [0, 0],
    rampCenterU: [0, 0],
    rampCenterV: [0, 0],
    raw: [],
  };
}

/** 同键**先删旧记录再写**（替换语义）+ 置场景脏 */
function writeCommandRecord(m: Machine, key: number, record: SceneCommandRecord): void {
  m.scene.commands.delete(key);
  m.scene.commands.set(key, record);
  m.scene.dirty = 1;
}

/** 按读出顺序把 1..argc 个操作数收成原始整型值（`raw` 留痕用，⛔ 不拿它反推记录格） */
function commandOperands(m: Machine, ops: Operand[]): number[] {
  const out: number[] = [];
  for (let k = 1; k <= ops.length; k += 1) out.push(readInt(m, ops, k));
  return out;
}

/**
 * `0x223` / `0x250` / `0x251` 在写表**之前**的那一次建 / 换纹理请求。
 *
 * spec 的两条判据（`CORE-OPS-SCENE-COMMAND-RECORDS`）：
 * ① 槽里**没有对象**，或 ② 对象的表面格式与**当前显示格式不符** ⇒ 请求按该编号**建 / 换纹理**；
 * **格式相符 ⇒ 一个请求都不发**（因此也不会产生失败上报）。
 *
 * ★ 核心能自己判的只有「槽空」；**「当前显示格式」在本层没有状态格** ⇒ 格式比较交给
 *   `RenderHost.textureFormatMatches`（可选）。平台不提供 ⇒ 记一条
 *   `scene-command-texture-format-unmodeled` 诊断，⛔ 不假装"相符"、⛔ 不猜一次换纹理。
 * ★ **请求的返回值被忽略**（spec / 证据：写入器调用后不看 `eax`）：无论成功失败，调用方
 *   照常删旧记录、写新记录、置脏。
 * ★ **失败后果**（spec 的「种类 0 的建 / 换纹理请求失败」）：
 *   - 错误通道收到**一条带编号、不带脚本位置前缀**的说明（`scene.errorBuffer`；与绘制阶段
 *     那些会带脚本位置前缀的上报不是同一个入口）；
 *   - 槽里**仍然留下一个「建失败」的对象**（引擎先把新对象落槽、再调平台）⇒ 之后
 *     「槽是否为空」这个判据不再能识别它，是否重建只由「格式是否相符」决定
 *     （见 `TextureObject.creationFailed`）；
 *   - 重试行为本身是残差 `RES-SCENE-COMMAND-TEXTURE-RETRY`（本层按格式判据如实重发）。
 * ★ 请求的尺寸 / 格式参数 spec 不规定 ⇒ 建用槽里对象的尺寸（槽空则 0×0）、种类按普通纹理。
 */
async function commandTextureRequest(m: Machine, textureId: number): Promise<void> {
  const object = m.scene.textureObjects.get(textureId);
  const create = object === undefined;
  if (!create) {
    const matches = m.renderHost?.textureFormatMatches;
    if (matches === undefined) {
      noteOnce(m, 'scene-command-texture-format-unmodeled', textureId);
      return;
    }
    if (await matches(textureId)) return;
  }
  const request = {
    textureId,
    width: object?.width ?? 0,
    height: object?.height ?? 0,
    kind: 'normal' as const,
  };
  m.effects.push({ kind: create ? 'texture.create.request' : 'texture.replace.request', detail: { ...request } });
  if (m.renderHost === undefined) {
    // 没有宿主 ⇒ 请求发不出去；⛔ 不静默（也不伪造"建失败"的对象：引擎里那一步没发生）
    noteOnce(m, 'scene-command-texture-no-host', textureId);
    return;
  }
  let ok: boolean;
  if (create) {
    ok = await m.renderHost.createTexture(request);
  } else if (m.renderHost.replaceTexture === undefined) {
    noteOnce(m, 'scene-command-texture-replace-unsupported', textureId);
    return;
  } else {
    ok = await m.renderHost.replaceTexture(request);
  }
  if (ok) {
    if (object !== undefined) object.creationFailed = false;
    return;
  }
  // ★ 失败：错误通道（不带脚本位置前缀；文本来自引擎的 CTexture 错误格式串）+ 槽里留"建失败"对象。
  m.scene.errorBuffer = `関数：CTexture エラー：テクスチャ作成に失敗． TEXTURE=${textureId}`;
  noteOnce(m, 'scene-command-texture-request-failed', textureId);
  if (object !== undefined) object.creationFailed = true;
  else m.scene.textureObjects.set(textureId, { kind: 'normal', width: 0, height: 0, creationFailed: true });
}

/**
 * `0x223` 场景命令记录：**种类 0**（8 个操作数）。**纹理覆盖命令**。
 *
 * 1 = 命令键、2 = 纹理编号、3 / 4 与 5 / 6 各一段（**起点键、个数**）、7 = 延迟、8 = 持续时间。
 * ★ 纹理槽检查与建 / 换请求在**写表之前**（见 `commandTextureRequest`）；请求失败**不阻止写表**。
 * ★ 本条**不立即画**（⛔ 不发绘制请求）。
 */
async function sceneCommand223(m: Machine, ops: Operand[]): Promise<void> {
  const key = readInt(m, ops, 1);
  const textureId = readInt(m, ops, 2);
  const startA = readInt(m, ops, 3);
  const countA = readInt(m, ops, 4);
  const startB = readInt(m, ops, 5);
  const countB = readInt(m, ops, 6);
  const delay = readInt(m, ops, 7);
  const duration = readInt(m, ops, 8);
  // ★ 忽略返回值（spec：失败也照常删旧 + 写新 + 置脏）
  await commandTextureRequest(m, textureId);
  const record = defaultCommandRecord(0, 0);
  record.textureId = textureId;
  record.segmentA = [startA, countA];
  record.segmentB = [startB, countB];
  record.delay = delay;
  record.duration = duration;
  writeCommandRecord(m, key, record);
}

/**
 * `0x24d` 场景命令记录：**种类 1**（12 个操作数）。**影片覆盖命令**：
 * 把一条**已有的影片纹理**按目标矩形叠进画面，并接管一段绘制键区间。
 *
 * 操作数 → 字段（`CORE-OPS-SCENE-COMMAND-RECORDS`）：
 * - 1 = 命令键，**同时**是影片名表的索引（**不写进记录**）；
 * - 2 = 一条**已经存在**的影片纹理的编号（本条**不建纹理**）；
 * - 3 = 覆盖区间的起点键、4 = 个数；
 * - 5 / 6 = 目标矩形的左 / 上，7 / 8 = 宽 / 高（⇒ 右 = 5 + 7、下 = 6 + 8，宽高是**加出来的**）；
 * - 9 = 取 0 / 1 的**标志**；10 = 另一个影片名表索引（**不写进记录**）；
 * - 11 = 延迟、12 = 持续时间。
 *
 * ★ **先判纹理槽**：槽空 ⇒ 引擎抛**显示消息型异常**终止本条（⛔ 不是错误通道上报）；
 *   此时**旧记录不动**（删旧记录在判槽之后）。
 * ★ 写成功 ⇒ 时间戳清 0、场景标脏，并置场景「有动画」（与「脏」是**两个不同的格**）。
 * ★ 「第一帧画 / 之后只标记」的判据是**记录的时间戳格**，时机在渲染步 —— ⛔ 不在本条里画。
 */
function sceneCommand24d(m: Machine, ops: Operand[]): void {
  // 读序前缀按证据是 2 → 1；判槽在删旧记录之前。
  const textureId = readInt(m, ops, 2);
  const key = readInt(m, ops, 1);
  if (!m.scene.textureObjects.has(textureId)) {
    // 文本逐字照引擎（`Command_ShowMessage_Exception`）；它会被显示给玩家 ⇒ ⛔ 不往里塞编号。
    throw new Error('display-message: ムービーの初期化に失敗しました．\r\nテクスチャが確保されていません．');
  }
  const rangeStart = readInt(m, ops, 3);
  const rangeCount = readInt(m, ops, 4);
  const left = readInt(m, ops, 5);
  const top = readInt(m, ops, 6);
  const width = readInt(m, ops, 7);
  const height = readInt(m, ops, 8);
  const flag = readInt(m, ops, 9);
  // 第 10 个操作数也是影片名表索引（不落记录），但它是**一次操作数读**（读到非法类型时会抛）。
  readInt(m, ops, 10);
  const delay = readInt(m, ops, 11);
  const duration = readInt(m, ops, 12);
  const record = defaultCommandRecord(1, 0);
  record.textureId = textureId;
  record.rangeStart = rangeStart;
  record.rangeCount = rangeCount;
  record.rect = { left, top, right: left + width, bottom: top + height };
  record.flag = flag;
  record.delay = delay;
  record.duration = duration;
  record.raw = commandOperands(m, ops);
  writeCommandRecord(m, key, record);
  m.scene.animating = 1;
}

/**
 * `0x24f` 场景命令记录：**种类 2**（10 个操作数）。**抹除命令** —— 把两段绘制键区间各画进一块离屏画布，
 * 再按类型选出的图案把它们合成到**本条自己的纹理**上。
 *
 * 操作数 → 记录（`CORE-OPS-SCENE-COMMAND-RECORDS`）：1 = **命令键**（**同时**记进记录 `ownerKey`，
 * 命令档用它去绘制项表找抹除矩形）、2 = **纹理编号**、3 / 4 = 区间 A、5 / 6 = 区间 B、
 * 7 = 类型 / 模式、8 = 分割幅、9 / 10 = 延迟 / 持续时间。另写：`kind = 2`、`timestamp = 0`。
 *
 * **先校验、后写表**（两条顺序固定、后果**不同**）：
 *
 * - **分割幅**（操作数 8）**< 1** ⇒ 错误通道「分割幅が０以下です」；把分割幅记 **1**、类型记 **−1**、
 *   延迟与持续时间**清 0**，然后**照常写记录**（跳过类型校验 ⇒ 表里会有一条 `mode = -1` 的命令）。
 * - **类型 / 模式**（操作数 7）∉ **[0, 11]**（**两端闭、含负数**）⇒ 错误通道「タイプが不正です」并
 *   **直接返回**：不建记录、**不删旧记录**、⛔ 也不发纹理请求（表完全不变）。
 * - 通过 ⇒ 先发建 / 换纹理请求（**校验之后、删旧记录之前**），再替换命令记录并置脏。
 *
 * ★ 两条**不许合并**成"非法就拒绝"；⛔ 不许把负数放行（负数与 > 11 走同一个错误支）。
 * ★ 本条**不置**场景「有动画」（与种类 1 不同）。
 * ★ 类型 < 0 的副作用（命令档把场景「强制完成」置 1 ⇒ 所有计时窗立刻到期）属**渲染侧**，本层未建模。
 */
async function sceneCommand24f(m: Machine, ops: Operand[]): Promise<void> {
  const key = readInt(m, ops, 1);
  const textureId = readInt(m, ops, 2);
  const startA = readInt(m, ops, 3);
  const countA = readInt(m, ops, 4);
  const startB = readInt(m, ops, 5);
  const countB = readInt(m, ops, 6);
  let mode = readInt(m, ops, 7);
  let split = readInt(m, ops, 8);
  let delay = readInt(m, ops, 9);
  let duration = readInt(m, ops, 10);
  if (split < 1) {
    m.scene.errorBuffer = '分割幅が０以下です';
    split = 1;
    mode = -1;
    delay = 0; // ★ 清的是**实参**：记录里落的也是 0（不是"反正默认 0 就不写"）
    duration = 0;
  } else if (mode < 0 || mode > 11) {
    m.scene.errorBuffer = 'タイプが不正です';
    return; // ⛔ 不写表、⛔ 不删旧记录、⛔ 不发纹理请求
  }
  if (mode < 0) {
    // 类型 < 0 的**唯一可见后果**在渲染侧：命令档每帧看到它就把场景「强制完成」置 1
    // （⇒ 所有计时窗立刻到期）。本层没有命令档 ⇒ 只留诊断，⛔ 不假装那个副作用已经实现。
    noteOnce(m, 'scene-command-kind2-negative-mode', key);
  }
  // ★ 建 / 换纹理请求在两条校验**之后**、删旧记录**之前**（返回值忽略；失败也照常写表）。
  await commandTextureRequest(m, textureId);
  const record = defaultCommandRecord(2, 0);
  record.ownerKey = key;
  record.textureId = textureId;
  record.segmentA = [startA, countA];
  record.segmentB = [startB, countB];
  record.mode = mode;
  record.split = split;
  record.delay = delay;
  record.duration = duration;
  record.raw = commandOperands(m, ops);
  writeCommandRecord(m, key, record); // 置脏；⛔ 不置「有动画」
}

/**
 * `0x250`（种类 3、**变体 0**、10 个操作数）/ `0x251`（种类 3、**变体 1**、12 个操作数）。
 *
 * 两条**同为种类 3（模糊转场命令）**，只有**变体格**不同：
 *
 * | 字段 | 变体 0（`0x250`） | 变体 1（`0x251`） |
 * |---|---|---|
 * | 长度「起 → 终」 | 5 → 7 | 5 → 8 |
 * | 角度「起 → 终」 | 6 → 8 | **不用**（两格 0） |
 * | 中心 U「起 → 终」 | **不用**（两格 0） | 6 → 9（像素） |
 * | 中心 V「起 → 终」 | **不用**（两格 0） | 7 → 10（像素） |
 * | 延迟 / 持续时间 | 9 / 10 | 11 / 12 |
 *
 * 公共：1 = 命令键、2 = 纹理编号（写表**之前**先发一次建 / 换纹理请求）、3 = 区间起点、4 = 个数；
 * 写成功 ⇒ 时间戳清 0、场景标脏（**不**置「有动画」——那由推进决定）。
 */
async function sceneCommand3(m: Machine, ops: Operand[], variant: number): Promise<void> {
  const key = readInt(m, ops, 1);
  const textureId = readInt(m, ops, 2);
  const rangeStart = readInt(m, ops, 3);
  const rangeCount = readInt(m, ops, 4);
  let rampLength: [number, number];
  let rampAngle: [number, number] = [0, 0];
  let rampCenterU: [number, number] = [0, 0];
  let rampCenterV: [number, number] = [0, 0];
  let delay: number;
  let duration: number;
  if (variant === 0) {
    const lengthFrom = readInt(m, ops, 5);
    const angleFrom = readInt(m, ops, 6);
    const lengthTo = readInt(m, ops, 7);
    const angleTo = readInt(m, ops, 8);
    delay = readInt(m, ops, 9);
    duration = readInt(m, ops, 10);
    rampLength = [lengthFrom, lengthTo];
    rampAngle = [angleFrom, angleTo];
  } else {
    const lengthFrom = readInt(m, ops, 5);
    const centerUFrom = readInt(m, ops, 6);
    const centerVFrom = readInt(m, ops, 7);
    const lengthTo = readInt(m, ops, 8);
    const centerUTo = readInt(m, ops, 9);
    const centerVTo = readInt(m, ops, 10);
    delay = readInt(m, ops, 11);
    duration = readInt(m, ops, 12);
    rampLength = [lengthFrom, lengthTo];
    rampCenterU = [centerUFrom, centerUTo];
    rampCenterV = [centerVFrom, centerVTo];
  }
  // ★ 写表之前的建 / 换纹理请求（返回值忽略）；失败也照常写表。
  await commandTextureRequest(m, textureId);
  const record = defaultCommandRecord(3, variant);
  record.textureId = textureId;
  record.rangeStart = rangeStart;
  record.rangeCount = rangeCount;
  record.delay = delay;
  record.duration = duration;
  record.rampLength = rampLength;
  record.rampAngle = rampAngle;
  record.rampCenterU = rampCenterU;
  record.rampCenterV = rampCenterV;
  record.raw = commandOperands(m, ops);
  writeCommandRecord(m, key, record); // 置脏；⛔ 不置「有动画」
}

/* ──────────────────────────── 渲染请求边界（60-ops-render.md） ──────────────────────────── */

/**
 * `0x20d` 的**分支阈值**：槽号 < 1000 ⇒ 纹理槽；≥ 1000（按无符号比较）⇒ 后缓冲。
 * ★ 它只是本条的分支阈值，**不是**纹理族的范围检查（纹理族对编号不做范围检查）。
 */
const RENDER_TARGET_SLOT_LIMIT = 1000;

/**
 * 取渲染宿主；没有注入 ⇒ `null`，并记一条 `render-host-missing` 诊断。
 * ★ ⛔ 不假装有后端：没有宿主时 `0x20d` 的取表面 / 设目标一律按失败处理（写错误通道），
 *   `0x204` 的入口条件 ② 因此不成立（什么都不做）。
 */
function renderHostOf(m: Machine): RenderHost | null {
  if (m.renderHost) return m.renderHost;
  if (!m.diagnostics.some((d) => d.kind === 'render-host-missing')) {
    m.diagnostics.push({ kind: 'render-host-missing' });
  }
  return null;
}

/**
 * 0x20d 设置渲染目标（CORE-OPS-RENDER-TARGET-20D）。
 *
 * 读**一个**操作数（纹理槽号），按整数读、按**无符号**比较：
 * - < 1000 ⇒ 该槽的纹理：槽里没有对象 ⇒ 错误通道（**不问宿主**）；取表面 / 设目标失败 ⇒ 错误通道；
 *   成功 ⇒ 当前目标 = 槽号。
 * - ≥ 1000（含负数）⇒ 后缓冲：取后缓冲 + 设目标；成功 ⇒ 当前目标 = 「后缓冲」哨兵。
 * ★ 所有失败都发生在**写当前目标之前**（失败时该格保持原值）。
 * ★ 成功路径的次序是合同：先取表面 → 再设目标 → **再归还表面** → 最后写当前目标。
 * ★ 本条不建纹理、不改纹理槽 / 绘制项 / 脏标记；错误文本内容本版不规定（`RES-RENDER-TARGET-TEXT`）。
 */
async function renderTarget20d(m: Machine, ops: Operand[]): Promise<void> {
  const slot = readInt(m, ops, 1);
  // ★ 无符号比较：负数落在「后缓冲」那一支（不是"槽号越界"，那是合法的复位入口）
  if ((slot >>> 0) < RENDER_TARGET_SLOT_LIMIT) {
    if (!m.scene.textureObjects.has(slot)) {
      m.scene.errorBuffer = `テクスチャ${slot}は作成されていません．`;
      return;
    }
    const host = renderHostOf(m);
    if (host === null) {
      m.scene.errorBuffer = `テクスチャ${slot}のレンダリングサーフェスを取得できません．`;
      return;
    }
    const surface = await host.getRenderSurface(slot);
    if (surface === null) {
      m.scene.errorBuffer = `テクスチャ${slot}のレンダリングサーフェスを取得できません．`;
      return;
    }
    const ok = await host.setRenderTarget(surface);
    await host.releaseSurface(surface); // 成功 / 失败路径都归还，且在写当前目标之前
    if (!ok) {
      m.scene.errorBuffer = `テクスチャ${slot}をレンダリングターゲットに設定できません．`;
      return;
    }
    m.render.target = slot;
    return;
  }
  const host = renderHostOf(m);
  if (host === null) {
    m.scene.errorBuffer = 'バックバッファを取得できません．';
    return;
  }
  const surface = await host.getBackBuffer();
  if (surface === null) {
    m.scene.errorBuffer = 'バックバッファを取得できません．';
    return;
  }
  const ok = await host.setRenderTarget(surface);
  await host.releaseSurface(surface);
  if (!ok) {
    m.scene.errorBuffer = 'バックバッファをレンダリングターゲットに設定できません．';
    return;
  }
  m.render.target = RENDER_TARGET_BACK_BUFFER;
}

/**
 * 0x33f 设置场景混合（CORE-OPS-RENDER-BLEND-33F）。
 *
 * 读序是 **2 → 3 →（按需）1 →（按需）1 → 1**：前两个总先读；第 1 个只在
 * 「透明度 < 0」/「颜色 < 0」两种回退时被读，最后写模式时再读一次。
 * - 归一化：透明度 > 255 ⇒ 255；< 0 ⇒ 编号 1 指向的**绘制项**当前颜色的透明度；
 *   颜色 < 0 ⇒ 该绘制项的当前颜色。该绘制项**不存在 ⇒ 无颜色哨兵**，⛔ 本条**不建**默认绘制项。
 * - 合成：颜色低 24 位 = 颜色（归一化后），高 8 位 = 透明度。
 * - 写两个格：模式 ← 第 1 个操作数**原样**（不归一、不夹取、不查表）；颜色 ← 合成结果。
 * ★ **不发外部请求**（⛔ 不调 RenderHost）、**不置脏标记 / animating**。
 * ★ 与场景 / 颜色族的共用口径的两处差别（spec 明写）：回退来源是绘制项（不是本条上次写的值），
 *   且不建默认绘制项。
 */
function blend33f(m: Machine, ops: Operand[]): void {
  let alpha = readInt(m, ops, 2);
  const color = readInt(m, ops, 3);
  /**
   * 回退来源：编号 1 指向的绘制项的当前颜色。
   * ★ 每次回退各自读一次第 1 个操作数（读序合同里是"（按需）1 →（按需）1"）；
   *   绘制项不存在 ⇒ 无颜色哨兵（⛔ 不建记录）。
   */
  const fallback = (): number => m.scene.drawItems.get(readInt(m, ops, 1))?.colorFrom ?? NO_COLOR;
  if (alpha > 255) alpha = 255;
  else if (alpha < 0) alpha = fallback() >>> 24;
  const c = color < 0 ? fallback() : color;
  const mode = readInt(m, ops, 1);
  m.render.blendMode = mode;
  m.render.blendColor = (((c & 0xffffff) | (alpha << 24)) >>> 0);
}

/**
 * `0x204` 第 4 个操作数按**文本**读：类型 5 的字节串、类型 2 的内联文本（先逐字节取反再解码）、
 * **数值型 ⇒ 十进制文本**。
 *
 * ★ spec 只给形态，**符号 / 宽度 / 浮点格式本版不规定**（`RES-RENDER-TEXT-ENCODING`）
 *   ⇒ 本层的选择（⛔ 不是取证过的格式）：
 *   - 整数族（0 / 3 / 6 / 9 / 12）：有符号 32 位十进制，负数带 `-`；
 *   - 浮点（1 / 4 / 7 / 10 / 13）：JS 的最短往返十进制（`String(v)` ⇒ `1.5`、`1e+21`），
 *     ⛔ 不是 `%f` 定宽格式；
 *   - 其余（8 / 11 / 14 与容器句柄）：本层没有文本读法 ⇒ **响亮失败**（⛔ 不补空串）。
 * ★ 这条"数值 → 十进制文本"口径与 `0x192` 的源侧是同一条，但本层只在 `0x204` 用它
 *   （`values.ts` 的 `readText` 不在本次改动范围）。
 */
function readText204(m: Machine, ops: Operand[], k: number): string {
  const op = ops[k - 1];
  if (!op) throw new Error(`operand-out-of-range:${k}/${ops.length}`);
  if (op.type === 5 || op.type === 2) return readText(m, ops, k);
  switch (op.type) {
    case 0: case 3: case 6: case 9: case 12:
      return String(readInt(m, ops, k));
    case 1: case 4: case 7: case 10: case 13:
      return String(readFloat(m, ops, k));
    default:
      throw new Error(`text-unsupported-operand-type:0x${op.type.toString(16)}`);
  }
}

/**
 * 0x204 离屏文字（CORE-OPS-RENDER-TEXT-204）。
 *
 * 读序 **3 → 2 → 4 → 1**（第 4 个按文本读，**先于**第 1 个）。
 * 三条入口条件都在**发请求之前**判，任一不满足 ⇒ 什么都不做（不发请求、不改引擎数据、不报错）：
 *   ① 槽里有纹理对象；② 该对象的一次只读询问返回非空（与 `0x20d` 取表面是**同一个询问**）；
 *   ③ 文本非空（判**首字节**）。
 * 请求：`host.drawText({ textureId, x, y, text, font, size, weight, color, shadowColor, drawMode, alignMode })`
 * —— 除坐标 / 文本之外全部来自**字体对象状态**，⛔ 不是操作数；一次同步绘制。
 * ★ 失败通道：`drawText` 返回 `false` = 加锁 / 取矩形失败（`0x46F342 call sub_4A3890` 的 LockTexture）。
 *   那时写错误通道（文本内容本版不规定）、**不**重建字形暂存区。
 *   ⚠ spec 正文说本条"没有引擎自己上报的失败分支"已被这条证据推翻 ⇒ **待 spec 修订**（本层不改 spec）。
 * ★ 本条不改纹理槽 / 绘制项 / 脏标记 / 池 / 帧 / 列表；字体对象的内部缓存与两个开关会被写
 *   （spec 的「引擎数据」：字形暂存区复用 + 两个布尔开关），本层只建模能确定的部分。
 */
async function drawText204(m: Machine, ops: Operand[]): Promise<void> {
  const y = readInt(m, ops, 3);
  const x = readInt(m, ops, 2);
  const text = readText204(m, ops, 4);
  const textureId = readInt(m, ops, 1);
  // 入口条件 ①：槽里有纹理对象（核心自己的表，不问宿主）
  if (!m.scene.textureObjects.has(textureId)) return;
  // 入口条件 ③：文本非空 —— 判首字节（⛔ 不是"去掉空白后为空"）。
  // ★ spec 没有规定三条条件的先后；本层把两条**本地**条件放在宿主询问之前，
  //   这样"没有对象 / 空文本"连一次外部询问都不会发生（严格满足"什么都不做"）。
  if (text.length === 0 || text.charCodeAt(0) === 0) return;
  // 入口条件 ②：只读询问（与 0x20d 取表面同一个询问）；取到 / 取不到都不建目标
  const host = renderHostOf(m);
  if (host === null) return;
  const surface = await host.getRenderSurface(textureId);
  if (surface === null) return;
  // ★ spec 说条件 ② 是**只读询问**、没规定要不要归还表面 ⇒ 本层不归还
  //   （`0x20d` 的"取到就归还"是那条自己的合同；本条只在询问，不持有引用）。
  const fontObject = m.render.fontObject;
  // 请求第 1 步：把一个内部布尔开关清 0（⛔ 不许在入口条件之前清）
  fontObject.switches[0] = 0;
  // 请求第 2–4 步（按 `renderState === 2` 选字体、取文本度量、两条绘制入口之一、
  // 加锁 / 逐字取字形写入 / 解锁）都在平台的 `drawText` 里：本层接口只表达
  // "把这段文本画进这个槽、用这套字体状态"。
  // ★ 字体 / 字号 / 字重 / 主色 / 副色 / 绘制模式 / 对位模式全部取自字体对象状态：
  //   face name（0x1a5 → Font+0x4EC）、size（0x75 → Font+0x313D4）、weight（0x2BD → 700/0）、
  //   color（Font+0x550，默认 0xFFFFFF）、shadowColor（Font+0x554）、drawMode（Font+0x55C）、
  //   alignMode（Font+0x313D0）。
  const drawn = await host.drawText({
    textureId,
    x,
    y,
    text,
    font: fontObject.font,
    size: fontObject.size,
    weight: fontObject.weight,
    color: fontObject.color,
    shadowColor: fontObject.shadowColor,
    drawMode: fontObject.drawMode,
    alignMode: fontObject.alignMode,
  });
  if (!drawn) {
    // 平台侧加锁 / 取矩形失败（`0x46F342` 的 LockTexture 失败）：写错误通道，⛔ 不重建字形暂存区。
    // ★ 文本内容本版不规定（`RES-RENDER-TARGET-TEXT` 的同一条口径），但必须非空、不得静默。
    m.scene.errorBuffer = 'LockTexture エラー';
    return;
  }
  // 引擎数据：字形暂存区被复用（布局属机器细节 ⇒ 只记"重建过"，不建假缓冲）
  fontObject.glyphCacheRebuilds += 1;
  // 第二个内部布尔开关的写入值本版不规定 ⇒ ⛔ 不猜，记一条诊断
  m.diagnostics.push({ kind: 'render-text-font-switch-unmodeled' });
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
 * 0x1ca：把操作数写进**配置库的命名键** `message:ReadTextSkip`（CORE-OPS-FORWARD-MESSAGE）。
 * 逐字：`(*((vtable+12)))(&Engine+0xAA514, "message:ReadTextSkip", op1)` —— 返回值交分派器（被丢弃）。
 * ★ `Engine+0xAA514` 是命名键存储（同一对象的 `+4` 读、`+0xC` 写；`0xae` 读 `set:SaveVersion*` 也走它）；
 *   键在 `config-keys.json` 的 ini + persist 词表里 ⇒ 这是配置写，不是文本渲染。
 * ★ 文本侧怎么消费这个键属 `0x71`，本层不碰。
 */
function message1ca(m: Machine, ops: Operand[]): void {
  // 0x1ca 不是空转发：它写**配置库的命名键** `message:ReadTextSkip`（该键在
  // config-keys.json 的 ini + persist 词表里）。文本侧怎么用这个键属 0x71，本层不碰。
  requireConfig(m).setInt('message:ReadTextSkip', readInt(m, ops, 1));
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
  const slot = listGroupOf(m, readInt(m, ops, 1));
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

/** 组号 0 = 用默认组（`0x80` 的选择子；初值 1） */
function listGroupOf(m: Machine, slot: number): number {
  return slot === 0 ? m.ui.defaultGroup : slot;
}

/** 按组号取记录；组号域 0..9 由构造期预建，域外是表读越界（响亮失败） */
function listRecordOf(m: Machine, slot: number): ListRecord {
  const found = m.ui.records.get(slot);
  if (!found) throw new Error(`list-group-out-of-domain:${slot}`);
  return found;
}

/**
 * 0x71 文本转发（CORE-OPS-LIST-TEXT-REQUEST）。
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

/** 0x80：把操作数原样写进列表 / 消息子系统的默认组选择子（初值 1） */
function setDefaultGroup80(m: Machine, ops: Operand[]): void {
  m.ui.defaultGroup = readInt(m, ops, 1);
}

/**
 * 0x73 写列表记录十格 + 一个固定格 + 可选第二请求（CORE-OPS-LIST-SUBSYSTEM-SET73，argc 10）。
 *
 * 逐字（`sub_41F250`）：
 * ① **先读第 10 个操作数**，写进固定格 `Engine+0x69220`（dword 107656）：值为 0 时写成 **1**；
 * ② 再**倒序**读第 9…1 个操作数，`sub_456430` 把它们按固定公式写进该组记录的 `+0x3C..+0x60`
 *    —— 这是**引擎数据写**，不是转发；
 * ③ 由引擎级 dword `Engine+0xA30D0`（= `set:DrawMode` 的缓存，初值来自配置）决定是否再发一次
 *    `vtable+4` 请求（实参 0x21 / 2×op7 / op8 / 2）。给了配置库（安装入口）时该格在构造期播种 ⇒
 *    `gateModeled: true`；`--script` / 单元测试没有 config ⇒ 未建模，请求照发并标 false。
 */
function forward73(m: Machine, ops: Operand[]): void {
  const op10 = readInt(m, ops, 10);
  m.scalars.set('Engine.d107656', op10 === 0 ? 1 : op10);
  const args: number[] = [];
  for (const k of [9, 8, 7, 6, 5, 4, 3, 2, 1]) args.push(readInt(m, ops, k));
  args.reverse();
  // 第 2 步不是转发：`sub_456430` 把 op1..op9 按固定公式写进该组记录的十格
  // （`60-ops-list.md` CORE-OPS-LIST-SUBSYSTEM-SET73；逐字公式见台账 01M5FK3XR…）。
  const rec = listRecordOf(m, listGroupOf(m, args[0]));
  rec.f3c = args[3];
  rec.f40 = args[4];
  rec.f44 = args[5];
  rec.f48 = (args[4] + args[6]) | 0;
  rec.f4c = (args[5] + args[7]) | 0;
  rec.f50 = args[1];
  rec.f54 = args[2];
  rec.f58 = 1;
  rec.f5c = args[8];
  rec.f60 = args[8];
  const gate = m.scalars.get('Engine.d166964') ?? 0;
  if (gate === 0) {
    m.effects.push({
      kind: 'vtable.request',
      detail: { callee: 'vtable+4', message: 0x21, a: 2 * args[6], b: args[7], c: 2, gateSlot: 'Engine.d166964', gateModeled: m.scalars.has('Engine.d166964') },
    });
  }
}

/**
 * 0x78 / 0x2db：把第 1 个操作数写进**字体对象的一个固定字段**，然后**尾跳**进字体重建入口
 * （CORE-OPS-LIST-SUBSYSTEM-SET-TAIL）。
 *
 * 逐字：两条同形 —— `0x78` 写 `Engine+0x14D30+0x55C`（绘制模式）；`0x2db` 写同一个字体对象的
 * `+0x313D0`（对位模式）；然后 `jmp sub_459F40`（尾跳：重建入口的返回值直接回给分派器）。
 * ★ 字段的领域用途已定（见 `60-ops-render.md` 的 0x204）；重建入口的其余内部行为未建模。
 */
function setFieldTail(m: Machine, ops: Operand[], field: string, apply: (value: number) => void): void {
  const value = readInt(m, ops, 1);
  m.scalars.set(field, value);
  // 字体对象的两个格：0x78 写绘制模式、0x2db 写对位模式（证据：0x41F476 / 0x426526）。
  apply(value);
  m.effects.push({ kind: 'subsystem.request', detail: { callee: 'sub_459F40', field } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_459F40' });
}

/** 0x78：写字体对象的绘制模式 +0x55C 后尾跳 */
function field78(m: Machine, ops: Operand[]): void {
  setFieldTail(m, ops, 'Engine.d21667', (value) => {
    m.render.fontObject.drawMode = value;
    m.render.fontObject.drawPath = value;
  });
}

/** 0x2db：写字体对象的对位模式 +0x313D0 后尾跳（与 0x78 同形，只差落点） */
function field2db(m: Machine, ops: Operand[]): void {
  setFieldTail(m, ops, 'Engine.d71744', (value) => {
    m.render.fontObject.alignMode = value;
  });
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
  const slot = listGroupOf(m, readInt(m, ops, 1));
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
  const slot = listGroupOf(m, readInt(m, ops, 1));
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
async function callScript3(m: Machine, ops: Operand[]): Promise<void> {
  const id = readInt(m, ops, 1);
  if (!m.loadScript) throw new Error('call-script-loader-missing');
  const child = await m.loadScript(id);
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
async function loadFrame6(m: Machine, ops: Operand[]): Promise<void> {
  const id = readInt(m, ops, 1);
  const slot = readInt(m, ops, 2);
  if (!m.loadScript) throw new Error('load-frame-loader-missing');
  // ★ key 非 0 是引擎的全局不变量（40-pools；EA 0x418940 / 0x41C7F2），不是 0x6 的局部前置：
  //   宿主给 0 的记账在 `Machine` 构造期一次完成，这里不重复记（否则每条 0x6 刷一条）。
  if (slot >= FRAME_SLOT_COUNT) {
    throw new Error(`load-frame：目标帧深 ${slot} ≥ ${FRAME_SLOT_COUNT} —— 引擎抛「ファイルの階層が深すぎます．最大は%dです．」`);
  }
  const body = await m.loadScript(id);
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
async function exitScript9(m: Machine): Promise<void> {
  if (!m.loadScript) throw new Error('call-script-loader-missing');
  // ★ 引擎在重装根脚本**之前**清 show-logo（`sub_428A60`：0x428A75 写 0，0x428D80 才 reload）。
  //   两者之间没有读者 ⇒ 先后不可观察；但失败路径上旗标值不同，代码照站点顺序写。
  m.showLogo = 0;
  const root = await m.loadScript(0);
  if (!root) throw new Error('call-script-load-failed:0');
  m.slots = [{
    id: 0,
    name: m.scriptName?.(0) ?? m.rootName,
    body: root,
    ip: 0,
    frame: m.freshFrame(0),
    parent: -1,
    length: 0,
    // ★ 重装的是**根脚本** ⇒ 表仍是 `rootTables`（`0x9` 的 id 恒 0，但表来源不是 `scriptTables(0)`）。
    tables: m.rootTables,
  }];
  m.cur = 0;
  m.operandCount = 0;
  // 复位：状态选择字归 0；三个全局池清回初值（整型池 decode 后为 0、浮点池 0、字符串池空）
  // ★ show-logo 标志已在 reload 之前清 0（见 `0x428A75`）：重跑的根脚本经 `0x130` 读到 0。
  // ★ 两份缓冲（SPC-POOL-MEMFLIP）下"是否连备用组一起复位"本版未观察 ⇒ 只清**当前选中**的那一组
  //   （备用组留原值）。这是实现口径、**不是规范合同**（spec 未规定）。
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

/* ──────────────────────────── 全局池的两份缓冲（40-pools.md） ──────────────────────────── */

/**
 * 0x6d memflip（SPC-POOL-MEMFLIP）：把六个全局池族的主 / 备选择**整体对调**。
 *
 * - 推进格数**先**写成 1，**然后**才查配置 ⇒ 异常路径上它已经是 1（spec 明写的先后）。
 * - 配置命名键 `set:EnableMemFlip`（整数键；缺失按 `55-config.md` 的缺键规则读 0）：
 *   为 0 或缺键 ⇒ 引擎抛**显示消息型异常**（「memflipは現在使用不可になっています．」），六个选择不变。
 * - 非 0 ⇒ 对调。对调**只改选择**：不搬数据、不重新分配、不改变任何计数、不改变元素宽度。
 * ★ 本实现的"两份缓冲"= 地址空间里两组窗口（`address.ts`）+ 一个共享选择量（`Machine.globalFlip`）；
 *   备用组是否存在由**启动期**同一个键决定。若该键在启动后被改写（spec 归为 UB），翻页本身按
 *   执行期读到的键成功，但随后的访问落到**未映射**的地址 ⇒ 池访问器响亮失败
 *   （⛔ 不在翻页时补分配来掩盖 —— spec 明写不许）。
 */
function memFlip6d(m: Machine): void {
  m.operandCount = 1;
  const enabled = m.config ? m.config.getInt('set:EnableMemFlip') : 0;
  if (enabled === 0) {
    throw new Error('display-message: memflipは現在使用不可になっています．');
  }
  m.globalFlip = m.globalFlip === 0 ? 1 : 0;
  m.effects.push({ kind: 'pool.memflip', detail: { flip: m.globalFlip } });
}

/* ──────────────────────────── 等待 / 输入 ──────────────────────────── */

/** 0x21c wait：置等待位（阻塞在门里，不在 handler 内）（CORE-OPS-WAIT） */
function wait21c(m: Machine): void {
  m.flags |= WAIT_BIT;
}

/** 0x101 poll-input：采样并丢弃（CORE-OPS-WAIT-POLL-INPUT） */
function pollInput101(m: Machine): void {
  m.effects.push({ kind: 'input.poll', detail: {} });
  // 清状态选择字第 27 位（只清这一位；CORE-OPS-WAIT-POLL-INPUT）
  m.flags &= ~0x08000000;
  m.input.word = 0;
  m.inputWord = 0;
  m.input.samples += 1;
  m.scalars.set('pollInput.a', 1);
  m.scalars.set('pollInput.b', 0);
}

/** 0x20f 播影片：建/配置槽上的影片对象、置影片位与"有活动对象"（CORE-OPS-MOVIE） */
function movie20f(m: Machine, ops: Operand[]): void {
  const slot = readInt(m, ops, 2);
  let obj = m.scene.objectSlots.get(slot) as MovieObject | undefined;
  if (!obj || obj.kind !== 'movie') {
    obj = {
      kind: 'movie', slot, resourceId: 0, opt: 0, started: false,
      handle: null, loop: false, currentTime: 0, stopTime: 0,
    };
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
  const option = readInt(m, ops, 3);
  if (m.movieHost) {
    const handle = m.movieHost.open({ slot, resourceId: obj.resourceId, option });
    if (handle === null) throw new Error(`movie-open-failed:${obj.resourceId}`);
    obj.handle = handle;
    m.movieHost.play(handle);
  }
  obj.opt = option;
  obj.loop = (option & 1) !== 0;
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
 * 0x192 set-string（CORE-OPS-TEXT-SET）：把**第 2 个操作数**的文本写进**第 1 个操作数**的串池格。
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

/**
 * 0x63 lea：把**第 2 个操作数的地址**原样写进**第 1 个操作数的引用槽**（CORE-OPS-POOL-LEA-63）。
 * ★ 不编码：写的是地址本身；第 1 个必须是引用族，否则 `writePointerSlot` 抛类型异常。
 */
function lea63(m: Machine, ops: Operand[]): void {
  const addr = addressOfOperand(m, ops, 2);
  writePointerSlot(m, ops, 1, addr);
  m.effects.push({
    kind: 'array.lookup',
    detail: { opcode: '0x63', pool: `0x${ops[0].type.toString(16)}`, address: addr },
  });
}

/**
 * 0x60 取随机数（CORE-OPS-RANDOM-60）：计数器 +1（超过 12 归 0）→ 取一次宿主随机数 → 读范围。
 * 范围 = 0 ⇒ 先把落点写 0，再抛显示消息型异常；否则落点 ← 随机数（有符号）对范围取余。
 */
function random60(m: Machine, ops: Operand[]): void {
  const range = readInt(m, ops, 2) | 0;
  m.randomCounter = (m.randomCounter + 1) % 13;
  const bits = Math.floor((m.random() % 1) * 0x100000000) >>> 0;
  if (range === 0) {
    writeInt(m, ops, 1, 0);
    throw new Error('random: 范围 0 —— 引擎抛显示消息型异常（落点已写 0）');
  }
  const value = ((bits | 0) % range) | 0;
  writeInt(m, ops, 1, value);
  m.effects.push({
    kind: 'engine.scalar.write',
    detail: { opcode: '0x60', slot: 'randomCounter', counter: m.randomCounter, value },
  });
}

/** 0x74：读整型(第 1 个) → 引擎级参数格（不归一、不夹取）。 */
function param74(m: Machine, ops: Operand[]): void {
  m.paramCell = readInt(m, ops, 1);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x74', slot: 'paramCell', value: m.paramCell } });
}

/** 0x7f：引擎级参数格 → 写整型(第 1 个)；未写过 ⇒ 响亮失败（规范说初值未观察，不伪造 0）。 */
function param7f(m: Machine, ops: Operand[]): void {
  if (m.paramCell === null) throw new Error('param-cell-uninitialized: 0x7f 先于 0x74');
  writeInt(m, ops, 1, m.paramCell);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x7f', slot: 'paramCell', value: m.paramCell } });
}

/** 0xd0：自己读一次宿主毫秒时钟 → 写整型(第 1 个)（不复用主循环缓存的那次读数）。 */
function clockD0(m: Machine, ops: Operand[]): void {
  const now = Math.trunc(m.clock ? m.clock() : m.now) | 0;
  writeInt(m, ops, 1, now);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0xd0', slot: 'clock', value: now } });
}

/** 命名表的键按**文本**读：字符串族走 `readText`；整型族按十进制文本（规范口径）。 */
function readKeyText(m: Machine, ops: Operand[], k: number): string {
  const op = ops[k - 1];
  if (op.type === 3 || op.type === 9) return String(readInt(m, ops, k));
  return readText(m, ops, k);
}

/** 0xa1：按 4095 个槽重置命名表（计数 0、键清空）。本层用 Map ⇒ 直接 clear。 */
function tableResetA1(m: Machine): void {
  m.namedTable.clear();
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0xa1', slot: 'namedTable', size: 4095 } });
}

/** 0xa2：读值(第 2 个) → 读键(第 1 个，按文本) → 写入命名表（覆盖不增计数）。 */
function tableBindA2(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 2);
  const key = readKeyText(m, ops, 1);
  m.namedTable.set(key, value);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0xa2', slot: 'namedTable', key, value } });
}

/** 0xa3：按键查命名表；命中 ⇒ ip ← 4×登记值；未命中才读第 2 个（默认位置）。两支都清推进格数。 */
function tableDispatchA3(m: Machine, ops: Operand[]): void {
  const key = readKeyText(m, ops, 1);
  const target = m.namedTable.has(key) ? m.namedTable.get(key)! : readInt(m, ops, 2);
  m.ip = 4 * target;
  m.operandCount = 0;
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0xa3', slot: 'namedTable', key, target } });
}

/** 0x191 浮点绝对值：op1（写浮点）← |读浮点(op2)|。 */
function floatAbs191(m: Machine, ops: Operand[]): void {
  writeFloat(m, ops, 1, Math.abs(readFloat(m, ops, 2)));
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x191', slot: 'float-abs' } });
}

/** 0x2d0 浮点加：op1 ← 读浮点(op2) + 读浮点(op3)。 */
function floatAdd2d0(m: Machine, ops: Operand[]): void {
  writeFloat(m, ops, 1, readFloat(m, ops, 2) + readFloat(m, ops, 3));
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x2d0', slot: 'float-add' } });
}

/** 0x2d1 浮点减：op1 ← 读浮点(op2) − 读浮点(op3)。 */
function floatSub2d1(m: Machine, ops: Operand[]): void {
  writeFloat(m, ops, 1, readFloat(m, ops, 2) - readFloat(m, ops, 3));
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x2d1', slot: 'float-sub' } });
}

/** 0x2d3 浮点除：op1 ← 读浮点(op2) ÷ 读浮点(op3)。 */
function floatDiv2d3(m: Machine, ops: Operand[]): void {
  writeFloat(m, ops, 1, readFloat(m, ops, 2) / readFloat(m, ops, 3));
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x2d3', slot: 'float-div' } });
}

/** 0x1fd 缩放：op2..4 ÷ 100 写缩放矩阵（矩阵 0）。 */
function sceneScale1fd(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const x = readFloat(m, ops, 2) / 100;
  const y = readFloat(m, ops, 3) / 100;
  const z = readFloat(m, ops, 4) / 100;
  const item = drawItemOf(m, id);
  const mat = identity4();
  mat[0] = x; mat[5] = y; mat[10] = z;
  item.matrices[0] = mat;
  item.config = 1;
  m.scene.dirty = 1;
}

/** 0x1ff 平移：op2..4 原值写平移矩阵（矩阵 4）。 */
function sceneTranslate1ff(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const x = readFloat(m, ops, 2);
  const y = readFloat(m, ops, 3);
  const z = readFloat(m, ops, 4);
  const item = drawItemOf(m, id);
  const mat = identity4();
  mat[12] = x; mat[13] = y; mat[14] = z;
  item.matrices[4] = mat;
  item.config = 1;
  m.scene.dirty = 1;
}

/** 0x1fe 旋转：op2..4 轴、op5 角度（度），写旋转矩阵（矩阵 2）并另存轴 / 角。 */
function sceneRotate1fe(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const ax = readFloat(m, ops, 2);
  const ay = readFloat(m, ops, 3);
  const az = readFloat(m, ops, 4);
  const deg = readFloat(m, ops, 5);
  const len = Math.hypot(ax, ay, az) || 1;
  const x = ax / len, y = ay / len, z = az / len;
  const rad = (deg * Math.PI) / 180;
  const c = Math.cos(rad), s = Math.sin(rad), t = 1 - c;
  const mat = identity4();
  mat[0] = t * x * x + c; mat[1] = t * x * y + s * z; mat[2] = t * x * z - s * y;
  mat[4] = t * x * y - s * z; mat[5] = t * y * y + c; mat[6] = t * y * z + s * x;
  mat[8] = t * x * z + s * y; mat[9] = t * y * z - s * x; mat[10] = t * z * z + c;
  const item = drawItemOf(m, id);
  item.matrices[2] = mat;
  item.vec2 = [ax, ay, az];
  item.config = 1;
  m.scene.dirty = 1;
}

/** 0x220：先建默认记录；位 0 为 0 ⇒ 只留记录；否则 G0..G3 + 第四个平移矩阵 + 脏 / 动画。 */
function sceneSetTransform220(m: Machine, ops: Operand[]): void {
  const fz = readFloat(m, ops, 6);
  const fy = readFloat(m, ops, 5);
  const fx = readFloat(m, ops, 4);
  const op3 = readInt(m, ops, 3) | 0;
  const op2 = readInt(m, ops, 2) | 0;
  const id = readInt(m, ops, 1) | 0;
  const item = drawItemOf(m, id);
  if ((item.flags & 1) === 0) return;
  item.flags |= 2;
  item.g0 = 0;
  item.g1 = op2;
  item.g2 = op3;
  item.config = 1;
  const mat = identity4();
  mat[12] = fx; mat[13] = fy; mat[14] = fz;
  item.matrices[5] = mat;
  m.scene.dirty = 1;
  m.scene.animating = 1;
}

/** 0x228：查记录（不建）；读平移矩阵（矩阵 4）的平移分量，状态 0 成功 / 1 失败。 */
function sceneGetLocalTrans228(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 2) | 0;
  const item = m.scene.drawItems.get(id);
  if (!item) {
    writeInt(m, ops, 1, 1);
    return;
  }
  const mat = item.matrices[4];
  writeFloat(m, ops, 3, mat[12]);
  writeFloat(m, ops, 4, mat[13]);
  writeFloat(m, ops, 5, mat[14]);
  writeInt(m, ops, 1, 0);
}

/** 场景 setter 的字段格：偏移只来自 bindings，正文不写。 */
function setField(item: DrawItem, offset: number, value: number): void {
  item.fields.set(offset, value);
}

function scaleMatrix(x: number, y: number, z: number): number[] {
  const m = identity4();
  m[0] = x; m[5] = y; m[10] = z;
  return m;
}

function translateMatrix(x: number, y: number, z: number): number[] {
  const m = identity4();
  m[12] = x; m[13] = y; m[14] = z;
  return m;
}

function rotateMatrix(ax: number, ay: number, az: number, deg: number): number[] {
  const len = Math.hypot(ax, ay, az) || 1;
  const x = ax / len, y = ay / len, z = az / len;
  const rad = (deg * Math.PI) / 180;
  const c = Math.cos(rad), s = Math.sin(rad), t = 1 - c;
  const m = identity4();
  m[0] = t * x * x + c; m[1] = t * x * y + s * z; m[2] = t * x * z - s * y;
  m[4] = t * x * y - s * z; m[5] = t * y * y + c; m[6] = t * y * z + s * x;
  m[8] = t * x * z + s * y; m[9] = t * y * z - s * x; m[10] = t * z * z + c;
  return m;
}

/** 0x21e：带位 0 门的一对整数 + 缩放矩阵。 */
function setter21e(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const a = readInt(m, ops, 2) | 0;
  const b = readInt(m, ops, 3) | 0;
  const x = readFloat(m, ops, 4) / 100;
  const y = readFloat(m, ops, 5) / 100;
  const z = readFloat(m, ops, 6) / 100;
  const item = drawItemOf(m, id);
  if ((item.flags & 1) === 0) return;
  item.flags |= 2;
  item.config = 1;
  setField(item, 0x34, 0);
  setField(item, 0x3c, a);
  setField(item, 0x50, b);
  item.extraMatrices.set(0xac, scaleMatrix(x, y, z));
  m.scene.dirty = 1;
  m.scene.animating = 1;
}

/** 0x21f：带位 0 门的一对整数 + 轴角 + 旋转矩阵。 */
function setter21f(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const a = readInt(m, ops, 2) | 0;
  const b = readInt(m, ops, 3) | 0;
  const ax = readFloat(m, ops, 4);
  const ay = readFloat(m, ops, 5);
  const az = readFloat(m, ops, 6);
  const deg = readFloat(m, ops, 7);
  const item = drawItemOf(m, id);
  if ((item.flags & 1) === 0) return;
  item.flags |= 2;
  item.config = 1;
  setField(item, 0x34, 0);
  setField(item, 0x40, a);
  setField(item, 0x54, b);
  setField(item, 0x1f8, ax);
  setField(item, 0x1fc, ay);
  setField(item, 0x200, az);
  setField(item, 0x208, deg);
  item.extraMatrices.set(0x12c, rotateMatrix(ax, ay, az, deg));
  m.scene.dirty = 1;
  m.scene.animating = 1;
}

/** 0x230：清附加组开关位与六个共用格；不标脏。 */
function setter230(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const item = drawItemOf(m, id);
  item.flags &= ~4;
  for (const off of [0x21c, 0x220, 0x224, 0x228, 0x22c, 0x230]) setField(item, off, 0);
}

/** 0x231：置附加组开关位 + 三个整数 + 清格；标脏。 */
function setter231(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const a = readInt(m, ops, 2) | 0;
  const b = readInt(m, ops, 3) | 0;
  const c = readInt(m, ops, 4) | 0;
  const item = drawItemOf(m, id);
  item.flags |= 4;
  setField(item, 0x21c, 0);
  setField(item, 0x230, a);
  setField(item, 0x238, b);
  setField(item, 0x23c, c);
  m.scene.dirty = 1;
}

/** 0x233：置附加开关位 + 值格 + 缩放矩阵；标脏。 */
function setter233(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const a = readInt(m, ops, 2) | 0;
  const x = readFloat(m, ops, 3) / 100;
  const y = readFloat(m, ops, 4) / 100;
  const z = readFloat(m, ops, 5) / 100;
  const item = drawItemOf(m, id);
  item.flags |= 4;
  setField(item, 0x210, 0);
  setField(item, 0x224, a);
  item.extraMatrices.set(0x250, scaleMatrix(x, y, z));
  m.scene.dirty = 1;
}

/** 0x234：置附加开关位 + 值格 + 三个浮点格；标脏。 */
function setter234(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const a = readInt(m, ops, 2) | 0;
  const x = readFloat(m, ops, 3);
  const y = readFloat(m, ops, 4);
  const z = readFloat(m, ops, 5);
  const item = drawItemOf(m, id);
  item.flags |= 4;
  setField(item, 0x214, 0);
  setField(item, 0x228, a);
  setField(item, 0x244, x);
  setField(item, 0x248, y);
  setField(item, 0x24c, z);
  m.scene.dirty = 1;
}

/** 0x235：置附加开关位 + 值格 + 平移矩阵；标脏。 */
function setter235(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const a = readInt(m, ops, 2) | 0;
  const x = readFloat(m, ops, 3);
  const y = readFloat(m, ops, 4);
  const z = readFloat(m, ops, 5);
  const item = drawItemOf(m, id);
  item.flags |= 4;
  setField(item, 0x218, 0);
  setField(item, 0x22c, a);
  item.extraMatrices.set(0x290, translateMatrix(x, y, z));
  m.scene.dirty = 1;
}

function cloneDrawItem(src: DrawItem): DrawItem {
  return {
    ...src,
    aux: [...src.aux], vec1: [...src.vec1], vec2: [...src.vec2],
    matrices: src.matrices.map((m) => [...m]),
    fields: new Map(src.fields),
    extraMatrices: new Map([...src.extraMatrices].map(([k, m]) => [k, [...m]])),
    unknown: new Map(src.unknown),
    // ★ 具名字段也要深拷：否则副本与源共享同一个 `sourceWindow` 对象（`0x239` 写副本会改到源）
    sourceWindow: src.sourceWindow === null ? null : { ...src.sourceWindow },
  };
}

/** 0x21d：按源编号在三张表逐表整份复制；三张都无 ⇒ 错误通道且不改。 */
function copy21d(m: Machine, ops: Operand[]): void {
  const dst = readInt(m, ops, 2) | 0;
  const src = readInt(m, ops, 1) | 0;
  let copied = false;
  const obj = m.scene.drawItems.get(src);
  if (obj) {
    m.scene.drawItems.set(dst, cloneDrawItem(obj));
    copied = true;
  }
  const mesh = m.scene.meshes.get(src);
  if (mesh) {
    m.scene.meshes.set(dst, { ...mesh, copies: mesh.copies.map((a) => [...a]) as Mesh['copies'], colors: [...mesh.colors], vbo: [...mesh.vbo], unknown: new Map(mesh.unknown) });
    copied = true;
  }
  for (const table of m.scene.auxTables) {
    if (table.has(src)) {
      table.set(dst, table.get(src));
      copied = true;
    }
  }
  if (!copied) {
    m.diagnostics.push({ kind: 'scene-copy-source-missing', index: src });
    return;
  }
  m.scene.dirty = 1;
}

/** 0x242：同一个整数写进对象记录与第二张表（第二张表的身份未取证，先按同编号记录记）。 */
function setter242(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const v = readInt(m, ops, 2) | 0;
  const item = drawItemOf(m, id);
  setField(item, 0x2d0, v);
  m.scene.aux242.set(id, v);
  // 不标脏、不置位；建默认记录那一步是否间接标脏见正文 / 残差。
}

/**
 * 0x239：带位 0 门的一对整数 + 附加组三个整数；标脏 + 动画。
 *
 * 过门后置**首字第 1 位**（"有窗"，与颜色窗同一位）、把**窗起点**清 0
 * （`item.start` 是那个共用起点；`fields[0x34]` 保留同一格的按偏移视图），
 * 再把五格写进**源矩形窗口**（`CORE-OPS-TEXTURE-DRAW-CONSUMER` 4.1）：
 * op2/op3 = 延迟 / 持续时间；op4/op5/op6 = 步进总数 / 每行步数 / 开关字。
 * ★ 后三格与 `0x231` 重叠（真实复用）：两条指令写的是**同一份格**。
 */
function setter239(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const a = readInt(m, ops, 2) | 0;
  const b = readInt(m, ops, 3) | 0;
  const c = readInt(m, ops, 4) | 0;
  const d = readInt(m, ops, 5) | 0;
  const e = readInt(m, ops, 6) | 0;
  const item = drawItemOf(m, id);
  if ((item.flags & 1) === 0) return;
  item.flags |= 2;
  item.start = 0;
  setField(item, 0x34, 0);
  setField(item, 0x48, a);
  setField(item, 0x5c, b);
  setField(item, 0x238, c);
  setField(item, 0x23c, d);
  setField(item, 0x234, e);
  item.sourceWindow = { delay: a, duration: b, steps: c, perRow: d, switchWord: e };
  m.scene.dirty = 1;
  m.scene.animating = 1;
}

/**
 * 0x229：**先重置场景自身状态**，再写作用键区间与三元平移（`CORE-OPS-SCENE-SELF-TRANSFORM-229`）。
 *
 * 读序按证据：先 op2（计数）再 op1（起点），然后三个浮点；重置在**读之前**（证据里的调用次序）。
 * ★ 重置**不许漏**：它同时清掉上一次的作用键区间（否则连写两条会残留）。
 * ★ 只重置被建模的那几格（平移 + 区间）；记录其余格本层不建模。
 */
function sceneSelfTransform229(m: Machine, ops: Operand[]): void {
  m.scene.selfRecord = { translate: { x: 0, y: 0, z: 0 }, keyStart: 0, keyCount: 0 };
  m.scene.dirty = 1;
  const count = readInt(m, ops, 2) | 0;
  const start = readInt(m, ops, 1) | 0;
  m.scene.selfRecord.keyStart = start;
  m.scene.selfRecord.keyCount = count;
  const x = readFloat(m, ops, 3);
  const y = readFloat(m, ops, 4);
  const z = readFloat(m, ops, 5);
  m.scene.selfRecord.translate = { x, y, z };
  m.scene.dirty = 1;
}

/** 0x217 / 0x219 写三元向量：读序 2→3→4→1；缺记录先建默认记录。 */
function sceneVecSet(m: Machine, ops: Operand[], which: 'vec1' | 'vec2'): void {
  const z = readFloat(m, ops, 4);
  const y = readFloat(m, ops, 3);
  const x = readFloat(m, ops, 2);
  const id = readInt(m, ops, 1) | 0;
  const item = drawItemOf(m, id);
  item[which] = [x, y, z];
  m.scene.dirty = 1;
  m.effects.push({ kind: 'scene.vec.write', detail: { opcode: which === 'vec1' ? '0x217' : '0x219', id } });
}

/** 0x218 / 0x21a 读三元向量：查不到写 0，且**不建记录**。 */
function sceneVecGet(m: Machine, ops: Operand[], which: 'vec1' | 'vec2'): void {
  const id = readInt(m, ops, 1) | 0;
  const item = m.scene.drawItems.get(id);
  const v = item?.[which] ?? [0, 0, 0];
  writeFloat(m, ops, 2, v[0]);
  writeFloat(m, ops, 3, v[1]);
  writeFloat(m, ops, 4, v[2]);
}

/** 0x258：按编号写两张 1000 项表的两个位（掩码只取低两位；越界是 UB ⇒ 抛）。 */
function sceneTableFlags258(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  const mask = readInt(m, ops, 2) | 0;
  if (id < 0 || id >= 1000) throw new Error(`0x258 编号 ${id} 越界（两张表各 1000 项）`);
  m.scene.flagTableA[id] = mask & 1;
  m.scene.flagTableB[id] = (mask >>> 1) & 1;
  m.effects.push({ kind: 'scene.table.flags', detail: { opcode: '0x258', id, bits: mask & 3 } });
}

/** 0x259：两张 1000 项表的这些标志全清 0。 */
function sceneTableFlags259(m: Machine): void {
  m.scene.flagTableA.fill(0);
  m.scene.flagTableB.fill(0);
  m.effects.push({ kind: 'scene.table.flags', detail: { opcode: '0x259', id: 'all', bits: 0 } });
}

/**
 * 0x2da：**先读 op1 并判上限**（`0x426447 cmp edi,0Ah; ja`）；越界 ⇒ 错误通道，
 * 其余七个操作数**一次都不读**；否则存七个整数格。
 * ★ 引擎是**无符号** `ja`：负数与 >10 走同一条错误路径（同一文本），不是未定义行为。
 */
function cgSet2da(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  if (id < 0 || id > 10) {
    m.scene.errorBuffer = 'CGNumberの引数が不正です．\r\n';
    m.diagnostics.push({ kind: 'cg-id-out-of-range', index: id });
    return;
  }
  const vals: number[] = [];
  for (let k = 2; k <= 8; k += 1) vals.push(readInt(m, ops, k) | 0);
  m.scene.cgRecords.set(id, vals);
  m.effects.push({ kind: 'scene.cg.write', detail: { opcode: '0x2da', id } });
}

/** 删 `[key, key+count)` 的键（`0x23b` 的旧项清理；引擎走 `sub_4ABB60`）。 */
function deleteKeyRange<T>(table: Map<number, T>, key: number, count: number): number {
  if (count <= 0) return 0;
  let removed = 0;
  for (const k of [...table.keys()]) {
    if (k >= key && k < key + count) {
      table.delete(k);
      removed += 1;
    }
  }
  return removed;
}

/**
 * 0x23b：按 CG 数字条画数值（`sub_424970`，读序 2 → 1 → 3 → 6 → 7 → 6 →（清旧项）→ 每个位 4 → 5）。
 *
 * ★ 为什么这条要建模：TITLE 的 `Version 0.00.0000` 的**七位数字**就是三条 `0x23b`
 *   （`i23b 6f/70/72`，位数 1/2/4）画出来的；`draw-texture 76/77` 只是两个小数点。
 *   把它留在 ADV 文字白名单里会让版本号只剩两个点（正是本轮缺陷的画面）。
 * ★ 记录来自 `0x2da`（已建模的七格：[纹理槽,x0,y0,单字宽,字高,字内空隙,字距]）；
 *   编号 > 0x0a 或记录不存在 / 纹理槽为 0 ⇒ 引擎只写错误缓冲并返回（不读其余操作数、不建项）。
 * ★ 几何在纯函数 `cgDigitSlots` 里（可单测）；这里只做操作数读取、旧项删除与写场景。
 * ★ 读序是合同：2 → 1 → 3 → 6 → 7 → 6 →（删四张同键表）→ **每个被画出的位**：4 → 5 → 建项。
 *   位数 ≤ 0 ⇒ 计划为空 ⇒ op4 / op5 一次都不读（非法类型也不会被碰到）。
 */
function cgDraw23b(m: Machine, ops: Operand[]): void {
  const recordId = readInt(m, ops, 2);
  const record = recordId >= 0 && recordId <= 0x0a ? m.scene.cgRecords.get(recordId) : undefined;
  if (record === undefined || (record[0] ?? 0) === 0) {
    m.scene.errorBuffer = 'CGNumberの引数が不正です．\r\n';
    m.diagnostics.push({ kind: 'cg-number-invalid', index: recordId });
    return;
  }
  const startId = readInt(m, ops, 1);
  const value = readInt(m, ops, 3);
  const digits = readInt(m, ops, 6);
  const flags = readInt(m, ops, 7);
  const cleanupLength = readInt(m, ops, 6); // 引擎在这里再读一次位数（读序是合同）
  // 引擎先删 [id, id+cleanupLength) 的旧条项（sub_4ABB60：对象记录 / 网格 / 两张辅助表），再逐位建项。
  let removed = 0;
  for (const table of sameKeyTables(m)) removed += deleteKeyRange(table, startId, cleanupLength);
  const plan = cgDigitSlots(record, startId, value, digits, flags);
  // 每个被画出的位各读一次 op4 / op5（与引擎建项分支一致；位数 ≤ 0 ⇒ 循环不执行）。
  for (const slot of plan) {
    const x = readInt(m, ops, 4);
    const y = readInt(m, ops, 5);
    const draw = defaultItem();
    draw.flags |= 1;
    draw.textureId = slot.textureId;
    draw.left = slot.srcLeft;
    draw.top = slot.srcTop;
    draw.right = slot.srcRight;
    draw.bottom = slot.srcBottom;
    draw.aux = [Math.fround(slot.dstXOffset + x), Math.fround(y), 0];
    m.scene.drawItems.set(slot.handle, draw);
  }
  if (plan.length > 0 || removed > 0) m.scene.dirty = 1;
  m.effects.push({
    kind: 'scene.cg.number',
    detail: { opcode: '0x23b', id: startId, record: recordId, value, digits, flags, items: plan.length },
  });
}

/** 0x1f6：清**四张**同键表（对象记录 / 网格 / 两张辅助）；脏标记清 0，动画标记不动。 */
function sceneClear1f6(m: Machine): void {
  for (const table of sameKeyTables(m)) table.clear();
  m.scene.dirty = 0;
  m.scene.hasObjects = 0;
  m.effects.push({ kind: 'scene.clear', detail: { opcode: '0x1f6' } });
}

/** 0x208 查询纹理尺寸：编号 > 999 ⇒ 错误通道 + 两个输出 0；槽空 ⇒ 0/0；有对象 ⇒ 逻辑宽高。 */
function textureSize208(m: Machine, ops: Operand[]): void {
  const id = readInt(m, ops, 1) | 0;
  if (id > 999) {
    writeInt(m, ops, 2, 0);
    writeInt(m, ops, 3, 0);
    m.diagnostics.push({ kind: 'texture-id-out-of-range', index: id });
    return;
  }
  const obj = m.scene.textureObjects.get(id);
  writeInt(m, ops, 2, obj?.width ?? 0);
  writeInt(m, ops, 3, obj?.height ?? 0);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x208', id, width: obj?.width ?? 0, height: obj?.height ?? 0 } });
}

/** 0x1b0 memcpy：取址(op1) → 取址(op2)，把源处 4×n 字节原样复制到目标处。 */
function memcpy1b0(m: Machine, ops: Operand[]): void {
  const src = addressOfOperand(m, ops, 1);
  const dst = addressOfOperand(m, ops, 2);
  const n = readInt(m, ops, 3) | 0;
  if (n > 0) for (let i = 0; i < n; i += 1) writeRawAt(m, dst, i, readRawAt(m, src, i));
  m.effects.push({ kind: 'array.copy', detail: { opcode: '0x1b0', from: src, to: dst, words: Math.max(0, n) } });
}

/** 0x1cf：读整型(op1) 原样覆盖状态字。 */
function statusWrite1cf(m: Machine, ops: Operand[]): void {
  m.statusWord = readInt(m, ops, 1) | 0;
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x1cf', slot: 'statusWord', value: m.statusWord } });
}

/** 0x1bf：第 16 位为 1 ⇒ 整字清 0；随后第 0 位为 0 ⇒ 相邻格写 1。 */
function statusConsume1bf(m: Machine): void {
  if ((m.statusWord & 0x10000) !== 0) m.statusWord = 0;
  if ((m.statusWord & 1) === 0) m.statusNeighbor = 1;
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x1bf', slot: 'statusWord', value: m.statusWord } });
}

/** 0x1cc：状态格原值写进 op1（不归一）。 */
function statusCellRead1cc(m: Machine, ops: Operand[]): void {
  writeInt(m, ops, 1, m.statusCell | 0);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x1cc', slot: 'statusCell', value: m.statusCell } });
}

/** 0x19a：格 X 的原值写进 op1。 */
function flagReadX19a(m: Machine, ops: Operand[]): void {
  writeInt(m, ops, 1, m.flagX | 0);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x19a', slot: 'flagX', value: m.flagX } });
}

/** 0x1b6：格 Y 归一成 0/1 后写进 op1。 */
function flagReadY1b6(m: Machine, ops: Operand[]): void {
  const v = m.flagY !== 0 ? 1 : 0;
  writeInt(m, ops, 1, v);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x1b6', slot: 'flagY', value: v } });
}

/** 0x1b7：op1 归一成 0/1 后写进格 Y。 */
function flagWriteY1b7(m: Machine, ops: Operand[]): void {
  m.flagY = readInt(m, ops, 1) !== 0 ? 1 : 0;
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x1b7', slot: 'flagY', value: m.flagY } });
}

/** 0x01：不写推进格数，直接走退出通道（码 1）。 */
function abort01(m: Machine): void {
  m.exitCode = 1;
  m.halted = true;
}

/** 0x1a0：读槽头（292 字节），把 7 个字段写进 op3..9；打不开 1 / 头不合法 2。 */
async function saveRead1a0(m: Machine, ops: Operand[]): Promise<void> {
  const host = m.saveHost;
  if (!host) throw new Error('save-host-missing:0x1a0');
  const slot = readInt(m, ops, 2) | 0;
  const bytes = await host.read(host.fileForSlot(slot, 'dat'));
  if (!bytes) {
    writeInt(m, ops, 1, 1);
    return;
  }
  const magic = String.fromCharCode(bytes[0] ?? 0, bytes[1] ?? 0, bytes[2] ?? 0, bytes[3] ?? 0);
  if (bytes.length < 292 || (magic !== 'S4SD' && magic !== 'S3SD')) {
    writeInt(m, ops, 1, 2);
    return;
  }
  const fields = [
    readU16LE(bytes, 264), readU16LE(bytes, 266), readU16LE(bytes, 270),
    readU16LE(bytes, 272), readU16LE(bytes, 274), readU16LE(bytes, 276),
    readU32LE(bytes, 280) | 0,
  ];
  for (let i = 0; i < 7; i += 1) writeInt(m, ops, 3 + i, fields[i]);
  writeInt(m, ops, 1, 0);
}

/**
 * 把**当前帧区**写成槽文件（`65-save` 的 `FMT-SAVE-CODEC`；`0x19e` 的写侧）。
 *
 * ★ 每帧记录 261 个 32 位字里本层写的四样：
 *   - `rec[0]` = **父帧号**（根帧 −1，子帧 = 建立它时的帧号）—— ⛔ 不是状态码；
 *   - `rec[1]` = 脚本文件索引（无 id ⇒ −1）；`rec[2]` / `rec[3..]` = 返回点条数与返回点；
 *   - `rec[259]` = 本帧位置在**表 1**（消息显示 `0x71`）里的**元素下标**；
 *   - `rec[260]` = 本帧位置在**表 2**（换脚本 `0x03`）里的**元素下标**；★ **当前帧强制 −1**。
 * ★ 两个下标都由 `indexInTable` 算：**在表项数组里线性查找该位置**（命中 = 下标，未命中 / 没有表 ⇒ −1）。
 *   ⛔ 不是"位置 − 起点"：表里只收该类 opcode 的位置，相邻表项之间隔着别的指令。
 * ★ 两者**不互斥**：同一条记录可以两个都有（同一个位置恰好同时出现在两张表里）。
 * ★ 空槽（帧区中间的空洞）也照写一条记录：父帧号 / 脚本索引 / 两个下标全是 −1（没有帧可查）。
 *
 * ★ **纹理记录表**（`+1252` 的 20000 B，`FMT-SAVE-CODEC`）**整段复制场景的备份表**：
 *   按编号 0..999 逐项填 `file` / `color` / 恢复重建门（表 A）/ 零读者标志（表 B）/ `blank`。
 *   - 门与零读者标志来自 `0x258` / `0x259` 写的那两张 1000 项表（`m.scene.flagTableA/B`）；
 *   - 其余三格来自纹理记录（`m.scene.textures`）；**没有记录**的编号 ⇒ 三格 0（表是清零初值，
 *     ⛔ 不写 −1：`file` 的 −1 是"有记录但无来源"，与"从没建过记录"不是同一状态）。
 *   ⛔ 不能写 0 了事：门全 0 ⇒ 恢复时一条纹理都不重建。
 */
function buildSlotFile(m: Machine): Uint8Array {
  const frame = m.cur;
  const records = [];
  for (let i = 0; i <= frame; i += 1) {
    const f = m.slots[i];
    if (!f) {
      records.push({ parent: -1, scriptIndex: -1, ret: [], tableA: -1, tableB: -1 });
    } else {
      const position = f.frame.position;
      records.push({
        parent: f.parent,
        scriptIndex: f.id ?? -1,
        ret: [...f.frame.retStack],
        tableA: indexInTable(position, f.tables?.msg),
        tableB: i === frame ? -1 : indexInTable(position, f.tables?.call),
      });
    }
  }
  const intCount = m.globalInt.size === 0 ? 0 : Math.max(...m.globalInt.keys()) + 1;
  const ints: number[] = [];
  for (let i = 0; i < intCount; i += 1) {
    const bits = m.globalInt.get(i) ?? encodeInt(0, m.key);
    ints.push(decodeInt(bits, m.key));
  }
  const floatCount = m.globalFloat.size === 0 ? 0 : Math.max(...m.globalFloat.keys()) + 1;
  const floats: number[] = [];
  for (let i = 0; i < floatCount; i += 1) floats.push(m.globalFloat.get(i) ?? 0);
  const strings = [...m.globalString.entries()].sort((a, b) => a[0] - b[0]).map(([, v]) => v);
  // 纹理记录表：1000 项按编号整段复制（缺失 = 三格 0；两张标志表各自一格）。
  const textureRecords: SaveTextureRecord[] = [];
  for (let id = 0; id < 1000; id += 1) {
    const rec = m.scene.textures.get(id);
    textureRecords.push({
      file: rec?.file ?? 0,
      color: rec?.color ?? 0,
      flag0: m.scene.flagTableA[id] ?? 0,
      flag1: m.scene.flagTableB[id] ?? 0,
      blank: rec?.blank ?? 0,
    });
  }
  const seed = (Math.floor(m.random() * 0x7fffffff) >>> 0);
  const saveKey = ((Math.floor(m.random() * 0x7fff) << 1) | 1) >>> 0;
  return buildSaveFile(
    { frame, records, ints, floats, strings, version1: 3, version2: 20, textureRecords },
    m.key,
    seed,
    saveKey,
  );
}

/**
 * 0x1a1：无确认的**读回**（只读打开该槽的 `.DAT` 并读头 + 读负载）。
 *
 * ★ 合同（`CORE-OPS-SAVE-WRITE` 的 `0x1a1` 那一半）：
 *   - 打不开 ⇒ 第 1 个操作数 ← 1，结束；这是**唯一**写第 1 个操作数的支路；
 *   - 能打开 ⇒ 读头 + 读负载；★ 成功路径**不写**第 1 个操作数（与 `0x19f` 不同）；
 *   - 读完之后整池重编码（全局整型池：计数格 + 1 格）。
 * ⛔ 这不是写：写是 `0x19e`（`saveWrite19e`），两条的**方向与分支都不同**，
 *   不许合并成一条带开关的实现。
 *
 * ⚠ **本层未建模**（记诊断、不假装做到）：
 *   - 负载的还原（帧 / 池 / 局部）：`save-restore-unmodeled`；
 *   - 整池重编码：core 没有现成的整池重编码入口 ⇒ `save-recode-unmodeled`。
 *   两者都只在**读成功**之后记（头坏 = 读失败，不进这两条）。
 * ⚠ 头不合法的行为**未取证**：合同只写了"打不开"，没有规定"头坏"这一支
 *   ⇒ 记 `save-1a1-header-invalid` 且**不写**第 1 个操作数（⛔ 不照抄 `0x1a0` 的 2）。
 */
async function saveRead1a1(m: Machine, ops: Operand[]): Promise<void> {
  const host = m.saveHost;
  if (!host) throw new Error('save-host-missing:0x1a1');
  const slot = readInt(m, ops, 2) | 0;
  const bytes = await host.read(host.fileForSlot(slot, 'dat'));
  if (!bytes) {
    writeInt(m, ops, 1, 1);
    return;
  }
  const magic = String.fromCharCode(bytes[0] ?? 0, bytes[1] ?? 0, bytes[2] ?? 0, bytes[3] ?? 0);
  if (bytes.length < 292 || (magic !== 'S4SD' && magic !== 'S3SD')) {
    m.diagnostics.push({ kind: 'save-1a1-header-invalid', index: slot });
    return;
  }
  m.diagnostics.push({ kind: 'save-restore-unmodeled', index: slot });
  m.diagnostics.push({ kind: 'save-recode-unmodeled', index: slot });
}

/** 0x19e：带覆写确认的写槽；目标存在且头读不回来时问宿主，拒绝 ⇒ 1。 */
async function saveWrite19e(m: Machine, ops: Operand[]): Promise<void> {
  const host = m.saveHost;
  if (!host) throw new Error('save-host-missing:0x19e');
  const slot = readInt(m, ops, 2) | 0;
  const existing = await host.read(host.fileForSlot(slot, 'dat'));
  if (existing) {
    const magic = String.fromCharCode(existing[0] ?? 0, existing[1] ?? 0, existing[2] ?? 0, existing[3] ?? 0);
    if (magic !== 'S4SD' && magic !== 'S3SD') {
      if (!host.confirmOverwrite) throw new Error('save-confirm-missing:0x19e');
      if (!(await host.confirmOverwrite(slot))) {
        writeInt(m, ops, 1, 1);
        return;
      }
    }
  }
  const ok = await host.write(host.fileForSlot(slot, 'dat'), buildSlotFile(m));
  writeInt(m, ops, 1, ok ? 0 : 1);
}

/**
 * 0x1ab：删槽的 .DAT → .STH。
 *
 * ★ 两个文件**都总是尝试**：第一个失败只把结果置 1、**不返回**；第二个失败再把结果改成 2
 *   ⇒ 两个都失败报 2（后一步覆盖前一步），只有第一个失败报 1，都成功报 0。
 */
async function saveDelete1ab(m: Machine, ops: Operand[]): Promise<void> {
  const host = m.saveHost;
  if (!host) throw new Error('save-host-missing:0x1ab');
  const slot = readInt(m, ops, 2) | 0;
  let code = 0;
  if (!(await host.remove(host.fileForSlot(slot, 'dat')))) code = 1;
  if (!(await host.remove(host.fileForSlot(slot, 'sth')))) code = 2;
  writeInt(m, ops, 1, code);
}

/**
 * 0x1ac：把源槽的 .DAT → 目标，再 .STH → 目标。
 *
 * ★ 与 `0x1ab` 同形：两个文件都总是尝试，第二个失败覆盖第一个的失败（两个都失败报 2）。
 */
async function saveCopy1ac(m: Machine, ops: Operand[]): Promise<void> {
  const host = m.saveHost;
  if (!host) throw new Error('save-host-missing:0x1ac');
  const from = readInt(m, ops, 2) | 0;
  const to = readInt(m, ops, 3) | 0;
  let code = 0;
  if (!(await host.copy(host.fileForSlot(from, 'dat'), host.fileForSlot(to, 'dat')))) code = 1;
  if (!(await host.copy(host.fileForSlot(from, 'sth'), host.fileForSlot(to, 'sth')))) code = 2;
  writeInt(m, ops, 1, code);
}

/**
 * 0x1ae：捕捉纹理 → BMP 字节 → 写槽的 .STH；0 / 2。
 *
 * ⚠ **实现现状，与 `FMT-SAVE-STH` 相反**：合同要求 core 自己组装 BMP 容器（头 + 对齐行宽的像素块），
 * 平台只给「像素 + 行宽 + 通道掩码」；这里却向平台要**整份 BMP 字节**再原样写盘。
 * 分歧登记在 `RES-SAVE-STH-CAPTURE`（本轮不重构）。
 */
async function thumbWrite1ae(m: Machine, ops: Operand[]): Promise<void> {
  const host = m.saveHost;
  if (!host) throw new Error('save-host-missing:0x1ae');
  const slot = readInt(m, ops, 2) | 0;
  const textureId = readInt(m, ops, 3) | 0;
  const rh = m.renderHost;
  if (!rh?.captureTexture) {
    m.diagnostics.push({ kind: 'render-host-capture-missing', index: textureId });
    writeInt(m, ops, 1, 2);
    return;
  }
  const bytes = await rh.captureTexture(textureId);
  if (!bytes) {
    m.diagnostics.push({ kind: 'texture-capture-failed', index: textureId });
    writeInt(m, ops, 1, 2);
    return;
  }
  const ok = await host.write(host.fileForSlot(slot, 'sth'), bytes);
  writeInt(m, ops, 1, ok ? 0 : 2);
}

/**
 * 0x1af：读槽的 .STH → BMP 字节 → 上传纹理；1 打开失败 / 2 内容或上传失败。
 *
 * ⚠ **实现现状，与 `FMT-SAVE-STH` 相反**：合同要求 core 按容器解析、把**像素**交给平台；
 * 这里把**整份文件字节**原样转给平台（`RES-SAVE-STH-CAPTURE`）。
 * ★ 「两段头不齐 ⇒ 2」也是实现自己的判别，**不是引擎合同**：引擎对每次读取只看 `ReadFile` 布尔、
 *   不比较读到的字节数，短读行为本版不规定（`RES-SAVE-STH-SHORTREAD`）。
 */
async function thumbRead1af(m: Machine, ops: Operand[]): Promise<void> {
  const host = m.saveHost;
  if (!host) throw new Error('save-host-missing:0x1af');
  const slot = readInt(m, ops, 2) | 0;
  const textureId = readInt(m, ops, 3) | 0;
  const bytes = await host.read(host.fileForSlot(slot, 'sth'));
  if (!bytes) {
    writeInt(m, ops, 1, 1);
    return;
  }
  if (bytes.length < 54 || bytes[0] !== 0x42 || bytes[1] !== 0x4d) {
    writeInt(m, ops, 1, 2);
    return;
  }
  const rh = m.renderHost;
  if (!rh?.uploadTextureBytes) {
    m.diagnostics.push({ kind: 'render-host-upload-missing', index: textureId });
    writeInt(m, ops, 1, 2);
    return;
  }
  const ok = await rh.uploadTextureBytes(textureId, bytes);
  writeInt(m, ops, 1, ok ? 0 : 2);
}

/** 0x20b：矩形填充；颜色打包 / alpha 上夹 / 夹到对象逻辑矩形 / 请求渲染宿主。 */
async function fillTexture20b(m: Machine, ops: Operand[]): Promise<void> {
  const id = readInt(m, ops, 1) | 0;
  let x = readInt(m, ops, 2) | 0;
  let y = readInt(m, ops, 3) | 0;
  const w = readInt(m, ops, 4) | 0;
  const h = readInt(m, ops, 5) | 0;
  let alpha = readInt(m, ops, 6) | 0;
  const color = readInt(m, ops, 7) | 0;
  if (alpha > 255) alpha = 255;
  const packed = ((color & 0xffffff) | 0xff000000) >>> 0;

  const rec = m.scene.textures.get(id) ?? { file: -1, color: 0, blank: 0 };
  rec.blank = 1;
  m.scene.textures.set(id, rec);

  if (id > 999) {
    m.diagnostics.push({ kind: 'texture-id-out-of-range', index: id });
    return;
  }
  const obj = m.scene.textureObjects.get(id);
  if (!obj) {
    m.diagnostics.push({ kind: 'texture-missing', index: id });
    return;
  }
  const rawX = x;
  const rawY = y;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  const right = Math.min(rawX + w, obj.width);
  const bottom = Math.min(rawY + h, obj.height);
  if (x >= right || y >= bottom) return;
  const host = m.renderHost;
  if (!host?.fillTexture) {
    m.diagnostics.push({ kind: 'render-host-fill-missing', index: id });
    return;
  }
  const ok = await host.fillTexture({ textureId: id, left: x, top: y, right, bottom, color: packed, alpha });
  if (!ok) m.diagnostics.push({ kind: 'texture-fill-lock-failed', index: id });
}

/** 0xc8 sleep：n ≥ 10 置第 0 位 + 定时器；n < 10 在宿主里阻塞。 */
async function sleepC8(m: Machine, ops: Operand[]): Promise<void> {
  m.operandCount = 3;
  if ((m.flags & (1 << 27)) !== 0) return;
  const n = readInt(m, ops, 1) | 0;
  if (n >= 10) {
    m.flags |= SLEEP_BIT;
    m.sleepTimer = n;
    m.sleepUntil = m.now + n;
    m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0xc8', waitMs: n } });
    return;
  }
  if (!m.sleepHost) throw new Error('sleep-host-missing:0xc8');
  await m.sleepHost(n);
  // ★ 短睡眠必须让时间前进（虚拟时钟 += n / 宿主时钟重读），否则轮询循环会 CPU 全速空转。
  m.advance(n);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0xc8', sleepMs: n } });
}

/** 0x0d9：清状态选择字第 12 位；若 d9Cell 非 0，再清 d9Word 的第 12 位。 */
function statusClearD9(m: Machine): void {
  m.flags &= ~(1 << 12);
  if (m.d9Cell !== 0) m.d9Word &= ~(1 << 12);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x0d9', bit12: 0 } });
}

/** 0x7b：读两个整数 → 本帧的主 / 副进入目标（位置，不是字节地址）。 */
function localEnterTargets7b(m: Machine, ops: Operand[]): void {
  m.operandCount = 5;
  m.frame.localMain = readInt(m, ops, 1) | 0;
  m.frame.localSub = readInt(m, ops, 2) | 0;
}

/**
 * 0x7c：身份 / 深度判据；成功则整字覆盖状态选择字、清存档字、跳回存档位置。
 * ★ 深度判据比较的是「**本帧记录里的身份编号**」（`frame.localDepth`，建帧时定下）与
 *   「引擎的入块深度」（`localSaveDepth`，由 `0x199` 首次进入写）—— 判的是**两个编号相等**，不是调用层数。
 */
function localRet7c(m: Machine): void {
  if ((m.localSaveWord & (1 << 25)) === 0) {
    m.operandCount = 1;
    throw new Error('display-message: 不在本地块里');
  }
  if (m.frame.localDepth !== m.localSaveDepth) {
    throw new Error(`display-message: 本地块深度不符（${m.frame.localDepth} != ${m.localSaveDepth}）`);
  }
  m.ip = 4 * m.localSavePos;
  m.operandCount = 0;
  // ★ 只清第 25 位 ⇒ 进入本地块时被置起的第 26 位会**带着 1 回来**（可观察；
  //   见 CORE-OPS-FRAME-LOCAL-RET-7C 的「第 26 位若被置起则保留」）。
  m.flags = m.localSaveWord & ~(1 << 25);
  m.localSaveWord = 0;
  m.localCellA = 0;
  m.localCellB = 0;
  m.localCellC = 0;
  m.localCellD = 0;
  if (m.localPending !== 0) m.localPending = 0;
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x7c', slot: 'localReturn' } });
}

/**
 * 0x199 进入本地块（`CORE-OPS-STATE-199`）：把执行位置交给本地块的**两个进入目标之一**
 * （`0x7b` 登记，`-1` = 未登记），并记下"回来时要用的状态"。
 *
 * 次序**是合同**：
 * 1. 推进格数 ← **1** —— 发生在**任何判断之前**（两条"什么都不做"的路径也留着 1）；
 * 2. 输入字 ← **0**，**无条件执行**（含两条"什么都不做"的路径）；
 * 3. 分支开关 = **存档字第 26 位**：
 *    - **= 0（首次进入形态）**：主目标 `-1` ⇒ 什么都不做；否则推进格数 ← 0、
 *      `save ← state 整字 | 第 25、26 位` 且 `state ← 0`、**返回点 ← 本条之后那一格（位置）**、
 *      **入块深度 ← 本帧记录里的身份编号**、执行位置 ← 4 × 主目标。★ 这一形态**不读** `save` 原值。
 *    - **= 1（已在块内形态）**：副目标 `-1` ⇒ 什么都不做；否则推进格数 ← 0、
 *      `save ← (save & ~第 26 位) | 第 25 位`（读-改-写）、执行位置 ← 4 × 副目标。
 *      ★ **不写**返回点、**不写**入块深度、**不动** `state`（保留外层记录）。
 *
 * ⛔ 三处易踩：
 * - **`m.ip` 是字节、`localSavePos` 是位置**（dword 下标）：入口处 `m.ip` 指向本条自身 ⇒ `m.ip / 4 + 1`，
 *   那个 `+1` 不能省（`0x7c` 用 `m.ip = 4 * localSavePos`）。
 * - **不要动 `frame.retStack`**：返回点走 `localSavePos` 这一**单格**，`0x7c` 不 pop 返回栈；
 *   混起来会让 `0x3` / `0x2` 的栈失衡。
 * - **"什么都不做" ≠ "什么都不写"**：两条早退路径仍然写了推进格数 1 与清输入字。
 * - 本条**不判断门位**、不产生异常、不切换当前帧号；尾段没有动画 / 标脏动作。
 */
function localEnter199(m: Machine): void {
  m.operandCount = 1;
  m.input.word = 0;
  m.inputWord = 0;
  const inside = (m.localSaveWord & (1 << 26)) !== 0;
  const target = inside ? m.frame.localSub : m.frame.localMain;
  if (target === -1) {
    // 未登记 ⇒ 退化成一条空指令（除上面两件已经写掉的事）
    m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x199', path: 'none', entered: false } });
    return;
  }
  m.operandCount = 0;
  if (inside) {
    m.localSaveWord = ((m.localSaveWord & ~(1 << 26)) | (1 << 25)) >>> 0;
  } else {
    m.localSaveWord = (m.flags | (1 << 25) | (1 << 26)) >>> 0;
    m.flags = 0;
    m.localSavePos = m.ip / 4 + 1;
    m.localSaveDepth = m.frame.localDepth;
  }
  m.ip = 4 * target;
  m.effects.push({
    kind: 'engine.scalar.write',
    detail: { opcode: '0x199', path: inside ? 'sub' : 'main', entered: true },
  });
}

/** 0x1f4 / 0x1f5：执行区间进入 / 离开；本实现没有待处理队列，泵在空队列上是空操作。 */
function runBracket1f4(m: Machine): void {
  m.operandCount = 1;
  if (m.bracketBusy === 0) {
    m.bracketBusy = 1;
    m.bracketPrev = m.bracketNow;
    m.bracketNow = m.now;
  } else {
    m.bracketDepth += 1;
  }
}

function runBracket1f5(m: Machine): void {
  m.operandCount = 1;
  if (m.bracketDepth > 0) {
    m.bracketDepth -= 1;
    return;
  }
  if (m.bracketBusy === 0) return;
  m.bracketBusy = 0;
  // 队列占用为 0 ⇒ 泵；本实现没有队列，泵是空操作。
}

/** 0x1ad：把当前层号写进引擎格（复位值 −1）。 */
function layerCell1ad(m: Machine): void {
  m.layerCell = m.currentLayer;
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x1ad', slot: 'layerCell', value: m.layerCell } });
}

/** 0x1ce：非 0 启用（置第 30 位 + 播种）；0 且位已置 ⇒ 清位。 */
function rngEnable1ce(m: Machine, ops: Operand[]): void {
  const v = readInt(m, ops, 1) | 0;
  if (v !== 0) {
    m.rngSwitch = v;
    m.flags |= 1 << 30;
    m.rngEnabled = 1;
    m.rngSeed = m.now;
  } else if ((m.flags & (1 << 30)) !== 0) {
    m.flags &= ~(1 << 30);
    m.rngEnabled = 0;
  }
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x1ce', enabled: m.rngEnabled } });
}

/** 0x19b：清第 27 位；再依次 D / A / B ← 0（顺序是合同）。 */
function modeClear19b(m: Machine): void {
  m.flags &= ~(1 << 27);
  m.modeD = 0;
  m.modeA = 0;
  m.modeB = 0;
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x19b', bit27: 0 } });
}

/** 0x19c：按 C / E / F 的条件置位；C 支置 D，E 支不碰 D，F 支什么都不改。 */
function modeConditional19c(m: Machine): void {
  m.modeA = 1;
  m.modeB = 0;
  if (m.modeC !== 0) {
    m.modeD = 1;
  } else if (m.modeE !== 0) {
    // ★ E 支不碰 D
  } else if (m.modeF !== 0) {
    return;
  } else {
    m.modeD = 0;
    m.flags &= ~(1 << 27);
    return;
  }
  m.flags |= 1 << 27;
  m.modeG = 1;
  if ((m.flags & (1 << 29)) !== 0) m.flags &= ~(1 << 29);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x19c', bit27: 1 } });
}

/** 0x1c7 读第 27 位：op1（写整型）← (状态选择字的第 27 位) ? 1 : 0。 */
function readModeBit1c7(m: Machine, ops: Operand[]): void {
  writeInt(m, ops, 1, (m.flags >>> 27) & 1);
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x1c7', slot: 'mode-bit-27', value: (m.flags >>> 27) & 1 } });
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
  const count = readU32LE(m.body, off) >>> 0;
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
    const src = readU32LE(m.body, off + 4 + 4 * i) >>> 0;
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
  // 显示用：真实脚本的 key 是 CP932 字节串；合成测试可能直接给 Unicode（>0xFF）⇒ 原样保留。
  const keyText = [...key].some((c) => (c.codePointAt(0) ?? 0) > 0xff) ? key : decodeCp932(key);
  m.effects.push({
    kind: 'font.lookup',
    detail: { opcode: '0x2de', key, keyText, table: m.fontNames.length, hit },
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
  // 字体对象的**主色**（`Font+0x550`；0x76 的落点）。证据：0x41F3D5。
  m.render.fontObject.color = value;
  m.effects.push({ kind: 'engine.scalar.write', detail: { opcode: '0x76', slot: 'Engine.d21664', form: 'bswap24(op1)', value } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_459F40' });
}

/** 0x77：与 0x76 同形，只差落点（`Engine.d21665`） */
function bswap77(m: Machine, ops: Operand[]): void {
  const value = bswap24(readInt(m, ops, 1));
  m.scalars.set('Engine.d21665', value);
  // 字体对象的**副色**（`Font+0x554`；0x77 的落点）。证据：0x41F435。
  m.render.fontObject.shadowColor = value;
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
  // 字体对象的**字号**（`Font+0x313D4`；0x75 的实参）。证据：0x418611。
  m.render.fontObject.size = value;
  m.effects.push({ kind: 'engine.forward', detail: { opcode: '0x75', callee: 'sub_4185F0', holder: 'Engine.d21324', arg: value } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_4185F0' });
}

/**
 * 0x1a5 set-font：`sub_4328F0(Engine+0x14D30, 文本(op1))`（文本原语 `sub_41B640`）—— 字体/文本子系统，未建模。
 * ★ 它与 `0x2de` 的那张"字体名表"是否同一张表**未取证**（若是，`0x2de` 的宿主表就该由这些 set-font 填充）⇒ 只记转发 + 诊断。
 */
function setFont1a5(m: Machine, ops: Operand[]): void {
  const name = readText(m, ops, 1);
  // 字体对象的**字面名**（`Font+0x4EC`；0x1a5 的实参）。证据：0x4329AF。
  m.render.fontObject.font = name;
  m.effects.push({ kind: 'engine.forward', detail: { opcode: '0x1a5', callee: 'sub_4328F0', holder: 'Engine.d21324', name } });
  m.diagnostics.push({ kind: 'call-target-unmodeled:sub_4328F0' });
}

/**
 * 0x2bd：`v = 读整(op1)` ⇒ 字体对象两格写 `v ? 0x2BC : 0`；`v != 0` 时再**尾跳** `sub_459F40(Engine+0x14D30)`（未建模）。
 * 落点 = 字体对象 `+0x35594` / `+0x4E0`（绝对 `Engine[75953]` / `Engine[21636]`）。
 */
function toggle2bd(m: Machine, ops: Operand[]): void {
  const v = readInt(m, ops, 1);
  const value = v !== 0 ? 0x2bc : 0;
  scalarWrite(m, '0x2bd', 'Engine.d75953', value);
  scalarWrite(m, '0x2bd', 'Engine.d21636', value);
  // 字体对象的**字重**（`Font+0x4E0/+0x35594`）：非 0 ⇒ 700、0 ⇒ 0。证据：0x42622F/0x426232。
  m.render.fontObject.weight = value;
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

/** 0x130 load-show-logo：`op1 ← Engine[0x5EB5C]`（引擎初始化置 1；0x9 handler `sub_428A60` 的 `0x428A75` 清 0） */
function loadShowLogo130(m: Machine, ops: Operand[]): void {
  writeInt(m, ops, 1, m.showLogo);
}

/* ──────────────────────────── 输入（60-ops-input） ──────────────────────────── */

/**
 * `0xfe` 键总数（CORE-INPUT-WORD）：无符号比较 > 31 ⇒ 显示消息型异常。
 * ★ 落点身份（键总数）由 `0x100` 的扫描上限读点支撑。
 * ★ 引擎**不建立**这一格的初值（唯一写点是本条）⇒ 在此之前读到它是**未定义行为**；
 *   本实现按 0 兜底（UB，不是合同），读者见 `readKeyTotal`（只记一次诊断）。
 */
function keyTotalFe(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 1) >>> 0;
  if (value > 31) throw new Error(`opcode 0xfe 的操作数 ${value} 超出引擎的范围检查（> 0x1f）—— 引擎这里抛异常`);
  m.input.keyTotal = value;
  m.input.keyTotalEstablished = true;
  scalarWrite(m, '0xfe', 'Engine.d517', value);
  m.effects.push({ kind: 'input.key-total', detail: { value } });
}

/**
 * 读引擎的**键总数**格。
 * ★ 引擎不建立初值（CORE-INPUT-WORD 的「成立条件」）⇒ 在 `0xfe` 之前读到它是**未定义行为**：
 *   本实现用 0 兜底，但那是 UB 兜底、**不是合同**；这里**只记一次**诊断，⛔ 不把它表述成"启动值"。
 */
function readKeyTotal(m: Machine): number {
  if (!m.input.keyTotalEstablished && !m.diagnostics.some((d) => d.kind === 'key-total-unset')) {
    m.diagnostics.push({ kind: 'key-total-unset' });
  }
  return m.input.keyTotal >>> 0;
}

/** `0xff` 采样按键位图（CORE-INPUT-WORD）：清输入字 → 平台采样 → 重置游标与上限 */
function keySampleFf(m: Machine): void {
  m.input.word = 0;
  m.inputWord = 0;
  if (m.inputHost) {
    m.input.word = m.inputHost.sampleKeys(m.now) >>> 0;
    m.inputWord = m.input.word;
  }
  m.input.samples += 1;
  m.frame.keyCursor = 0;
  // ★ 按帧上限是**键总数在这一次采样时的快照**；它的唯一读者是未实现的 `0x104`（`0x100` 读键总数本身）。
  const keyTotal = readKeyTotal(m);
  m.frame.keyLimit = keyTotal;
  m.effects.push({ kind: 'input.sample', detail: { mask: m.input.word, keyTotal } });
}

/** `0xfb` 登记按键回调（CORE-INPUT-CALLBACK-REGISTER） */
function keyRegisterFb(m: Machine, ops: Operand[]): void {
  const key = readInt(m, ops, 1) >>> 0;
  if (key > 31) throw new Error('display-message: SetKeyJump の引数が不正です');
  const target = readInt(m, ops, 2) | 0;
  m.frame.keyJumps.set(key, target);
}

/** `0xcc` 登记鼠标回调（CORE-INPUT-CALLBACK-REGISTER）：间隔 / 目标 / 注册层数 / 上次时刻 */
function mouseCallbackCc(m: Machine, ops: Operand[]): void {
  const interval = readInt(m, ops, 1) | 0;
  const target = readInt(m, ops, 2) | 0;
  m.input.mouse.interval = interval === 0 ? 1 : interval;
  m.input.mouse.target = target;
  m.input.mouse.depth = m.frameDepth();
  m.input.mouse.last = m.now;
}

/**
 * `0xcd` 定时派发鼠标回调（CORE-INPUT-MOUSE-CALLBACK-PUMP）：
 * 时间闸（或第 27 位）→ 压返回位置（当前位置 + 1）→ 目标 -1 撤销 → 深度校验 → 跳到目标、推进格数 0。
 */
function mouseCallbackCd(m: Machine): void {
  m.operandCount = 1;
  const elapsed = m.now - m.input.mouse.last;
  const force = (m.flags & (1 << 27)) !== 0;
  if (elapsed < m.input.mouse.interval && !force) return;
  m.input.mouse.last = m.now;
  m.frame.retStack.push(m.ip / 4 + 1);
  if (m.input.mouse.target === -1) {
    m.frame.retStack.pop();
    return;
  }
  if (m.frameDepth() !== m.input.mouse.depth) {
    throw new Error('display-message: Depth が不正です');
  }
  m.input.mouseDispatches += 1;
  m.effects.push({ kind: 'input.mouse.dispatch', detail: { target: m.input.mouse.target } });
  m.ip = 4 * m.input.mouse.target;
  m.operandCount = 0;
}

/**
 * `0x100` 派发按键回调（CORE-INPUT-KEY-DISPATCH）。
 * ★ 输入字为 0 ⇒ 走"无按键"表项、返回位置 = 下一条；非 0 ⇒ 压当前位置（有意重派发）、游标前进。
 * ★ 扫描上界 = **引擎的键总数本身**（不是 `0xff` 抄下的按帧上限；那一格的唯一读者是未实现的 `0x104`）。
 */
function keyDispatch100(m: Machine): void {
  m.operandCount = 1;
  const word = m.input.word >>> 0;
  const limit = readKeyTotal(m);
  const table = m.frame.keyJumps;
  if (word === 0) {
    const target = table.get(limit);
    if (target === undefined) return;
    m.frame.retStack.push(m.ip / 4 + 1);
    m.input.keyDispatches += 1;
    m.effects.push({ kind: 'input.key.dispatch', detail: { key: limit, target } });
    m.ip = 4 * target;
    m.operandCount = 0;
    return;
  }
  let cursor = m.frame.keyCursor;
  while (cursor < limit && ((word >>> cursor) & 1) === 0) cursor += 1;
  if (cursor >= limit) return;
  m.frame.keyCursor = cursor + 1;
  const target = table.get(cursor);
  if (target === undefined) return; // 未登记 ⇒ 放弃本轮剩余置位
  m.frame.retStack.push(m.ip / 4);
  m.input.keyDispatches += 1;
  m.effects.push({ kind: 'input.key.dispatch', detail: { key: cursor, target } });
  m.ip = 4 * target;
  m.operandCount = 0;
}

/**
 * `0x102` 登记按住派发槽（CORE-INPUT-102-SLOT）。
 * ★ 表按**当前帧**分层、每（帧 × 键）固定 **10 槽**；键 0..31 与槽 0..9 是两个**独立**写死的上界。
 * ★ 越界在**写表之前**返回 ⇒ 一个槽都不写（显示消息型异常）；异常文本只提"第 1 个操作数"，
 *   ⛔ 不许拿消息文本判断是哪个操作数错了。
 * ★ 目标位置**完全不做检查**：−1 = 取消登记，按位置原样写进格。键编号的检查与键总数**无关**。
 */
function holdSlot102(m: Machine, ops: Operand[]): void {
  m.operandCount = 2 * 3 + 1; // 推进格数 7
  const key = readInt(m, ops, 1) | 0;
  const slot = readInt(m, ops, 2) | 0;
  const target = readInt(m, ops, 3) | 0;
  if (key < 0 || key > 31) throw new Error('display-message: SetKeyJumpEx の第１引数が不正です（键编号越界）');
  if (slot < 0 || slot > 9) throw new Error('display-message: SetKeyJumpEx の第１引数が不正です（槽号越界）');
  m.frame.holdSlots.set(key * 10 + slot, target);
  m.effects.push({ kind: 'input.hold.register', detail: { key, slot, target } });
}

/**
 * `0x103` 设置按住游标模数（CORE-INPUT-103-MODULUS）。
 * ★ 模数是**引擎级**一格（所有帧共用）：原样写、**不做任何范围检查**（0 / 负数照写、不抛）。
 * ★ 按住游标按**帧槽号**存（引擎级数组 + 当前帧号索引，`Machine.holdCursorBySlot`）⇒ 只清**当前帧槽**的
 *   游标；其它槽（含已释放 / 待重载的帧号）保持原值 —— 帧释放 / 重载也不复位。
 * ★ 模数 0 的后果是**延迟**的：本条不拦，下一次 `0x104` 的游标推进才会响亮失败。
 */
function holdModulus103(m: Machine, ops: Operand[]): void {
  m.operandCount = 2 * 1 + 1; // 推进格数 3
  const modulus = readInt(m, ops, 1) | 0;
  m.hold.modulus = modulus;
  m.holdCursorBySlot[m.cur] = 0;
  // 0 是脚本自己制造的 UB，不是"设置失败"：只留诊断，下一次派发才响。
  if (modulus === 0) m.diagnostics.push({ kind: 'hold-modulus-zero:0x103' });
  m.effects.push({ kind: 'input.hold.modulus', detail: { modulus } });
}

/**
 * `0x105` 设置按住掩码（CORE-INPUT-105-MASK）：32 位位集（第 i 位 = 编号 i），**原样、不归一**。
 * ★ 引擎级（帧释放 / 重载不清它，`0xfc` 也不清）；唯一读者是 `0x104`。
 * ★ 它**不是**输入字：本条不碰输入字，`0x104` 也不拿它判"哪个键被按住"。
 */
function holdMask105(m: Machine, ops: Operand[]): void {
  m.operandCount = 2 * 1 + 1; // 推进格数 3
  m.hold.mask = readInt(m, ops, 1) >>> 0;
  m.effects.push({ kind: 'input.hold.mask', detail: { mask: m.hold.mask } });
}

/**
 * 按住游标推进：`(游标 + 1) mod 模数`。**三条出口都要走这里**（跳 / 不跳、登记 / 未登记）。
 * ★ 游标按**帧槽号**读写（`holdCursorBySlot[m.cur]`）：帧重载不换槽 ⇒ 从原值继续。
 * ★ `modulus === 0` ⇒ 目标机上是整数除零（UB）。本仓口径：**显式响亮失败**，
 *   ⛔ 不按 0 / 不按 1 / 不用默认模数兜底（那会把 UB 伪装成合同）。
 * ★ 游标未被定义（引擎不建立初值；只有"槽从没被 `0x104` 写过 + 编号 == 静态格"的残值路径可达）
 *   ⇒ 同样是 UB，也响亮失败（⛔ 不把 `null` 当 0）。
 * ★ 取余**保持 JS 的 `%`**：它与目标机 `idiv` 同语义（向零截断、余数符号随被除数），
 *   ⛔ 不要改成"数学取模再调整到 `[0, |m|)`"（那样负模数会给出不同槽号序列）。
 */
function advanceHoldCursor(m: Machine): void {
  const modulus = m.hold.modulus | 0;
  if (modulus === 0) {
    m.diagnostics.push({ kind: 'hold-cursor-divide-by-zero:0x104' });
    throw new Error('input-hold: modulus is 0（0x104 的游标推进是整数除零；先执行 0x103 设一个非 0 模数）');
  }
  const cursor = m.holdCursorBySlot[m.cur];
  if (cursor === null) {
    m.diagnostics.push({ kind: 'hold-cursor-uninitialized' });
    throw new Error('input-hold: hold cursor is uninitialized（引擎不建立这一格的初值）');
  }
  // ★ 商溢出：`游标 = INT_MAX` ⇒ 目标机的 `cursor + 1` 先在 32 位里回绕成 INT_MIN，
  //   再 `idiv -1` ⇒ 商 2147483648 装不进 32 位 ⇒ 结构化异常。⛔ 不许让 JS 算出 2147483648（那是错的合同）。
  if (cursor === 0x7fffffff && modulus === -1) {
    m.diagnostics.push({ kind: 'hold-cursor-divide-overflow' });
    throw new Error('input-hold: cursor + 1 divided by -1 overflows int32（0x104 的游标推进是商溢出；游标保持原值）');
  }
  m.holdCursorBySlot[m.cur] = (cursor + 1) % modulus;
}

/**
 * `0x104` 按住派发按键回调（CORE-OPS-INPUT-104）。
 *
 * 与 `0x100` 的五处分工（⛔ 别把它写成 `0x100` 的参数化版本）：
 * | | `0x100` | 本条 |
 * |---|---|---|
 * | 上界 | **键总数** | **按键扫描上限** `frame.keyLimit`（本条还会夹它） |
 * | 方向 | 正向、从扫描游标起 | **反向**、从 `limit − 1` 递减取第一个置位 |
 * | 游标 | 按键扫描游标（找到时 ← 键号 + 1） | 按住游标（**每条出口** ← `(游标+1) mod 模数`） |
 * | 表 | `keyJumps`（每（帧 × 键）一个目标） | `holdSlots`（每（帧 × 键）**10 槽**） |
 * | 返回点 | 输入字 0 ⇒ 下一条；按键 ⇒ 本条自身 | **掩码**该位置起 ⇒ 本条自身 + 夹上限；否则下一条 |
 *
 * ★ 掩码只在**找到键**时判（无按键路径固定"下一条"）；夹上限发生在**查表之前**且**不撤销**。
 * ★ 编号 = 无按键编号（键总数）时同样走上下文闸与查表；`limit ≤ 0` 时**不扫描**。
 */
function holdDispatch104(m: Machine): void {
  m.operandCount = 1; // 推进格数 1（缺省只前进一条）
  const here = m.ip / 4; // 本条指令自己的位置（掩码命中时的返回点）
  const word = m.input.word >>> 0;
  const limit = m.frame.keyLimit | 0;

  // ① 反向找编号：`limit − 1` 递减到 0，取**第一个**置位；`limit ≤ 0` ⇒ 不扫描。
  let key = -1;
  for (let i = limit - 1; i >= 0; i -= 1) {
    if (((word >>> i) & 1) === 1) { key = i; break; }
  }
  const found = key >= 0;
  const index = found ? key : readKeyTotal(m); // 无按键 ⇒ 编号 = 键总数

  // ② 上下文闸：编号变了 ⇒ 只清**当前帧槽**的游标；然后把编号写进引擎级格。
  //    ★ 静态格初值 −1（≠ 任何编号）⇒ 进程里第一次走本条必然把当前槽的游标定义成 0。
  if (m.hold.contextKey !== index) m.holdCursorBySlot[m.cur] = 0;
  m.hold.contextKey = index;

  // ③ 返回点：掩码只在"找到键"时判；夹上限在**查表之前**，且未登记时也**不回滚**。
  const masked = found && ((m.hold.mask >>> key) & 1) === 1;
  if (masked) m.frame.keyLimit = key;
  m.frame.retStack.push(masked ? here : here + 1);

  // ④ 查表：槽号 = 当前游标（按**帧槽号**读，帧重载不换槽 ⇒ 从原值继续）；缺格与写进去的 −1 都算"未登记"。
  const cursor = m.holdCursorBySlot[m.cur];
  if (cursor === null) {
    // UB：上下文闸在第一次调用时必然定义它（contextKey 初值 −1 ≠ 任何编号）⇒ 走到这里是实现错
    //   （或"该槽从没被写过 + 编号 == 静态格"的残值路径）。
    m.frame.retStack.pop();
    m.diagnostics.push({ kind: 'hold-cursor-uninitialized' });
    throw new Error('input-hold: hold cursor is uninitialized（引擎不建立这一格的初值）');
  }
  const target = m.frame.holdSlots.get(index * 10 + cursor);
  if (target === undefined || target === -1) {
    m.frame.retStack.pop(); // 未登记 ⇒ 撤销刚压的返回点；推进格数留在 1
  } else {
    m.hold.dispatches += 1;
    m.input.keyDispatches += 1;
    m.effects.push({ kind: 'input.hold.dispatch', detail: { index, slot: cursor, target } });
    m.ip = 4 * target;
    m.operandCount = 0;
  }

  // ⑤ 每条出口都推进游标（跳 / 不跳、登记 / 未登记）。
  advanceHoldCursor(m);
}

/**
 * `0xfc` 输入复位（CORE-INPUT-RESET-FC）：把**两张回调表**（`0xfb` 的按键回调表与按住派发表）
 * **全部**清成"未登记"，再把按住游标模数写成 0。
 * ★ 清的是**所有帧**（引擎清整条数组），⛔ 不是只清当前帧。
 * ★ 不碰：输入字 / 门位 / 键总数 / **按住掩码** / 任何游标 / 按键扫描上限。
 * ★ 模数归 0 是**有意的复位**：之后必须先 `0x103`，否则 `0x104` 在游标推进处除零。
 */
function inputResetFc(m: Machine): void {
  m.operandCount = 1;
  let frames = 0;
  for (const rec of m.slots) {
    if (!rec) continue;
    rec.frame.keyJumps.clear();
    rec.frame.holdSlots.clear();
    frames += 1;
  }
  m.hold.modulus = 0;
  m.effects.push({ kind: 'input.reset', detail: { frames } });
}

/**
 * 0x12e 鼠标命中查找：**显式部分实现**。
 *
 * 完整条目表 / 偏移数组的布局还没写成 spec（audit.gaps），所以这里不猜命中，只写 **-1（没有命中）**
 * 并留一条请求记录 + 首次诊断。目的：鼠标移动不让 TITLE 停在"未实现 opcode"上；
 * 悬停 / 点击菜单的命中语义仍然缺（见 README 的未建模白名单）。
 */
function hitTest12e(m: Machine, ops: Operand[]): void {
  writeInt(m, ops, 1, -1);
  m.effects.push({ kind: 'input.hit-test.unmodeled', detail: { opcode: '0x12e' } });
  if (!m.unmodeledSeen.has(0x12e)) {
    m.unmodeledSeen.add(0x12e);
    m.diagnostics.push({ kind: 'unmodeled-input-hit-test:0x12e' });
  }
}

/** `0x2fc` 取一条鼠标事件（CORE-INPUT-MOUSE-EVENT）：没有事件时只写结果 0 */
function mouseEvent2fc(m: Machine, ops: Operand[]): void {
  const event = m.inputHost?.nextMouseEvent(m.now) ?? null;
  if (!event) {
    writeInt(m, ops, 1, 0);
    m.effects.push({ kind: 'input.mouse.event', detail: { present: false } });
    return;
  }
  writeInt(m, ops, 1, 1);
  writeInt(m, ops, 2, event.x);
  writeInt(m, ops, 3, event.y);
  writeInt(m, ops, 4, event.flags);
  writeInt(m, ops, 5, event.extra);
  m.effects.push({ kind: 'input.mouse.event', detail: { present: true, x: event.x, y: event.y, flags: event.flags } });
}

/** `0x109` 读光标位置（CORE-INPUT-MOUSE-POS）：设备不可用写 (-100000, -100000) */
function mousePos109(m: Machine, ops: Operand[]): void {
  const pos = m.inputHost?.cursor(m.now) ?? null;
  const x = pos ? pos.x : -100000;
  const y = pos ? pos.y : -100000;
  writeInt(m, ops, 1, x);
  writeInt(m, ops, 2, y);
  m.effects.push({ kind: 'input.cursor', detail: pos ? { x, y } : { available: false } });
}

/** `0x108` 读鼠标按钮位图（CORE-INPUT-MOUSE-BUTTON）：设备不可用写 0 */
function mouseButton108(m: Machine, ops: Operand[]): void {
  const mask = m.inputHost?.buttons(m.now) ?? 0;
  writeInt(m, ops, 1, mask);
  m.effects.push({ kind: 'input.buttons', detail: { mask } });
}

/** `0x24e` 主循环模式字（CORE-INPUT-MODE-WORD）：原样写入；位 0 由等待服务读取 */
function modeWord24e(m: Machine, ops: Operand[]): void {
  m.input.mode = readInt(m, ops, 1) | 0;
}

/* ──────────────────────────── 音频请求边界（60-ops-voice） ──────────────────────────── */

/** `0xb4` 音效装载（CORE-OPS-AUDIO-WAV）：只发请求，不改引擎数据 */
function audioWavLoadB4(m: Machine, ops: Operand[]): void {
  const resource = readInt(m, ops, 1);
  const slot = readInt(m, ops, 2);
  m.effects.push({ kind: 'audio.wav.load', detail: { resource, slot } });
}

/** `0xb5` 音效播放（CORE-OPS-AUDIO-WAV）：只发请求，不改引擎数据 */
function audioWavPlayB5(m: Machine, ops: Operand[]): void {
  const slot = readInt(m, ops, 1);
  m.effects.push({ kind: 'audio.wav.play', detail: { slot } });
}

/** `0xbf` BGM（CORE-OPS-AUDIO-BGM）：清状态字第 9 位 + 发播放请求 */
function audioBgmBf(m: Machine, ops: Operand[]): void {
  const resource = readInt(m, ops, 1);
  m.flags &= ~0x200;
  m.effects.push({ kind: 'audio.bgm.play', detail: { resource } });
}

/* ──────────────────────────── 场景 / 渲染 / 文本增补 ──────────────────────────── */

/** `0x344` 辅助表 A（Live2D 模型条目表，按用户要求不展开）:置「已存在」位 + 写第二格 + 标脏（CORE-OPS-SCENE-ENTRY-344）；本实现只把条目当不透明桶保存。 */
function sceneEntry344(m: Machine, ops: Operand[]): void {
  const value = readInt(m, ops, 2);
  const id = readInt(m, ops, 1) | 0;
  m.scene.entries.set(id, { present: 1, value });
  m.scene.dirty = 1;
}

/** `0x20c` 帧步进（CORE-OPS-RENDER-FRAME-STEP）：时间戳 + 一次渲染请求 + 清帧标志 */
function renderFrameStep20c(m: Machine): void {
  if (m.scene.frameDriver === 0) {
    m.scene.framePrev = m.scene.frameNow;
    m.scene.frameNow = m.now;
  }
  m.effects.push({ kind: 'render.frame.step', detail: { now: m.now } });
  m.scene.frameFlags = 0;
}

/** `0x2eb` 版本字符串（CORE-OPS-TEXT-GAMEVERSION-2EB）：配置 `set:GameVersion` → 第 1 个操作数 */
function textGameVersion2eb(m: Machine, ops: Operand[]): void {
  if (!m.config) {
    m.diagnostics.push({ kind: 'config-missing:set:GameVersion' });
    writeText(m, ops, 1, '');
    return;
  }
  writeText(m, ops, 1, m.config.getString('set:GameVersion'));
}

/** C 的 `atoi` 口径：忽略前导空白、可选符号、连续十进制数字；没有数字 ⇒ 0 */
function atoiC(text: string): number {
  const match = /^\s*([+-]?)(\d+)/.exec(text);
  if (!match) return 0;
  const magnitude = Number.parseInt(match[2], 10);
  const value = match[1] === '-' ? -magnitude : magnitude;
  return value | 0;
}

/** `0x2ec` 十进制文本 → 整数（CORE-OPS-TEXT-ATOI-2EC） */
function textAtoi2ec(m: Machine, ops: Operand[]): void {
  const source = readText(m, ops, 2);
  writeInt(m, ops, 1, atoiC(source));
}

/* ──────────────────────────── 未建模子系统（显式请求 + 诊断） ──────────────────────────── */

/**
 * 只向未建模子系统发请求的 opcode：**不是静默 no-op**——每次执行记一条 `unmodeled.subsystem`
 * 效果（可被 `--trace` / 报告计数），首次出现再记一条诊断。
 *
 * ★ 白名单只含"不改变已规定引擎数据"的两块：ADV 文字（`0x2c7` SJIS 字符串工具）
 *   与 Live2D（`0x34e` 动作装载 / `0x352` 动作参数）。它们不置门位、不改帧 / 返回栈 / 输入字。
 * ★ `0x23b`（数字绘制）**已建模**：TITLE 的 `Version` 数字经它画（见 `cgDraw23b`），
 *   不再是未建模请求 —— 把它留在白名单会让版本号只剩两个小数点。
 * ★ 未列入的 gap（`0x12e` / `0x140` / `0x23d` / `0x32b`）仍然"未实现即停"。
 */
const UNMODELED_SUBSYSTEM = new Map<number, { subsystem: string; why: string }>([
  [0x2c7, { subsystem: 'adv-text', why: 'SJIS 字符串工具' }],
  [0x341, { subsystem: 'live2d', why: '模型文件装载' }],
  [0x345, { subsystem: 'live2d', why: '纹理文件装载' }],
  [0x34e, { subsystem: 'live2d', why: '动作文件装载' }],
  [0x352, { subsystem: 'live2d', why: '动作 / 参数' }],
]);

function unmodeledHandler(opcode: number, subsystem: string): Handler {
  return (m: Machine): void => {
    m.effects.push({ kind: 'unmodeled.subsystem', detail: { opcode, subsystem } });
    if (!m.unmodeledSeen.has(opcode)) {
      m.unmodeledSeen.add(opcode);
      m.diagnostics.push({ kind: `unmodeled-subsystem:0x${opcode.toString(16)}` });
    }
  };
}

function operandAt(ops: Operand[], k: number): Operand {
  const op = ops[k - 1];
  if (!op) throw new Error(`operand-out-of-range:${k}/${ops.length}`);
  return op;
}

export const HANDLERS = new Map<number, Handler>([
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
  [0x80, setDefaultGroup80],
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
  [0x223, sceneCommand223],
  [0x24d, sceneCommand24d],
  [0x24f, sceneCommand24f],
  [0x250, (m, o) => sceneCommand3(m, o, 0)],
  [0x251, (m, o) => sceneCommand3(m, o, 1)],
  [0x20d, renderTarget20d],
  [0x33f, blend33f],
  [0x204, drawText204],
  [0x101, pollInput101],
  [0xfe, keyTotalFe],
  [0xff, keySampleFf],
  [0xfb, keyRegisterFb],
  [0xcc, mouseCallbackCc],
  [0xcd, mouseCallbackCd],
  [0x100, keyDispatch100],
  [0x102, holdSlot102],
  [0x103, holdModulus103],
  [0x104, holdDispatch104],
  [0x105, holdMask105],
  [0xfc, inputResetFc],
  [0x12e, hitTest12e],
  [0x2fc, mouseEvent2fc],
  [0x109, mousePos109],
  [0x108, mouseButton108],
  [0x24e, modeWord24e],
  [0xb4, audioWavLoadB4],
  [0xb5, audioWavPlayB5],
  [0xbf, audioBgmBf],
  [0x344, sceneEntry344],
  [0x20c, renderFrameStep20c],
  [0x2eb, textGameVersion2eb],
  [0x2ec, textAtoi2ec],
  [0x23b, cgDraw23b],
  [0x341, unmodeledHandler(0x341, 'live2d')],
  [0x345, unmodeledHandler(0x345, 'live2d')],
  [0x2c7, unmodeledHandler(0x2c7, 'adv-text')],
  [0x34e, unmodeledHandler(0x34e, 'live2d')],
  [0x352, unmodeledHandler(0x352, 'live2d')],
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
  [0x6d, memFlip6d],
  [0x2d8, setArrayTo2d8],
  [0x1aa, loadString1aa],
  [0x63, lea63],
  [0x60, random60],
  [0x74, param74],
  [0x7f, param7f],
  [0xd0, clockD0],
  [0xa1, tableResetA1],
  [0xa2, tableBindA2],
  [0xa3, tableDispatchA3],
  [0x191, floatAbs191],
  [0x2d0, floatAdd2d0],
  [0x2d1, floatSub2d1],
  [0x2d3, floatDiv2d3],
  [0x1c7, readModeBit1c7],
  [0x1b0, memcpy1b0],
  [0x208, textureSize208],
  [0x1cf, statusWrite1cf],
  [0x1bf, (m) => statusConsume1bf(m)],
  [0x1cc, statusCellRead1cc],
  [0x19a, flagReadX19a],
  [0x1b6, flagReadY1b6],
  [0x1b7, flagWriteY1b7],
  [0x258, sceneTableFlags258],
  [0x259, (m) => sceneTableFlags259(m)],
  [0x2da, cgSet2da],
  [0x1f6, (m) => sceneClear1f6(m)],
  [0x19b, (m) => modeClear19b(m)],
  [0x19c, (m) => modeConditional19c(m)],
  [0x217, (m, o) => sceneVecSet(m, o, 'vec1')],
  [0x219, (m, o) => sceneVecSet(m, o, 'vec2')],
  [0x218, (m, o) => sceneVecGet(m, o, 'vec1')],
  [0x21a, (m, o) => sceneVecGet(m, o, 'vec2')],
  [0x1fd, sceneScale1fd],
  [0x1ff, sceneTranslate1ff],
  [0x1fe, sceneRotate1fe],
  [0x220, sceneSetTransform220],
  [0x228, sceneGetLocalTrans228],
  [0x01, (m) => abort01(m)],
  [0x0d9, (m) => statusClearD9(m)],
  [0x1f4, (m) => runBracket1f4(m)],
  [0x1f5, (m) => runBracket1f5(m)],
  [0x1ad, (m) => layerCell1ad(m)],
  [0x1ce, rngEnable1ce],
  [0x7b, localEnterTargets7b],
  [0x7c, (m) => localRet7c(m)],
  [0x199, (m) => localEnter199(m)],
  [0x21e, setter21e],
  [0x21f, setter21f],
  [0x230, setter230],
  [0x231, setter231],
  [0x233, setter233],
  [0x234, setter234],
  [0x235, setter235],
  [0x229, sceneSelfTransform229],
  [0x239, setter239],
  [0x242, setter242],
  [0x21d, copy21d],
  [0x1ab, saveDelete1ab],
  [0x1ac, saveCopy1ac],
  [0x1a0, saveRead1a0],
  [0xc8, sleepC8],
  [0x1a1, saveRead1a1],
  [0x19e, saveWrite19e],
  [0x20b, fillTexture20b],
  [0x1ae, thumbWrite1ae],
  [0x1af, thumbRead1af],
]);

export function unimplemented(opcode: number): never {
  throw new Error(`unimplemented opcode 0x${opcode.toString(16)}`);
}

