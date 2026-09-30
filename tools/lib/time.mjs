/**
 * tools/lib/time.mjs — **纯时间工具**（不认识任何领域数据）
 *
 * 存在理由：我们记 mtime 时要把"instant"还原成"采集时区下的墙上时间"，
 * 而这件事**必须与跑命令的机器时区无关**（win32 + macOS 两台机器）。
 */

/** 本机时区偏移（分钟，东为正；UTC+8 ⇒ 480），与 JS 的 getTimezoneOffset() 反号 */
export const tzOffsetMinutes = (d = new Date()) => -d.getTimezoneOffset();

/** instant + 时区 ⇒ 墙上时间 `yyyy-MM-ddTHH:mm:ss`（用 UTC getter，**结果与机器时区无关**） */
export function wallClock(ms, tzOff) {
  const pad = (n) => String(n).padStart(2, '0');
  const d = new Date(ms + tzOff * 60000);
  return (
    `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())}` +
    `T${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}:${pad(d.getUTCSeconds())}`
  );
}
