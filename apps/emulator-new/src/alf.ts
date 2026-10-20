/**
 * ALF 索引（`data/spec/chapters/20-alf.md`）：索引头 → 段（LZSS）→ 目录区（TOC）。
 * ★ 纯函数：吃索引文件的字节，产出"归档列表 + 文件表"；不碰文件系统、不读载荷。
 * ★ 只实现 S4 族（S3IC / S4IC / S3AC / S4AC）—— 本作语料只有 S4；S5 缺样本，遇到就响亮失败。
 */
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
}

const S4 = {
  header: 300,
  section: 300,
  appendSection: 268,
  archiveEntry: 256,
  nameLength: 64,
  fileRecord: 80,
  encoding: 'latin1' as const,
};

/** 读定长字段里的 NUL 结尾字符串（字段尾部可能是未初始化字节，⛔ 不许"拼名字+补零"重写） */
function readField(buf: Buffer, off: number, size: number): string {
  const end = buf.indexOf(0, off);
  const stop = end === -1 || end > off + size ? off + size : end;
  return buf.toString(S4.encoding, off, stop);
}

export function parseAlfIndex(buf: Buffer): AlfIndex {
  if (buf.length < 4) throw new Error('alf-too-small');
  const signature = buf.toString('latin1', 0, 4);
  if (!/^S[34](IC|AC)$/.test(signature)) {
    throw new Error(`alf-unsupported-signature:${signature}（本实现只覆盖 S4 族；S5 缺样本）`);
  }
  const appended = signature.endsWith('AC');
  const sectionPos = appended ? S4.appendSection : S4.section;
  if (sectionPos + 12 > buf.length) throw new Error(`alf-section-past-eof:${sectionPos}`);
  const originalLength = buf.readUInt32LE(sectionPos);
  const originalLength2 = buf.readUInt32LE(sectionPos + 4);
  const packedLength = buf.readUInt32LE(sectionPos + 8);
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
  const archiveCount = toc.readUInt32LE(take(4));
  const archives: string[] = [];
  for (let i = 0; i < archiveCount; i += 1) {
    const off = take(S4.archiveEntry);
    archives.push(readField(toc, off, S4.archiveEntry));
  }
  const fileCount = toc.readUInt32LE(take(4));
  const files: AlfFile[] = [];
  for (let i = 0; i < fileCount; i += 1) {
    const off = take(S4.fileRecord);
    files.push({
      name: readField(toc, off, S4.nameLength),
      archiveIndex: toc.readUInt32LE(off + S4.nameLength),
      fileIndex: toc.readUInt32LE(off + S4.nameLength + 4),
      offset: toc.readUInt32LE(off + S4.nameLength + 8),
      length: toc.readUInt32LE(off + S4.nameLength + 12),
    });
  }
  return { signature, sectionPos, originalLength, packedLength, archives, files };
}
