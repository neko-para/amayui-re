#!/usr/bin/env node
/**
 * tools/lift.mjs — **CLI**：`data/lift/overrides.json`（emulator-lift 覆盖清单）的自描述 / 校验 / 同步 / 报告
 *               + `.lst` 语料报告。
 *
 * 经派发器：`pnpm tools lift <describe|check|report|corpus-report|sync> [args…]`
 * 也可独立跑：`node tools/lift.mjs --check`
 *
 * ★ 分工（`tools/README.md` §0）：本文件只做"读 → 判 → 打印 / 落盘"；字段语义的唯一真源
 *   就是本文件自己的 `describe()`（没有第二份 schema 副本）。
 * ★ 覆盖清单是**显式**数据，不是生成物：它的每条 `stub` 都必须与 C++ 注册表
 *   （`apps/emulator-lift/src/shims/*.cpp` 的 `addUnimplemented` / `addStub`）一一对应。
 *   `sync --write` 只是把"已登记但还没抄进 JSON 的那几条"落盘；`check` 会逐条比对，
 *   所以 **JSON 漂了会红**（不是靠人记得）。
 * ★ `src/shims/register_import_gaps.cpp` 是**派生文件**（由本工具生成）：
 *   PE 导入表里有、但既没实现也没登记的条目 ⇒ 全部变成 `addUnimplemented`（带 reason）。
 *   目标是"静态缺口 100% 有理由"，⛔ 不是"静默跳过"。
 * ★ PE 导入表由本文件自己解析（不依赖构建）：判据是拆壳件的字节，不是运行时快照。
 *   `.lst` 头部 `; Input SHA256 :` 与 PE 的 sha256 必须相等（与 A3 ⓪ 同一条锚）。
 */
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';

import { REPO_ROOT } from './lib/paths.mjs';
import { peImageBase, peMap } from './lib/ledger.mjs';

export const DOMAIN = {
  id: 'lift',
  title: 'emulator-lift · **覆盖清单**（`data/lift/overrides.json`）与 `.lst` 语料报告',
  data: [
    '`data/lift/overrides.json`（覆盖清单：每条 stub 必须对应 C++ 注册表里的一条登记）',
    '`apps/emulator-lift/src/shims/*.cpp`（**注册表真源**：`add` / `addStub` / `addUnimplemented`）',
    '`apps/emulator-lift/src/shims/register_import_gaps.cpp`（**派生、入库**：导入表缺口的显式登记，`sync --write` 生成）',
    '`corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`（PE 导入表 / sha256，只读）',
    '`corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst`（语料：指令行 / 段 / collapsed，只读）',
  ],
  access: '`overrides.json` 与 `register_import_gaps.cpp` 只经 `sync --write` 写；其余全部只读',
  tool: 'tools/lift.mjs',
};

export const OPERATIONS = [
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：字段 / 枚举 / 不变量（含谁在守）/ 操作（schema 真源）' },
  { name: 'check', argv: ['--check'], mutates: false, summary: '全量校验：JSON ↔ C++ 注册表 ↔ PE 导入表 ↔ 派生缺口文件（红 ⇒ 退出码 1）' },
  { name: 'report', argv: ['--report'], mutates: false, summary: '覆盖报告：implemented / stub / bare / never-imported 四类计数 + 逐条清单；`--bench N` 顺带把 MIPS 写进报告（S8）' },
  { name: 'sync', argv: ['--sync'], mutates: true, summary: '把 C++ 注册表与 PE 导入表派生到 `overrides.json` + `register_import_gaps.cpp`（缺省 dry-run，`--write` 落盘）' },
  { name: 'corpus-report', argv: ['--corpus-report'], mutates: false, summary: '`.lst` 语料报告：指令行 / 助记符形态 / 段 / collapsed / x87·MMX·SSE 计数' },
];

export const OVERRIDES_FILE = 'data/lift/overrides.json';
export const OVERRIDES_DOC = 'data/lift/overrides.md';
export const SHIM_DIR = 'apps/emulator-lift/src/shims';
export const GAP_FILE = 'apps/emulator-lift/src/shims/register_import_gaps.cpp';
export const EXE_FILE = 'corpus/assets/age/AGE.EXE__dumped.sectfix.EXE';
export const LST_FILE = 'corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst';

export const SCHEMA = 1;
export const KINDS = ['replace', 'stub', 'hook'];
export const BOUNDARY_KINDS = ['import', 'function', 'subsystem'];
export const DOC_POINTER =
  '由 tools/lift.mjs 拥有：字段语义 / 不变量 / 操作看 `pnpm tools lift describe`（tools/lift.mjs 的自描述）。';

/** 逐模块的缺口理由（生成 `register_import_gaps.cpp` 用；改这里 ⇒ `sync --write` 后重生成） */
export const MODULE_REASONS = {
  kernel32: 'M1b 尚未覆盖的 kernel32 导入（S0 显式登记：导入表里有、但既没实现也没登记）',
  user32: 'S6 窗口 / 消息族（M1b 后续；菜单 / 模态按 SCOPE §1 另记 stub）',
  gdi32: 'M2 字体 / 2D 图形簇（plan §4 D6：GDI32 的 7 条不在 T0 七族）',
  advapi32: '注册表族：M1b 之后（配置读写走 S6 之后的批次）',
  winmm: 'winmm 的非计时导入（摇杆 / MCI / mixer / mmio）：M2/M3 的输入·音频·多媒体文件 IO，M1 不做（计时族的 timeGetTime / timeSetEvent / timeKillEvent / timeGetDevCaps 已实现）',
  version: '版本信息族：M1 未实现（启动路径不需要）',
  shell32: 'shell32：M1 未实现（CRT / 启动路径不需要）',
  shlwapi: 'shlwapi：M1 未实现（CRT / 启动路径不需要）',
  imagehlp: 'imagehlp：M1 未实现（崩溃转储不在本线）',
  imm32: '输入法族：M2（窗口 / 输入）',
  ole32: 'COM 内存族已实现（同一个 guest 堆）；其余 ole32 导入属 M2/M3',
  oleaut32: 'oleaut32：M2/M3（VARIANT / 自动化）',
  ddraw: 'M2 DirectDraw 簇',
  d3d9: 'M3 D3D9 簇（SCOPE §1：3D 不做 / 走 stub）',
  d3dx9_43: 'M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub）',
  mf: 'M3 视频簇（SCOPE §1：skip，只保留影片 opcode 推进）',
  mfplat: 'M3 视频簇（SCOPE §1：skip）',
  evr: 'M3 视频簇（SCOPE §1：skip）',
  dxva2: 'M3 视频簇（SCOPE §1：skip）',
};
export const DEFAULT_GAP_REASON = 'M1 未实现（S0 显式登记：导入表里有、但既没实现也没登记）';

// ---------------------------------------------------------------------------------------
// D6 七族（A5 的判据数据）
// ★ 这是**判据数据**，不是文档：`check()` 用它把 stub 分组（plan §4 D6 / §5 A5）。
// ★ 只列"会出现在导入表 / 注册表里的键"；不在任何族里的键归"未归类"（不判红）。
// ★ 名字集合按 `pnpm tools lift report` 的真实导入表 + 各 shim 文件的注册名核对过；
//   ⛔ 不要为了方便增删名字（改判据 = 改 A5 的范围，要走 SCOPE）。
// ---------------------------------------------------------------------------------------

/**
 * D6 七族（plan §4 D6）—— 族 id / 中文标题 / 名字集合（`module!name` 精确匹配，模块名小写）。
 * ★ 这是**判据数据**，不是文档：`check()` 用它把 stub 分组。
 * ★ 只列"会出现在导入表 / 注册表里的键"；不在任何族里的键归"未归类"（不判红）。
 */
