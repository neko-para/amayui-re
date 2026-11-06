# emulator-lift · docs 索引

本目录放**设计与分析长文**：口径的展开理由、决策与反面对价、切片的判据。
**不放进度、不放完成度、不放计数快照**（`AGENTS.md` §10）。

| 想知道 | 看哪 |
|---|---|
| 这个子项目是什么 / 六个已定决策 / 落点 | [`../README.md`](../README.md) |
| **与 Wine 的三条路线分野**（"还是这条线"的判据 / 为什么不能拿 Wine 的 DLL 当 shim） | 同文件 **§1.1** |
| 用户拍板的可接受偏差（菜单 / 模态 / 音频 / 视频 / 3D） | [`../SCOPE.md`](../SCOPE.md) |
| **M1 = guest CPU 核心 + §3.1 共享底座** 的完整计划与判据 | [`plan-m1-cpu-base.md`](plan-m1-cpu-base.md) |
| **M2 = 2D 画面 + 文本（窗口/消息 · DirectDraw · GDI 字体·停手点 A）** 的计划、切片与 go/no-go | [`plan-m2-2d-text.md`](plan-m2-2d-text.md) |
| **音频可行性**（DirectSound 消费端 / 伪线程 / 最小可听路径 / 需裁决项） | [`audio-feasibility.md`](audio-feasibility.md) |
| **真线程可行性**（真线程 / 单令牌 / 完全 native override 喂料线程 / arm64 内存序） | [`thread-model-feasibility.md`](thread-model-feasibility.md) |
| **AGERC.DLL 的取证**（ALF 条目 / 安装树根件 / 正版加壳件；各自 sha256 / 节表 / 资源映射失败数 / 版本；引擎按名字加载的 `.lst` 证据）**与装载口径**（只有 `--game-dir/AGERC.DLL` 一条来源，`--agerc` 可显式覆盖；ALF 条目只作取证对照，⛔ 不是装载来源） | [`agerc-image.md`](agerc-image.md) |
| **S4 的形态 → 状态事实表**（x87 / MMX / SSE 每条形态：语料条数 / 是否实现 / 哪条守卫 / 缺口去了哪） | [`s4-fpu-audit.md`](s4-fpu-audit.md) |
| 平台面十簇的复杂度 / 工程量 / 切分顺序（上游分析） | [`../../../docs/03-emulator/platform-effort.md`](../../../docs/03-emulator/platform-effort.md) |
| 还要做什么、到哪一步了 | `pnpm tools requirements plan` |

## 写作纪律

1. **只写不变的东西**：为什么这么切、判据是什么、边界在哪。判据一变就重写，不用"修订记录"。
2. **每条结论说清"怎么重算"**：数字要么能现查，要么不写。
3. **与 `../README.md` / `../SCOPE.md` 冲突时以它们为先**：本目录负责展开理由，⛔ 不另立口径；
   要改口径就改那两份，并在这里留下"为什么"。
4. 引擎语义结论**不进这里**（那走 `AGENTS.md` §6 准入门，落 `data/spec/`）。
