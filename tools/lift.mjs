#!/usr/bin/env node
/**
 * tools/lift.mjs — **CLI**：`data/lift/overrides.json`（emulator-lift 覆盖清单）的自描述 / 校验 / 同步 / 报告
 *               + `.lst` 语料报告。
 *
 * 经派发器：`pnpm tools lift <describe|check|report|bench|corpus-report|sync|diff> [args…]`
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
    '`data/lift/overrides.json`（覆盖清单：每条 gap 必须对应 C++ 注册表里的一条登记）',
    '`apps/emulator-lift/src/shims/*.cpp`（**注册表真源**：`add` / `addStub` / `addUnimplemented`）',
    '`apps/emulator-lift/src/shims/register_import_gaps.cpp`（**派生、入库**：导入表缺口的显式登记，`sync --write` 生成）',
    '`corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`（guest 模块 ①，executed=true；PE 导入表 / sha256，只读）',
    '`corpus/assets/agerc/AGERC.DLL`（guest 模块 ②，executed=false；PE 导入表，只读 —— 裸导入只报告）',
    '`corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst`（语料：指令行 / 段 / collapsed，只读）',
    '`apps/emulator-lift/scripts/bench/*.txt`（`bench` 的工况脚本 = `--input-script` 时间线，**入库**）',
    '`.cache/lift-bench/*.json`（`bench` 的读数：N 遍原始值 + 中位数 + build / 二进制 / 工作树指纹；**派生、gitignored**）',
  ],
  access: '`overrides.json` 与 `register_import_gaps.cpp` 只经 `sync --write` 写；其余全部只读',
  tool: 'tools/lift.mjs',
};

export const OPERATIONS = [
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：字段 / 枚举 / 不变量（含谁在守）/ 操作（schema 真源）' },
  { name: 'check', argv: ['--check'], mutates: false, summary: '全量校验：JSON ↔ C++ 注册表 ↔ PE 导入表 ↔ 派生缺口文件（红 ⇒ 退出码 1）' },
  { name: 'report', argv: ['--report'], mutates: false, summary: '覆盖报告：implemented / noop / trap / bare / bareUnexecuted / not-referenced 计数 + 逐条清单；`--bench N` 顺带把 MIPS 写进报告（S8）' },
  { name: 'bench', argv: ['--bench'], mutates: true, summary: '吞吐基线（一条命令 + 落文件）：同一 `--input-script` 工况独立子进程跑 N 遍，取中位数 ms/百万条指令 + MIPS，落 `.cache/lift-bench/*.json`（⛔ 不重建、不跑 ctest；与 `report --bench` 的合成循环单样本**不是**一回事）' },
  { name: 'sync', argv: ['--sync'], mutates: true, summary: '把 C++ 注册表与 PE 导入表派生到 `overrides.json` + `register_import_gaps.cpp`（缺省 dry-run，`--write` 落盘）' },
  { name: 'corpus-report', argv: ['--corpus-report'], mutates: false, summary: '`.lst` 语料报告：指令行 / 助记符形态 / 段 / collapsed / x87·MMX·SSE 计数' },
  { name: 'diff', argv: ['--diff'], mutates: false, summary: '两张 BMP 逐像素比对：max-diff / diff-pixels / diff-ratio / at=x,y；退出码 0=全在 tolerance 内 · 1=有像素超差 · 2=参数/读文件/格式错' },
];

export const OVERRIDES_FILE = 'data/lift/overrides.json';
export const OVERRIDES_DOC = 'data/lift/overrides.md';
export const SHIM_DIR = 'apps/emulator-lift/src/shims';
export const GAP_FILE = 'apps/emulator-lift/src/shims/register_import_gaps.cpp';
export const EXE_FILE = 'corpus/assets/age/AGE.EXE__dumped.sectfix.EXE';
export const AGERC_FILE = 'corpus/assets/agerc/AGERC.DLL';
export const LST_FILE = 'corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst';

/**
 * **guest 模块集**（扫导入表的扫集合）。
 * ★ `executed=false` 的模块：镜像会被装载（`main.cpp` 的 `--game-dir/AGERC.DLL`），但它的**代码今天
 *   从不执行** —— 导出全被 shim 拦截（`kernel32_agerc.cpp` 明写"有地址但不把真实代码地址交出去"）。
 *   ⇒ 它的**裸导入**只报告（`bareUnexecuted`），⛔ 不登记进派生缺口文件、也⛔ 不判 `check` 红；
 *   哪天它的代码真的执行了，把这些条目按导入登记即可（`check` 会点名）。
 * ★ 用 `corpus/assets/agerc/AGERC.DLL`（发布用免壳件），⛔ 不是 `install/AGERC.DLL`（带壳原件）。
 */
export const GUEST_MODULES = [
  { path: EXE_FILE, executed: true },
  { path: AGERC_FILE, executed: false },
];

export const SCHEMA = 1;
/**
 * `kind` 三值（按 **C++ 注册来源**决定，不是手选）：
 *   `replace` = `add`（有实现）· `noop` = `addStub`（有 fn、按 SCOPE 偏差返回合理值）·
 *   `trap` = `addUnimplemented` / 派生缺口（无 fn、执行到就响亮失败）。
 * ⛔ 不再有 `stub`（它同时吞掉 noop 与 trap，用户 2026-11-13 裁决三值化）；
 *   ⛔ 也没有 `hook`（guest-code hook 是本线尚未出现的形态，等真出现再立值，别留空档）。
 */
export const KINDS = ['replace', 'noop', 'trap'];
export const BOUNDARY_KINDS = ['import', 'function', 'subsystem'];
export const DOC_POINTER =
  '由 tools/lift.mjs 拥有：字段语义 / 不变量 / 操作看 `pnpm tools lift describe`（tools/lift.mjs 的自描述）。';

/** 逐模块的缺口理由（生成 `register_import_gaps.cpp` 用；改这里 ⇒ `sync --write` 后重生成） */
export const MODULE_REASONS = {
  kernel32: '未实现（S0 显式登记：导入表里有、但既没实现也没登记）',
  user32: 'M2 窗口 / 消息族（不属 M1；菜单 / 模态按 SCOPE §1 另记 stub）',
  gdi32: 'M2 GDI 字体 / 2D 图形簇（不属 M1）',
  advapi32: '注册表族：不属 M1（L3 = M2 起点；归属待裁决）',
  winmm: 'winmm 其余导入：摇杆永久 no-op、音频延后（SCOPE §1）；计时族已实现',
  version: '版本信息族：不属 M1（启动路径不需要；归属待裁决）',
  shell32: 'shell32：不属 M1（启动路径不需要；归属待裁决）',
  shlwapi: 'shlwapi：不属 M1（启动路径不需要；归属待裁决）',
  imagehlp: 'imagehlp：不属 M1（崩溃转储不在本线；归属待裁决）',
  imm32: '输入法族：M2（窗口 / 输入），不属 M1',
  ole32: 'COM 内存族已实现（同一个 guest 堆）；其余 ole32 导入属 M2/M3，不属 M1',
  oleaut32: 'oleaut32：M2/M3（VARIANT / 自动化），不属 M1',
  ddraw: 'M2 DirectDraw 簇（不属 M1）',
  d3d9: 'M3 D3D9 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）',
  d3dx9_43: 'M3 D3DX 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）',
  mf: 'M3 视频簇（SCOPE §1：skip；不属 M1；归属待裁决）',
  mfplat: 'M3 视频簇（SCOPE §1：skip；不属 M1；归属待裁决）',
  evr: 'M3 视频簇（SCOPE §1：skip；不属 M1；归属待裁决）',
  dxva2: 'M3 视频簇（SCOPE §1：skip；不属 M1；归属待裁决）',
};
export const DEFAULT_GAP_REASON = '未实现（S0 显式登记：导入表里有、但既没实现也没登记）';

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
  // ★ 2026-11-13：`resource` 族已删 —— 它的 4 个名字是**不可调用**的登记（不在任一 guest 模块
  //   导入表、镜像里也没有字面量），资源能力由 `runtime/pe_resource.h` 的 `PeResources` 提供
  //   （用户裁决：删登记）。
  //   ⛔ 不为它留一个空族：空 names 会让"族是划分"的守卫红（`tools/test/lift.test.mjs`）。
];

/**
 * 已获 SCOPE 批准的"允许继续是 stub"的键（白名单；`SCOPE.md` §1「线程 / 定时器队列」+「SEH / C++ 异常」，
 * 用户 2026-11-04）。
 * ★ 只列 **PE 导入表里真实出现** 的键：`CreateRemoteThread` / `_beginthreadex` 不在导入表 ⇒ 不进这里
 *   （否则下面"白名单漂了"那条判定会红）。
 */
