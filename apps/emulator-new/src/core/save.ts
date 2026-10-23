/**
 * 存档文件的平台边界（`0x1ab` / `0x1ac` 等）。
 *
 * ★ 核心不认识目录、也不碰 `node:fs`：槽号 → 路径、删 / 拷都由平台实现。
 * ★ 返回 `false` 表示该动作失败；调用方按状态码 0/1/2 落操作数。
 */
export interface SaveHost {
  /** 槽号 + 文件种类 → 平台路径（`.DAT` / `.STH`）。 */
  fileForSlot(slot: number, kind: 'dat' | 'sth'): string;
  /** 读整个文件；不存在 / 打不开 ⇒ null。 */
  read(path: string): Promise<Uint8Array | null>;
  /** 删除一个文件；不存在也算失败（调用方据此写 1）。 */
  remove(path: string): Promise<boolean>;
  /** 复制一个文件；目标已存在时覆盖。 */
  copy(from: string, to: string): Promise<boolean>;
  /** 写一个新文件（整份字节）。 */
  write(path: string, bytes: Uint8Array): Promise<boolean>;
  /** 原子替换：把 from 改名为 to（用于临时文件落盘后替换）。 */
  rename(from: string, to: string): Promise<boolean>;
  /** 0x19e 的覆写确认；缺省 = 平台没有确认框（该支响亮失败）。 */
  confirmOverwrite?(slot: number): Promise<boolean>;
}
