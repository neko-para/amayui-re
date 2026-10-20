/**
 * 启动阶段的**显式记账**：从 `_WinMain@16` 到主循环之间，哪些阶段被建模、哪些被跳过、为什么。
 *
 * ★ 为什么要有这个文件：参考实现从"根脚本 id 0"开始执行，**不等于**引擎的启动只有这一步。
 *   真实入口在脚本之前还有单实例、COM、窗口类、引擎构造、命令行、索引、自更新、窗口、配置、定时器……
 *   这些阶段必须**逐条可见**（`modeled` / `partial` / `host-only` / `not-modeled`），⛔ 不许静默跳过。
 *
 * ★ `anchor` 是 `_WinMain@16` 里该阶段的**指令起点 EA**（只做溯源；不参与任何状态，也不进数据模型）。
 *   证据窗口：`pnpm tools disasm-at at --ea <anchor>`。
 * ★ 本目录是 spec 的消费者：这里的"跳过"不是规范，是本实现的**保真欠账清单**；规范侧见
 *   `data/spec/core/README.md` §1.1 的 `CORE-BOOT` / `CORE-FS`（显式延后）。
 */

export type BootDisposition = 'modeled' | 'partial' | 'host-only' | 'not-modeled';

export const BOOT_DISPOSITIONS: BootDisposition[] = ['modeled', 'partial', 'host-only', 'not-modeled'];

export interface BootStage {
  id: string;
  what: string;
  disposition: BootDisposition;
  /** 为什么是这个处置（⛔ 不许空） */
  why: string;
  /** `_WinMain@16` 里该阶段的指令起点 EA（溯源用） */
  anchor: string;
}

export const BOOT_STAGES: BootStage[] = [
  {
    id: 'instance-guard',
    what: '单实例信号量 / 互斥（"ARCGameEngine"）',
    disposition: 'host-only',
    why: '无 Win32 内核对象；headless 由调用方保证单实例',
    anchor: '0x4ba8f5',
  },
  {
    id: 'com-init',
    what: 'CoInitialize',
    disposition: 'host-only',
    why: 'COM 初始化属于宿主；参考实现不使用 COM',
    anchor: '0x4ba95d',
  },
  {
    id: 'window-class',
    what: '注册窗口类（RegisterClassExA）',
    disposition: 'host-only',
    why: '窗口属于表现层；core 的边界之外',
    anchor: '0x4baa02',
  },
  {
    id: 'engine-construct',
    what: '构造引擎对象（分派表 / 帧基址 / 编解码 key）',
    disposition: 'partial',
    why: '本层只建执行器状态（池 / 场景 / 门）；key 由 CLI 显式给，窗口与设备字段不建',
    anchor: '0x4baa2e',
  },
  {
    id: 'command-line',
    what: '解析命令行（B/SP/PATH=/RPATH=/RKEY=/LF=/NH/D=/DOF=/DL=/DV=/DR=/DE=）',
    disposition: 'host-only',
    why: 'headless 用显式参数代替；这些键的语义未逐条取证',
    anchor: '0x4baa59',
  },
  {
    id: 'index-open',
    what: '打开索引 SYS4INI.BIN / SYS3INI.BIN',
    disposition: 'modeled',
    why: 'ALF 索引解析已落地（S4 族；S3 无样本、未覆盖）',
    anchor: '0x4bab26',
  },
  {
    id: 'self-update',
    what: '自更新 / 卸载 / AGERC 版本检查（_.exe、/FIX）',
    disposition: 'host-only',
    why: '进程与文件替换；只读参考实现不执行',
    anchor: '0x4bad94',
  },
  {
    id: 'window-create',
    what: '创建窗口（CreateWindowExA / ShowWindow / 菜单 / IME）',
    disposition: 'host-only',
    why: '表现层；不改变脚本可观察的引擎数据',
    anchor: '0x4bb082',
  },
  {
    id: 'display-config',
    what: '读显示与运行配置（display:ScreenMode / set:RegRootPath / set:ClickOnUp …）',
    disposition: 'not-modeled',
    why: '配置服务尚未建模；脚本随后用 load-int / load-string 读它（0x1a2 / 0x1a3 / 0x1a9 / 0x1aa）',
    anchor: '0x4bb0dd',
  },
  {
    id: 'script-load',
    what: '装载根脚本：sub_40ED40(Engine, hWnd, 0)，第三个实参 = 统一文件 id 0',
    disposition: 'modeled',
    why: '按 id 0 从松散文件 / 归档取 SYSTEM4.BIN（索引 → 条目 → 切片）',
    anchor: '0x4bb4cc',
  },
  {
    id: 'timer',
    what: 'SetTimer(hWnd, 2, 10000, …)',
    disposition: 'host-only',
    why: '宿主定时器；本层用虚拟时钟（步长是仿真参数，不是引擎常量）',
    anchor: '0x4bb50b',
  },
  {
    id: 'main-loop',
    what: '主循环 sub_412290',
    disposition: 'partial',
    why: '执行 / 门 / 时钟已建模；消息泵与影片位服务未建模',
    anchor: '0x4bb517',
  },
];

export interface StartupReport {
  stages: BootStage[];
  counts: Record<BootDisposition, number>;
}

export function startupReport(): StartupReport {
  const counts: Record<BootDisposition, number> = { modeled: 0, partial: 0, 'host-only': 0, 'not-modeled': 0 };
  for (const s of BOOT_STAGES) counts[s.disposition] += 1;
  return { stages: BOOT_STAGES.map((s) => ({ ...s })), counts };
}