export const FAMILY_STUB_APPROVED = [
  // ★ 2026-11-05：CreateThread / ResumeThread / SetThreadPriority / SetThreadAffinityMask /
  //   GetProcessAffinityMask 已改成**伪线程实现**（SCOPE §1），不再属于"允许继续是 stub"。
  'kernel32!ExitThread',
  'kernel32!FreeLibraryAndExitThread',
  'kernel32!CreateTimerQueue',
  'kernel32!CreateTimerQueueTimer',
  'kernel32!DeleteTimerQueueTimer',
  // ★ 2026-11-12：RaiseException / RtlUnwind 已从 stub 改为实现（REQ-01M72JFCB0W0924H3H6EQSGRK4）
  //   ⇒ 从白名单移除；`pnpm tools lift sync --write` 已同时删掉 overrides.json 的两条 stub。
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

/**
 * 把一个 C++ 实参表达式解成字符串：`const char* X = "…"` 的常量名，或**一串相邻字符串字面量**。
 *
 * ★ 相邻字面量必须按序拼接（C++ 的常规语义）：调用点写
 *   `s.addStub(…, "…存 *arg1 进 " "g_nameLenMax、返回 0…")` 时，旧实现只做
 *   `slice(1, -1)` ⇒ 落成 `…进 "\n              "g_nameLenMax…`（引号 + 换行 + 缩进被当内容），
 *   而 `check` 拿它与 C++ 的同一份错值比 ⇒ **绿**（派生器是有损解码器）。见审计 §3.1。
 * ★ 只接受"纯字面量序列"：后面跟 `+` / 标识符 / 别的 token ⇒ 返回 null（⛔ 不猜）。
 * @param {string} expr 实参原文（`findCalls` 给出的那一段）
 * @param {Map<string,string>} [consts] `parseStringConsts` 的结果
 * @returns {string|null} 解不出来 ⇒ null（调用方报 problem，⛔ 不落成 "(无法解析 reason)" 的假值）
 */
export function reasonOf(expr, consts = new Map()) {
  const e = String(expr ?? '').trim();
  if (consts.has(e)) return consts.get(e);
  const parts = [];
  let i = 0;
  for (;;) {
    while (i < e.length && /\s/.test(e[i])) ++i;
    if (i >= e.length) break;
    if (e[i] !== '"') return null; // 不是字面量（算术 / 变量 / 宏……）：不猜
    let j = i + 1;
    let cur = '';
    for (; j < e.length; ++j) {
      if (e[j] === '\\') {
        cur += e[j] + (e[j + 1] ?? '');
        ++j;
      } else if (e[j] === '"') {
        break;
      } else {
        cur += e[j];
      }
    }
    if (j >= e.length) return null; // 引号没闭合
    parts.push(cur);
    i = j + 1;
  }
  return parts.length ? unescapeCpp(parts.join('')) : null;
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
  return { path: exePath, imports, sizeOfImage, entryRva, imageBase, sha256: sha256File(exePath) };
}

/**
 * 读**整份 guest 模块集**的导入表，并合并成一张 `key → { module, name, byOrdinal, modules[] }` 表。
 *
 * ★ `modules[]` 保留**归属**（哪个镜像导入的）—— 没有它就无法区分"AGE 导入"（executed=true，
 *   裸缺口必须为 0）与"只有 AGERC 导入"（executed=false，裸缺口只报告）。
 * ★ 每个 `modules[]` 都是**绝对仓库相对路径**（与 `GUEST_MODULES[].path` 同一口径）。
 * @param {Array<{path:string, executed:boolean}>} [modules]
 * @returns {{imports: Map<string, object>, modules: Array<{path:string, executed:boolean, pe:object}>}}
 */
export function readGuestImports(modules = GUEST_MODULES) {
  const loaded = [];
  const imports = new Map();
  for (const m of modules) {
    const pe = readPeImports(m.path);
    loaded.push({ path: m.path, executed: !!m.executed, pe });
    for (const [key, imp] of pe.imports) {
      const hit = imports.get(key);
      if (hit) hit.modules.push(m.path);
      else imports.set(key, { ...imp, modules: [m.path] });
    }
  }
  return { imports, modules: loaded };
}

/** 按"实现 / 显式登记 / 派生缺口文件"给一个镜像的导入表分类（`check` 与 `report` 共用同一口径）。 */
export function classifyImports(imports, regs, gapFileKeys) {
  const implemented = [];
  const stubs = [];
  const bare = [];
  for (const [key] of imports) {
    if (regs.implemented.has(key)) implemented.push(key);
    else if (regs.stubs.has(key) || gapFileKeys.has(key)) stubs.push(key);
    else bare.push(key);
  }
  return { implemented, stubs, bare };
}

/** 被拦截的 AGERC 导出（`agerc!_*`）：它们**不是导入**，但经 `GetProcAddress` 可达 ⇒ 不算 not-referenced。 */
const isInterceptedExport = (key) => key.startsWith('agerc!');

/**
 * `not-referenced`：C++ 里登记了 stub、但**不在任一 guest 模块的导入表里**，且**不是被拦截的导出**。
 * ★ 它们可以是**显式裁决**的（例 `SCOPE.md` 点名的线程族）⇒ 只进报告，⛔ 不判红。
 */
export function notReferencedKeys(regs, imports) {
  return [...regs.stubs.keys()].filter((k) => !imports.has(k) && !isInterceptedExport(k)).sort();
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

/**
 * C++ 注册来源 → `kind`（**唯一映射**；`expectedEntries` 与 `check` 都走它，⛔ 不各写一份）。
 * `add` → `replace` · `addStub` → `noop` · `addUnimplemented` → `trap` · 派生缺口 → `trap`。
 * @returns {'replace'|'noop'|'trap'|null} 未知来源 ⇒ null（判据坏了，调用方负责报红）
 */
export function kindForOrigin(origin) {
  switch (origin) {
    case 'add': return 'replace';
    case 'addStub': return 'noop';
    case 'addUnimplemented': return 'trap';
    case 'gap': return 'trap';
    default: return null;
  }
}

/**
 * 由「C++ 注册表 + 导入表」派生期望的 overrides 条目。
 *
 * @param {{imports: Map, regs: object, existing: object|null}} args
 *   ★ `imports` **只传 executed=true 的模块**：派生缺口（`register_import_gaps.cpp`）只对**会执行**的
 *     镜像强制；把 AGERC 的裸导入也塞进来会凭空多出 30 条 trap，并把它们写进 C++ 注册链
 *     （用户 2026-11-13 裁决：AGERC 不登记为 gap，单列 `bareUnexecuted`）。
 */
export function expectedEntries({ imports, regs, existing }) {
  const entries = [];
  // 派生条目（noop / trap）由注册表与导入表重新生成；`stub` 是 2026-11-13 前的旧值，
  // 读旧文件时同样按"派生项"丢弃（`sync --write` 会重写成三值）。
  const derivedKinds = new Set(['noop', 'trap', 'stub']);
  const keep = (existing?.entries ?? []).filter((e) => !(e.target && derivedKinds.has(e.kind)));
  for (const [key, s] of [...regs.stubs].sort((a, b) => a[0].localeCompare(b[0]))) {
    const kind = kindForOrigin(s.origin);
    if (kind === null) continue; // 判据坏了由 check 报；这里不制造未知值
    entries.push({
      target: key,
      kind,
      impl: s.origin === 'addStub' ? s.file : 'stub_trap',
      boundary: 'import',
      reason: s.reason,
    });
  }
  for (const g of expectedGapKeys(imports, regs)) {
    entries.push({
      target: g.key,
      kind: kindForOrigin('gap'),
      impl: 'stub_trap',
      boundary: 'import',
      reason: gapReasonFor(g.module),
    });
  }
  // 手写的 guest-code 覆盖项（replace，或没有 target 的 EA 条目）原样保留
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
// diff：两张 BMP 的逐像素比对（REQ-01M6WSANRPND80XW52QY4Y5WA8）
//
// ★ 为什么自己写 BMP 解析：本仓不引图像库（`npm i` 一个解码器只为读 4×4 的帧不值得，
//   而且它会把"写出侧是什么字节"变成第三方库的实现细节）。写出口径是
//   `apps/emulator-lift/src/runtime/surface.cpp` 的 `dumpBmp`：BI_RGB · 24/32bpp ·
//   行按 4 字节对齐 · 自底向上；读入必须能字节级吃回这份输出（并顺手支持自顶向下
//   `biHeight < 0` 与 16bpp 565 —— 后者是 DDraw 的常见 present 格式）。
// ★ 为什么逐通道取绝对值、不换算成感知差：要回答的是"两次运行从哪一帧开始不一样"，
//   不是"人眼看不看得出来"。任何 gamma / 加权都会让判据依赖一个没人拍板的口径。
// ---------------------------------------------------------------------------------------

/** 命令行用法（错误信息与自描述共用一份，⛔ 不抄第二份） */
export const DIFF_USAGE = 'node tools/lift.mjs diff <a.bmp> <b.bmp> [--tolerance <0..255>] [--json]';

/**
 * 解一份 BMP 成**自顶向下**的 RGB 平面（`rgb[(y*width+x)*3 + {0,1,2}]` = R/G/B）。
 * ★ 只认 `BI_RGB`（`biCompression=0`）：调色板 / RLE / BI_BITFIELDS 都是另一套语义，
 *   猜错时会静默换成"看起来像差异"的数字 ⇒ 明确拒绝（退出码 2），⛔ 不猜。
 * ★ 32bpp 的 A 通道**不进比对**：`dumpBmp` 对 32bpp 固定写 0；若拿 A 参与比较，
 *   同一张画面存成 24bpp（无 A）与 32bpp（A=0）会被判成"全帧差异"。
 */
export function decodeBmp(bytes) {
  const bad = (why) => {
    throw new Error(why);
  };
  if (bytes.length < 54) bad(`文件太短（${bytes.length} 字节，连 BITMAPFILEHEADER + BITMAPINFOHEADER 都不够）`);
  if (bytes[0] !== 0x42 || bytes[1] !== 0x4d) bad('缺 "BM" 签名');
  const u16 = (o) => bytes.readUInt16LE(o);
  const u32 = (o) => bytes.readUInt32LE(o);
  const i32 = (o) => bytes.readInt32LE(o);
  const headerSize = u32(14);
  if (headerSize < 40) bad(`不支持的 BMP 头（biSize=${headerSize}；只认 BITMAPINFOHEADER 及以后的 40+ 字节头）`);
  const width = i32(18);
  const rawHeight = i32(22);
  const bpp = u16(28);
  if (u16(26) !== 1) bad(`biPlanes=${u16(26)}（应为 1）`);
  if (width <= 0) bad(`宽度非法：${width}`);
  if (rawHeight === 0) bad('高度非法：0');
  if (bpp !== 16 && bpp !== 24 && bpp !== 32) bad(`不支持的位深：${bpp}bpp（只认 16 / 24 / 32）`);
  if (u32(30) !== 0) bad(`不支持的压缩方式：biCompression=${u32(30)}（只认 BI_RGB=0）`);

  // `biHeight < 0` = 自顶向下（Windows 的合法变体；早期 BMP 约定不明确，这里的判据是符号）
  const topDown = rawHeight < 0;
  const height = Math.abs(rawHeight);
  const bytesPerPixel = bpp / 8;
  const stride = (width * bytesPerPixel + 3) & ~3; // 行按 4 字节对齐（dumpBmp 的 out_row 同式）
  const offBits = u32(10);
  if (offBits < 14 + headerSize) bad(`bfOffBits=${offBits} 落在头部里（应 ≥ ${14 + headerSize}）`);
  if (offBits + stride * height > bytes.length) {
    bad(`像素数据被截断（要 ${offBits + stride * height} 字节，文件只有 ${bytes.length}）`);
  }

  const rgb = new Uint8Array(width * height * 3);
  for (let y = 0; y < height; y += 1) {
    // 归一成"y=0 是最上面一行"：自底向上时文件里的第一行是**最后**一行
    const srcRow = topDown ? y : height - 1 - y;
    const row = offBits + srcRow * stride;
    for (let x = 0; x < width; x += 1) {
      const p = row + x * bytesPerPixel;
      const o = (y * width + x) * 3;
      if (bpp === 16) {
        // 565（与 dumpBmp 的写出换算同一条公式：乘 255 再整除 31/63；
        // 换一条"四舍五入"的公式会让 16bpp 的 round-trip 假红）
        const v = bytes.readUInt16LE(p);
        rgb[o] = (((v >> 11) & 0x1f) * 255 / 31) | 0;
        rgb[o + 1] = (((v >> 5) & 0x3f) * 255 / 63) | 0;
        rgb[o + 2] = ((v & 0x1f) * 255 / 31) | 0;
      } else {
        // BMP 的像素顺序是 B,G,R(,A)，不是 RGB
        rgb[o] = bytes[p + 2];
        rgb[o + 1] = bytes[p + 1];
        rgb[o + 2] = bytes[p];
      }
    }
  }
  return { width, height, rgb };
}

/**
 * 纯比对（已解码的两帧）。
 * ★ `diffPixels` 的判据是 **`单像素最大通道差 > tolerance`**（不是"有任何字节不同"）：
 *   退出码就按它判；若把"差 1 但在 tolerance 内"也算进 diff-pixels，输出会自相矛盾
 *   （"有差异却绿"）。`maxDiff` 仍然如实报告最大值，不受 tolerance 影响。
 * ★ `at` = **第一个**超差像素（按行优先，y 自上而下），tolerance 内 ⇒ null。
 */
export function diffDecoded(a, b, tolerance = 0) {
  if (a.width !== b.width || a.height !== b.height) {
    return { sizeMismatch: true, aSize: `${a.width}×${a.height}`, bSize: `${b.width}×${b.height}` };
  }
  const total = a.width * a.height;
  let maxDiff = 0;
  let diffPixels = 0;
  let at = null;
  for (let i = 0; i < total; i += 1) {
    const o = i * 3;
    const dr = Math.abs(a.rgb[o] - b.rgb[o]);
    const dg = Math.abs(a.rgb[o + 1] - b.rgb[o + 1]);
    const db = Math.abs(a.rgb[o + 2] - b.rgb[o + 2]);
    const d = Math.max(dr, dg, db); // 单像素 = 三通道里最大的那个（"最大单通道差"的定义）
    if (d > maxDiff) maxDiff = d;
    if (d > tolerance) {
      diffPixels += 1;
      if (at === null) at = { x: i % a.width, y: Math.floor(i / a.width) };
    }
  }
  // 保留 4 位：比例进 JSON 是数字（尾随 0 会被 JSON 吃掉），人读行用 toFixed(4)
  const diffRatio = Number((total ? diffPixels / total : 0).toFixed(4));
  return { sizeMismatch: false, width: a.width, height: a.height, totalPixels: total, maxDiff, diffPixels, diffRatio, at };
}

/** 人读的一行（字段顺序固定：宽高 → 最大差 → 差异像素 → 比例 → 首个超差点 → 口径） */
export function formatDiffLine(r) {
  return (
    `lift diff: ${r.width}×${r.height}  max-diff=${r.maxDiff}  diff-pixels=${r.diffPixels}  ` +
    `diff-ratio=${r.diffRatio.toFixed(4)}  at=${r.at ? `${r.at.x},${r.at.y}` : '-'}  tolerance=${r.tolerance}`
  );
}

/**
 * `diff` 子命令的全部逻辑（**不打印**，返回 {code, result|error}，方便纯档测试直接断言出口码）。
 * 退出码：0 = 所有像素差 ≤ tolerance；1 = 有像素超差；2 = 参数 / 读文件 / 格式错误。
 * ★ 后两类**必须分开**：把"读不到"混成"有差异"会让 CI 把基础设施故障当成画面回归。
 */
export function bmpDiff(argv) {
  const pos = [];
  let tolerance = 0;
  let json = false;
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--json') {
      json = true;
    } else if (arg === '--tolerance' || arg.startsWith('--tolerance=')) {
      const raw = arg === '--tolerance' ? argv[(i += 1)] : arg.slice('--tolerance='.length);
      if (raw === undefined || !/^\d+$/.test(raw)) {
        return { code: 2, error: `--tolerance 需要 0..255 的整数（收到 ${JSON.stringify(raw ?? '')}）` };
      }
      tolerance = Number(raw);
      if (tolerance > 255) return { code: 2, error: `--tolerance 越界：${raw}（只认 0..255）` };
    } else if (arg.startsWith('-')) {
      return { code: 2, error: `未知参数：${arg}（用法：${DIFF_USAGE}）` };
    } else {
      pos.push(arg);
    }
  }
  if (pos.length !== 2) {
    return { code: 2, error: `要恰好两张 BMP（收到 ${pos.length} 个）—— 用法：${DIFF_USAGE}` };
  }
  const [aPath, bPath] = pos;
  const decoded = [];
  for (const p of [aPath, bPath]) {
    // 用户给的路径按 **cwd** 解析（不是 REPO_ROOT）：相对路径就是"在当前目录跑"的直觉
    const absPath = path.resolve(p);
    let bytes;
    try {
      bytes = fs.readFileSync(absPath);
    } catch (e) {
      return { code: 2, error: `读不到 ${p}：${e.code === 'ENOENT' ? '文件不存在' : e.message}` };
    }
    try {
      decoded.push(decodeBmp(bytes));
    } catch (e) {
      return { code: 2, error: `${p} 不是可读的 BMP：${e.message}` };
    }
  }
  const [a, b] = decoded;
  const d = diffDecoded(a, b, tolerance);
  if (d.sizeMismatch) {
    // ⛔ 不做缩放对齐：缩放会引入一个"怎么缩"的口径，而两张尺寸不同的帧本来就不是同一个观测
    return { code: 2, error: `尺寸不同：${aPath} 是 ${d.aSize}，${bPath} 是 ${d.bSize} —— 不做缩放对齐` };
  }
  // 只把报告字段带出去（`sizeMismatch` 是内部判据，不进输出）
  const result = {
    a: aPath,
    b: bPath,
    width: d.width,
    height: d.height,
    totalPixels: d.totalPixels,
    maxDiff: d.maxDiff,
    diffPixels: d.diffPixels,
    diffRatio: d.diffRatio,
    at: d.at,
    tolerance,
  };
  return { code: d.diffPixels === 0 ? 0 : 1, json, result };
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

  // ★ 模块集是**声明**（`GUEST_MODULES`）：`executed=false` 的镜像也被读，但它的裸导入只报告。
  //   `exePath` 只为兼容旧签名 / 测试注入；不等于缺省 EXE_FILE 时退化成单模块。
  const modules = exePath === EXE_FILE ? GUEST_MODULES : [{ path: exePath, executed: true }];
  let guest = null;
  try {
    guest = readGuestImports(modules);
  } catch (e) {
    problems.push(`读 guest 模块导入表失败：${e.message}`);
  }

  if (guest && data) {
    const entries = data.entries ?? [];
    const byTarget = new Map();
    for (const e of entries) if (typeof e.target === 'string') byTarget.set(e.target, e);

    // executed=true 的导入（派生缺口 / 裸缺口 / 覆盖率的**唯一**口径）
    const executed = new Map();
    for (const m of guest.modules) {
      if (!m.executed) continue;
      for (const [k, imp] of m.pe.imports) executed.set(k, imp);
    }
    const executedPe = guest.modules.find((m) => m.executed)?.pe ?? null;
    const pe = executedPe;

    // ① kind 由**注册来源**决定（add → replace / addStub → noop / addUnimplemented → trap），reason 逐字相等
    for (const [key, s] of regs.stubs) {
      const want = kindForOrigin(s.origin);
      const e = byTarget.get(key);
      if (!e) {
        problems.push(`C++ 登记了 ${key}（${s.origin}），但 ${OVERRIDES_FILE} 里没有对应条目`);
      } else if (e.kind !== want) {
        problems.push(`${key} 在 C++ 是 ${s.origin}，JSON 里 kind=${e.kind}（应为 ${want}）`);
      } else if (e.reason !== s.reason) {
        problems.push(`${key} 的 reason 两边不一致：\n    C++  : ${s.reason}\n    JSON : ${e.reason}`);
      }
    }

    // ② JSON 的每条 gap 条目（noop / trap）必须能落到 C++（登记 / 派生缺口）
    const gaps = expectedGapKeys(executed, regs);
    const gapByKey = new Map(gaps.map((g) => [g.key, g]));
    for (const e of entries) {
      if (e.kind !== 'noop' && e.kind !== 'trap') continue;
      if (typeof e.target !== 'string') {
        problems.push(`${entryKey(e)}：M1 只有平台 shim 覆盖项，${e.kind} 必须用 target（module!name）`);
        continue;
      }
      const inRegs = regs.stubs.has(e.target);
      const gap = gapByKey.get(e.target);
      if (!inRegs && !gap) {
        if (regs.implemented.has(e.target)) {
          problems.push(`${e.target}：C++ 里是**实现**（add），JSON 却写成 ${e.kind} —— 要么删条目，要么在 C++ 改登记`);
        } else if (guest.imports.has(e.target)) {
          problems.push(`${e.target}：JSON 里有 ${e.kind} 条目，但 C++ 没登记（该导入只存在于未执行模块，或登记漏了）`);
        } else {
          problems.push(`${e.target}：JSON 里有 ${e.kind} 条目，但 C++ 没登记、导入表里也没有这个导入`);
        }
      }
      if (gap && e.reason !== gapReasonFor(gap.module)) {
        problems.push(`${e.target} 的 reason 与 MODULE_REASONS 不一致：JSON=${e.reason} 期望=${gapReasonFor(gap.module)}`);
      }
    }

    // ③ 派生缺口文件必须与"（executed=true 的）导入表 − 实现 − 登记"逐字节一致
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

    // ④ bare gap：**只对 executed=true 判红**；executed=false 的裸导入单列 bareUnexecuted（只报告）
    const covered = classifyImports(executed, regs, gapFileKeys);
    for (const key of covered.bare) {
      problems.push(`裸缺口（导入表里有、既没实现也没登记，且派生缺口文件里也没有）：${key}`);
    }
    let bareUnexecuted = 0;
    const unexecutedModules = [];
    for (const m of guest.modules) {
      if (m.executed) continue;
      const c = classifyImports(m.pe.imports, regs, gapFileKeys);
      bareUnexecuted += c.bare.length;
      unexecutedModules.push({
        path: m.path,
        imports: m.pe.imports.size,
        implemented: c.implemented.length,
        stub: c.stubs.length,
        bareUnexecuted: c.bare.length,
        bareKeys: c.bare.sort(),
      });
    }
    const notReferenced = notReferencedKeys(regs, guest.imports);
    const counts = {
      imported: executed.size,
      importedAll: guest.imports.size,
      implementedImported: covered.implemented.length,
      stubImported: covered.stubs.length,
      bare: covered.bare.length,
      bareUnexecuted,
      unexecutedModules,
      staticGap: executed.size - covered.implemented.length,
      registeredStubTotal: regs.stubs.size,
      notReferenced,
      gapFileEntries: gapFileKeys.size,
    };
    // 静态缺口的恒等式（executed=true 的镜像）：static = stub + bare
    if (counts.staticGap !== counts.stubImported + counts.bare) {
      problems.push(`静态缺口恒等式不成立：${counts.staticGap} != ${counts.stubImported} + ${counts.bare}`);
    }

    // ④b **逐字节**断言：`overrides.json` 必须等于 `renderOverrides(expectedEntries(...))`。
    // ★ 唯一写入口 = `lift sync --write`；手改一个字符（哪怕只是缩进）就在这里红。
    //   `expectedEntries` 只吃 executed=true 的导入 —— AGERC 的裸导入⛔ 不进清单。
    const expectedOverrides = renderOverrides(expectedEntries({ imports: executed, regs, existing: data }));
    if (readText(OVERRIDES_FILE) !== expectedOverrides) {
      problems.push(
        `${OVERRIDES_FILE} 与 \`lift sync\` 的派生结果不一致（逐字节）—— ` +
          '唯一写入口是 `pnpm tools lift sync --write`（手改会被这条断言打回）',
      );
    }

    // ⑤ sha256：PE = .lst 头部（与 A3 ⓪ 同一条锚）
    let lstSha = null;
    if (pe && exists(lstPath)) {
      const head = fs.readFileSync(abs(lstPath), 'utf8').slice(0, 20000);
      const m = head.match(/;\s*Input SHA256\s*:\s*([0-9A-Fa-f]{64})/);
      lstSha = m ? m[1].toLowerCase() : null;
      if (!lstSha) problems.push(`.lst 头部没有 Input SHA256（${lstPath}）`);
      else if (pe.sha256 !== lstSha) problems.push(`PE sha256 ≠ .lst 头部：${pe.sha256} vs ${lstSha}`);
    } else {
      problems.push(`找不到 .lst（${lstPath}）—— 无法校验字节与语料同源`);
    }

    // ⑥ D6 七族（A5）：七族内的导入不许是 gap，除非键在 FAMILY_STUB_APPROVED（= SCOPE 偏差表已批准）。
    // ★ 只对 executed=true 的**导入表里 status=gap** 的键归类；实现的不需要族别。
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
    for (const [key] of executed) {
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
            `七族内 gap 未获 SCOPE 批准：${key}（${fam.title}）—— 实现它，或写进 SCOPE.md 的偏差表` +
              '并把键加进 tools/lift.mjs 的 FAMILY_STUB_APPROVED',
          );
        }
      } else {
        ++extraCounts[fam.id];
        ++extraStubTotal;
        if (ok) ++extraApproved;
      }
    }
    // 白名单漂了：用「executed × regs」判定，⛔ 不能只看字符串（实现掉的 / 从未被导入的都不能留在白名单里）
    for (const key of FAMILY_STUB_APPROVED) {
      const isImportedStub =
        executed.has(key) && !regs.implemented.has(key) && (regs.stubs.has(key) || gapByKey.has(key));
      if (!isImportedStub) {
        problems.push(`FAMILY_STUB_APPROVED 里的 ${key} 不是（导入的）gap —— 白名单漂了（删掉它或说明）`);
      }
    }
    counts.families = {
      seven: sevenCounts, // 每族的 gap 数（含已批准；族内 gap 总和 = sevenStubTotal + sevenApproved）
      sevenStubTotal, // 未批准的七族 gap 数（判据；必须是 0）
      sevenApproved, // 落在七族但已白名单的
      extra: extraCounts, // 每族的 gap 数（含已批准；不判红）
      extraStubTotal,
      extraApproved,
      unclassified, // 不在任何族里的 gap（user32 / gdi32 / d3d9 / kernel32!CreateProcessA …）；不判红
    };

    if (!quiet) {
      console.log(`lift check · ${OVERRIDES_FILE}`);
      if (pe) console.log(`  PE        : ${modules.find((m) => m.executed).path}  (sha256 ${pe.sha256.slice(0, 16)}…)`);
      console.log(`  导入      : ${counts.imported}（实现 ${counts.implementedImported} · 显式 gap ${counts.stubImported} · 裸缺口 ${counts.bare}）`);
      console.log(`  静态缺口  : ${counts.staticGap} = 已登记 gap ${counts.stubImported} + 未登记 ${counts.bare}`);
      for (const m of unexecutedModules) {
        console.log(
          `  未执行模块: ${m.path} ${m.imports} 导入（实现 ${m.implemented} · 显式 gap ${m.stub} · ` +
            `bareUnexecuted ${m.bareUnexecuted}，不判红）`,
        );
      }
      console.log(
        `  C++ 登记  : add ${regs.implemented.size} · addUnimplemented/addStub ${counts.registeredStubTotal}；` +
          `其中 not-referenced ${counts.notReferenced.length}（不在任一模块导入表 ∧ 不是 AGERC 拦截导出）`,
      );
      if (counts.notReferenced.length) console.log(`             ${counts.notReferenced.join(' ')}`);
      const kindCount = (k) => (data.entries ?? []).filter((e) => e.kind === k).length;
      console.log(
        `  overrides : ${(data.entries ?? []).length} 条（noop ${kindCount('noop')} · trap ${kindCount('trap')}）` +
          ` ← 与 sync 输出逐字节一致`,
      );
      // ⑥ D6 七族（A5）：族标题一律取数据里的 title，⛔ 不另写一份汉字表
      const fam = counts.families;
      console.log(
        `  D6 七族   : gap ${fam.sevenStubTotal + fam.sevenApproved}（未批准 ${fam.sevenStubTotal} · 已批准 ${fam.sevenApproved}）`,
      );
      console.log(`             ${D6_FAMILIES.map((f) => `${f.title} ${fam.seven[f.id]}`).join(' · ')}`);
      console.log(
        `             七族外：${EXTRA_FAMILIES.map((f) => `${f.title} ${fam.extra[f.id]}`).join(' · ')}` +
          ` · 未归类 ${fam.unclassified}（待 SCOPE 批准 ${fam.extraStubTotal - fam.extraApproved} 条）`,
      );
    }
    return { problems, counts, regs, pe, data, guest };
  }

  if (!quiet) {
    console.log(`lift check · ${OVERRIDES_FILE}`);
    for (const p of problems) console.log(`  ✗ ${p}`);
  }
  return { problems, counts: null, regs, pe: null, data };
}

