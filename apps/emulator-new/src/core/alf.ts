/**
 * ALF 索引（`data/spec/chapters/20-alf.md`）：索引头 → 段（LZSS）→ 目录区（TOC）。
 * ★ 纯函数：吃索引文件的字节，产出"归档列表 + 文件表"；不碰文件系统、不读载荷。
 * ★ 只实现 S4 族（S3IC / S4IC / S3AC / S4AC）—— 本作语料只有 S4；S5 缺样本，遇到就响亮失败。
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
  sectionPos: number;
  originalLength: number;
  packedLength: number;
  archives: string[];
  files: AlfFile[];
  /** 索引尾部携带的**发行默认配置**（`55-config.md`）；没有尾部 ⇒ 空数组 */
  configDefaults: { key: string; value: string }[];
}

const S4 = {
  header: 300,
  section: 300,
  appendSection: 268,
  archiveEntry: 256,
  nameLength: 64,
  fileRecord: 80,
};

/** 读定长字段里的 NUL 结尾字符串（字段尾部可能是未初始化字节，⛔ 不许"拼名字+补零"重写） */
function readField(buf: Uint8Array, off: number, size: number): string {
  const end = indexOfByte(buf, 0, off);
  const stop = end === -1 || end > off + size ? off + size : end;
  return decodeLatin1(buf, off, stop);
}

export function parseAlfIndex(buf: Uint8Array): AlfIndex {
  if (buf.length < 4) throw new Error('alf-too-small');
  const signature = decodeLatin1(buf, 0, 4);
  if (!/^S[34](IC|AC)$/.test(signature)) {
    throw new Error(`alf-unsupported-signature:${signature}（本实现只覆盖 S4 族；S5 缺样本）`);
  }
  const appended = signature.endsWith('AC');
  const sectionPos = appended ? S4.appendSection : S4.section;
  if (sectionPos + 12 > buf.length) throw new Error(`alf-section-past-eof:${sectionPos}`);
  const originalLength = readU32LE(buf, sectionPos);
  const originalLength2 = readU32LE(buf, sectionPos + 4);
  const packedLength = readU32LE(buf, sectionPos + 8);
  if (originalLength !== originalLength2) {
    throw new Error(`alf-section-length-mismatch:${originalLength}/${originalLength2}`);
  }
  if (sectionPos + 12 + packedLength > buf.length) {
    throw new Error(`alf-packed-past-eof:${packedLength}`);
  }
  const packed = buf.subarray(sectionPos + 12, sectionPos + 12 + packedLength);
  const toc = decompressLzss(packed, originalLength);

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
    const off = take(S4.archiveEntry);
    archives.push(readField(toc, off, S4.archiveEntry));
  }
  const fileCount = readU32LE(toc, take(4));
  const files: AlfFile[] = [];
  for (let i = 0; i < fileCount; i += 1) {
    const off = take(S4.fileRecord);
    files.push({
      name: readField(toc, off, S4.nameLength),
      archiveIndex: readU32LE(toc, off + S4.nameLength),
      fileIndex: readU32LE(toc, off + S4.nameLength + 4),
      offset: readU32LE(toc, off + S4.nameLength + 8),
      length: readU32LE(toc, off + S4.nameLength + 12),
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

  return { signature, sectionPos, originalLength, packedLength, archives, files, configDefaults };
}
