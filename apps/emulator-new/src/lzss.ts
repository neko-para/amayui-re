/**
 * ALF 目录区的 LZSS 解压（`data/spec/chapters/20-alf.md` 的「ALF 的 LZSS」）。
 * ★ 纯函数：不碰文件系统；输入是压缩字节 + 解压后长度。
 * ★ 位置是 4 KiB 环形窗口内的**绝对写位置**（12 位），不是"距离"；长度 = (hi & 0x0F) + 3。
 * ★ 窗口初值 0（与真实 SYS4INI.BIN 的目录区一致；判据 = 解出的 TOC 条目数与名字）。
 */
const WINDOW = 4096;
const RING_MASK = WINDOW - 1;

export function decompressLzss(packed: Buffer, outLength: number): Buffer {
  if (!Number.isInteger(outLength) || outLength < 0) throw new Error(`lzss-bad-out-length:${outLength}`);
  const ring = Buffer.alloc(WINDOW);
  // 引擎的环形窗口写指针起点（与真实语料一致；不是算法的自由选择）
  let r = 0xfee;
  const out = Buffer.alloc(outLength);
  let o = 0;
  let p = 0;
  const need = (n: number): void => {
    if (p + n > packed.length) throw new Error(`lzss-truncated:${p}+${n}>${packed.length}`);
  };
  while (o < outLength) {
    need(1);
    const flags = packed[p++];
    for (let bit = 0; bit < 8 && o < outLength; bit += 1) {
      if ((flags & (1 << bit)) !== 0) {
        need(1);
        const b = packed[p++];
        out[o++] = b;
        ring[r] = b;
        r = (r + 1) & RING_MASK;
      } else {
        need(2);
        const b1 = packed[p++];
        const b2 = packed[p++];
        const pos = b1 | ((b2 & 0xf0) << 4);
        const len = (b2 & 0x0f) + 3;
        for (let i = 0; i < len && o < outLength; i += 1) {
          const b = ring[(pos + i) & RING_MASK];
          out[o++] = b;
          ring[r] = b;
          r = (r + 1) & RING_MASK;
        }
      }
    }
  }
  return out;
}