export function reportCoverage({ exePath = EXE_FILE } = {}) {
  const regs = readRegistrations({ skipGapFile: true });
  const modules = exePath === EXE_FILE ? GUEST_MODULES : [{ path: exePath, executed: true }];
  const guest = readGuestImports(modules);
  const executed = new Map();
  for (const m of guest.modules) {
    if (!m.executed) continue;
    for (const [k, imp] of m.pe.imports) executed.set(k, imp);
  }
  const gaps = expectedGapKeys(executed, regs);
  // ★ 与 check ④ 同一口径：只有**盘上的派生文件**与期望一致时，缺口才算"已登记"；
  //   否则报告里的 bare 会像旧实现那样恒为 0（审计 C1）。
  const gapByKey = new Set();
  const gapFileOk = exists(GAP_FILE) && readText(GAP_FILE) === renderGapFile(gaps);
  if (gapFileOk) {
    for (const g of gaps) gapByKey.add(g.key);
  }
  const statusOf = (key) => {
    if (regs.implemented.has(key)) return 'implemented';
    const s = regs.stubs.get(key);
    if (s) return kindForOrigin(s.origin) ?? 'trap';
    if (gapByKey.has(key)) return 'trap';
    return 'bare';
  };
  const rows = [];
  for (const [key, imp] of executed) {
    rows.push({ target: key, module: imp.module, name: imp.name, status: statusOf(key) });
  }
  rows.sort((a, b) => a.target.localeCompare(b.target));

  // executed=false 的模块：只报告裸导入（⛔ 不判红、⛔ 不进 overrides / 派生缺口文件）
  const unexecutedRows = [];
  const unexecutedModules = [];
  for (const m of guest.modules) {
    if (m.executed) continue;
    const bare = [];
    let implemented = 0;
    let stub = 0;
    for (const key of m.pe.imports.keys()) {
      const st = statusOf(key);
      if (st === 'implemented') ++implemented;
      else if (st === 'bare') {
        bare.push(key);
        unexecutedRows.push({ target: key, module: m.pe.imports.get(key).module, status: 'bareUnexecuted' });
      } else ++stub;
    }
    bare.sort();
    unexecutedRows.sort((a, b) => a.target.localeCompare(b.target));
    unexecutedModules.push({
      path: m.path,
      executed: false,
      imports: m.pe.imports.size,
      implemented,
      stub,
      bareUnexecuted: bare.length,
      bareKeys: bare,
    });
  }

  const notReferenced = notReferencedKeys(regs, guest.imports).map((k) => ({ target: k, status: 'not-referenced' }));
  const countStatus = (s) => rows.filter((r) => r.status === s).length;
  const counts = {
    imports: executed.size,
    importedAll: guest.imports.size,
    implemented: countStatus('implemented'),
    noop: countStatus('noop'),
    trap: countStatus('trap'),
    // `stub` = 显式 gap 的合计（noop + trap）；保留这个名字是为了与旧读数对得上
    stub: rows.filter((r) => r.status === 'noop' || r.status === 'trap').length,
    bare: countStatus('bare'),
    bareUnexecuted: unexecutedModules.reduce((n, m) => n + m.bareUnexecuted, 0),
    unexecutedModules,
    notReferenced: notReferenced.length,
    gapFileOk,
  };
  const covered = counts.implemented + counts.stub;
  return {
    rows,
    notReferenced,
    unexecutedRows,
    counts,
    coverage: counts.imports ? covered / counts.imports : 0,
  };
}

