/**
 * ALF 索引（`data/spec/chapters/20-alf.md`）：索引头 → 段（LZSS）→ 目录区（TOC）。
 * ★ 纯函数：吃索引文件的字节，产出"归档列表 + 文件表"；不碰文件系统、不读载荷。
 * ★ 签名族：`S3AI` / `S4AI` / `S3AC` / `S4AC`（ALF 索引）与 `S3IC` / `S4IC`（初始化文件，
 *   头长 300 —— `SYS?INI.BIN`）。本作语料只有 S4；S5 缺样本，遇到就响亮失败。
 * ★ **目录区起点由签名尾部决定**（`CORE-FS-EXT-INDEX-KEY`）：`AC ⇒ 268 + 12`（压缩信息块）、
 *   `AI ⇒ 268`（裸 TOC，不压缩）、`IC ⇒ 300`。⛔ 不要把它们并成两档：
 *   `S4IC` 的段头在 300，把它当 268 会把 `SYS4INI.BIN` 读错位。
 */
import { decodeLatin1, indexOfByte, readI32LE, readU32LE } from './bytes.ts';
import { decompressLzss } from './lzss.ts';

export interface AlfFile {
  name: string;
  archiveIndex: number;
  fileIndex: number;
  offset: number;
  length: number;
}

export interface AlfIndex {
  signature: string;
  /** 头的**标题串**（`+8` 起、NUL 结束的字节串）；扩展索引装载时要与基础索引逐字节比较 */
  title: string;
  /**
   * 头内**区号**（`+264` 的 u32）。★ 只对 ALF 族（`S?AI` / `S?AC`）有意义：
   * `S?IC` 的头长 300、同一个偏移是**另一个字段**（实测 `SYS4INI.BIN` 这里是 640）⇒ `-1`。
   * 区号不从文件名解析（`CORE-FS-EXT-INDEX-KEY`）。
   */
  zone: number;
  sectionPos: number;
  originalLength: number;
  packedLength: number;
  archives: string[];
  files: AlfFile[];
  /** 索引尾部携带的**发行默认配置**（`55-config.md`）；没有尾部 ⇒ 空数组 */
  configDefaults: { key: string; value: string }[];
}

const ALF = {
  /** ALF 族的定长头（`S?AI` / `S?AC`）；`S?IC` 是另一个加载器的 300 字节头 */
  head: 268,
  icHead: 300,
  /** 压缩族紧接头之后的压缩信息块长度（解压后长度 ×2 + 压缩数据长度） */
  packInfo: 12,
  archiveEntry: 256,
  nameLength: 64,
  fileRecord: 80,
  /** 头内区号字段的偏移（ALF 族才有；`S?IC` 的同一偏移不是区号） */
  zoneOffset: 264,
};

/** 读定长字段里的 NUL 结尾字符串（字段尾部可能是未初始化字节，⛔ 不许"拼名字+补零"重写） */
function readField(buf: Uint8Array, off: number, size: number): string {
  const end = indexOfByte(buf, 0, off);
  const stop = end === -1 || end > off + size ? off + size : end;
  return decodeLatin1(buf, off, stop);
}

/** 头内标题串（`+8`，NUL 结束；规范不规定宽度，但必须终结在定长头之内） */
function readTitle(buf: Uint8Array): string {
  const end = indexOfByte(buf, 0, 8);
  if (end < 0 || end > ALF.head) throw new Error(`alf-title-unterminated:${end}`);
  return decodeLatin1(buf, 8, end);
}

