// emulator-lift · D3DX9 **effect 资源读取 + FX 文本解析 + ID3DXEffect 最小 COM 面**（GPU 路线 P0/P1/P2）
//
// ★ P2（`REQ-01M7MSGRRHCA23RAX1QGDAYJ3X`）在本模块新加的东西：
//   ① **FX 源文本解析**（`parseEffectSource`）：4 份资源里出现过的语法子集（常量 / sampler_state /
//      shader 函数 / technique / pass）；pass 块里**只建模** `PixelShader` / `VertexShader` 两条
//      赋值 —— 出现任何别的状态名 ⇒ `UnsupportedState` + **具名原因**（⛔ 不静默忽略）。
//   ② **程序解析**（`effectDrawProgram`）：把"当前 technique + pass"翻成
//      `d3d9::EffectDrawProgram`（实现集内的程序 + 按声明顺序的常量表 + SetTexture 记下的纹理 EA）。
//      不在实现集里的程序（203 / 202）⇒ `implemented=false` + 具名原因 ⇒ 调用方整批拒绝（bit 44）。
//   ③ `Begin` 的 `*pPasses` 改成**解析结果**（P1 的常量 1 退役）。
//
// ★ 范围（P0 + P1 原样保留）：
//   ① **P0**：从 PE `.rsrc` 的 **RT_RCDATA** 按 id / 字符串名取 200/201/202/203 的字节
//      （`readEffectSource`）。字节真源 = **拆壳件**（AOT 臂 pin 的那份执行源
//      `corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`；S0 已证明原版 `links/raw/AGE.EXE` 的
//      `.rsrc` raw 只有 0xE00、数据 RVA 0x1AD4A0 越界 ⇒ 读不到，不是"我们的读取器不行"）。
//   ② **P1**：`D3DXCreateEffectFromResourceA`（9 参）+ `ID3DXEffect` 的 9 个取证槽，
//      **只记状态、不执行 shader、不提交 draw**。
//
// ★ 取法（S0 §4 P0 的两个选项里选 **①**：运行时只读，无构建步骤）：
//   · 输入是 `X86::image`（AOT 臂 / Machine 装配时指向**已 pin sha256 的那份 PE** 的 `PeImage`），
//     经既有的 `runtime/pe_resource.h` 解析 —— 与 `user32_resource.cpp` / `kernel32_version.cpp`
//     读同一棵树用的是**同一份**解析器（⛔ 不新写第二份 PE 解析）。
//   · 为什么是确定的（"同一份 PE ⇒ 同一份字节"）：
//     ① 读的是**宿主侧不可变的 `PeImage::bytes`**（文件字节的拷贝），不是 guest 可写内存
//        （guest 的 `.text` 可以 SMC，`.rsrc` 也没有写点 —— 但"不可写"是观测，不是机制）；
//     ② AOT 臂启动前核对 PE 的 sha256（`AGENTS.md` §12.5），不一致 `exit 2` ⇒ 跑到这里的镜像
//        就是 pin 的那一份；
//     ③ 本模块再**按字节内容的 sha256 查入表**（`effectResourcePins()`，4 条）⇒ 换了一份 PE /
//        改了资源 ⇒ 立刻 `Unrecorded` 响亮停（⛔ 不返回一个看起来成功的句柄）。
//   · 为什么不是 ②（构建期生成常量表）：那要给"生成器"加一个构建期 target + 一份新的派生源
//     （超出本轮输入边界），而 ① 的确定性由上面三条**机械**成立；且 ② 仍需要一个"资源 id →
//     字节"的运行时读取器来回答"这个 id 存不存在"（P1 的响亮停判据）。⇒ ① 更小且证据一点不少。
//   ⚠ 边界：本模块**不**从 guest 内存里重新解析 PE 头（`.rsrc` 没有映射进 guest 的语义必要性，
//     而且那会让"读到什么"取决于 guest 是否改过内存）。`X86::image` 为空 ⇒ 响亮失败。
//
// ★ `ID3DXEffect` 的槽（S0 §3.2；槽号与 `d3dx9effect.h` 的声明顺序逐位吻合，**不是猜的**）：
//     +0x08 Release · +0x78 SetFloat · +0xD0 SetTexture · +0xE8 SetTechnique · +0xFC Begin ·
//     +0x100 BeginPass · +0x104 CommitChanges · +0x108 EndPass · +0x10C End
//   ⇒ 本模块填这 9 个 + IUnknown 的 QueryInterface/AddRef/Release（`createObject` 不自动给）；
//   取证里**没读到**的 `GetDevice +0x110` / `OnLostDevice +0x114` / `OnResetDevice +0x118`
//   显式登记成 `fn=nullptr` ⇒ `comUnsupported` 的 loud trap（⛔ 不"猜着实现"）。
//   其余槽（0x0C..0x74 / 0x7C..0xCC 之间的 SetInt/SetBool/SetMatrix/Get* 等）由
//   `ComRegistry::createObject` 统一填成 `unsupported+0xNN` 的 loud trap（同一约定）。
//
// ★ 语义（**P2**：pass 真的执行）：
//   · `SetFloat` / `SetTexture` / `SetTechnique` 把"参数名 → 值"记进 guest 堆上的状态块
//     （P1 原样保留：这一层仍**不**碰 GPU、**不**提交 draw）。
//   · `Begin` 的 `*pPasses` = **FX 文本解析出来的** pass 数（P1 的常量 1 已退役）；technique 没设 /
//     名字不在文本里 ⇒ **0**（该作用域里的任何 draw 都会带具名原因被拒，⛔ 不静默接受）。
//   · `Begin` / `BeginPass` / `CommitChanges`（以及作用域内的 `SetFloat`/`SetTexture`/`SetTechnique`）
//     会把"这一批该怎么画"（`d3d9::EffectDrawProgram`：程序 + 常量表 + 纹理 EA + UV 集 + sampler 状态）
//     推给 `d3d9` 的 effect 作用域 —— `gpuEligible` 据此走后端 effect 管线（每 draw 一个 shader +
//     常量表 + 最多 2 张纹理，见 `runtime/d3d9_effect_gpu.h`）。
//   · `EndPass` / `End` 只推进状态并返回 `S_OK`；乱序只 warn。
//
// ★ 未收录 / 解析不了 ⇒ **响亮停**（ADR `REQ-01M7MNYPDXM2NEQR77DXYZM3CJ` 的第 2 条后果）：
//   导出层打印**资源 id + sha256 + 前 32 字节**，并 `cpu.running=false`（`HostStopKind::Trap`）。
//   ⛔ 不返回"看起来成功的句柄"、⛔ 不回退到软件路。
//
// ★ 状态放哪儿：与 D3DX Sprite / D3D9 同一套 —— 状态块从 **guest 堆**分配、EA 挂在对象的
//   `obj+8` 扩展位（`ComRegistry::createObject(..., user)`）⇒ 生命周期跟 refcount 走；
//   `Release` 到 0 时由 `on_zero` 钩子回收（状态块 + 参数表 + 纹理表 + 名字串）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/d3d9.h"  // `d3d9::EffectDrawProgram` / `EffectProgram`（跨层的中性名字）