// ---------------------------------------------------------------------------------------
// bench（S8：吞吐 MIPS 进报告）
// ---------------------------------------------------------------------------------------
export const BENCH_BIN = process.platform === 'win32'
  ? 'apps/emulator-lift/out/tests/lift-bench.exe'
  : 'apps/emulator-lift/out/tests/lift-bench';

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
// bench（吞吐基线：**一条命令 + 落文件**）
//
// ★ 与上面 `runBench`（`lift-bench.exe` 的合成循环、单次样本、不落文件）**不是**一回事：
//   那个只能当 core 探针（`pnpm tools lift report --bench N`），⛔ 不当基线。
//   基线的定义：**真工况**（`--input-script` 驱动真游戏）+ **N 个独立子进程** + **中位数** + **落文件**，
//   并把口径一起记下来（build 类型 / 二进制 sha256+mtime / 工作树指纹 / 命令行原文）。
//
// ★ 为什么不需要 profiler：`--present window` 是 **real-time** 时钟 ⇒ 两个
//   `at insn N` + `wait 0 insn` 打点之间 `guest_ms` 的差 = **宿主毫秒**。
//   所以窗口吞吐 = Δinsns / Δguest_ms，直接读 `[input-script]` 打点行即可（profiler 有 +33~40% 膨胀）。
//
// ★ 本工具**只调 exe**：⛔ 不重建（cmake）、⛔ 不跑 ctest。exe 不存在就响亮失败。
// ---------------------------------------------------------------------------------------
export const BENCH_SCRIPTS_DIR = 'apps/emulator-lift/scripts/bench';
export const BENCH_OUT_DIR = '.cache/lift-bench';
export const EMULATOR_EXE = process.platform === 'win32'
  ? 'apps/emulator-lift/out/emulator-lift.exe'
  : 'apps/emulator-lift/out/emulator-lift';