export function parseAlfIndex(buf: Uint8Array): AlfIndex {
  if (buf.length < 4) throw new Error('alf-too-small');
  const signature = decodeLatin1(buf, 0, 4);
  if (!/^S[34](IC|AC|AI)$/.test(signature)) {
    throw new Error(`alf-unsupported-signature:${signature}（本实现只覆盖 S3/S4 族；S5 缺样本）`);
  }
  if (buf.length < ALF.head) throw new Error(`alf-header-past-eof:${buf.length}`);
  const title = readTitle(buf);
  // ★ `S?IC` 的 +264 是别的字段（不是区号）⇒ 报 -1，免得把 640 当成"区号 640"
  const zone = signature.endsWith('IC') ? -1 : readU32LE(buf, ALF.zoneOffset);
  const rawToc = signature.endsWith('AI');
  // AC ⇒ 268（压缩信息块在 [268,280)）；AI ⇒ 268（裸 TOC）；IC ⇒ 300
  const sectionPos = signature.endsWith('AC') ? ALF.head : rawToc ? ALF.head : ALF.icHead;
  let originalLength: number;
  let packedLength: number;
  let toc: Uint8Array;
  if (rawToc) {
    // 非压缩族：头之后直接是裸 TOC 段（`FMT-ALF-HEADER`）；长度信息不存在 ⇒ 到 EOF 为止
    if (sectionPos > buf.length) throw new Error(`alf-section-past-eof:${sectionPos}`);
    toc = buf.subarray(sectionPos);
    originalLength = toc.length;
    packedLength = toc.length;
  } else {
    if (sectionPos + ALF.packInfo > buf.length) throw new Error(`alf-section-past-eof:${sectionPos}`);
    originalLength = readU32LE(buf, sectionPos);
    const originalLength2 = readU32LE(buf, sectionPos + 4);
    packedLength = readU32LE(buf, sectionPos + 8);
    if (originalLength !== originalLength2) {
      throw new Error(`alf-section-length-mismatch:${originalLength}/${originalLength2}`);
    }
    if (sectionPos + ALF.packInfo + packedLength > buf.length) {
      throw new Error(`alf-packed-past-eof:${packedLength}`);
    }
    const packed = buf.subarray(sectionPos + ALF.packInfo, sectionPos + ALF.packInfo + packedLength);
    toc = decompressLzss(packed, originalLength);
  }

  let at = 0;
  const take = (n: number): number => {
    if (at + n > toc.length) throw new Error(`alf-toc-truncated:${at}+${n}>${toc.length}`);
    const v = at;
    at += n;
    return v;
  };
  const archiveCount = readU32LE(toc, take(4));
  const archives: string[] = [];
  for (let i = 0; i < archiveCount; i += 1) {
    const off = take(ALF.archiveEntry);
    archives.push(readField(toc, off, ALF.archiveEntry));
  }
  const fileCount = readU32LE(toc, take(4));
  const files: AlfFile[] = [];
  for (let i = 0; i < fileCount; i += 1) {
    const off = take(ALF.fileRecord);
    files.push({
      name: readField(toc, off, ALF.nameLength),
      archiveIndex: readU32LE(toc, off + ALF.nameLength),
      fileIndex: readU32LE(toc, off + ALF.nameLength + 4),
      offset: readU32LE(toc, off + ALF.nameLength + 8),
      length: readU32LE(toc, off + ALF.nameLength + 12),
    });
  }

  // ── 尾部：发行默认配置（`55-config.md`）──
  // ★ 两个计数数组不属于配置；引擎只把它们读进别处，这里只跳过。
  // ★ 两个长度标记都只做“非零”判断；配置串前面的 953 不是循环上界。
  // ★ 目录区之后一个字节都没有 ⇒ 没有尾部（AAI 等）；只要开始读尾部，越界一律
  //   `alf-tail-truncated:<偏移>`，⛔ 不把截断当成“没有默认表”。
  const configDefaults: { key: string; value: string }[] = [];
  if (at < toc.length) {
    const tailTake = (n: number): number => {
      if (n < 0 || at + n > toc.length) throw new Error(`alf-tail-truncated:${at}`);
      const off = at;
      at += n;
      return off;
    };
    const readI32 = (): number => readI32LE(toc, tailTake(4));
    const skipCounted = (): void => {
      const n = readI32();
      if (n > 0) tailTake(n * 4);
    };
    const readCString = (what: string): string => {
      const end = indexOfByte(toc, 0, at);
      if (end < 0) throw new Error(`alf-tail-truncated:${what}@${at}`);
      const s = decodeLatin1(toc, at, end);
      at = end + 1;
      return s;
    };
    const len1 = readI32();
    if (len1 > 0) {
      skipCounted(); // 第一个计数数组（与配置无关）
      skipCounted(); // 第二个计数数组（与配置无关）
    }
    const len2 = readI32();
    if (len2 > 0) {
      const n = readI32();
      if (n < 0) throw new Error(`alf-tail-truncated:${at}`);
      for (let i = 0; i < n; i += 1) {
        const key = readCString('key');
        const value = readCString('value');
        configDefaults.push({ key, value });
      }
    }
  }

  return { signature, title, zone, sectionPos, originalLength, packedLength, archives, files, configDefaults };
}