namespace lift {

struct X86;
struct PeImage;
class ShimRegistry;

namespace d3dx9 {

// RT_RCDATA（winuser.h 的 `RT_RCDATA = MAKEINTRESOURCE(10)`；本线只按**数字**用）
constexpr std::uint16_t kRtRcData = 10;

// S0 §2.2 的读数：本镜像的 RT_RCDATA **恰好** 4 项，ID 与 4 个调用点一一对应。
constexpr std::uint32_t kEffectIdFadePs10 = 200;  // `compile ps_1_0`（引擎效果等级 1）
constexpr std::uint32_t kEffectIdFadePs20 = 201;  // `compile ps_2_0`（等级 2）
constexpr std::uint32_t kEffectIdSnowPs10 = 202;  // vs_1_0 + ps_1_0（Set3DEffectSnow）
constexpr std::uint32_t kEffectIdBlurPs20 = 203;  // SlideBlur / ZoomBlur（等级 >1）

// 入表的一条（**唯一一份**：实现用它做"未收录 ⇒ 响亮停"，纯用例拿它跟**文件字节**对账）。
//   `sha256` 是完整 64 位小写十六进制；`size` 是 RT_RCDATA 数据项的字节数。
struct EffectResourcePin {
    std::uint32_t id;
    std::uint32_t size;
    const char* sha256;
    const char* what;  // 内容摘要（来自 S0 §2.3 的读数，只作诊断）
};
// 4 条 + 终止项（`id == 0`）。⚠ 改动这张表 = 改"收录集"，必须同时改纯用例的期望值。
const EffectResourcePin* effectResourcePins();
// 按内容 sha256 查表（大小写不敏感）；不在表里 ⇒ nullptr。
const EffectResourcePin* effectResourcePinBySha(const std::string& sha256);

// 一次读取的结果
enum class EffectSourceStatus {
    Ok,          // 读到字节，且 sha256 命中入表
    Missing,     // 这个 id / 名字在 RT_RCDATA 里不存在（why 带解析器的原因）
    Unrecorded,  // 字节读到了，但**不在**入表里（why 带 id + sha256 + 前 32 字节）
};

struct EffectSource {
    std::uint32_t id = 0;      // 命中的数字 id（按名字取时是资源树里的 id）
    std::uint32_t size = 0;
    std::string sha256;        // 实际字节的 sha256（小写十六进制）
    std::uint8_t first32[32] = {};
    std::uint32_t first32_len = 0;
    std::vector<std::uint8_t> bytes;
    const EffectResourcePin* pin = nullptr;  // `Ok` 时非空
};

// 从 `img` 的 `.rsrc` 读 RT_RCDATA。`why` 只在这三个非 Ok 分支被写（Ok 时清空）。
//   ★ `img` 必须是**拆壳件**（原版 `links/raw/AGE.EXE` 的 `.rsrc` raw 只有 0xE00 ⇒ 这条会
//     `Missing` 并带"数据 RVA 越界"的原因 —— 那是**真话**，⛔ 不要回退去读别的东西）。
EffectSourceStatus readEffectSource(const PeImage& img, std::uint32_t id, EffectSource* out,
                                    std::string* why);
EffectSourceStatus readEffectSourceByName(const PeImage& img, const std::string& name,
                                          EffectSource* out, std::string* why);

// "id + 大小 + sha256 + 前 32 字节" 的一行文本（响亮停 / 日志共用**一份**格式）。
std::string effectSourceNote(const EffectSource& src);

// ---------------------------------------------------------------------------------------
// FX 源文本模型（P2）—— 4 份资源里出现过的语法子集，**逐条**有语义
// ---------------------------------------------------------------------------------------
// ★ 为什么要有这一层：引擎的资源是 **HLSL 源码**（S0 §2.3），不是编译后的 FX 二进制 ⇒ "这一批该
//   怎么画"必须从文本读出来（technique → pass → `compile ps_2_0 FadePS()`），⛔ 不能靠"资源 id ⇒ 程序"
//   的硬编码（那会与文本分叉且没人发现）。硬编码只允许出现在**最后一跳**（entry + profile ⇒ 后端
//   shader 程序，见下面的 `EffectProgramBinding`）。
struct FxConstant {           // uniform 常量（`float Alpha;` / `float4 Color;` / `float4x4 Proj;`）
    std::string type;
    std::string name;
};
struct FxSampler {
    std::string name;
    std::string texture;                  // `Texture = <Tex0>` ⇒ "Tex0"
    // D3D9 `D3DSAMPLERSTATETYPE` 的缺省值（D3DTEXF_POINT / D3DTADDRESS_WRAP），文本里覆盖成什么就记什么。
    std::string address_u = "Wrap";
    std::string address_v = "Wrap";
    std::string min_filter = "Point";
    std::string mag_filter = "Point";
    std::string mip_filter = "None";
};
struct FxParam {              // shader / 函数形参
    std::string type;
    std::string name;
    std::string semantic;     // 冒号后的语义（COLOR0 / TEXCOORD0…；无 ⇒ 空）
};
struct FxShader {
    std::string name;
    std::string return_type;
    std::string return_semantic;
    std::vector<FxParam> params;
    // 函数体原文（注释已剥离）。★ 只给"哪张纹理用哪套 UV"这一处用：绑定器从 `tex2D(<sampler>,
    // <uv>)` 里读第二个实参、再查它的语义（TEXCOORDn）⇒ ⛔ 不把映射硬编码进程序表。
    std::string body;
};
struct FxPass {
    std::string name;
    bool has_pixel_shader = false;
    std::string pixel_entry;
    std::string pixel_profile;   // ps_2_0 / ps_1_0
    bool has_vertex_shader = false;
    std::string vertex_entry;
    std::string vertex_profile;  // vs_1_0
};
struct FxTechnique {
    std::string name;
    std::vector<FxPass> passes;
};
struct FxModule {
    std::vector<FxConstant> constants;  // **声明顺序** = 常量表顺序
    std::vector<FxSampler> samplers;
    std::vector<FxShader> shaders;
    std::vector<FxTechnique> techniques;
    // 诊断：文本里出现过、但本模块**没有建模**的东西（非空 ⇒ 解析失败，⛔ 不静默跳过）
    std::vector<std::string> unmodeled;
};
enum class FxParseStatus {
    Ok,
    Error,             // 语法不认识（why 带行号 + 那一行的开头）
    UnsupportedState,  // pass / sampler_state 里出现**没建模**的状态名（why 点名）
};
// 解析一份 FX 源码。`why` 在非 Ok 时带**行号**与点名（响亮停用）。
FxParseStatus parseEffectSource(const std::string& text, FxModule* out, std::string* why);
// 取某个 technique 的第 `pass` 个 pass（找不到 ⇒ nullptr）。
const FxPass* fxPassAt(const FxModule& module, const std::string& technique, std::uint32_t pass);

// ---------------------------------------------------------------------------------------
// 程序解析（P2）：FX 文本 + `SetFloat`/`SetTexture` 的记录 ⇒ `d3d9::EffectDrawProgram`
// ---------------------------------------------------------------------------------------
// ★ 实现集（"entry + profile ⇒ 后端程序"）是**唯一**一张表；它只做最后一跳的映射，
//   门牌（technique / pass / 参数名 / 纹理名 / UV 语义）全部来自 `FxModule`。
struct EffectProgramBinding {
    std::uint32_t resource_id;     // 200 / 201
    const char* technique;         // "Fade"
    const char* pixel_entry;       // "FadePS"
    const char* pixel_profile;     // "ps_2_0" / "ps_1_0"
    d3d9::EffectProgram program;   // 后端要画的那一档
};
const EffectProgramBinding* effectProgramBindings();  // 以 `resource_id == 0` 的项终止

// 解析出的模块（进程内缓存：4 份资源的文本来自**不可变**的 PE 字节，按内容 sha256 缓存；
// 同一份 sha256 ⇒ 同一份模型）。`effectModuleOf` 的失败原因写 `why`。
const FxModule* effectModuleForSha(const std::string& sha256, const std::string& text, std::string* why);
const FxModule* effectModuleOf(X86& cpu, std::uint32_t state, std::string* why);

// 一次 Begin/BeginPass/CommitChanges 的"这批该怎么画"快照：程序 + 常量表 + 纹理 EA。
//   · `implemented == false` ⇒ `out->reason` 有名有姓（资源 id / technique / pass / entry / profile）；
//   · 常量按 `FxModule::constants` 的**声明顺序**取值（没 SetFloat 过 ⇒ 0，D3D 的 uniform 初值）；
//   · 纹理按 sampler → texture 参数名 → `SetTexture` 的 EA（未绑 ⇒ 0 + count 仍然算上，好让
//     调用方**具名拒绝**"shader 要两张纹理但只绑了一张"）。
bool effectDrawProgram(X86& cpu, std::uint32_t state, d3d9::EffectDrawProgram* out, std::string* why);

// ---------------------------------------------------------------------------------------
// 状态块 / 对象
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kFxMaxFloatParams = 32;  // 引擎实测只设 7 个名字（Alpha/Angle/…）
constexpr std::uint32_t kFxMaxTextures = 8;      // 实测 Tex0/Tex1

// 一次 effect 的可观察状态（夹具 / 日志用；字段全部来自 guest 状态块）
struct EffectInfo {
    std::uint32_t state = 0;        // 'FX01' 状态块 EA
    std::uint32_t device = 0;       // 建它时传入的 IDirect3DDevice9 EA（⛔ 不 AddRef）
    std::uint32_t resource_id = 0;  // RT_RCDATA 的数字 id
    std::uint32_t source_size = 0;
    std::string source_sha256;
    bool active = false;    // 在 Begin..End 之间
    bool in_pass = false;   // 在 BeginPass..EndPass 之间
    std::string technique;  // 当前 technique 名（空 = 没有 / 被清掉）
    std::uint32_t pass_index = 0;       // 最近一次 BeginPass 的索引
    std::uint32_t passes_reported = 0;  // 最近一次 Begin 写给 *pPasses 的值
    std::uint32_t set_floats = 0;
    std::uint32_t set_textures = 0;
    std::uint32_t set_techniques = 0;
    std::uint32_t begins = 0;
    std::uint32_t begin_passes = 0;
    std::uint32_t commits = 0;
    std::uint32_t end_passes = 0;
    std::uint32_t ends = 0;
    std::uint32_t float_param_count = 0;
    std::uint32_t texture_count = 0;
};

// 建 effect 状态块（guest 堆）+ 记下资源身份；返回状态块 EA（0 = 失败，why 点名）。
// ⛔ 不在这里建 COM 对象（shim 拿到 state 后调 `ComRegistry::createObject`）。
// ★ `out_of_memory`：失败原因是 guest 堆不够 ⇒ 调用方必须报"资源耗尽"，⛔ 不许记成
//   "unimplemented"（与 `d3dx9::createSpriteState` 同一口径）。
std::uint32_t createEffectState(X86& cpu, std::uint32_t device_object, const EffectSource& src,
                                std::string* why, bool* out_of_memory = nullptr);
bool effectInfoOfState(X86& cpu, std::uint32_t state, EffectInfo* out, std::string* why);
bool effectInfo(X86& cpu, std::uint32_t object, EffectInfo* out, std::string* why);

// 参数表逐条读（name 是 guest 侧名字串的宿主拷贝；index 越界 ⇒ false）。
bool effectFloatParamAt(X86& cpu, std::uint32_t state, std::uint32_t index, std::string* name,
                        float* value);
bool effectTextureAt(X86& cpu, std::uint32_t state, std::uint32_t index, std::string* name,
                     std::uint32_t* texture);

// 释放状态块 + 参数表 + 纹理表 + 名字串（Release 到 0 时由 shim / `on_zero` 调；幂等）。
void destroyEffectState(X86& cpu, std::uint32_t state);

// ---------------------------------------------------------------------------------------
// ID3DXEffect 方法（返回 HRESULT；`state` 是状态块 EA）
// ---------------------------------------------------------------------------------------
std::uint32_t effectSetFloat(X86& cpu, std::uint32_t state, const std::string& name, float value,
                       std::string* why);
std::uint32_t effectSetTexture(X86& cpu, std::uint32_t state, const std::string& name, std::uint32_t texture,
                         std::string* why);
std::uint32_t effectSetTechnique(X86& cpu, std::uint32_t state, const std::string& name, std::string* why);
// Begin：`passes_ea` = UINT*（0 = NULL 合法）；`flags` 只记录。写 `*passes_ea = 1`。
std::uint32_t effectBegin(X86& cpu, std::uint32_t state, std::uint32_t passes_ea, std::uint32_t flags,
                    std::string* why);
std::uint32_t effectBeginPass(X86& cpu, std::uint32_t state, std::uint32_t index, std::string* why);
std::uint32_t effectCommitChanges(X86& cpu, std::uint32_t state, std::string* why);
std::uint32_t effectEndPass(X86& cpu, std::uint32_t state, std::string* why);
std::uint32_t effectEnd(X86& cpu, std::uint32_t state, std::string* why);

// 注册 `d3dx9_43!D3DXCreateEffectFromResourceA`（9 参 stdcall）+ 上面那套方法表。
// ★ 由 `registerD3DX9Shims`（`shims/d3dx9.cpp`）调用 —— 一处名字只能注册一次。
void registerD3DX9EffectShims(ShimRegistry& shims);

}  // namespace d3dx9
}  // namespace lift