export const CMAKE_CACHE = 'apps/emulator-lift/out/CMakeCache.txt';

/**
 * 解析一次运行的日志 ⇒ 窗口 + ms/百万条指令 + 块统计（**纯函数**，不读文件、不起进程）。
 *
 * 打点行 = `[input-script] frame=<f> guest_ms=<t> insns=<n> <动作原文>`（`input_script.cpp` 在动作
 * **被应用**的那一刻打；`wait 0 insn` 就是"什么都不做、只把这一刻记下来"）。
 * ★ 同一 tick 里的第二条（典型：脚本尾部的 `quit` 与最后一个 `wait` 同时到期）⇒ **同 (guest_ms, insns)
 *   的连续重复点去掉**（零宽窗口没有意义），去掉几个记在 `duplicateMarks`。
 *
 * 窗口 = **第一个打点 → 最后一个打点**（脚本写两个打点就是唯一窗口；多写只作诊断）。
 *
 * 会 **throw** 的（= 算不出数，⛔ 不许用估的数值充基线）：没有打点 / 打点 < 2 / Δguest_ms ≤ 0 / Δinsns ≤ 0。
 * 只会进 `problems` 的（= 数能算，但这次运行不完整）：缺 `insns :` / 缺 `blocks :` 摘要行。
 */
export function parseBenchLog(text) {
  const all = [];
  let summaryInsns = null;
  let blocks = null;
  let shims = null;
  let stop = null;
  const lines = String(text ?? '').split(/\r?\n/);
  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i];
    const m = /^\[input-script\]\s+frame=(\d+)\s+guest_ms=(\d+)\s+insns=(\d+)\s*(.*)$/.exec(line);
    if (m) {
      all.push({
        line: i + 1,
        frame: Number(m[1]),
        guestMs: Number(m[2]),
        insns: Number(m[3]),
        action: m[4].trim(),
      });
      continue;
    }
    // 摘要行（`Machine::run` 收口时打；顺序固定，但这里不依赖顺序）
    const s = /^insns\s*:\s*(\d+)\s*$/.exec(line);
    if (s) { summaryInsns = Number(s[1]); continue; }
    const b = /^blocks\s*:\s*(\d+)\s*\(insns cached (\d+),\s*hits (\d+),\s*misses (\d+)\)\s*$/.exec(line);
    if (b) {
      blocks = { blocks: Number(b[1]), insnsCached: Number(b[2]), hits: Number(b[3]), misses: Number(b[4]) };
      continue;
    }
    const h = /^shims\s*:\s*imported (\d+)\s*\/\s*called-unimplemented (\d+)\s*$/.exec(line);
    if (h) { shims = { imported: Number(h[1]), calledUnimplemented: Number(h[2]) }; continue; }
    const st = /^stop\s*:\s*(.+)$/.exec(line);
    if (st) stop = st[1].trim();
  }

  if (all.length === 0) {
    throw new Error(
      '日志里没有 `[input-script]` 打点（找不到 `… guest_ms= … insns= …`）⇒ 算不出窗口。\n' +
      '  脚本里要有 **两处** `at insn N` + `wait 0 insn`（第二条用 `after N insn` / `at insn` 都比 `at frame` 可靠），\n' +
      '  并且这次运行真的跑到了它们（看同一份日志里有没有 `input-script: <path>（N 条动作` 与 `stop :`）。',
    );
  }

  // 去掉"同一 tick 的连续重复点"（典型是脚本尾部的 `quit` 与最后一个 `wait` 同时到期）
  const marks = [];
  let duplicateMarks = 0;
  for (const m of all) {
    const last = marks[marks.length - 1];
    if (last && last.guestMs === m.guestMs && last.insns === m.insns) { duplicateMarks += 1; continue; }
    marks.push(m);
  }

  if (marks.length < 2) {
    throw new Error(
      `只有 ${marks.length} 个打点（去掉同一 tick 的重复点后）⇒ 窗口至少要 2 个点。` +
      (duplicateMarks ? `（另有 ${duplicateMarks} 个同 tick 重复点被去掉）` : '') + '\n' +
      '  一个点只知道"某一刻在哪"，两个点才给出 Δinsns / Δguest_ms。',
    );
  }

  const from = marks[0];
  const to = marks[marks.length - 1];
  const hostMs = to.guestMs - from.guestMs;
  const insns = to.insns - from.insns;
  if (!(hostMs > 0)) {
    throw new Error(`窗口 Δguest_ms = ${hostMs} ≤ 0（first=${from.guestMs} last=${to.guestMs}）⇒ 两个打点没有时间跨度`);
  }
  if (!(insns > 0)) {
    throw new Error(`窗口 Δinsns = ${insns} ≤ 0（first=${from.insns} last=${to.insns}）⇒ 两个打点没有指令跨度`);
  }

  const problems = [];
  if (summaryInsns === null) problems.push('日志里没有 `insns     : <N>`（运行摘要没打 ⇒ 这次运行可能没正常收口）');
  if (blocks === null) problems.push('日志里没有 `blocks    : …`（同上 ⇒ 块命中率 / 平均块长缺席）');

  const blockTotal = blocks ? blocks.hits + blocks.misses : 0;
  return {
    marks,
    duplicateMarks,
    window: { from, to, hostMs, insns },
    // ms / 百万条指令（越小越快）；mips = 百万条/秒
    msPerMinsn: (hostMs * 1e6) / insns,
    mips: insns / hostMs / 1000,
    summary: summaryInsns === null && blocks === null && shims === null && stop === null
      ? null
      : {
        insns: summaryInsns,
        stop,
        shims,
        blocks: blocks ? blocks.blocks : null,
        insnsCached: blocks ? blocks.insnsCached : null,
        hits: blocks ? blocks.hits : null,
        misses: blocks ? blocks.misses : null,
      },
    // 块命中率 / 平均块长：运行摘要 `blocks :` 行的三个数**免费**给出（不需要 profiler）
    blockHitRate: blockTotal ? blocks.hits / blockTotal : null,
    avgBlockLen: blockTotal && summaryInsns ? summaryInsns / blockTotal : null,
    problems,
  };
}

