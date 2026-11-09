// emulator-lift · 纯档夹具 harness（自造机器码字节，不碰游戏 / LFS）
//
// ★ 判据 A1 的形状：每条语义夹具都必须是 `setup`（放代码 / 置初值）+ `verify`（断言结果）
//   两段。runner 在 setup 之后、run 之前拍一份**全状态快照**，跑完后要求：
//     ① verify 里至少做过一次断言（否则"什么都没验"）；
//     ② 状态与快照**不同**（至少一个寄存器 / 标志 / x87 / 内存槽变了）——
//        这条是机械的：恒等夹具（expected 抄成 actual、或者根本没执行）会当场红。
// ★ 夹具的机器码是**我们自己构造的字节**，与 PE / `.lst` 无关 ⇒ `@env pure`。
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/machine.h"

namespace lift_test {

// 夹具里最常用的两个地址（与 GuestLayout 对齐：代码区落在镜像附近，栈在 0x70000000）
constexpr std::uint32_t kCodeEa = 0x00401000u;
constexpr std::uint32_t kDataEa = 0x00480000u;
constexpr std::uint32_t kSentinelEa = 0xFFFFFFFFu;  // 返回地址岗哨：跑到它就停

class Fixture {
public:
    bool init();
    bool reset();

    lift::Machine& m() { return *machine_; }
    lift::GuestMemory& mem() { return machine_->mem(); }
    lift::X86& cpu() { return machine_->cpu(); }

    // 放代码（十六进制字符串，允许空格；也可用 bytes 版本）
    bool code(std::uint32_t ea, const std::vector<std::uint8_t>& bytes);
    bool codeHex(std::uint32_t ea, const char* hex);
    bool dataHex(std::uint32_t ea, const char* hex);

    // 执行到岗哨（`ret` 回到 kSentinelEa）或 max_insns；返回是否正常停下
    bool run(std::uint64_t max_insns = 200000);

    // 全状态快照（寄存器 + 标志 + x87 元信息 + XMM / MXCSR + 代码区/栈区内存摘要）
    // ★ `eip` 虽然被记下来，但**不参与**恒等比较（run() 的终点永远是哨兵，见 conformance.cpp）。
    struct Snapshot {
        std::uint32_t r[8] = {};
        std::uint32_t eip = 0, eflags = 0;
        std::uint32_t mxcsr = 0;   // ★ SSE 的 IE 位（`comiss` 与 `ucomiss` 的 QNaN 差别落在这里）
        std::uint64_t memHash = 0;
        std::uint64_t x87Hash = 0;
        std::uint64_t xmmHash = 0;  // ★ 只改 XMM 的用例必须能被看见（否则恒等夹具会误判成"通过"）
        bool operator==(const Snapshot& o) const;
        bool operator!=(const Snapshot& o) const { return !(*this == o); }
    };
    Snapshot snapshot() const;

private:
    std::unique_ptr<lift::Machine> machine_;
    std::string err_;
};

struct Case {
    const char* name;
    void (*setup)(Fixture&);
    void (*verify)(Fixture&);
};

// 跑一组用例；返回失败数
int runCases(const Case* cases, std::size_t count);

// 由 conformance_cases.cpp 提供
const Case* conformanceCases(std::size_t* count);

}  // namespace lift_test
