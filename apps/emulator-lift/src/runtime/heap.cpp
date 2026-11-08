// emulator-lift · guest 堆实现（地址有序的块表 + 首次适配 + 合并）
//
// ★ 元数据放宿主侧（`std::map`），guest 只看到指针 —— 但**载荷本身在 guest 内存里**，
//   所以 guest 的 CRT 可以直接读写它（这也是"同一个堆"的意义，README §2.6）。
// ★ 每个块头 8 字节（size + magic），载荷 8 字节对齐。magic 用来在 free 时认出"这不是我的块"。
// ★ 野指针 / double free ⇒ 打印 + abort（⛔ 静默忽略会让内存错误跑到别处才炸）。
#include "runtime/heap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime/guest_token.h"
#include "runtime/host_encoding.h"

namespace lift {
namespace {
constexpr std::uint32_t kMagic = 0x4C494654u;  // 'LIFT'
inline std::uint32_t alignUp(std::uint32_t v, std::uint32_t a) { return (v + a - 1u) & ~(a - 1u); }
}  // namespace

bool GuestHeap::init(GuestMemory* mem, std::uint32_t begin, std::uint32_t size) {
    return init(mem, begin, size, Options{});
}

bool GuestHeap::init(GuestMemory* mem, std::uint32_t begin, std::uint32_t size, const Options& opt) {
    if (!mem || size < 0x10000u || begin > 0xFFFFFFFFu - size) {
        std::snprintf(last_error_, sizeof(last_error_), "堆初始化参数不合法（begin=%u size=%u）", begin,
                      size);
        return false;
    }
    mem_ = mem;
    begin_ = begin;
    end_ = begin + size;
    opt_ = opt;
    if (!mem_->commit(begin_, size, RegionKind::Heap, "guest-heap")) {
        std::snprintf(last_error_, sizeof(last_error_), "提交堆区间失败");
        return false;
    }
    blocks_.clear();
    free_offsets_.clear();
    pending_merge_.clear();
    Block b;
    b.off = 0;
    b.size = size;
    b.used = false;
    blocks_.emplace(0, b);
    free_offsets_.insert(0);
    in_use_ = 0;
    peak_ = 0;
    alloc_count_ = 0;
    return true;
}

void* GuestHeap::payloadOf(std::uint32_t off) const { return mem_->host(begin_ + off + opt_.header); }

bool GuestHeap::findBlock(std::uint32_t ptr, std::uint32_t* off) const {
    if (!ready() || ptr < begin_ + opt_.header || ptr >= end_) return false;
    const std::uint32_t o = ptr - begin_ - opt_.header;
    const auto it = blocks_.find(o);
    if (it == blocks_.end() || !it->second.used) return false;
    // 额外核对块头 magic：能抓住"指针指向块中间"这类错误
    if (mem_->rd32(begin_ + o) != kMagic) return false;
    *off = o;
    return true;
}

std::uint32_t GuestHeap::alloc(std::uint32_t bytes, bool zero) {
    // ★ 单令牌不变式（guest_token.h）：改 `blocks_` / `in_use_` 的线程必须持有令牌。
    //   违规 **不**在这里 abort：计数要能活着进 run summary（`--assert-guest-token` 判红），
    //   而且真实的并发损坏本来就会在后续堆操作上响亮失败。
    guestToken().check("GuestHeap::alloc");
    if (!ready()) {
        std::snprintf(last_error_, sizeof(last_error_), "堆未初始化");
        return 0;
    }
    // ★ `bytes + header` 必须用 64 位算：32 位回绕时（例如 0xFFFFFFF8 + 8）`need` 会变成 0，
    //   切出一个 size=0 的块并**返回非空指针**；带 ZERO_MEMORY 时 `b.size - header` 还会回绕成
    //   0xFFFFFFF8 ⇒ 4 GiB memset（先踩到 PROT_NONE 就是一次假 guest fault）。
    const std::uint64_t need64 = static_cast<std::uint64_t>(bytes) + opt_.header;
    if (need64 > 0xFFFFFFFFull) {
        std::snprintf(last_error_, sizeof(last_error_), "guest 堆请求过大（%u 字节 + 块头）", bytes);
        return 0;
    }
    const std::uint32_t need = alignUp(static_cast<std::uint32_t>(need64), opt_.align);
    if (static_cast<std::uint64_t>(need) < need64) {  // alignUp 自己回绕（对齐 > 1 时）
        std::snprintf(last_error_, sizeof(last_error_), "guest 堆请求对齐后溢出（%u 字节）", bytes);
        return 0;
    }
    // ★ 首次适配**只扫空闲块**（`free_offsets_` 按偏移有序 ⇒ 命中的仍是"偏移最小的那一块"，
    //   与"扫整张块表取第一个能装下的"**结果逐字相同**）。为什么必须改：实测过渡帧里
    //   7131 个块只有 15 个空闲 ⇒ 每次 alloc 平均扫 5.5K 个**已用**块（一帧 1.04e7 次节点访问）。
    for (auto fo = free_offsets_.begin(); fo != free_offsets_.end(); ++fo) {
        const std::uint32_t cand = *fo;
        auto fit = blocks_.find(cand);
        if (fit == blocks_.end()) continue;  // 索引与块表短暂不同步 ⇒ 跳过（防御，不是常态）
        Block& b = fit->second;
        ++scan_iters_;  // 可观测计数：首次适配真正比较过的**空闲**块数（供 A/B）
        if (b.used || b.size < need) continue;
        free_offsets_.erase(fo);
        if (b.size - need >= opt_.min_split) {
            Block rest;
            rest.off = b.off + need;
            rest.size = b.size - need;
            rest.used = false;
            b.size = need;
            blocks_.emplace(rest.off, rest);
            free_offsets_.insert(rest.off);
            // 切分造出来的相邻：rest 的右邻若**空闲**且紧邻，旧实现靠"下次 free 整表扫"并掉 ⇒
            // 记进 pending_merge_（⛔ 不能在这里就地并：那会让本次/后续 alloc 多出一块更大的空闲块，
            // 与旧实现的中间态不同 ⇒ 分配地址会变）。
            const auto nxt = blocks_.find(rest.off + rest.size);
            if (nxt != blocks_.end() && !nxt->second.used && nxt->second.off == rest.off + rest.size)
                pending_merge_.push_back(rest.off);
        }
        b.used = true;
        b.req = bytes;
        mem_->wr32(begin_ + b.off, kMagic);
        mem_->wr32(begin_ + b.off + 4, bytes);
        if (zero) std::memset(payloadOf(b.off), 0, b.size - opt_.header);
        if (zero) zero_bytes_ += b.size - opt_.header;  // 可观测计数（清零字节，供 A/B）
        in_use_ += b.size;
        if (in_use_ > peak_) peak_ = in_use_;
        ++alloc_count_;
        return begin_ + b.off + opt_.header;
    }
    // ★ 失败点必须能区分"总量满"与"碎片化"：首次适配只认**一块连续**空间，
    //   所以把「最大空闲块」与「请求量（含块头对齐）」并排打出来（`in_use/peak` 说明总量）。
    std::snprintf(last_error_, sizeof(last_error_),
                  "guest 堆耗尽（请求 %u 字节，含块头对齐需 %u；最大空闲块 %u 字节；in_use=%u peak=%u；"
                  "区间 0x%08X..0x%08X）",
                  bytes, need, largestFreeBlock(), in_use_, peak_, begin_, end_);
    return 0;
}

bool GuestHeap::free(std::uint32_t ptr) {
    guestToken().check("GuestHeap::free");
    std::uint32_t off = 0;
    if (!findBlock(ptr, &off)) {
        std::fprintf(stderr,
                     "\n=== guest heap: 非法 free ===\n  ptr = 0x%08X（不在堆区间内 / 不是块首 / magic 不对 / 已释放）\n"
                     "  堆区间 = 0x%08X..0x%08X  in_use=%u peak=%u\n",
                     ptr, begin_, end_, in_use_, peak_);
        std::fflush(nullptr);
        restoreConsoleUtf8();  // _Exit / abort 不跑析构：控制台码页必须显式恢复
        std::abort();
    }
    Block& b = blocks_[off];
    b.used = false;
    b.req = 0;
    in_use_ -= b.size;
    free_offsets_.insert(off);
    mem_->wr32(begin_ + off, 0);
    mem_->wr32(begin_ + off + 4, 0);
    coalesce(off);
    return true;
}

std::uint32_t GuestHeap::realloc(std::uint32_t ptr, std::uint32_t bytes, bool zero) {
    guestToken().check("GuestHeap::realloc");
    if (ptr == 0) return alloc(bytes, zero);
    std::uint32_t off = 0;
    if (!findBlock(ptr, &off)) {
        std::fprintf(stderr, "\n=== guest heap: 非法 realloc ===\n  ptr = 0x%08X\n", ptr);
        std::fflush(nullptr);
        restoreConsoleUtf8();
        std::abort();
    }
    Block& b = blocks_[off];
    // 与 alloc 同一条溢出检查：`bytes + header` 回绕会算出 need=0 ⇒ 误判"现有块够大"，
    // 随后的 ZERO_MEMORY 清零按 `bytes - old_req` 一路写成 4 GiB。
    const std::uint64_t need64 = static_cast<std::uint64_t>(bytes) + opt_.header;
    if (need64 > 0xFFFFFFFFull) {
        std::snprintf(last_error_, sizeof(last_error_), "guest 堆 realloc 请求过大（%u 字节）", bytes);
        return 0;
    }
    const std::uint32_t need = alignUp(static_cast<std::uint32_t>(need64), opt_.align);
    if (static_cast<std::uint64_t>(need) < need64) {
        std::snprintf(last_error_, sizeof(last_error_), "guest 堆 realloc 对齐后溢出（%u 字节）", bytes);
        return 0;
    }
    if (need <= b.size) {
        const std::uint32_t old_req = b.req;
        if (zero && bytes > old_req) std::memset(mem_->host(begin_ + off + opt_.header + old_req), 0, bytes - old_req);
        b.req = bytes;
        mem_->wr32(begin_ + off + 4, bytes);
        return ptr;
    }
    const std::uint32_t np = alloc(bytes, zero);
    if (!np) return 0;
    std::memcpy(mem_->host(np), mem_->host(ptr), b.req);
    free(ptr);
    return np;
}

std::uint32_t GuestHeap::sizeOf(std::uint32_t ptr) const {
    std::uint32_t off = 0;
    if (!findBlock(ptr, &off)) return 0xFFFFFFFFu;
    return blocks_.at(off).req;
}

bool GuestHeap::owns(std::uint32_t ptr) const {
    std::uint32_t off = 0;
    return findBlock(ptr, &off);
}

std::uint32_t GuestHeap::largestFreeBlock() const {
    std::uint32_t best = 0;
    for (const auto& kv : blocks_) {
        if (kv.second.used) continue;
        if (kv.second.size > best) best = kv.second.size;
    }
    return best;
}

// ★ 收口只碰**可能相邻**的那几块，不再扫整张块表（旧实现每次 free 扫 7131 个节点 ⇒
//   过渡帧里 1.01e7 次访问）。等价性靠两件事保住：
//   ① 从被释放块出发**向左走到这一串的最左块**，再向右逐块并 —— 得到的是同一段"极大连续空闲"；
//   ② alloc 切分留下的 rest 可能**恰好贴在右邻空闲块上**（这种相邻只有切分能造出来），旧实现靠
//      "下次 free 时整表扫"把它并掉 ⇒ 这里把它记进 `pending_merge_`，在下次 free 收口时一起处理。
//   ⇒ 每次操作结束后块表**与旧实现逐字相同**（同一组块、同一组偏移、同一组大小）。
void GuestHeap::coalesce(std::uint32_t off) {
    coalesceAround(off);
    // 先把候选拷出来：合并过程会改 pending_merge_（不，这里它只被清空）/ blocks_。
    const std::vector<std::uint32_t> pending(pending_merge_.begin(), pending_merge_.end());
    pending_merge_.clear();
    for (const std::uint32_t o : pending) coalesceAround(o);
}

void GuestHeap::coalesceAround(std::uint32_t off) {
    auto it = blocks_.find(off);
    if (it == blocks_.end() || it->second.used) return;
    // ① 走到这一串的最左块（不要改 key：map 的 key 必须等于 Block::off）
    for (;;) {
        if (it == blocks_.begin()) break;
        auto p = std::prev(it);
        if (p->second.used || p->second.off + p->second.size != it->second.off) break;
        it = p;
    }
    // ② 从最左块向右吞掉所有"空闲且紧邻"的块
    for (;;) {
        auto n = std::next(it);
        if (n == blocks_.end()) break;
        if (n->second.used || it->second.off + it->second.size != n->second.off) break;
        free_offsets_.erase(n->second.off);
        it->second.size += n->second.size;
        blocks_.erase(n);
        ++coalesce_iters_;  // 可观测计数：合并掉的相邻空闲块数（供 A/B）
    }
}

void GuestHeap::dumpLeak(std::uint32_t ptr, const char* api) const {
    std::fprintf(stderr, "  [heap] %s: ptr=0x%08X 不是活动块（in_use=%u）\n", api, ptr, in_use_);
}

}  // namespace lift