export const D6_FAMILIES = [
  { id: 'sync', title: '同步', names: [
    'kernel32!CloseHandle','kernel32!CreateEventA','kernel32!CreateEventW','kernel32!CreateMutexA','kernel32!CreateMutexW',
    'kernel32!CreateSemaphoreA','kernel32!CreateSemaphoreW','kernel32!DeleteCriticalSection','kernel32!DuplicateHandle',
    'kernel32!EnterCriticalSection','kernel32!InitializeCriticalSection','kernel32!InitializeCriticalSectionAndSpinCount',
    'kernel32!InitializeCriticalSectionEx','kernel32!InitializeSListHead','kernel32!InterlockedCompareExchange',
    'kernel32!InterlockedDecrement','kernel32!InterlockedExchange','kernel32!InterlockedExchangeAdd',
    'kernel32!InterlockedFlushSList','kernel32!InterlockedIncrement','kernel32!InterlockedPopEntrySList',
    'kernel32!InterlockedPushEntrySList','kernel32!LeaveCriticalSection','kernel32!OpenEventA','kernel32!OpenMutexA',
    'kernel32!OpenSemaphoreA','kernel32!PulseEvent','kernel32!QueryDepthSList','kernel32!ReleaseMutex',
    'kernel32!ReleaseSemaphore','kernel32!ResetEvent','kernel32!SetEvent','kernel32!SignalObjectAndWait','kernel32!Sleep',
    'kernel32!SleepEx','kernel32!SwitchToThread','kernel32!TryEnterCriticalSection','kernel32!WaitForMultipleObjects',
    'kernel32!WaitForMultipleObjectsEx','kernel32!WaitForSingleObject','kernel32!WaitForSingleObjectEx',
  ] },
  { id: 'memory', title: '内存', names: [
    'kernel32!GlobalAlloc','kernel32!GlobalFlags','kernel32!GlobalFree','kernel32!GlobalLock','kernel32!GlobalReAlloc',
    'kernel32!GlobalSize','kernel32!GlobalUnlock','kernel32!HeapAlloc','kernel32!HeapCompact','kernel32!HeapCreate',
    'kernel32!HeapDestroy','kernel32!HeapFree','kernel32!HeapLock','kernel32!HeapQueryInformation','kernel32!HeapReAlloc',
    'kernel32!HeapSetInformation','kernel32!HeapSize','kernel32!HeapUnlock','kernel32!HeapValidate','kernel32!GetProcessHeap',
    'kernel32!GetProcessHeaps','kernel32!LocalAlloc','kernel32!LocalFree','kernel32!LocalReAlloc','kernel32!LocalSize',
    'kernel32!VirtualAlloc','kernel32!VirtualFree','kernel32!VirtualLock','kernel32!VirtualProtect','kernel32!VirtualQuery',
    'kernel32!CoTaskMemAlloc','kernel32!CoTaskMemFree','kernel32!CoTaskMemRealloc','ole32!CoTaskMemAlloc','ole32!CoTaskMemFree',
  ] },
  { id: 'file', title: '文件与路径', names: [
    'kernel32!CopyFileA','kernel32!CreateDirectoryA','kernel32!CreateFileA','kernel32!CreateFileW','kernel32!DeleteFileA',
    'kernel32!FindClose','kernel32!FindFirstFileA','kernel32!FindNextFileA','kernel32!FlushFileBuffers',
    'kernel32!GetCurrentDirectoryA','kernel32!GetCurrentDirectoryW','kernel32!GetDriveTypeA','kernel32!GetFileAttributesA',
    'kernel32!GetFileSize','kernel32!GetFileTime','kernel32!GetFullPathNameA','kernel32!GetLogicalDrives',
    'kernel32!GetModuleFileNameA','kernel32!GetModuleFileNameW','kernel32!GetSystemDirectoryA','kernel32!GetTempFileNameA',
    'kernel32!GetTempPathA','kernel32!GetVolumeInformationA','kernel32!GetWindowsDirectoryA','kernel32!MoveFileA',
    'kernel32!ReadFile','kernel32!RemoveDirectoryA','kernel32!SetCurrentDirectoryA','kernel32!SetFileAttributesA',
    'kernel32!SetFilePointer','kernel32!SetFilePointerEx','kernel32!SetFileTime','kernel32!WriteFile',
  ] },
  { id: 'time', title: '计时', names: [
    'kernel32!GetLocalTime','kernel32!GetSystemTime','kernel32!GetSystemTimeAsFileTime',
    'kernel32!GetSystemTimePreciseAsFileTime','kernel32!GetTickCount','kernel32!GetTickCount64',
    'kernel32!QueryPerformanceCounter','kernel32!QueryPerformanceFrequency',
    'winmm!timeBeginPeriod','winmm!timeEndPeriod','winmm!timeGetDevCaps','winmm!timeGetTime','winmm!timeKillEvent',
    'winmm!timeSetEvent',
  ] },
  { id: 'error_tls', title: '错误码与 TLS', names: [
    'kernel32!DecodePointer','kernel32!DecodeSystemPointer','kernel32!EncodePointer','kernel32!EncodeSystemPointer',
    'kernel32!GetLastError','kernel32!SetLastError','kernel32!TlsAlloc','kernel32!TlsFree','kernel32!TlsGetValue',
    'kernel32!TlsSetValue','kernel32!FlsAlloc','kernel32!FlsFree','kernel32!FlsGetValue','kernel32!FlsSetValue',
  ] },
  { id: 'string', title: '码页与字符串', names: [
    'kernel32!EnumSystemLocalesA','kernel32!GetACP','kernel32!GetCPInfo','kernel32!GetLocaleInfoA','kernel32!GetLocaleInfoW',
    'kernel32!GetOEMCP','kernel32!GetStringTypeW','kernel32!GetSystemDefaultLCID','kernel32!GetThreadLocale',
    'kernel32!GetUserDefaultLCID','kernel32!GetUserDefaultUILanguage','kernel32!IsValidCodePage','kernel32!IsValidLocale',
    'kernel32!LCMapStringW','kernel32!MultiByteToWideChar','kernel32!SetThreadLocale','kernel32!WideCharToMultiByte',
    'kernel32!lstrlenA','user32!wsprintfA',
  ] },
  { id: 'process', title: '进程·模块信息', names: [
    'kernel32!DisableThreadLibraryCalls','kernel32!ExitProcess','kernel32!FreeEnvironmentStringsW','kernel32!FreeLibrary',
    'kernel32!GetCommandLineA','kernel32!GetCommandLineW','kernel32!GetComputerNameA','kernel32!GetConsoleCP',
    'kernel32!GetConsoleMode','kernel32!GetConsoleOutputCP','kernel32!GetCurrentProcess','kernel32!GetCurrentProcessId',
    'kernel32!GetCurrentThread','kernel32!GetCurrentThreadId','kernel32!GetEnvironmentStringsW',
    'kernel32!GetEnvironmentVariableA','kernel32!GetFileType','kernel32!GetModuleHandleA','kernel32!GetModuleHandleExA',
    'kernel32!GetModuleHandleExW','kernel32!GetModuleHandleW','kernel32!GetProcAddress','kernel32!GetStartupInfoA',
    'kernel32!GetStartupInfoW','kernel32!GetStdHandle','kernel32!GetSystemInfo','kernel32!GetVersion','kernel32!GetVersionExA',
    'kernel32!GetVersionExW','kernel32!IsProcessorFeaturePresent','kernel32!LoadLibraryA','kernel32!LoadLibraryW',
    'kernel32!MulDiv','kernel32!OutputDebugStringA','kernel32!SetConsoleCtrlHandler','kernel32!SetConsoleOutputCP',
    'kernel32!SetHandleCount','kernel32!SetStdHandle','kernel32!SetErrorMode','kernel32!TerminateProcess',
    'kernel32!WriteConsoleW',
  ] },
];

// ★ 七族之外、按族列出（不进 A5 的七族判据；D5 线程族与 R1 SEH 族均已 SCOPE 批准，见 FAMILY_STUB_APPROVED）
/** 七族**之外**、按族列出以便看见进度的族（不进 A5 的七族判据） */
export const EXTRA_FAMILIES = [
  { id: 'thread', title: '线程 / 定时器队列', names: [
    'kernel32!CreateThread','kernel32!CreateRemoteThread','kernel32!_beginthreadex','kernel32!ExitThread',
    'kernel32!FreeLibraryAndExitThread','kernel32!GetProcessAffinityMask','kernel32!GetThreadPriority',
    'kernel32!GetThreadTimes','kernel32!ResumeThread','kernel32!SetThreadAffinityMask','kernel32!SetThreadPriority',
    'kernel32!CreateTimerQueue','kernel32!CreateTimerQueueTimer','kernel32!DeleteTimerQueueTimer',
  ] },
  { id: 'seh', title: 'SEH', names: ['kernel32!RaiseException','kernel32!RtlUnwind'] },
  { id: 'resource', title: '资源', names: [
    'kernel32!FindResourceA','kernel32!LoadResource','kernel32!LockResource','kernel32!SizeofResource',
  ] },
];

