/**
 * tools/lib/ui-bake/agf-source.mjs —— 纯工具：从**原始游戏件**取 UI 图（ALF 索引 → AGF → RGBA）
 *
 * 只读来源的定位口径（已实测，不要凭印象改）：
 *   · 索引**不是** `DATA1.AAI` —— 那个文件不存在。DATA1..DATA8 的索引在 **`SYS4INI.BIN`**
 *     （S4 布局：300 B 头 + 目录区；归档名 `DATA1.ALF`…`DATA8.ALF` 写在目录里）。
 *   · `SO0xx.AGF` 在 `DATA1.ALF` 里；`SO025/SO034/SO043A/SO044` 这几个在安装根目录另有**裸件**，
 *     但入库/复现统一走 ALF 那一份（`corpus/assets.json` 的 `assets/ui-images-baked` 也是按它登记的）。
 *   · 解出的 RGBA 与 `corpus/assets/ui-images/*-0.png` **逐像素相同**（17 张全量实测 14/17 命中，
 *     3 张例外见 `docs/` 的审计记录）⇒ 这条链本身就是"旧图从哪来"的可复现答案。
 *
 * ★ 只读：本模块只 `fs.readSync` 原始游戏件，**不写、不删**（`AGENTS.md` §0 第 1 条）。
 */
import fs from 'node:fs';
import path from 'node:path';

import { readAlf, indexEntries, readAgfBuffer, decodeRgba, parseWhBpp, extractPaletteRgb } from './age-format.mjs';

/** 索引文件名（按存在性挑：都装档才有 AAI，裸 DATA1.ALF 的场子只有 SYS4INI.BIN） */
const INDEX_CANDIDATES = ['SYS4INI.BIN', 'DATA1.AAI'];

/**
 * ★ **裸件优先于 ALF 条目**（游戏运行期语义，实测确证）：安装根目录下的 `<名>.AGF`
 * 优先于 `DATA1.ALF` 里同名的那一条。`SO025` 就是活例子 ——
 *   · 安装根 `so025.AGF`（258633 B）解码后 = 旧仓 `res/images/SO025-0.png`（逐像素相同）；
 *   · `DATA1.ALF` 里的同名条目（257625 B）是**另一个文件**：同尺寸 / 同 bpp / 同 meta / 同 ACIF，
 *     但调色板 256 项只有 18 项相同 ⇒ 全图 7 万余像素不同。那是官方的 overlay 变体，
 *     **不是**由 ALF 载荷派生的（任何 clean 算子都表达不了）。
 * ⇒ recipe 的 `source.kind` 有两种：`"alf"`（从 ALF 取，缺省）与 `"loose"`（从安装根裸件取）。
 */
export const loosePath = (gameDir, agf) => path.join(gameDir, agf);

export function hasLoose(gameDir, agf) {
  try {
    return fs.statSync(loosePath(gameDir, agf)).isFile();
  } catch {
    return false;
  }
}

/** 打开一个游戏安装目录的 ALF 索引 */
export function openGame(gameDir) {
  for (const cand of INDEX_CANDIDATES) {
    const p = path.join(gameDir, cand);
    if (fs.existsSync(p)) {
      const alf = readAlf(p);
      return { alf, indexFile: p, gameDir, entries: indexEntries(alf).map };
    }
  }
  throw new Error(`在 ${gameDir} 里找不到 ALF 索引（找过：${INDEX_CANDIDATES.join(' / ')}）`);
}

/** 从 ALF 数据体里读一个条目的原始字节 */
export function readEntry(archive, name) {
  const e = archive.entries.get(name);
  if (!e) throw new Error(`ALF 目录里没有 ${name}`);
  const file = path.join(archive.gameDir, archive.alf.archives[e.archiveIndex].filename);
  const fd = fs.openSync(file, 'r');
  try {
    const buf = Buffer.alloc(e.length);
    let read = 0;
    while (read < e.length) read += fs.readSync(fd, buf, read, e.length - read, e.offset + read);
    return { buf, entry: e, file };
  } finally {
    fs.closeSync(fd);
  }
}

/**
 * 取一张 UI 图的**原始** RGBA（未改动的日文原图）。
 * @param {object} archive `openGame` 的返回值
 * @param {string} name AGF 名
 * @param {{kind?: 'alf'|'loose'}} [opts] `loose` = 取安装根裸件（见上面 ★）
 * @returns {{name:string,width:number,height:number,bpp:number,rgba:Buffer,agf:object,buf:Buffer,palette:Array|null,file:string,source:string}}
 */
export function loadOriginal(archive, name, opts = {}) {
  const kind = opts.kind ?? 'alf';
  let buf;
  let file;
  if (kind === 'loose') {
    file = loosePath(archive.gameDir, name);
    if (!fs.existsSync(file)) throw new Error(`安装根没有裸件 ${name}：${file}`);
    buf = fs.readFileSync(file);
  } else {
    ({ buf, file } = readEntry(archive, name));
  }
  const agf = readAgfBuffer(buf, name);
  const [width, height, bpp] = parseWhBpp(agf.meta);
  const { rgba } = decodeRgba(agf);
  return { name, width, height, bpp, rgba, agf, buf, palette: extractPaletteRgb(agf.meta), file, source: kind };
}

/** 取一个块的 AGF 名与干净名（`SO009A` → `SO009A.AGF`） */
export const agfNameOf = (block) => `${block}.AGF`;
