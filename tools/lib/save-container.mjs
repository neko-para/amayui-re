/**
 * tools/lib/save-container.mjs —— **存档容器的解码器**（槽容器与系统容器共用同一套外壳）
 *
 * 这是 `data/spec/chapters/65-save.md` 里 `FMT-SAVE-HEADER` / `FMT-SAVE-ENVELOPE` / `FMT-SAVE-CODEC` /
 * `FMT-SAVE-SYSTEM` 四条的**可执行对照物**：写法**按规范文字**来，不从任何实现里抄；
 * 规范说得不够、必须"猜"才写得下去的地方，一律记进 `NOTES` 并在返回结果里点名（⛔ 不静默补偏移）。
 *
 * ★ 为什么它在这里而不是在某个 app 里：它是**核对样本用**的工具（`corpus/fixtures/` 的
 *   `SAVE.DAT` / `RT.DAT` / `SAVE76.DAT`），只依赖 64 位 JS 整数运算，不碰平台 API。
 * ★ 它**不是**模拟器的存档实现，也**不许**被模拟器当实现用：这个仓的规矩是
 *   "emulator 不许当 spec 的真源"，反过来 spec 的对照物也不许住进 emulator。
 */
import { unpack } from '@amayui/age-format/src/lzss.mts';

/** 规范缺口 / 措辞不足的登记（`code` 供测试断言；`text` 是给人看的） */
export const NOTES = [
  {
    code: 'padding-36',
    text:
      'FMT-SAVE-CODEC 只说压缩段前缀，没说"解密结果 = 12 + 压缩字节数"还是可以更长：' +
      '实测真机容器在压缩流之后还有零填充（余量 37~38 字节）。本模块按"取前 12 字节前缀、' +
      '压缩流取满声明的压缩字节数、其余当填充"解，并把填充长度报出来。',
  },
  {
    code: 'system-trailer',
    text:
      'FMT-SAVE-SYSTEM 原来只写到"末尾一个 0 字"，实测其后还有 8 个 0 字（两份样本一致）。' +
      '本模块把一个 0 字与那 8 个 0 字记成两段（`tail-zero` / `trailer`），形状固定、身份未定。',
  },
  {
    code: 'version-gate-unbound',
    text:
      'FMT-SAVE-SYSTEM 的版本门说"主版本 / 次版本"，但没绑到头的哪个字段：本模块按头的 +284 / +288 取' +
      '（该样本 3 / 20，两种读法都开门 ⇒ 样本判不了）。',
  },
];

/* ───────────────────────────── 基础读取 ───────────────────────────── */

export const u16 = (b, o) => (b[o] | (b[o + 1] << 8)) & 0xffff;
export const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
export const i32 = (b, o) => u32(b, o) | 0;
export const hex = (b, o, n) => [...b.subarray(o, o + n)].map((x) => x.toString(16).padStart(2, '0')).join(' ');

/* ───────────────────────────── 两个 CRC-32（FMT-SAVE-CODEC） ───────────────────────────── */