/** CMake 配置回读（性能判据固定 RelWithDebInfo；⛔ 不认 Debug 的读数）。 */
export function readBuildType(cachePath = CMAKE_CACHE) {
  if (!exists(cachePath)) return { configured: false, buildType: null, path: cachePath };
  const m = /^CMAKE_BUILD_TYPE:STRING=(.*)$/m.exec(readText(cachePath));
  return { configured: true, buildType: m ? m[1].trim() : null, path: cachePath };
}

/** 跑一条子进程，stdout+stderr 一起重定向到文件（⛔ 不用管道：受限沙箱里管道会 EPERM）。 */
function runRedirected(cmd, argv, { cwd = REPO_ROOT, logPath, timeoutMs = 0 } = {}) {
  const fd = fs.openSync(logPath, 'w');
  let r;
  try {
    r = spawnSync(cmd, argv, { cwd, stdio: ['ignore', fd, fd], timeout: timeoutMs, windowsHide: true });
  } finally {
    fs.closeSync(fd);
  }
  const text = fs.existsSync(logPath) ? fs.readFileSync(logPath, 'utf8') : '';
  return { status: r.status, signal: r.signal ?? null, error: r.error ?? null, text };
}

/**
 * **工作树指纹**（同一个 sha256 才可比的另一半：换一份二进制 / 换一棵树就不许混表）。
 * 取 `git status --porcelain` 的全文 sha256 + 行数；git 不可用 ⇒ available=false（读数里说清楚，⛔ 不假装）。
 */
export function worktreeFingerprint() {
  const log = path.join(os.tmpdir(), `lift-bench-git-${process.pid}.log`);
  try {
    const r = runRedirected('git', ['status', '--porcelain'], { logPath: log, timeoutMs: 60000 });
    if (r.error || r.status !== 0) {
      return { available: false, dirtyLines: null, porcelainSha256: null, reason: r.error ? r.error.message : `git 退出码 ${r.status}` };
    }
    const lines = r.text.split(/\r?\n/).filter((s) => s.length > 0);
    return {
      available: true,
      dirtyLines: lines.length,
      porcelainSha256: crypto.createHash('sha256').update(r.text).digest('hex'),
    };
  } finally {
    fs.rmSync(log, { force: true });
  }
}

/** 工况名 → 脚本路径（`idle` ⇒ `apps/emulator-lift/scripts/bench/idle.txt`；带分隔符 / `.txt` 的当路径）。 */
export function resolveBenchScript(scenario) {
  if (!scenario) throw new Error('bench 需要一个工况名或脚本路径（例：`pnpm tools lift bench idle`）');
  const asPath = /[\\/]/.test(scenario) || scenario.endsWith('.txt');
  const cand = asPath ? scenario : `${BENCH_SCRIPTS_DIR}/${scenario}.txt`;
  if (!exists(cand)) {
    const avail = (() => {
      try {
        return fs.readdirSync(abs(BENCH_SCRIPTS_DIR)).filter((f) => f.endsWith('.txt')).map((f) => f.replace(/\.txt$/, ''));
      } catch { return []; }
    })();
    throw new Error(`找不到工况脚本 ${cand}${avail.length ? `\n  可用工况：${avail.join(' / ')}` : `（目录 ${BENCH_SCRIPTS_DIR} 不存在）`}`);
  }
  return cand;
}

const BENCH_BASE_ARGS = [
  // ★ 口径A（docs/03-emulator/observations.md 顶部）：window + hidden + native-overrides all + skip-logo。
  //   光栅化只有 GPU 一条实现（没有 `--raster` 开关）⇒ 口径A 里不再有它。
  '--present', 'window', '--present-vsync', '0', '--present-hidden',
  '--native-overrides', 'all', '--skip-logo',
];

