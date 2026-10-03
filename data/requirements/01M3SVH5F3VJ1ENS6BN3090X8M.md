# windows 资源重建（res 的 rc 系列重新设计组织）

- id: REQ-01M3SVH5F3VJ1ENS6BN3090X8M
- type: req
- status: open
- parent: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- order: 25

## 范围
旧仓 `res/` 根下的 Windows 资源件 —— 它们都是**从 `AGERC.DLL` 导出、本身没有任何修改**的：
`AGERC.DLL.rc` / `AGERC_RAW.DLL.rc` · `MANIFEST2_1.txt` · `build-localized-agerc.ps1` ·
`inject-localized-agerc.rsh`（另有 `CURSOR*.cur` / `IDI_ICON1.ico` 同类：已由 M1b 入 `corpus/assets/ui-images/`）。

## 口径
**本轮只登记，不搬入**。后续按本需求单**重新设计这些文件的组织方式**（落点、构建链、与 `AGERC.DLL`
三条二进制的关系一起定），因此 **M1b 有意不迁 `res/` 的 rc 系列**。

## 出发点（参考件在旧仓只读位置）
`oldRepo:res/AGERC.DLL.rc` · `AGERC_RAW.DLL.rc` · `MANIFEST2_1.txt` · `build-localized-agerc.ps1` ·
`inject-localized-agerc.rsh`；三条 `AGERC.DLL` 二进制登记见 `corpus/assets.json` 的 `binary/agerc-*`。

## ★ 评估已完成（2026-10-03）→ `docs/01-translation/agerc-design.md`
实测 + 六个方案的评估 + 五条待做实验都在那份文档里；这里只留**会改变设计的结论**：

1. 成品是**两条链叠加**，两条都必要：① Resource Hacker 从 `.rc` 重建 DIALOG/MENU 资源；
   ② `oldRepo:scripts/patch-menu.js` 按**硬编码偏移表**原位改写（**46** 处模板文字 = 主菜单区 38 + 调试菜单区 8；
   + 3 个**代码内嵌串槽** + 2 个**导入名** `InsertMenuItemA/SetMenuItemInfoA` → `W`）。② 不可省：有些菜单项是引擎**运行时**插的，不在 MENU 资源里。
   ★ 别把三个数混了：**46** = 改了几处文字、**38** = dispatcher 的命令 ID 数（`40003–40048` 去 8 个缺号）、**16** = DIALOG 模板数（15 个有调用点，`DIALOG 5` 无）。
2. 实测（脱壳原版 → 汉化产物，**按节名**对齐）：`.text` / `.data` / `.reloc` **逐字节 0 差异**；
   `.rdata` 58 B（= 那 3 个串槽）、`.idata` 2 B（= 那 2 个导入名）—— 此前"解释不了的小差异"就是链②。
   资源侧**只有 3 条**真改（`MENU/110` · `MENU/124` · `DIALOG/3`），另 7 条 DIALOG 文字一字未改却被工具重写（每条 +2 B）。
   ⇒ **整份改动 < 2.5 KB**，其中对玩家可见的只有**主菜单栏 + 一个退出确认框**（其余 15 个 DIALOG 未翻译）。
3. 交付物有一处**没有断言的隐藏耦合**：对话框文本用**占位码位**书写（`结→俟 / 吗→龜 / 标→標`，
   取自 `data/translations/subs-cn-jp.json`）+ 模板 FONT 改成 `Amayui CN`（字形由字体 cmap 还原）+ 引擎侧按 SJIS 解。
   ⇒ 换一次字典，对话框中文会**静默**渲染错；AGE 脚本侧的 `patch.json` 有 `subsSha`，**DLL 侧什么都没有**。
4. 旧链**现在已经跑不动**：链① 的基线路径（`install\DATA1\AGERC.dll`）在本机不存在，
   链② 的偏移表也按那份文件算、而当前产物该区已与偏移不一致 ⇒ 只读前提下**不可复现**。
5. ★ **加载与基线（代码级）**：引擎 `LoadLibraryA("AGERC.DLL")` 是按**名字**搜
   ⇒ 加载的是**游戏根目录**那份；`DATA1.ALF` 内那份（`SYS4INI` id 21072，849,408 B）**不参与加载**，
   但它是**唯一可编辑的基**（根目录那份是加壳件，26 条资源的数据不在盘上）。
   ⇒ 设计上**必须**：基线取 **ALF 里那份**（**不能**照 `patch baseline` 的"散装优先"去取根目录的加壳件），
   产物写回**根目录**覆盖。
