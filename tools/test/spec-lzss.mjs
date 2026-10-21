/** 4 KiB 环形窗口 LZSS（`20-alf.md`「ALF 的 LZSS」）：只读、纯函数，供 ALF / AGF 两个 spec 守卫共用 */
/** 4 KiB 环形窗口 LZSS（参数见「ALF 的 LZSS」）；返回解压结果与"是否跑过解压" */
export function lzssDecode(src, outLen) {
  const win = new Uint8Array(4096);
  let pos = 4078;
  const out = new Uint8Array(outLen);
  let oi = 0;
  let si = 0;
  let flags = 0;
  let mask = 0;
  while (si < src.length) {
    if (mask === 0) { flags = src[si++]; mask = 1; }
    // ★ flag 位为 1 ⇒ 字面量；为 0 ⇒ (位置, 长度) 匹配（低位先；与真实 *.AAI 的解出一致）
    if (flags & mask) {
      if (si >= src.length) break;
      const v = src[si++];
      win[pos] = v; pos = (pos + 1) & 0xfff;
      if (oi < out.length) out[oi++] = v;
    } else {
      if (si + 1 >= src.length) break;
      const b0 = src[si++];
      const b1 = src[si++];
      const p = b0 | ((b1 & 0xf0) << 4);
      const n = (b1 & 0x0f) + 3;
      for (let k = 0; k < n; k += 1) {
        const v = win[(p + k) & 0xfff];
        win[pos] = v; pos = (pos + 1) & 0xfff;
        if (oi < out.length) out[oi++] = v;
      }
    }
    mask = (mask << 1) & 0xff;
  }
  return { out, written: oi };
}