function shellQuote(token) {
  return /[\s"]/.test(token) ? `"${token.replace(/"/g, '\\"')}"` : token;
}

function median(values) {
  const v = [...values].sort((a, b) => a - b);
  if (v.length === 0) return null;
  const mid = Math.floor(v.length / 2);
  return v.length % 2 ? v[mid] : (v[mid - 1] + v[mid]) / 2;
}

function writeBenchJson(outDir, name, obj) {
  const p = abs(`${outDir}/${name}.json`);
  fs.mkdirSync(path.dirname(p), { recursive: true });
  const text = `${JSON.stringify(obj, null, 2)}\n`;
  fs.writeFileSync(p, text);
  if (fs.readFileSync(p, 'utf8') !== text) throw new Error(`${outDir}/${name}.json 写后回读不一致`);
  return `${outDir}/${name}.json`;
}

/**
 * 跑 N 遍同一工况，取中位数，落 JSON。
 *
 * @param {{scenario?: string, n?: number, outDir?: string, install?: string, appdata?: string,
 *          extraArgs?: string[], label?: string, timeoutSec?: number, write?: boolean,
 *          run?: Function}} opts
 *   `run` 只为测试注入（缺省用真子进程）；生产路径上不许传。
 */
export function bench(opts = {}) {
  const n = opts.n ?? 5;
  if (!Number.isInteger(n) || n < 1) throw new Error(`--n 必须是正整数（收到 ${opts.n}）`);
  const timeoutSec = opts.timeoutSec ?? 600;
  if (!Number.isFinite(timeoutSec) || timeoutSec <= 0) throw new Error(`--timeout 必须是正数秒（收到 ${opts.timeoutSec}）`);
  const install = opts.install ?? 'links/install';
  const outDir = opts.outDir ?? BENCH_OUT_DIR;
  const script = resolveBenchScript(opts.scenario);
  const label = opts.label ?? path.basename(script).replace(/\.txt$/, '');
  const write = opts.write !== false;

  if (!exists(EMULATOR_EXE)) {
    throw new Error(
      `找不到 ${EMULATOR_EXE} —— bench **只调 exe，不重建**（⛔ 不 cmake、⛔ 不 ctest）。\n` +
      '  先手工：`cd apps/emulator-lift && cmake --preset windows-x64 && cmake --build --preset windows-x64`',
    );
  }
  if (!exists(install)) throw new Error(`找不到安装树 ${install}（--install 可指定；本工具不猜游戏装在别处）`);

  const build = readBuildType();
  if (build.buildType === 'Debug') {
    throw new Error(
      `CMAKE_BUILD_TYPE = Debug（${CMAKE_CACHE}）⇒ ⛔ 不用 Debug 构建的读数当性能判据（固定 RelWithDebInfo）。`,
    );
  }

  const appdata = opts.appdata ?? `${outDir}/appdata`;
  fs.mkdirSync(abs(appdata), { recursive: true });

  const argv = [
    '--game-dir', install,
    '--appdata', appdata,
    ...BENCH_BASE_ARGS,
    '--input-script', script,
    ...(opts.extraArgs ?? []),
  ];
  const exeAbs = abs(EMULATOR_EXE);
  const stat = fs.statSync(exeAbs);
  const meta = {
    schema: 1,
    tool: 'pnpm tools lift bench',
    note: '口径A（window / vsync 0 / hidden / gpu / native-overrides all / skip-logo）+ 工况脚本；'
      + '窗口吞吐 = Δinsns / Δguest_ms（window 模式是 real-time 时钟）；⛔ 未开 profiler、⛔ 未重建',
    timestamp: new Date().toISOString(),
    label,
    scenario: opts.scenario,
    script: { path: script, sha256: sha256File(script) },
    build: { cmakeBuildType: build.buildType, configured: build.configured, cmakeCache: CMAKE_CACHE },
    binary: {
      path: EMULATOR_EXE,
      sha256: sha256File(EMULATOR_EXE),
      bytes: stat.size,
      mtime: stat.mtime.toISOString(),
    },
    worktree: worktreeFingerprint(),
    host: {
      platform: process.platform,
      arch: process.arch,
      cpus: os.cpus().length,
      cpuModel: os.cpus()[0] ? os.cpus()[0].model : null,
      totalMemMiB: Math.round(os.totalmem() / (1024 * 1024)),
      node: process.version,
    },
    n,
    timeoutSec,
    argv,
    commandLine: [EMULATOR_EXE, ...argv].map(shellQuote).join(' '),
    passes: [],
  };

  const passes = [];
  const run = opts.run ?? ((logRel) => runRedirected(exeAbs, argv, { logPath: abs(logRel), timeoutMs: timeoutSec * 1000 }));
  for (let i = 1; i <= n; i += 1) {
    const logRel = `${outDir}/${label}-pass${i}.log`;
    fs.mkdirSync(abs(outDir), { recursive: true });
    const r = run(logRel);
    const pass = { index: i, log: logRel, exitCode: r.status, signal: r.signal ?? null, spawnError: r.error ? (r.error.message ?? String(r.error)) : null };
    let parsed = null;
    let failure = null;
    if (r.error) failure = `起不来：${r.error.message ?? r.error}`;
    else {
      try {
        parsed = parseBenchLog(r.text);
        if (parsed.problems.length) failure = `日志不完整：${parsed.problems.join('；')}`;
      } catch (err) {
        failure = err.message;
      }
    }
    if (parsed) {
      pass.window = parsed.window;
      pass.hostMs = parsed.window.hostMs;
      pass.insns = parsed.window.insns;
      pass.msPerMinsn = parsed.msPerMinsn;
      pass.mips = parsed.mips;
      pass.marks = parsed.marks;
      pass.duplicateMarks = parsed.duplicateMarks;
      pass.frameFrom = parsed.window.from.frame;
      pass.frameTo = parsed.window.to.frame;
      pass.blockHitRate = parsed.blockHitRate;
      pass.avgBlockLen = parsed.avgBlockLen;
      pass.summary = parsed.summary;
    }
    if (r.status !== 0 && !failure) failure = `exe 退出码 ${r.status}${r.signal ? `（signal ${r.signal}）` : ''}`;
    if (failure) pass.error = failure;
    passes.push(pass);
    meta.passes = passes;

    if (failure) {
      const where = parsed ? '.' : `\n  ── 原始日志片段（${logRel}）──\n${excerpt(r.text)}`;
      if (write) {
        const p = writeBenchJson(outDir, `${label}-FAILED`, meta);
        throw new Error(`第 ${i} 遍失败：${failure}\n  日志：${logRel}${where}\n  （失败记录已落 ${p}；⛔ 不拿估的数值充基线）`);
      }
      throw new Error(`第 ${i} 遍失败：${failure}\n  日志：${logRel}${where}`);
    }
  }

  const values = passes.map((p) => p.msPerMinsn);
  const med = median(values);
  const min = Math.min(...values);
  const max = Math.max(...values);
  const spread = med > 0 ? (max - min) / med : 0;
  const mean = values.reduce((a, b) => a + b, 0) / values.length;
  const sd = values.length > 1 ? Math.sqrt(values.reduce((a, b) => a + (b - mean) ** 2, 0) / (values.length - 1)) : 0;
  // 要多少遍才够（95% CI、目标 ±5% 半宽）：n ≈ (1.96·sd / (0.05·median))²
  const suggestedN = med > 0 ? Math.max(5, Math.ceil(((1.96 * sd) / (0.05 * med)) ** 2)) : null;

  meta.median = { msPerMinsn: med, mips: med > 0 ? 1e6 / med / 1000 : null };
  meta.spread = { min, max, ratio: spread };
  meta.sd = sd;
  meta.jitter = {
    warn: spread > 0.3,
    text: spread > 0.3
      ? `逐遍极差 ${(spread * 100).toFixed(1)}% > 30% ⇒ 这批读数不足以定基线（每遍原始值见 passes）；`
        + `按当前离散度，±5%（95% CI）要约 ${suggestedN} 遍`
      : null,
    suggestedN,
  };

  let written = null;
  if (write) {
    const stamp = meta.timestamp.replace(/[-:]/g, '').replace(/\.\d+Z$/, 'Z');
    written = {
      stamp: writeBenchJson(outDir, `${label}-${stamp}`, meta),
      latest: writeBenchJson(outDir, `${label}-latest`, meta),
    };
  }
  return { ...meta, written };
}

/** 失败时贴得进报告的那一段（尾部 40 行；摘要 / 打点都在尾部）。 */
function excerpt(text, lines = 40) {
  const all = String(text ?? '').split(/\r?\n/);
  const tail = all.slice(Math.max(0, all.length - lines));
  return tail.map((s) => `  | ${s}`).join('\n');
}

export function benchText(r) {
  const L = [];
  const pct = (x) => (x === null || x === undefined ? '—' : `${(x * 100).toFixed(3)}%`);
  const f = (x, d = 2) => (x === null || x === undefined ? '—' : x.toFixed(d));
  L.push(`lift bench —— ${r.label} × ${r.n}（${r.script.path}）`);
  L.push(`  构建      : ${r.build.cmakeBuildType ?? '（未知）'}${r.build.cmakeBuildType === 'Debug' ? ' ⛔ Debug 不当判据' : ''}`);
  L.push(`  二进制    : ${r.binary.path}  sha256=${r.binary.sha256.slice(0, 16)}…  mtime=${r.binary.mtime}  ${r.binary.bytes} B`);
  L.push(`  工作树    : ${r.worktree.available ? `${r.worktree.dirtyLines} 行未提交（porcelain sha256=${String(r.worktree.porcelainSha256).slice(0, 16)}…）` : `不可用（${r.worktree.reason}）`}`);
  L.push(`  脚本      : sha256=${r.script.sha256.slice(0, 16)}…`);
  L.push(`  命令      : ${r.commandLine}`);
  L.push('');
  for (const p of r.passes) {
    L.push(`  第 ${p.index} 遍   : ${p.insns} 条 / ${p.hostMs} ms ⇒ ${f(p.msPerMinsn)} ms/M = ${f(p.mips, 3)} M insn/s  `
      + `（frame ${p.frameFrom}→${p.frameTo}；块命中 ${pct(p.blockHitRate)} / 平均块长 ${f(p.avgBlockLen, 2)}）`);
  }
  L.push('');
  L.push(`  中位数    : ${f(r.median.msPerMinsn)} ms/百万条 = ${f(r.median.mips, 3)} M insn/s`
    + `（极差 ${f(r.spread.ratio * 100, 1)}%：min ${f(r.spread.min)} / max ${f(r.spread.max)}）`);
  if (r.jitter.warn) L.push(`  ⚠ ${r.jitter.text}`);
  if (r.written) L.push(`  落盘      : ${r.written.stamp} · ${r.written.latest}`);
  return `${L.join('\n')}\n`;
}

// ---------------------------------------------------------------------------------------
// describe / 文本
// ---------------------------------------------------------------------------------------
export function describe() {
  return {
    file: OVERRIDES_FILE,
    doc: OVERRIDES_DOC,
    derivedGapFile: GAP_FILE,
    registry: `${SHIM_DIR}/*.cpp（add / addStub / addUnimplemented 是 kind 与 reason 的真源）`,
    schema: SCHEMA,
    entries: {
      fields: [
        { name: 'target', type: 'string "module!name"', note: '平台 shim 的键（M1 只有这一种）；与 ea / range 三选一' },
        { name: 'ea', type: 'hex string "0x00401234"', note: '函数级覆盖的 guest EA（三选一）' },
        { name: 'range', type: '[start, end] hex/数字', note: '子系统级覆盖的 EA 范围（三选一）' },
        { name: 'kind', type: 'replace | noop | trap', note: '由 C++ 注册来源决定：add ⇒ replace（有实现）· addStub ⇒ noop（有 fn，按 SCOPE 偏差返回合理值）· addUnimplemented / 派生缺口 ⇒ trap（无 fn，执行到就响亮失败）' },
        { name: 'impl', type: 'string', note: 'trap 用 "stub_trap"；有 fn 的条目用源文件路径' },
        { name: 'boundary', type: 'import | function | subsystem | 说明串', note: '粒度；kind=replace 时必须说明保住的返回码 / 共享数据' },
        { name: 'reason', type: '非空 string', note: 'SCOPE §3 硬要求：每条 gap（noop/trap）都要有理由' },
      ],
      oneOfKeys: ['target', 'ea', 'range'],
      kinds: KINDS,
      boundaryKinds: BOUNDARY_KINDS,
      kindSource: 'kind 不是手选的：noop = C++ 的 addStub，trap = addUnimplemented 或派生缺口，replace = add（手写 guest-code 覆盖也是 replace）',
    },
    invariants: [
      { id: 'json-md', text: 'overrides.json 旁边必须有 overrides.md，含「怎么查 / 怎么改」并指向 describe', enforcedBy: 'tools/test/json-docs.test.mjs' },
      { id: 'schema', text: 'schema=1；entries[].kind ∈ replace|noop|trap；reason 非空', enforcedBy: 'tools/lift.mjs check（也由 tools/test/lift.test.mjs 纯档复跑）' },
      { id: 'registry-sync', text: 'C++ 的每条 addUnimplemented/addStub 在 JSON 里恰好一条，且 kind 由注册来源决定（addStub ⇒ noop / addUnimplemented ⇒ trap），reason 逐字相等；反向也成立', enforcedBy: 'tools/lift.mjs check' },
      { id: 'byte-identical', text: 'overrides.json 必须**逐字节**等于 renderOverrides(expectedEntries(...))（唯一写入口 = pnpm tools lift sync --write；手改一个字符就红）', enforcedBy: 'tools/lift.mjs check · tools/test/lift.test.mjs（纯档：用 C++ 注册表 + 派生缺口文件重算，不读 PE）· tools/mutate-check.mjs' },
      { id: 'guest-modules', text: 'guest 模块集是声明的（GUEST_MODULES：AGE.EXE executed=true、corpus/assets/agerc/AGERC.DLL executed=false）；导入表按模块集合并，并保留"哪个模块导入"的归属', enforcedBy: 'tools/lift.mjs check · tools/test/lift.test.mjs' },
      { id: 'no-bare-gap', text: 'executed=true 的模块导入表里每条导入要么有实现、要么有显式 gap 登记（裸缺口 = 0）；executed=false 的模块（AGERC）的裸导入单列 bareUnexecuted，不判红、不登记', enforcedBy: 'tools/lift.mjs check' },
      { id: 'not-referenced', text: 'not-referenced = C++ 登记了 gap 但不在任一模块导入表 ∧ 不是被拦截的 AGERC 导出（agerc!_*）；只进报告，不判红', enforcedBy: 'tools/lift.mjs check / report' },
      { id: 'gap-file', text: 'register_import_gaps.cpp 必须逐字节等于「（executed=true 的）导入表 − 实现 − 登记」的派生结果', enforcedBy: 'tools/lift.mjs check' },
      { id: 'sha-anchor', text: 'PE sha256 == .lst 头部 Input SHA256（与 A3 ⓪ 同一条锚）', enforcedBy: 'tools/lift.mjs check · tests/decode_crosscheck.cpp' },
      { id: 'no-dup-registration', text: '同一个 module!name 不许在 C++ 里注册两次（实现与未实现并存会让 find() 命中的那条不可预期）', enforcedBy: 'tools/lift.mjs check（实测曾遮蔽 GetEnvironmentVariableA）' },
      { id: 'seven-family-no-stub',
        text: 'D6 七族（同步 / 内存 / 文件与路径 / 计时 / 错误码与TLS / 码页与字符串 / 进程·模块信息）里的导入不许是 gap，除非键在 tools/lift.mjs 的 FAMILY_STUB_APPROVED（= SCOPE.md 偏差表已批准）；未批准 = check 红（A5）',
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
  const modules = exePath === EXE_FILE ? GUEST_MODULES : [{ path: exePath, executed: true }];
  const guest = readGuestImports(modules);
  // ★ 派生缺口只对 executed=true 的镜像：AGERC 的裸导入⛔ 不进 `register_import_gaps.cpp`
  //   （它的代码今天从不执行；用户 2026-11-13 裁决：单列 bareUnexecuted，不登记）。
  const executed = new Map();
  for (const m of guest.modules) {
    if (!m.executed) continue;
    for (const [k, imp] of m.pe.imports) executed.set(k, imp);
  }
  const gaps = expectedGapKeys(executed, regs);
  const { data } = readOverrides();
  const entries = expectedEntries({ imports: executed, regs, existing: data });
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

const HELP = `tools/lift.mjs — emulator-lift 覆盖清单（describe / check / report / bench / corpus-report / sync / diff）

  pnpm tools lift describe       字段 / 不变量 / 操作（schema 真源）
  pnpm tools lift check          全量校验（红 ⇒ 退出码 1）
  pnpm tools lift report         覆盖报告（实现 / noop / trap / 裸缺口 / bareUnexecuted / not-referenced；--bench N 出 MIPS）
  pnpm tools lift bench <工况|脚本> [--n N] [--out DIR] [--install DIR] [--appdata DIR]
                     [--arg TOKEN]... [--label NAME] [--timeout S] [--no-write] [--json]
                                 吞吐基线（一条命令 + 落文件）：同一 --input-script 工况**独立子进程**跑 N 遍
                                 （缺省 5），取中位数 ms/百万条 + MIPS，落 .cache/lift-bench/*.json
                                 （含 exe sha256+mtime / CMAKE_BUILD_TYPE 回读 / 工作树指纹 / 命令行原文）。
                                 口径A：--present window --present-vsync 0 --present-hidden
                                 --native-overrides all --skip-logo（光栅化恒为 GPU）。⛔ 不重建、不跑 ctest。
                                 ⛔ 与 report --bench（合成循环、单样本）不是一回事
  pnpm tools lift corpus-report  .lst 语料报告
  pnpm tools lift sync [--write] 派生 overrides.json + register_import_gaps.cpp（缺省 dry-run）
  pnpm tools lift diff <a.bmp> <b.bmp> [--tolerance N] [--json]
                                 两张 BMP 逐像素比对（缺省 tolerance=0；0=全在容差内 / 1=有超差 / 2=参数或读文件错）
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
  if ((argv[0] === '--bench' || argv[0] === 'bench') && !has('--report')) {
    // ★ 与 `report --bench N`（合成循环 MIPS，**不是**基线）分开：本 op 是"真工况 + N 遍 + 中位数 + 落文件"。
    //   两处都叫 `--bench`：`report` 那个是**取值 flag**（`--report` 在前时归 report），本 op 是**动作**
    //   （派发器给的首项就是它）⇒ 用 `!has('--report')` 这条互斥判据分开，⛔ 不靠参数顺序猜。
    const rest = argv.slice(1);
    const usage = (msg) => (msg ? `${msg}\n\n` : '') + HELP;
    const takeVal = (name) => {
      for (let i = 0; i < rest.length; i += 1) {
        if (rest[i] === name) {
          const v = rest[i + 1];
          if (v === undefined) throw new Error(`${name} 后面缺一个值`);
          rest.splice(i, 2);
          return v;
        }
        if (rest[i].startsWith(`${name}=`)) {
          const v = rest[i].slice(name.length + 1);
          rest.splice(i, 1);
          return v;
        }
      }
      return null;
    };
    try {
      const nRaw = takeVal('--n');
      const outDir = takeVal('--out');
      const install = takeVal('--install');
      const appdata = takeVal('--appdata');
      const label = takeVal('--label');
      const timeoutRaw = takeVal('--timeout');
      const extraArgs = [];
      for (let i = 0; i < rest.length;) {
        if (rest[i] === '--arg') {
          if (rest[i + 1] === undefined) throw new Error('--arg 后面缺一个 token');
          extraArgs.push(rest[i + 1]);
          rest.splice(i, 2);
          continue;
        }
        if (rest[i].startsWith('--arg=')) {
          extraArgs.push(rest[i].slice('--arg='.length));
          rest.splice(i, 1);
          continue;
        }
        i += 1;
      }
      const noWrite = has('--no-write');
      const json = has('--json');
      const scenario = rest.find((a) => !a.startsWith('--'));
      const unknown = rest.filter((a) => a !== scenario && a !== '--no-write' && a !== '--json');
      if (unknown.length) {
        process.stderr.write(`lift bench: 不认识的参数 ${unknown.join(' ')}\n\n${usage()}`);
        return 2;
      }
      const r = bench({
        scenario,
        n: nRaw === null ? 5 : Number(nRaw),
        outDir: outDir ?? undefined,
        install: install ?? undefined,
        appdata: appdata ?? undefined,
        label: label ?? undefined,
        timeoutSec: timeoutRaw === null ? undefined : Number(timeoutRaw),
        extraArgs,
        write: !noWrite,
      });
      process.stdout.write(json ? `${JSON.stringify(r, null, 2)}\n` : benchText(r));
      return 0;
    } catch (err) {
      process.stderr.write(`lift bench: ${err.message}\n`);
      return 2;
    }
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
    console.log(
      `  导入 ${r.counts.imports} = 实现 ${r.counts.implemented} + 显式 gap ${r.counts.stub}` +
        `（noop ${r.counts.noop} · trap ${r.counts.trap}）+ 裸缺口 ${r.counts.bare}`,
    );
    console.log(`  覆盖率 = ${(r.coverage * 100).toFixed(1)}%（(实现+显式 gap)/导入；executed=true 的裸缺口必须 0）`);
    console.log(
      `  bareUnexecuted ${r.counts.bareUnexecuted} 条（executed=false 模块的裸导入；不判红、不登记）` +
        (r.counts.unexecutedModules.length
          ? `：${r.counts.unexecutedModules.map((m) => `${m.path} ${m.bareUnexecuted}/${m.imports}`).join(' · ')}`
          : ''),
    );
    console.log(`  not-referenced ${r.counts.notReferenced} 条（不在任一 guest 模块导入表 ∧ 不是 AGERC 拦截导出；只报告）`);
    if (bench) {
      if (bench.unavailable) console.log(`  bench   : 无法测量（${bench.unavailable}）`);
      else console.log(`  bench   : ${bench.insns} insns in ${bench.seconds} s = ${bench.mips} MIPS`);
    }
    for (const row of r.rows) console.log(`  ${row.status.padEnd(15)} ${row.target}`);
    for (const n of r.notReferenced) console.log(`  ${n.status.padEnd(15)} ${n.target}`);
    if (r.unexecutedRows.length) {
      console.log('  ── bareUnexecuted（executed=false 的模块：裸导入只报告，⛔ 不登记为 gap）──');
      for (const row of r.unexecutedRows) console.log(`  ${row.status.padEnd(15)} ${row.target}`);
    }
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
  if (has('--diff') || argv[0] === 'diff') {
    // 两种入口：`node tools/lift.mjs diff …`（人读用法）与派发器的 `--diff …`（`pnpm tools lift diff …`）
    const rest = argv[0] === 'diff' ? argv.slice(1) : argv.filter((a) => a !== '--diff');
    const r = bmpDiff(rest);
    if (r.error) {
      process.stderr.write(`lift diff: ${r.error}\n`);
      return 2;
    }
    process.stdout.write(r.json ? `${JSON.stringify(r.result, null, 2)}\n` : `${formatDiffLine(r.result)}\n`);
    return r.code;
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