6. **资源读取全是显式带 hModule 的 Win32 API**（引擎 `LoadMenuA(<AGERC 的 HINSTANCE>,110)`；
   AGERC 自己 14×`DialogBoxParamA` + 13×`CreateDialogParamA` + `LoadMenuA(hInstance,124)`，无自研资源解析器）
   ⇒ 任何"外部资源覆盖"类方案都得让**这些既有调用点**解析到译文，且 AGERC 仍须是真 DLL。
7. ★ **效果投射媒介（实测）**：AGERC 不直接改引擎状态 —— 它拿两个宿主对象
   **`AGE:reg`**（配置存储，4 槽 `Get(int)/GetString/Set(key,int)/SetString`）与
   **`AGE:IAGEService`**（动作通道，≈60 槽被用到、**逐槽各自签名**；`+32 = void(const char*)` = "让引擎重载某支 `CALLBACK_*.BIN`"）。
   ⇒ **配置层可直接归一化入库**：一条 = `(key, kind, value)`，键是 6 个命名空间下实测抠出的 **59 个字符串键**
   （`set:`/`message:`/`sound:`/`display:`/`system:`/`debug:`）；**动作层归一不成单一 `(cmd,param)`**，得先逐槽定签名与语义（证据只有部分）。
   ⇒ 这把"重建"收窄成**两张有限表**（59 键 + ~60 槽）：瓶颈在**证据**，不在规模。

## 建议方向（二选一，见设计文档 §4/§5）
* **C 叠加层 + 纯 Node 重建**：基线取 ALF 内那份脱壳原版，
  入库真源 = 菜单文本 + `DIALOG/3` 模板 + 那 3 个串槽的新串（**存定位规则，不存偏移**）+ 2 个改名；
  判据五条见设计文档 §4（资源集合不变 · 未列出的资源逐字节相同 · `.text` 必须 0 差异 ·
  搬迁期与旧产物**资源级**对账 · **必须带 `subsSha` 指纹**）。
* **E 降级为可选附加包**：主包保持全链可复现，把"菜单栏也得是中文"这件事与主包解耦。
* ❌ **不建议**"照旧继续改"：删整类+重灌没有资源集合断言，字典耦合没有断言 ⇒ 下次改动会静默出问题。

## 判据（先做实验，再动代码）
* **先做设计文档 §6 的 E1 · E2 · E4 · E5 · E6**（**E3 已由代码级侦查回答**，只剩"改名根目录文件"这个运行时复核）。
  尤其 **E1**（`DIALOG/3` 的语言标记能否保持 `0x411` —— 若不必改语言，目录树完全不用动）
  与 **E2**（那 3 个串槽能否按内容定位而非硬编码偏移）。
* 之后：「重新设计后的组织」落进新仓 + **一条命令**从基线打出产物 + 判据全绿 + 旧仓只读边界不变。

## ★ 另有一条更大的路：**凭空重建 DLL**（设计文档 §G；**未否决**，拆到独立节点）
可行性与风险都评估过了（支持：17/21 导出无调用者、3D 单入口、122 个函数；
反对：剩下 3 个导出就是**整个设置 UI**、硬依赖 `AGE:IAGEService` 的槽语义（证据只有部分）、
**判据会从"逐字节"退化成"行为看着一样"**）。前置动作 = **探针版**。
⇒ 归新节点「AGERC.DLL 二进制重建」，本节点不再展开。
★ 取证纪律：**新仓语料是"原版"、旧仓 `engine/AGERC.DLL_utf8.*` 是"汉化产物"且已作废** —— **数字不要跨语料引用**。

## ★ 2026-10-03 定案：**本版先用「可信产物」，二进制重建另开节点**
菜单使用面小 + 16 个对话框基本没汉化（只 `DIALOG 3`）⇒ 自建链收益有限；
先把旧仓随补丁发布的那份**入库当可信产物**用起来：
`corpus/assets/agerc/AGERC.DLL`（条目 `binary/agerc-dist`，`storage: lfs`），
"可信"由 `tools/test/agerc-artifact.test.mjs` 机械复核（基准 = 清单里 `binary/agerc-modified-install` 的 sha256）。
⇒ **本节点范围收窄为「res/rc 系列的重新设计」+ 维护设计文档**；「怎么把那份二进制造出来」拆到新节点。