/** 非反射 CRC-32：poly `0x04C11DB7`，init `0xFFFFFFFF`，每字节 MSB 先，末步取反 */
export function crc32NonReflected(bytes) {
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i += 1) {
    crc = (crc ^ (bytes[i] << 24)) >>> 0;
    for (let k = 0; k < 8; k += 1) {
      crc = crc & 0x80000000 ? ((crc << 1) ^ 0x04c11db7) >>> 0 : (crc << 1) >>> 0;
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

/** 反射 CRC-32：poly `0xEDB88320`，init `0xFFFFFFFF`，末步取反 */
export function crc32Reflected(bytes) {
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i += 1) {
    crc = (crc ^ bytes[i]) >>> 0;
    for (let k = 0; k < 8; k += 1) crc = ((crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0)) >>> 0;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

/* ───────────────────────────── 1→2 字变换的逆（FMT-SAVE-CODEC） ───────────────────────────── */

/**
 * 密文 → 明文双字流：每源双字 → 两密文双字（高半字 × t、低半字 × t）。
 * 逆运算 = 逐对两次**整除**；余数不为 0 是唯一失败判据。
 */
export function untransform(cipher, seed, key) {
  if (cipher.length % 8 !== 0) throw new Error(`密文字节数 ${cipher.length} 不是 8 的倍数`);
  const n = cipher.length / 8;
  const out = new Uint8Array(n * 4);
  const dv = new DataView(out.buffer);
  let s = seed >>> 0;
  let k = key >>> 0;
  for (let i = 0; i < n; i += 1) {
    const t = k & 0xffff;
    if (t === 0) throw new Error('随机键低 16 位为 0 ⇒ 规范定为未定义行为');
    const hi = u32(cipher, 8 * i);
    const lo = u32(cipher, 8 * i + 4);
    if (hi % t !== 0 || lo % t !== 0) throw new Error(`第 ${i} 对余数不为 0 ⇒ 解密失败`);
    const x = ((((hi / t) & 0xffff) << 16) | ((lo / t) & 0xffff)) >>> 0;
    dv.setUint32(4 * i, (x ^ s) >>> 0, true);
    s = (s + 0x0b0b0b0b) >>> 0;
    k = (k + 0x0b02) >>> 0;
  }
  return out;
}

/* ───────────────────────────── 292 字节头（FMT-SAVE-HEADER） ───────────────────────────── */

export const HEADER_BYTES = 292;
export const PARAM_BYTES = 20;

export function parseSaveHeader(buf) {
  if (buf.length < HEADER_BYTES) throw new Error(`文件只有 ${buf.length} 字节，不足 ${HEADER_BYTES} 字节头`);
  const magic = String.fromCharCode(buf[0], buf[1], buf[2], buf[3]);
  let end = 8;
  while (end < 240 && buf[end] !== 0) end += 1;
  if (end >= 240) throw new Error('头 +8 的标题没有 NUL 结尾（超出 232 字节区）');
  const t = Array.from({ length: 8 }, (_, i) => u16(buf, 264 + 2 * i));
  return {
    magic,
    family: magic.slice(0, 2),
    kind: magic.slice(2),
    versionString: String.fromCharCode(buf[4], buf[5], buf[6], buf[7]),
    titleBytes: buf.subarray(8, end),
    declaredRawBytes: u32(buf, 240),
    time: { year: t[0], month: t[1], dayOfWeek: t[2], day: t[3], hour: t[4], minute: t[5], second: t[6], ms: t[7] },
    gameTime: i32(buf, 280),
    version1: u32(buf, 284),
    version2: u32(buf, 288),
  };
}

/* ───────────────────────────── 信封（ENVELOPE + CODEC） ───────────────────────────── */

/**
 * 解一份存档容器，返回**明文负载**（= 内层头之后的主体）与沿途每一步的判据结果。
 * 硬失败（魔数 / 校验和 / 整除 / 解压长度）一律抛 —— 调用方要么断言它不抛，要么断言它抛。
 */
export function decodeSaveContainer(buf) {
  const header = parseSaveHeader(buf);
  if (buf.length < HEADER_BYTES + PARAM_BYTES) throw new Error('不足 头 + 参数块');
  const p = HEADER_BYTES;
  const params = {
    words: u32(buf, p),
    crc1: u32(buf, p + 4),
    crc2: u32(buf, p + 8),
    seed: u32(buf, p + 12),
    key: u16(buf, p + 16),
  };
  const cipherBytes = params.words * 4;
  const bound = 2 * (header.declaredRawBytes + 24);
  if (cipherBytes > bound) {
    throw new Error(`密文 ${cipherBytes} 字节超过读取缓冲边界 2×(+240 + 24) = ${bound}`);
  }
  const cipherStart = HEADER_BYTES + PARAM_BYTES;
  if (cipherStart + cipherBytes > buf.length) {
    throw new Error(`参数块声明 ${cipherBytes} 字节密文，文件只余 ${buf.length - cipherStart}`);
  }
  const cipher = buf.subarray(cipherStart, cipherStart + cipherBytes);
  const crc1 = crc32NonReflected(cipher);
  const crc2 = crc32Reflected(cipher);
  if (crc1 !== params.crc1) throw new Error(`校验和 1 不符（期望 0x${params.crc1.toString(16)}，实算 0x${crc1.toString(16)}）`);
  if (crc2 !== params.crc2) throw new Error(`校验和 2 不符（期望 0x${params.crc2.toString(16)}，实算 0x${crc2.toString(16)}）`);

  const plain = untransform(cipher, params.seed, params.key);
  const prefix = { declaredRaw: u32(plain, 0), mirror: u32(plain, 4), compressed: u32(plain, 8) };
  if (prefix.declaredRaw !== prefix.mirror) throw new Error('压缩段前缀第 1、2 字不相等（规范说它们恒等）');
  if (12 + prefix.compressed > plain.length) throw new Error('压缩字节数超出解密结果长度');
  const stream = plain.subarray(12, 12 + prefix.compressed);
  const padding = plain.subarray(12 + prefix.compressed);

  let decompressed;
  let mode;
  if (prefix.declaredRaw === prefix.compressed) {
    decompressed = stream;
    mode = 'stored';
  } else {
    const { data, size } = unpack(stream, stream.length, prefix.declaredRaw);
    if (size !== prefix.declaredRaw) throw new Error(`解压产出 ${size} 字节 ≠ 声明的 ${prefix.declaredRaw}`);
    decompressed = data.subarray(0, size);
    mode = 'lzss';
  }
  if (decompressed.length < 8) throw new Error('解压结果不足内层 8 字节头');
  const innerCrc = { crc1: u32(decompressed, 0), crc2: u32(decompressed, 4) };
  const body = decompressed.subarray(8);
  if (crc32NonReflected(body) !== innerCrc.crc1) throw new Error('内层校验 1 不符');
  if (crc32Reflected(body) !== innerCrc.crc2) throw new Error('内层校验 2 不符');

  return {
    header,
    params,
    cipherBytes,
    bound,
    prefix,
    mode,
    paddingBytes: padding.length,
    innerCrc,
    body,
    /** 密文之后还剩多少字节（槽容器的"尾段"住在这里；系统容器应当为 0） */
    trailingBytes: buf.length - (cipherStart + cipherBytes),
    trailing: buf.subarray(cipherStart + cipherBytes),
  };
}

/* ───────────────────────────── 系统容器的负载分段（FMT-SAVE-SYSTEM） ───────────────────────────── */

/** 版本门：主版本 > 3，或主版本 == 3 且次版本 ≥ 10（`NOTES: version-gate-unbound`） */
export const systemGateOpen = (version1, version2) => version1 > 3 || (version1 === 3 && version2 >= 10);

/**
 * 按 `FMT-SAVE-SYSTEM` 逐段走一遍**系统容器**的明文负载（内层头之后的主体）。
 *
 * ★ 判据是**恰好消费到 EOF**：段边界由各段的计数算出，不许"为了让分段对上而调偏移"。
 *   对不上就把剩余字节原样报出来（`residual`），那是发现，不是需要抹平的误差。
 */
export function walkSystemPayload(body, { version1, version2 }) {
  const segs = [];
  const problems = [];
  let p = 0;
  const take = (n, what) => {
    if (p + n > body.length) {
      problems.push(`${what}：需要 ${n} 字节（偏移 ${p}），负载只剩 ${body.length - p}`);
      return false;
    }
    return true;
  };

  // 段 1：[u32 n][n × u32]
  if (!take(4, '段1 计数')) return { segs, problems, consumed: p, total: body.length, residual: body.subarray(p) };
  const n1 = u32(body, p);
  p += 4;
  if (!take(4 * n1, '段1 数组')) return { segs, problems, consumed: p, total: body.length, residual: body.subarray(p) };
  segs.push({ id: 'array', off: 0, count: n1, len: 4 + 4 * n1, values: (i) => u32(body, 4 + 4 * i) });
  p += 4 * n1;

  // 段 2：[u32 c][c × (12 字节键 + u32 值)]
  if (!take(4, '段2 计数')) return { segs, problems, consumed: p, total: body.length, residual: body.subarray(p) };
  const c = u32(body, p);
  const off2 = p;
  p += 4;
  if (!take(16 * c, '段2 记录')) return { segs, problems, consumed: p, total: body.length, residual: body.subarray(p) };
  const intKeys = Array.from({ length: c }, (_, i) => {
    const o = off2 + 4 + 16 * i;
    return { key: body.subarray(o, o + 12), value: i32(body, o + 12) };
  });
  segs.push({ id: 'intKeys', off: off2, count: c, len: 4 + 16 * c, records: intKeys });
  p += 16 * c;

  // 段 3：[u32 m][u32 字数][m × (键 NUL 串, 值 NUL 串)]
  if (!take(8, '段3 头')) return { segs, problems, consumed: p, total: body.length, residual: body.subarray(p) };
  const m = u32(body, p);
  const words = u32(body, p + 4);
  const off3 = p;
  p += 8;
  const strEnd = p + 4 * words;
  if (strEnd > body.length) {
    problems.push(`段3 声明 ${words} 字（${4 * words} B），超出负载尾部`);
    return { segs, problems, consumed: p, total: body.length, residual: body.subarray(p) };
  }
  const stringKeys = [];
  const readNul = () => {
    let e = p;
    while (e < strEnd && body[e] !== 0) e += 1;
    if (e >= strEnd) return null;
    const s = body.subarray(p, e);
    p = e + 1;
    return s;
  };
  for (let i = 0; i < m; i += 1) {
    const k = readNul();
    const v = readNul();
    if (k === null || v === null) {
      problems.push(`段3 第 ${i} 条的串在声明字数内没有 NUL`);
      break;
    }
    stringKeys.push({ key: k, value: v });
  }
  const paddingInSeg3 = strEnd - p;
  segs.push({
    id: 'stringKeys',
    off: off3,
    count: m,
    declaredWords: words,
    len: 8 + 4 * words,
    records: stringKeys,
    /** 两条串首尾相接实际占多少字节（不含那段补齐） */
    packedBytes: p - (off3 + 8),
    paddingInSeg3,
  });
  p = strEnd;

  // 段 4：版本门之后的 0x400 字节块 + [u32 k][k × u32]
  if (systemGateOpen(version1, version2)) {
    if (!take(0x400, '段4 0x400 块')) return { segs, problems, consumed: p, total: body.length, residual: body.subarray(p) };
    const block = body.subarray(p, p + 0x400);
    p += 0x400;
    if (!take(4, '段4 [u32 k]')) return { segs, problems, consumed: p, total: body.length, residual: body.subarray(p) };
    const k = u32(body, p);
    p += 4;
    if (!take(4 * k, '段4 k 个 u32')) return { segs, problems, consumed: p, total: body.length, residual: body.subarray(p) };
    const words4 = Array.from({ length: k }, (_, i) => u32(body, p + 4 * i));
    p += 4 * k;
    segs.push({ id: 'gated', off: strEnd, count: k, len: 0x400 + 4 + 4 * k, block, words: words4 });
  }

  // 段 5：末尾一个 0 字
  if (!take(4, '段5 末尾 0 字')) return { segs, problems, consumed: p, total: body.length, residual: body.subarray(p) };
  const tailZero = u32(body, p);
  segs.push({ id: 'tailZero', off: p, count: 1, len: 4, value: tailZero });
  if (tailZero !== 0) problems.push(`段5 的"末尾 0 字"实际是 0x${tailZero.toString(16)}`);
  p += 4;

  // 段 6：收尾的 8 个 0 字（形状固定、身份未定 —— 见 NOTES: system-trailer）
  // ★ 这一段**不能**用"吃掉剩余全部"来写，否则"恰好消费到 EOF"就变成恒真了：
  //   规范给的是一条**定长**形状（8 个 0 字 = 32 字节），长度不符就报出来。
  const TRAILER_BYTES = 32;
  const remaining = body.length - p;
  if (remaining !== TRAILER_BYTES) {
    problems.push(`段6 收尾 0 字串应为 ${TRAILER_BYTES} 字节，实测剩 ${remaining} 字节`);
  }
  const trailerWords = [];
  for (let q = p; q + 4 <= body.length; q += 4) trailerWords.push(u32(body, q));
  segs.push({ id: 'trailer', off: p, count: trailerWords.length, len: 4 * trailerWords.length, words: trailerWords });
  p = body.length;

  return { segs, problems, consumed: p, total: body.length, residual: body.subarray(p), eof: p === body.length };
}