/**
 * 已获 SCOPE 批准的"允许继续是 stub"的键（白名单；`SCOPE.md` §1「线程 / 定时器队列」+「SEH / C++ 异常」，
 * 用户 2026-11-04）。
 * ★ 只列 **PE 导入表里真实出现** 的键：`CreateRemoteThread` / `_beginthreadex` 不在导入表 ⇒ 不进这里
 *   （否则下面"白名单漂了"那条判定会红）。
 */
export const FAMILY_STUB_APPROVED = [
  'kernel32!CreateThread',
  'kernel32!ExitThread',
  'kernel32!ResumeThread',
  'kernel32!SetThreadPriority',
  'kernel32!SetThreadAffinityMask',
  'kernel32!GetProcessAffinityMask',
  'kernel32!FreeLibraryAndExitThread',
  'kernel32!CreateTimerQueue',
  'kernel32!CreateTimerQueueTimer',
  'kernel32!DeleteTimerQueueTimer',
  'kernel32!RaiseException',
  'kernel32!RtlUnwind',
];

const rel = (p) => path.relative(REPO_ROOT, p).split(path.sep).join('/');
const abs = (p) => path.join(REPO_ROOT, p);
const readText = (p) => fs.readFileSync(abs(p), 'utf8');
const exists = (p) => fs.existsSync(abs(p));
const sha256File = (p) => crypto.createHash('sha256').update(fs.readFileSync(abs(p))).digest('hex');

/** C++ 源码字符串字面量反转义（只需要本仓用到的那几种） */
function unescapeCpp(s) {
  return s.replace(/\\(["\\nrt])/g, (_, c) => ({ '"': '"', '\\': '\\', n: '\n', r: '\r', t: '\t' })[c]);
}
function escapeCpp(s) {
  return s.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\n/g, '\\n').replace(/\r/g, '\\r');
}

/**
 * 在 C++ 源码里找 `name(...)` 调用，返回参数数组。
 * ★ 必须做**括号平衡 + 引号感知**：理由串里可能有逗号 / 圆括号（例 `"（S6）"`、`"(plan §9 R1)"`）。
 */
export function findCalls(text, name) {
  const out = [];
  const re = new RegExp(`\\b${name}\\s*\\(`, 'g');
  for (let m = re.exec(text); m; m = re.exec(text)) {
    // 跳过声明（`void addUnimplemented(` 之类）——参数里不会有字符串字面量
    let i = m.index + m[0].length;
    let depth = 1;
    let quote = null;
    const args = [];
    let cur = '';
    for (; i < text.length && depth > 0; ++i) {
      const c = text[i];
      if (quote) {
        cur += c;
        if (c === '\\') {
          cur += text[++i] ?? '';
        } else if (c === quote) {
          quote = null;
        }
        continue;
      }
      if (c === '"' || c === "'") {
        quote = c;
        cur += c;
      } else if (c === '(') {
        ++depth;
        cur += c;
      } else if (c === ')') {
        --depth;
        if (depth === 0) {
          args.push(cur.trim());
          break;
        }
        cur += c;
      } else if (c === ',' && depth === 1) {
        args.push(cur.trim());
        cur = '';
      } else {
        cur += c;
      }
    }
    out.push({ args, index: m.index });
    re.lastIndex = i;
  }
  return out;
}

function parseStringConsts(text) {
  const consts = new Map();
  // 声明 `const char* [const] NAME =` 之后允许**多个相邻字符串字面量**（C++ 会拼接）：
  //   实测 kernel32_version.cpp 的 kReason 就是两行字面量，早期的单字面量正则解析不出来 ⇒
  //   清单里落成 "(无法解析 reason)"，而两边都错成一样的字符串 ⇒ 守卫假绿。
  const head = /const\s+char\s*\*\s*(?:const\s+)?(\w+)\s*=/g;
  for (let m = head.exec(text); m; m = head.exec(text)) {
    let i = m.index + m[0].length;
    const parts = [];
    for (;;) {
      while (i < text.length && /\s/.test(text[i])) ++i;
      if (text[i] !== '"') break;
      let j = i + 1;
      let cur = '';
      for (; j < text.length; ++j) {
        if (text[j] === '\\') {
          cur += text[j] + (text[j + 1] ?? '');
          ++j;
        } else if (text[j] === '"') {
          break;
        } else {
          cur += text[j];
        }
      }
      parts.push(cur);
      i = j + 1;
    }
    if (parts.length) consts.set(m[1], unescapeCpp(parts.join('')));
  }
  return consts;
}

function reasonOf(expr, consts) {
  const e = expr.trim();
  if (e.startsWith('"') && e.endsWith('"')) return unescapeCpp(e.slice(1, -1));
  if (consts.has(e)) return consts.get(e);
  return null;
}

function normModule(m) {
  const s = String(m).toLowerCase();
  return s.endsWith('.dll') ? s.slice(0, -4) : s;
}
const normKey = (module, name) => `${normModule(module)}!${name}`;
const displayKey = (module, name) => `${normModule(module)}!${name}`;

/**
 * 解析 C++ 注册表。`skipGapFile` = 不把派生缺口文件当成"人手登记"（否则缺口永远消不掉）。
 * 返回：
 *   implemented: Map<key, {conv, argc, file}>
 *   stubs:       Map<key, {reason, file, origin: 'addUnimplemented'|'addStub'}>
 *   problems:    string[]
 */
export function readRegistrations({ skipGapFile = true } = {}) {
  const implemented = new Map();
  const stubs = new Map();
  const problems = [];
  const dir = abs(SHIM_DIR);
  if (!fs.existsSync(dir)) return { implemented, stubs, problems: [`找不到 ${SHIM_DIR}`] };
  const gapBase = path.basename(GAP_FILE);
  for (const file of fs.readdirSync(dir).sort()) {
    if (!file.endsWith('.cpp')) continue;
    if (skipGapFile && file === gapBase) continue;
    const text = fs.readFileSync(path.join(dir, file), 'utf8');
    const consts = parseStringConsts(text);

    for (const call of findCalls(text, 'add')) {
      const [mod, name, conv, argc] = call.args;
      if (!mod?.startsWith('"') || !name?.startsWith('"')) continue; // 泛型 / 非字面量注册：跳过
      const key = normKey(unescapeCpp(mod.slice(1, -1)), unescapeCpp(name.slice(1, -1)));
      if (implemented.has(key)) problems.push(`${key} 在 C++ 里实现了两次（${implemented.get(key).file} / ${file}）`);
      implemented.set(key, {
        conv: conv?.replace('Conv::', '') ?? '?',
        argc: Number.parseInt(argc ?? '0', 10) || 0,
        file: `${SHIM_DIR}/${file}`,
      });
    }
    for (const call of findCalls(text, 'addStub')) {
      const [mod, name, , , , reasonExpr] = call.args;
      if (!mod?.startsWith('"') || !name?.startsWith('"')) continue;
      const key = normKey(unescapeCpp(mod.slice(1, -1)), unescapeCpp(name.slice(1, -1)));
      const reason = reasonOf(reasonExpr ?? '', consts);
      if (reason === null) problems.push(`${key} 的 addStub reason 解析不出来（表达式：${reasonExpr ?? '(缺)'}）`);
      if (stubs.has(key)) problems.push(`${key} 登记了两次 stub（${stubs.get(key).file} / ${file}）`);
      stubs.set(key, { reason: reason ?? '(无法解析 reason)', file: `${SHIM_DIR}/${file}`, origin: 'addStub' });
      if (implemented.has(key)) {
        problems.push(`${key} 同时是 add（实现）与 addStub（整条 stub）—— 一处名字只能有一种身份`);
      }
    }
    for (const call of findCalls(text, 'addUnimplemented')) {
      const [mod, name, reasonExpr] = call.args;
      if (!mod?.startsWith('"') || !name?.startsWith('"')) continue;
      const key = normKey(unescapeCpp(mod.slice(1, -1)), unescapeCpp(name.slice(1, -1)));
      const reason = reasonOf(reasonExpr ?? '', consts);
      if (reason === null) problems.push(`${key} 的 addUnimplemented reason 解析不出来（表达式：${reasonExpr ?? '(缺)'}）`);
      if (stubs.has(key)) problems.push(`${key} 登记了两次未实现（${stubs.get(key).file} / ${file}）`);
      stubs.set(key, { reason: reason ?? '(无法解析 reason)', file: `${SHIM_DIR}/${file}`, origin: 'addUnimplemented' });
      if (implemented.has(key)) {
        problems.push(
          `${key} 同时出现在 add（${implemented.get(key).file}）与 addUnimplemented（${file}）—— ` +
            '后注册的那条会被 find() 忽略（实测曾让已实现的 GetEnvironmentVariableA 被未实现版本遮蔽）',
        );
      }
    }
  }
  return { implemented, stubs, problems };
}

// ---------------------------------------------------------------------------------------
// PE 导入表（只读；与 A3 ⓪ 同一条锚）
// ★ **EA/RVA → 文件偏移只走一条路**：`tools/lib/ledger.mjs` 的 `peMap`（`ledger.test.mjs` 守这条）。
//   本文件只读 PE 头的导入目录项，**不自己复制一份节表映射** —— 否则"两份映射在某些记录上不同"
//   会变成结构性问题（那正是 `tools/test/ledger.test.mjs` 那条守卫要防的）。
// ---------------------------------------------------------------------------------------
export function readPeImports(exePath = EXE_FILE) {
  const b = fs.readFileSync(abs(exePath));
  const u16 = (o) => b.readUInt16LE(o);
  const u32 = (o) => b.readUInt32LE(o);
  // e_lfanew：按 4 个字节手算（那条 "u32(0x3c)" 形态被 ledger 守卫钉给 tools/lib/ledger.mjs 的唯一一份）
  const pe = b[0x3c] | (b[0x3d] << 8) | (b[0x3e] << 16) | (b[0x3f] << 24);
  if (b[pe] !== 0x50 || b[pe + 1] !== 0x45 || b[pe + 2] !== 0 || b[pe + 3] !== 0) {
    throw new Error(`${exePath} 不是 PE`);
  }
  const opt = pe + 24;
  const magic = u16(opt);
  const dirOff = opt + (magic === 0x20b ? 112 : 96);
  const impRva = u32(dirOff + 8);
  const sizeOfImage = u32(opt + 56);
  const entryRva = u32(opt + 16);
  const imageBase = peImageBase(b, pe);
  const offOfRva = (rva) => peMap(b, imageBase + rva)?.offset ?? -1;
  const cstr = (off) => {
    if (off < 0) return '';
    let e = off;
    while (e < b.length && b[e] !== 0) ++e;
    return b.toString('latin1', off, e);
  };
  const imports = new Map();
  let d = impRva ? offOfRva(impRva) : -1;
  while (d > 0) {
    const oft = u32(d);
    const nameRva = u32(d + 12);
    const ft = u32(d + 16);
    if (!oft && !nameRva && !ft) break;
    const module = cstr(offOfRva(nameRva));
    let t = offOfRva(oft || ft);
    while (t > 0) {
      const v = u32(t);
      if (!v) break;
      const name = v & 0x80000000 ? `#${v & 0xffff}` : cstr(offOfRva(v) + 2);
      imports.set(normKey(module, name), { module: normModule(module), name, byOrdinal: !!(v & 0x80000000) });
      t += 4;
    }
    d += 20;
  }
  return { imports, sizeOfImage, entryRva, imageBase, sha256: sha256File(exePath) };
}

export function gapReasonFor(module) {
  return MODULE_REASONS[normModule(module)] ?? DEFAULT_GAP_REASON;
}

/** 派生缺口：导入表里有、但既没实现也没登记（大小写不敏感比较） */
export function expectedGapKeys(imports, regs) {
  const out = [];
  for (const [key, imp] of imports) {
    if (!regs.implemented.has(key) && !regs.stubs.has(key)) out.push({ key, ...imp });
  }
  out.sort((a, b) => (a.module === b.module ? a.name.localeCompare(b.name) : a.module.localeCompare(b.module)));
  return out;
}

export function renderGapFile(gaps) {
  const lines = [];
  lines.push('// emulator-lift · PE 导入表的**显式缺口登记**（派生文件）');
  lines.push('//');
  lines.push('// ⛔ **不要手改**：本文件由 `pnpm tools lift sync --write` 生成。');
  lines.push('// 它登记的是**导入表里有、但既没有实现也没有 addUnimplemented 的条目** ——');
  lines.push('// 每条都会分到唯一桩地址，执行到就响亮失败并带上这里的 reason（SCOPE §3：⛔ 不许静默跳过）。');
  lines.push('//');
  lines.push('// 怎么改：实现它（在族文件里 add，删掉这里的行由 sync 负责）· 改理由（tools/lift.mjs 的');
  lines.push('// MODULE_REASONS）· 判据：`pnpm tools lift check`（红 = 本文件与导入表/注册表不一致）。');
  lines.push('#include "shims/register.h"');
  lines.push('');
  lines.push('namespace lift {');
  lines.push('namespace {');
  lines.push('// 理由按**模块**分组（见 tools/lift.mjs 的 MODULE_REASONS）：这里不写引擎语义，只写批次归属。');
  lines.push('}  // namespace');
  lines.push('');
  lines.push('void registerImportGaps(ShimRegistry& shims) {');
  for (const g of gaps) {
    lines.push(
      `    shims.addUnimplemented("${escapeCpp(g.module)}", "${escapeCpp(g.name)}", "${escapeCpp(gapReasonFor(g.module))}");`,
    );
  }
  lines.push('}');
  lines.push('');
  lines.push('}  // namespace lift');
  lines.push('');
  return lines.join('\n');
}

// ---------------------------------------------------------------------------------------
// overrides.json
// ---------------------------------------------------------------------------------------
export function readOverrides() {
  const p = abs(OVERRIDES_FILE);
  if (!fs.existsSync(p)) return { data: null, error: `缺 ${OVERRIDES_FILE}（用 \`pnpm tools lift sync --write\` 生成）` };
  try {
    return { data: JSON.parse(fs.readFileSync(p, 'utf8')), error: null };
  } catch (e) {
    return { data: null, error: `${OVERRIDES_FILE} 不是合法 JSON：${e.message}` };
  }
}

const entryKey = (e) => {
  if (typeof e.target === 'string') return e.target;
  if (typeof e.ea === 'string') return `ea:${e.ea.toLowerCase()}`;
  if (Array.isArray(e.range)) return `range:${e.range.map((x) => String(x).toLowerCase()).join('-')}`;
  return '(no-key)';
};

export function validateOverrides(data) {
  const problems = [];
  if (!data || typeof data !== 'object') return ['overrides.json 顶层不是对象'];
  if (data.schema !== SCHEMA) problems.push(`schema 必须是 ${SCHEMA}（现在是 ${JSON.stringify(data.schema)}）`);
  if (!Array.isArray(data.entries)) return [...problems, 'entries 必须是数组'];
  const seen = new Set();
  for (const [i, e] of data.entries.entries()) {
    const where = `entries[${i}]`;
    if (!e || typeof e !== 'object') {
      problems.push(`${where} 不是对象`);
      continue;
    }
    const keys = ['target', 'ea', 'range'].filter((k) => e[k] !== undefined);
    if (keys.length !== 1) problems.push(`${where} 必须恰好有 target / ea / range 之一（现在 ${keys.length} 个）`);
    if (e.kind !== undefined && !KINDS.includes(e.kind)) problems.push(`${where}.kind=${JSON.stringify(e.kind)} 不在 ${KINDS.join('|')}`);
    if (typeof e.reason !== 'string' || e.reason.trim() === '') problems.push(`${where} 缺 reason（SCOPE §3 硬要求）`);
    if (e.kind === 'replace' && (typeof e.boundary !== 'string' || e.boundary.trim() === '')) {
      problems.push(`${where} 是 replace ⇒ 必须写 boundary（入口 / 退出 / 共享数据 / 必须保住的返回码）`);
    }
    if (typeof e.impl !== 'string' || e.impl.trim() === '') problems.push(`${where} 缺 impl`);
    if (e.boundary !== undefined && !BOUNDARY_KINDS.includes(e.boundary) && typeof e.boundary === 'string') {
      // `import` / `function` / `subsystem` 是粒度；自定义说明也可以，但必须非空
      if (e.boundary.trim() === '') problems.push(`${where}.boundary 为空`);
    }
    const k = entryKey(e);
    if (seen.has(k)) problems.push(`${where} 与前面的条目重复（${k}）`);
    seen.add(k);
  }
  return problems;
}

/** 由「C++ 注册表 + 导入表」派生期望的 overrides 条目（保持 replace/hook 等手写条目） */
export function expectedEntries({ imports, regs, existing }) {
  const entries = [];
  const keep = (existing?.entries ?? []).filter((e) => e.kind !== 'stub' || !e.target);
  for (const [key, s] of [...regs.stubs].sort((a, b) => a[0].localeCompare(b[0]))) {
    entries.push({
      target: key,
      kind: 'stub',
      impl: s.origin === 'addStub' ? s.file : 'stub_trap',
      boundary: 'import',
      reason: s.reason,
    });
  }
  for (const g of expectedGapKeys(imports, regs)) {
    entries.push({
      target: g.key,
      kind: 'stub',
      impl: 'stub_trap',
      boundary: 'import',
      reason: gapReasonFor(g.module),
    });
  }
  // 手写的 guest-code 覆盖项（replace / hook，或没有 target 的 EA 条目）原样保留
  const derivedKeys = new Set(entries.map((e) => e.target));
  for (const e of keep) {
    if (e.target && derivedKeys.has(e.target)) continue;
    entries.push(e);
  }
  entries.sort((a, b) => entryKey(a).localeCompare(entryKey(b)));
  return entries;
}

export function renderOverrides(entries) {
  const data = { _doc: DOC_POINTER, schema: SCHEMA, entries };
  return `${JSON.stringify(data, null, 2)}\n`;
}

// ---------------------------------------------------------------------------------------
// `.lst` 语料（只读；口径与 tests/corpus_mnemonics.cpp 同形，但只用文本形态判断）
// ---------------------------------------------------------------------------------------
const LST_PSEUDO = new Set([
  'db', 'dd', 'dw', 'dq', 'dt', 'df', 'do', 'proc', 'endp', 'segment', 'ends', 'struct', 'struc',
  'align', 'even', 'assume', 'public', 'extrn', 'externdef', 'include', 'org', 'dup', 'unicode',
  'byte', 'word', 'dword', 'qword', 'tbyte', 'real4', 'real8', 'real10', 'label', 'equ', 'textequ',
  'ends', 'end', 'model', 'flat', 'code', 'data', 'const', 'stack', 'macro', 'endm', 'if', 'endif',
  'else', 'ifdef', 'ifndef', 'option', 'radix', 'title', 'subttl', 'page',
]);
const LST_PREFIX = new Set(['rep', 'repe', 'repz', 'repne', 'repnz', 'lock']);
const X87 = new Set(
  ('f2xm1 fabs fadd faddp fbld fbstp fchs fclex fcmovb fcmovbe fcmove fcmovnb fcmovnbe fcmovne ' +
    'fcmovnu fcmovu fcom fcomi fcomip fcomp fcompp fcos fdecstp fdiv fdivp fdivr fdivrp ffree fiadd ' +
    'ficom ficomp fidiv fidivr fild fimul fincstp finit fist fistp fisub fisubr fld fld1 fldcw fldenv ' +
    'fldl2e fldl2t fldlg2 fldln2 fldpi fldz fmul fmulp fnclex fndisi fneni fninit fnop fnsave fnstcw ' +
    'fnstenv fnstsw fpatan fprem fprem1 fptan frndint frstor fsave fscale fsin fsincos fsqrt fst ' +
    'fstcw fstenv fstp fstsw fsub fsubp fsubr fsubrp ftst fucom fucomi fucomip fucomp fucompp fwait ' +
    'fxam fxch fxtract fyl2x fyl2xp1 fldenv fdecstp fincstp').split(/\s+/),
);
const MMX_SSE = new Set(
  ('emms movd movq paddb paddd paddq paddw pand pandn pcmpeqb pcmpeqd pcmpeqw pcmpgtb pcmpgtd pcmpgtw ' +
    'pmaddwd pmulhw pmullw por psllq pslld psrad psraw psrld psrlq psubb psubd psubq psubw punpckhbw ' +
    'punpckhdq punpckhwd punpcklbw punpckldq punpcklwd pxor movaps movups movss movsd addps addss ' +
    'subps mulps divps comiss ucomiss cvtpi2ps cvtps2pi cvtsi2ss cvtss2si cvttps2pi cvttss2si maxps ' +
    'minps rcpps rsqrtps shufps andps andnps orps xorps').split(/\s+/),
);

function lstInstructionLine(rest) {
  const m = rest.match(/^([A-Za-z_][A-Za-z0-9_.$]*)(?:\s+|$)(.*)$/);
  if (!m) return null;
  const first = m[1];
  const rest2 = m[2] ?? '';
  if (first.endsWith('=') || first.endsWith(':')) return null;
  if (first[0] >= '0' && first[0] <= '9') return null;
  if (first[0] === '<' || first[0] === '-' || first[0] === '+') return null;
  if (/^[a-z_][a-z0-9_]*$/.test(first)) {
    if (LST_PSEUDO.has(first)) return null;
    if (LST_PREFIX.has(first) || X87.has(first) || MMX_SSE.has(first)) return { mnemonic: first, prefixed: false };
    const second = (rest2.match(/^([^\s,]+)/) ?? [])[1] ?? '';
    if (LST_PSEUDO.has(second) || second === 'proc' || second === 'segment' || second === 'struct' || second === 'ends') return null;
    if (rest2.startsWith('<')) return null;
    // 已知 x86 助记符都是小写字母开头；这里把 `push` / `mov` / `jz` 一类都收进来
    return { mnemonic: first, prefixed: false };
  }
  return null;
}

export function corpusReport({ lstPath = LST_FILE } = {}) {
  const text = readText(lstPath);
  const lines = text.split(/\r?\n/);
  let headerSha = null;
  let collapsed = 0;
  const segments = new Set();
  const sectionHeaders = [];
  const mnemonicCounts = new Map();
  let instructions = 0;
  for (const line of lines) {
    const colon = line.indexOf(':');
    if (colon <= 0) continue;
    const ea8 = line.slice(colon + 1, colon + 9);
    if (!/^[0-9A-Fa-f]{8}$/.test(ea8)) continue;
    const seg = line.slice(0, colon);
    segments.add(seg);
    let rest = line.slice(colon + 9).replace(/^\s+/, '');
    if (!rest) continue;
    if (headerSha === null) {
      const h = rest.match(/;\s*Input SHA256\s*:\s*([0-9A-Fa-f]{64})/);
      if (h) headerSha = h[1].toLowerCase();
    }
    const sh = rest.match(/^;\s*Section\s+(\d+)\.\s*\(virtual address ([0-9A-Fa-f]+)\)/);
    if (sh) sectionHeaders.push({ section: Number(sh[1]), va: parseInt(sh[2], 16), seg });
    if (rest.includes('COLLAPSED')) {
      ++collapsed;
      continue;
    }
    const ins = lstInstructionLine(rest);
    if (!ins) continue;
    ++instructions;
    mnemonicCounts.set(ins.mnemonic, (mnemonicCounts.get(ins.mnemonic) ?? 0) + 1);
  }
  const sorted = [...mnemonicCounts].sort((a, b) => b[1] - a[1]);
  const x87 = [...mnemonicCounts].filter(([m]) => X87.has(m)).reduce((n, [, c]) => n + c, 0);
  const mmx = [...mnemonicCounts].filter(([m]) => MMX_SSE.has(m)).reduce((n, [, c]) => n + c, 0);
  const shaOk = headerSha && exists(EXE_FILE) ? sha256File(EXE_FILE) === headerSha : null;
  return {
    lst: lstPath,
    instructions,
    mnemonicForms: mnemonicCounts.size,
    mnemonics: sorted.map(([m, count]) => ({ mnemonic: m, count })),
    collapsedLines: collapsed,
    segments: [...segments],
    sectionHeaders,
    x87,
    mmxSse: mmx,
    headerSha256: headerSha,
    peSha256MatchesLstHeader: shaOk,
  };
}

// ---------------------------------------------------------------------------------------
// check / report
// ---------------------------------------------------------------------------------------
export function check({ exePath = EXE_FILE, lstPath = LST_FILE, quiet = false } = {}) {
  const problems = [];
  const regs = readRegistrations({ skipGapFile: true });
  problems.push(...regs.problems);

  const { data, error } = readOverrides();
  if (error) problems.push(error);
  else problems.push(...validateOverrides(data));

  let pe = null;
  try {
    pe = readPeImports(exePath);
  } catch (e) {
    problems.push(`读 PE 导入表失败（${exePath}）：${e.message}`);
  }

  if (pe && data) {
    const entries = data.entries ?? [];
    const byTarget = new Map();
    for (const e of entries) if (typeof e.target === 'string') byTarget.set(e.target, e);

    // ① C++ 里每条 addUnimplemented / addStub 都必须在 JSON 里（reason 逐字相等）
    for (const [key, s] of regs.stubs) {
      const e = byTarget.get(key);
      if (!e) {
        problems.push(`C++ 登记了 ${key}（${s.origin}），但 ${OVERRIDES_FILE} 里没有对应条目`);
      } else if (e.kind !== 'stub') {
        problems.push(`${key} 在 C++ 是 ${s.origin}，JSON 里 kind=${e.kind}（应为 stub）`);
      } else if (e.reason !== s.reason) {
        problems.push(`${key} 的 reason 两边不一致：\n    C++  : ${s.reason}\n    JSON : ${e.reason}`);
      }
    }

    // ② JSON 的每条 stub 必须能落到 C++（实现 / 登记 / 派生缺口）
    const gaps = expectedGapKeys(pe.imports, regs);
    const gapByKey = new Map(gaps.map((g) => [g.key, g]));
    for (const e of entries) {
      if (e.kind !== 'stub') continue;
      if (typeof e.target !== 'string') {
        problems.push(`${entryKey(e)}：M1 只有平台 shim 覆盖项，stub 必须用 target（module!name）`);
        continue;
      }
      const inRegs = regs.stubs.has(e.target);
      const gap = gapByKey.get(e.target);
      if (!inRegs && !gap) {
        if (regs.implemented.has(e.target)) {
          problems.push(`${e.target}：C++ 里是**实现**（add），JSON 却写成 stub —— 要么删条目，要么在 C++ 用 addStub`);
        } else {
          problems.push(`${e.target}：JSON 里有 stub 条目，但 C++ 没登记、导入表里也没有这个导入`);
        }
      }
      if (gap && e.reason !== gapReasonFor(gap.module)) {
        problems.push(`${e.target} 的 reason 与 MODULE_REASONS 不一致：JSON=${e.reason} 期望=${gapReasonFor(gap.module)}`);
      }
    }

    // ③ 派生缺口文件必须与"导入表 − 实现 − 登记"逐字节一致
    // ★ ④ 的 "裸缺口" 必须相对**盘上的派生文件**算：`gapByKey`（期望缺口）覆盖所有未实现导入，
    //   拿它算 bare 会构造上恒为 0（审计 C1）。盘上缺文件 / 过期 ⇒ gapFileKeys 为空 ⇒ bare 真的 >0。
    const gapFileKeys = new Set();
    if (exists(GAP_FILE)) {
      const expected = renderGapFile(gaps);
      const actual = readText(GAP_FILE);
      if (actual !== expected) {
        problems.push(`${GAP_FILE} 过期（与导入表/注册表不一致）—— 重跑 \`pnpm tools lift sync --write\``);
      } else {
        for (const g of gaps) gapFileKeys.add(g.key);
      }
    } else if (gaps.length > 0) {
      problems.push(`缺派生缺口文件 ${GAP_FILE}（有 ${gaps.length} 条未登记导入）—— 跑 \`pnpm tools lift sync --write\``);
    }

    // ④ bare gap 必须是 0（实现 / 显式 stub / 派生缺口文件三者必居其一）
    let implementedImported = 0;
    let stubImported = 0;
    let bare = 0;
    for (const [key, imp] of pe.imports) {
      if (regs.implemented.has(key)) ++implementedImported;
      else if (regs.stubs.has(key) || gapFileKeys.has(key)) ++stubImported;
      else {
        ++bare;
        problems.push(`裸缺口（导入表里有、既没实现也没登记，且派生缺口文件里也没有）：${key}`);
      }
      void imp;
    }
    const neverImported = [...regs.stubs.keys()].filter((k) => !pe.imports.has(k));
    const counts = {
      imported: pe.imports.size,
      implementedImported,
      stubImported,
      bare,
      staticGap: pe.imports.size - implementedImported,
      registeredStubTotal: regs.stubs.size,
      neverImported,
      gapFileEntries: gapFileKeys.size,
    };
    // 静态缺口的恒等式：static = stub + bare；stub 总数 = 导入的 stub + 从未被引用的
    if (counts.staticGap !== counts.stubImported + counts.bare) {
      problems.push(`静态缺口恒等式不成立：${counts.staticGap} != ${counts.stubImported} + ${counts.bare}`);
    }

    // ⑤ sha256：PE = .lst 头部（与 A3 ⓪ 同一条锚）
    let lstSha = null;
    if (exists(lstPath)) {
      const head = fs.readFileSync(abs(lstPath), 'utf8').slice(0, 20000);
      const m = head.match(/;\s*Input SHA256\s*:\s*([0-9A-Fa-f]{64})/);
      lstSha = m ? m[1].toLowerCase() : null;
      if (!lstSha) problems.push(`.lst 头部没有 Input SHA256（${lstPath}）`);
      else if (pe.sha256 !== lstSha) problems.push(`PE sha256 ≠ .lst 头部：${pe.sha256} vs ${lstSha}`);
    } else {
      problems.push(`找不到 .lst（${lstPath}）—— 无法校验字节与语料同源`);
    }

    // ⑥ D6 七族（A5）：七族内的导入不许是 stub，除非键在 FAMILY_STUB_APPROVED（= SCOPE 偏差表已批准）。
    // ★ 只对**导入表里 status=stub** 的键归类（= counts.stubImported 那一批）；实现的不需要族别。
    // ★ 七族外（EXTRA_FAMILIES）与未归类只计数、不判红 —— 它们不在 A5 的七族范围内。
    const familyOf = new Map();
    for (const f of D6_FAMILIES) for (const n of f.names) familyOf.set(n, { kind: 'seven', id: f.id, title: f.title });
    for (const f of EXTRA_FAMILIES) for (const n of f.names) familyOf.set(n, { kind: 'extra', id: f.id, title: f.title });
    const approved = new Set(FAMILY_STUB_APPROVED);
    const sevenCounts = Object.fromEntries(D6_FAMILIES.map((f) => [f.id, 0]));
    const extraCounts = Object.fromEntries(EXTRA_FAMILIES.map((f) => [f.id, 0]));
    let sevenStubTotal = 0;
    let sevenApproved = 0;
    let extraStubTotal = 0;
    let extraApproved = 0;
    let unclassified = 0;
    for (const [key] of pe.imports) {
      if (regs.implemented.has(key)) continue;
      if (!regs.stubs.has(key) && !gapByKey.has(key)) continue; // 裸缺口已由 ④ 报过
      const fam = familyOf.get(key);
      const ok = approved.has(key);
      if (!fam) {
        ++unclassified;
        continue;
      }
      if (fam.kind === 'seven') {
        ++sevenCounts[fam.id];
        if (ok) ++sevenApproved;
        else {
          ++sevenStubTotal;
          problems.push(
            `七族内 stub 未获 SCOPE 批准：${key}（${fam.title}）—— 实现它，或写进 SCOPE.md 的偏差表` +
              '并把键加进 tools/lift.mjs 的 FAMILY_STUB_APPROVED',
          );
        }
      } else {
        ++extraCounts[fam.id];
        ++extraStubTotal;
        if (ok) ++extraApproved;
      }
    }
    // 白名单漂了：用「pe.imports × regs」判定，⛔ 不能只看字符串（实现掉的 / 从未被导入的都不能留在白名单里）
    for (const key of FAMILY_STUB_APPROVED) {
      const isImportedStub =
        pe.imports.has(key) && !regs.implemented.has(key) && (regs.stubs.has(key) || gapByKey.has(key));
      if (!isImportedStub) {
        problems.push(`FAMILY_STUB_APPROVED 里的 ${key} 不是（导入的）stub —— 白名单漂了（删掉它或说明）`);
      }
    }
    counts.families = {
      seven: sevenCounts, // 每族的 stub 数（含已批准；族内 stub 总和 = sevenStubTotal + sevenApproved）
      sevenStubTotal, // 未批准的七族 stub 数（判据；必须是 0）
      sevenApproved, // 落在七族但已白名单的
      extra: extraCounts, // 每族的 stub 数（含已批准；不判红）
      extraStubTotal,
      extraApproved,
      unclassified, // 不在任何族里的 stub（user32 / gdi32 / d3d9 / kernel32!CreateProcessA …）；不判红
    };

    if (!quiet) {
      console.log(`lift check · ${OVERRIDES_FILE}`);
      console.log(`  PE        : ${exePath}  (sha256 ${pe.sha256.slice(0, 16)}…)`);
      console.log(`  导入      : ${counts.imported}（实现 ${counts.implementedImported} · 显式 stub ${counts.stubImported} · 裸缺口 ${counts.bare}）`);
      console.log(`  静态缺口  : ${counts.staticGap} = 已登记 stub ${counts.stubImported} + 未登记 ${counts.bare}`);
      console.log(
        `  C++ 登记  : add ${regs.implemented.size} · addUnimplemented/addStub ${counts.registeredStubTotal}；` +
          `其中**从未被导入引用** ${counts.neverImported.length}`,
      );
      if (counts.neverImported.length) console.log(`             ${counts.neverImported.join(' ')}`);
      console.log(`  overrides : ${(data.entries ?? []).length} 条（stub ${(data.entries ?? []).filter((e) => e.kind === 'stub').length}）`);
      // ⑥ D6 七族（A5）：族标题一律取数据里的 title，⛔ 不另写一份汉字表
      const fam = counts.families;
      console.log(
        `  D6 七族   : stub ${fam.sevenStubTotal + fam.sevenApproved}（未批准 ${fam.sevenStubTotal} · 已批准 ${fam.sevenApproved}）`,
      );
      console.log(`             ${D6_FAMILIES.map((f) => `${f.title} ${fam.seven[f.id]}`).join(' · ')}`);
      console.log(
        `             七族外：${EXTRA_FAMILIES.map((f) => `${f.title} ${fam.extra[f.id]}`).join(' · ')}` +
          ` · 未归类 ${fam.unclassified}（待 SCOPE 批准 ${fam.extraStubTotal - fam.extraApproved} 条）`,
      );
    }
    return { problems, counts, regs, pe, data };
  }

  if (!quiet) {
    console.log(`lift check · ${OVERRIDES_FILE}`);
    for (const p of problems) console.log(`  ✗ ${p}`);
  }
  return { problems, counts: null, regs, pe: null, data };
}

export function reportCoverage({ exePath = EXE_FILE } = {}) {
  const regs = readRegistrations({ skipGapFile: true });
  const pe = readPeImports(exePath);
  const gaps = expectedGapKeys(pe.imports, regs);
  // ★ 与 check ④ 同一口径：只有**盘上的派生文件**与期望一致时，缺口才算"已登记"；
  //   否则报告里的 bare 会像旧实现那样恒为 0（审计 C1）。
  const gapByKey = new Set();
  if (exists(GAP_FILE) && readText(GAP_FILE) === renderGapFile(gaps)) {
    for (const g of gaps) gapByKey.add(g.key);
  }
  const rows = [];
  for (const [key, imp] of pe.imports) {
    let status;
    if (regs.implemented.has(key)) status = 'implemented';
    else if (regs.stubs.has(key) || gapByKey.has(key)) status = 'stub';
    else status = 'bare';
    rows.push({ target: key, module: imp.module, name: imp.name, status });
  }
  const never = [...regs.stubs.keys()].filter((k) => !pe.imports.has(k)).map((k) => ({ target: k, status: 'never-imported' }));
  rows.sort((a, b) => a.target.localeCompare(b.target));
  const counts = {
    imports: pe.imports.size,
    implemented: rows.filter((r) => r.status === 'implemented').length,
    stub: rows.filter((r) => r.status === 'stub').length,
    bare: rows.filter((r) => r.status === 'bare').length,
    neverImported: never.length,
  };
  const covered = counts.implemented + counts.stub;
  return { rows, never, counts, coverage: counts.imports ? covered / counts.imports : 0 };
}

// ---------------------------------------------------------------------------------------
// bench（S8：吞吐 MIPS 进报告）
// ---------------------------------------------------------------------------------------
export const BENCH_BIN = process.platform === 'win32'
  ? 'apps/emulator-lift/out/lift-bench.exe'
  : 'apps/emulator-lift/out/lift-bench';

/**
 * 跑一次 `--bench N` 取 MIPS。★ 不捕获管道（受限沙箱里会 EPERM）：用**文件描述符重定向**
 * （与 tools/corpus.mjs 的 runCapture 同一手法）。构建目录没配 / exe 不在 ⇒ 返回 unavailable。
 */
export function runBench(insns = 2000000) {
  const bin = abs(BENCH_BIN);
  if (!fs.existsSync(bin)) return { unavailable: `找不到 ${BENCH_BIN}（先 cmake --build --preset windows-x64）` };
  const log = path.join(os.tmpdir(), `lift-bench-${process.pid}.log`);
  const fd = fs.openSync(log, 'w');
  try {
    const r = spawnSync(bin, [String(insns)], { cwd: REPO_ROOT, stdio: ['ignore', fd, fd] });
    const text = fs.readFileSync(log, 'utf8');
    if (r.error) return { unavailable: `跑不起来：${r.error.message}` };
    const m = /^bench\s*:\s*(\d+) insns in ([\d.]+) s = ([\d.]+) MIPS/m.exec(text);
    if (!m) return { unavailable: `输出里没有 bench 行（退出码 ${r.status}）` };
    return { insns: Number(m[1]), seconds: Number(m[2]), mips: Number(m[3]) };
  } finally {
    fs.closeSync(fd);
    fs.rmSync(log, { force: true });
  }
}

// ---------------------------------------------------------------------------------------
// describe / 文本
// ---------------------------------------------------------------------------------------
export function describe() {
  return {
    file: OVERRIDES_FILE,
    doc: OVERRIDES_DOC,
    derivedGapFile: GAP_FILE,
    registry: `${SHIM_DIR}/*.cpp（add / addStub / addUnimplemented 是 stub 的真源）`,
    schema: SCHEMA,
    entries: {
      fields: [
        { name: 'target', type: 'string "module!name"', note: '平台 shim 的键（M1 只有这一种）；与 ea / range 三选一' },
        { name: 'ea', type: 'hex string "0x00401234"', note: '函数级覆盖的 guest EA（三选一）' },
        { name: 'range', type: '[start, end] hex/数字', note: '子系统级覆盖的 EA 范围（三选一）' },
        { name: 'kind', type: 'replace | stub | hook', note: 'stub = 不实现、trap 并打 reason；replace = 有实现；hook = 挂在 shim 上' },
        { name: 'impl', type: 'string', note: 'stub 用 "stub_trap"；有实现用 "路径:符号"' },
        { name: 'boundary', type: 'import | function | subsystem | 说明串', note: '粒度；kind=replace 时必须说明保住的返回码 / 共享数据' },
        { name: 'reason', type: '非空 string', note: 'SCOPE §3 硬要求：每个 stub 都要有理由' },
      ],
      oneOfKeys: ['target', 'ea', 'range'],
      kinds: KINDS,
      boundaryKinds: BOUNDARY_KINDS,
    },
    invariants: [
      { id: 'json-md', text: 'overrides.json 旁边必须有 overrides.md，含「怎么查 / 怎么改」并指向 describe', enforcedBy: 'tools/test/json-docs.test.mjs' },
      { id: 'schema', text: 'schema=1；entries[].kind ∈ replace|stub|hook；reason 非空', enforcedBy: 'tools/lift.mjs check（也由 tools/test/lift.test.mjs 纯档复跑）' },
      { id: 'registry-sync', text: 'C++ 的每条 addUnimplemented/addStub 在 JSON 里恰好一条，reason 逐字相等；反向也成立', enforcedBy: 'tools/lift.mjs check' },
      { id: 'no-bare-gap', text: 'PE 导入表里每条导入要么有实现、要么有显式 stub 登记（裸缺口 = 0）', enforcedBy: 'tools/lift.mjs check' },
      { id: 'gap-file', text: 'register_import_gaps.cpp 必须逐字节等于「导入表 − 实现 − 登记」的派生结果', enforcedBy: 'tools/lift.mjs check' },
      { id: 'sha-anchor', text: 'PE sha256 == .lst 头部 Input SHA256（与 A3 ⓪ 同一条锚）', enforcedBy: 'tools/lift.mjs check · tests/decode_crosscheck.cpp' },
      { id: 'no-dup-registration', text: '同一个 module!name 不许在 C++ 里注册两次（实现与未实现并存会让 find() 命中的那条不可预期）', enforcedBy: 'tools/lift.mjs check（实测曾遮蔽 GetEnvironmentVariableA）' },
      { id: 'seven-family-no-stub',
        text: 'D6 七族（同步 / 内存 / 文件与路径 / 计时 / 错误码与TLS / 码页与字符串 / 进程·模块信息）里的导入不许是 stub，除非键在 tools/lift.mjs 的 FAMILY_STUB_APPROVED（= SCOPE.md 偏差表已批准）；未批准 = check 红（A5）',
        enforcedBy: 'tools/lift.mjs check（分组数据见 D6_FAMILIES / EXTRA_FAMILIES）' },
    ],
    operations: OPERATIONS,
    writePath: '`pnpm tools lift sync --write`（唯一写入口；缺省 dry-run）。它重写 overrides.json 与 register_import_gaps.cpp。',
  };
}

export function describeText() {
  const d = describe();
  const L = [];
  L.push(`lift —— 覆盖清单（${d.file}）`);
  L.push('');
  L.push('字段（entry = 恰好一个 key + kind/impl/boundary/reason）：');
  for (const f of d.entries.fields) L.push(`  ${f.name.padEnd(9)} ${f.type}  —— ${f.note}`);
  L.push('');
  L.push('不变量：');
  for (const x of d.invariants) L.push(`  [${x.id}] ${x.text}\n       谁在守：${x.enforcedBy}`);
  L.push('');
  L.push(`写入口：${d.writePath}`);
  L.push('操作：');
  for (const o of d.operations) L.push(`  ${o.name}${o.mutates ? '(会写)' : ''}  ${o.summary}`);
  return `${L.join('\n')}\n`;
}

// ---------------------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------------------
function writeFileVerified(relPath, content) {
  const p = abs(relPath);
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, content);
  const back = fs.readFileSync(p, 'utf8');
  if (back !== content) throw new Error(`${relPath} 写后回读不一致`);
}

export function sync({ write = false, exePath = EXE_FILE } = {}) {
  const regs = readRegistrations({ skipGapFile: true });
  const pe = readPeImports(exePath);
  const gaps = expectedGapKeys(pe.imports, regs);
  const { data } = readOverrides();
  const entries = expectedEntries({ imports: pe.imports, regs, existing: data });
  const gapText = renderGapFile(gaps);
  const overridesText = renderOverrides(entries);
  const gapOld = exists(GAP_FILE) ? readText(GAP_FILE) : '';
  const ovOld = exists(OVERRIDES_FILE) ? readText(OVERRIDES_FILE) : '';
  console.log(`lift sync${write ? '' : '（dry-run；加 --write 落盘）'}`);
  console.log(`  ${GAP_FILE}: ${gapOld === gapText ? '已是最新' : `将变（${gaps.length} 条）`}`);
  console.log(`  ${OVERRIDES_FILE}: ${ovOld === overridesText ? '已是最新' : `将变（${entries.length} 条）`}`);
  if (write) {
    writeFileVerified(GAP_FILE, gapText);
    writeFileVerified(OVERRIDES_FILE, overridesText);
    console.log('  已写入（写后回读一致）。');
  }
  return { gaps, entries };
}

const HELP = `tools/lift.mjs — emulator-lift 覆盖清单（describe / check / report / corpus-report / sync）

  pnpm tools lift describe       字段 / 不变量 / 操作（schema 真源）
  pnpm tools lift check          全量校验（红 ⇒ 退出码 1）
  pnpm tools lift report         覆盖报告（实现 / stub / 裸缺口 / 从未被引用；--bench N 出 MIPS）
  pnpm tools lift corpus-report  .lst 语料报告
  pnpm tools lift sync [--write] 派生 overrides.json + register_import_gaps.cpp（缺省 dry-run）
`;

export async function main(argv = process.argv.slice(2)) {
  const has = (n) => argv.includes(n);
  if (has('--help') || has('-h')) {
    process.stdout.write(HELP);
    return 0;
  }
  if (has('--describe') || argv.length === 0) {
    process.stdout.write(describeText());
    return 0;
  }
  if (has('--check')) {
    const { problems, counts } = check();
    if (problems.length) {
      process.stdout.write('\n');
      for (const p of problems) process.stdout.write(`  ✗ ${p}\n`);
      process.stdout.write(`\nlift check: ${problems.length} 个问题（红）\n`);
      return 1;
    }
    void counts;
    process.stdout.write('lift check: 绿（0 个问题）\n');
    return 0;
  }
  if (has('--report')) {
    const r = reportCoverage();
    // ★ S8：吞吐 MIPS 入报告（`--bench N` 才跑；缺省不跑，免得每次 report 都吃几秒）
    const bi = argv.indexOf('--bench');
    const benchN = bi >= 0 ? Number(argv[bi + 1] ?? 2000000) : 0;
    const bench = benchN ? runBench(benchN) : null;
    if (bench) r.bench = bench;
    if (has('--json')) {
      process.stdout.write(`${JSON.stringify(r, null, 2)}\n`);
      return 0;
    }
    console.log('lift report —— 导入级覆盖（平台 shim）');
    console.log(`  导入 ${r.counts.imports} = 实现 ${r.counts.implemented} + 显式 stub ${r.counts.stub} + 裸缺口 ${r.counts.bare}`);
    console.log(`  覆盖率 = ${(r.coverage * 100).toFixed(1)}%（(实现+stub)/导入；裸缺口必须 0）`);
    console.log(`  从未被导入引用 ${r.counts.neverImported} 条（仍是显式 stub，不会静默）`);
    if (bench) {
      if (bench.unavailable) console.log(`  bench   : 无法测量（${bench.unavailable}）`);
      else console.log(`  bench   : ${bench.insns} insns in ${bench.seconds} s = ${bench.mips} MIPS`);
    }
    for (const row of r.rows) console.log(`  ${row.status.padEnd(11)} ${row.target}`);
    for (const n of r.never) console.log(`  ${n.status.padEnd(11)} ${n.target}`);
    return r.counts.bare === 0 ? 0 : 1;
  }
  if (has('--corpus-report')) {
    const r = corpusReport();
    if (has('--json')) {
      process.stdout.write(`${JSON.stringify(r, null, 2)}\n`);
      return 0;
    }
    console.log('lift corpus-report —— .lst（只读；分类口径与 tests/corpus_mnemonics.cpp 同形）');
    console.log(`  文件        : ${r.lst}`);
    console.log(`  指令行      : ${r.instructions}`);
    console.log(`  助记符形态  : ${r.mnemonicForms}（IDA 原样；规范名映射在 C++ 的 mnemonics.cpp）`);
    console.log(`  collapsed 行: ${r.collapsedLines}`);
    console.log(`  段          : ${r.segments.join(' ')}`);
    console.log(`  Section 头  : ${r.sectionHeaders.map((s) => `${s.section}@${s.va.toString(16)}`).join(' ')}`);
    console.log(`  x87 / MMX·SSE: ${r.x87} / ${r.mmxSse}`);
    console.log(`  PE sha = .lst 头: ${r.peSha256MatchesLstHeader === null ? '无法校验' : r.peSha256MatchesLstHeader}`);
    console.log('  top 15      :');
    for (const { mnemonic, count } of r.mnemonics.slice(0, 15)) console.log(`    ${mnemonic.padEnd(10)} ${count}`);
    return 0;
  }
  if (has('--sync')) {
    sync({ write: has('--write') });
    return 0;
  }
  process.stderr.write(`未知参数：${argv.join(' ')}\n\n${HELP}`);
  return 2;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
if (isDirectRun) {
  main()
    .then((code) => {
      process.exitCode = code ?? 0;
    })
    .catch((err) => {
      process.stderr.write(`ERROR: ${err.stack ?? err.message}\n`);
      process.exitCode = 2;
    });
}
