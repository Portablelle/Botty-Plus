"""Compile the real bitmap traversal headers with minimal shims; run only on dedie.

Ports the lifecycle/order checks from Djihads80/Eglantine 0028592 without Catch2.
Also runs the scheduler cases with the previous linear scans as an order baseline.
"""
from pathlib import Path
import platform
import subprocess
import tempfile
import sys

assert platform.system() == 'Linux', 'Compile on dedie'
root = Path(sys.argv[1]).resolve()
source = Path((root / '.local/headless-cache').read_text().strip()) / 'source/src'

# Keep the real queue implementation, substituting only the pre-port bitmap scans.
legacy = (source / 'core/hle/kernel/k_priority_queue.h').read_text()
def replace_scan(signature, replacement):
    global legacy
    start = legacy.index(signature, legacy.index('class KPriorityQueueImpl'))
    end = legacy.index('{', start) + 1
    depth = 1
    while depth:
        depth += (legacy[end] == '{') - (legacy[end] == '}')
        end += 1
    legacy = legacy[:start] + replacement + legacy[end:]

replace_scan('constexpr Member* GetFront(s32 core) const', r'''
constexpr Member* GetFront(s32 core) const {
    ASSERT(IsValidCore(core));
    const s32 priority = s32([](auto const& e) {
        for (size_t i = 0; i < e.size(); ++i)
            if (e[i])
                return i;
        return e.size();
    }(m_available_priorities[core]));
    return priority <= LowestPriority ? m_queues[priority].GetFront(core) : nullptr;
}''')
replace_scan('template<size_t N>', r'''
template<size_t N>
constexpr size_t GetNextSet(std::bitset<N> const& bit, size_t n) const {
    for (size_t i = n + 1; i < bit.size(); i++)
        if (bit[i])
            return i;
    return bit.size();
}''')
legacy = legacy.replace('namespace Kernel {', 'namespace LegacyKernel {', 1)

cpp = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <vector>
#include "common/slot_vector.h"
#include "core/hle/kernel/k_priority_queue.h"
#include "legacy_priority_queue.h"

void CheckSlots() {
    Common::SlotVector<u32> slots;
    assert(slots.begin() == slots.end());
    std::vector<Common::SlotId> ids;
    for (u32 i = 0; i < 513; ++i) ids.push_back(slots.insert(i));
    // Capture a valid iterator before clearing the leading words. Comparing it
    // avoids dereferencing the invalid ID produced by begin() on main.
    auto expected_first = slots.begin();
    while ((*expected_first).first != ids[512]) ++expected_first;
    assert(ids[512].Value() >= 512);
    for (u32 i = 0; i < 512; ++i) slots.erase(ids[i]);
    auto it = slots.begin();
    assert(it == expected_first && "first live slot after empty bitmap words");
    assert((*it).first == ids[512] && *(*it).second == 512);
    assert(++it == slots.end());
    slots.erase(ids[512]);
    assert(slots.begin() == slots.end());

    // Cross empty words, adjacent word boundaries, and the highest bit in a word.
    std::map<u32, u32> live;
    ids.clear();
    for (u32 i = 0; i < 1024; ++i) ids.push_back(slots.insert(i));
    for (u32 i = 0; i < ids.size(); ++i) {
        const u32 index = ids[i].Value();
        if (index == 0 || index == 63 || index == 64 || index == 127 ||
            index == 511 || index == 512 || index == 1023) live.emplace(index, i);
        else slots.erase(ids[i]);
    }
    auto check_live = [&] {
        auto expected = live.begin();
        for (const auto [id, value] : slots) {
            assert(expected != live.end());
            assert(id.Value() == expected->first && *value == expected->second);
            ++expected;
        }
        assert(expected == live.end());
    };
    check_live();
    for (const auto& [id, value] : live) slots.erase(Common::SlotId{id});
    live.clear();
    check_live();
    std::mt19937 random{9813};
    for (u32 batch = 0; batch < 200; ++batch) {
        for (u32 i = 0; i < 100; ++i) {
            if (!live.empty() && random() % 3 == 0) {
                auto selected = live.begin();
                std::advance(selected, random() % live.size());
                slots.erase(Common::SlotId{selected->first});
                live.erase(selected);
            } else {
                const u32 value = random();
                const auto id = slots.insert(value);
                assert(live.emplace(id.Value(), value).second);
            }
        }
        check_live();
    }
}

struct Affinity {
    u64 GetAffinityMask() const { return 3; }
    void SetAffinityMask(u64) {}
    bool GetAffinity(s32) const { return true; }
    void SetAffinity(s32, bool) {}
    void SetAll() {}
};
struct Member {
    struct QueueEntry {
        Member* prev{};
        Member* next{};
        void Initialize() { prev = next = nullptr; }
        void SetPrev(Member* value) { prev = value; }
        void SetNext(Member* value) { next = value; }
        Member* GetNext() const { return next; }
        Member* GetPrev() const { return prev; }
    };
    std::array<QueueEntry, 2> entries{};
    s32 priority{};
    QueueEntry& GetPriorityQueueEntry(s32 core) { return entries[core]; }
    const QueueEntry& GetPriorityQueueEntry(s32 core) const { return entries[core]; }
    Affinity GetAffinityMask() const { return {}; }
    s32 GetActiveCore() const { return 0; }
    s32 GetPriority() const { return priority; }
    bool IsDummyThread() const { return false; }
};

template <size_t Priorities, class Queue>
void CheckQueue() {
    typename Queue::KPriorityQueueImpl queue;
    std::array<Member, 160> members{};
    std::array<std::array<bool, 2>, 160> present{};
    std::array<std::array<std::vector<Member*>, Priorities>, 2> model{};
    std::mt19937 random{6152};
    for (size_t i = 0; i < members.size(); ++i)
        members[i].priority = static_cast<s32>(i % Priorities);
    for (s32 core = 0; core < 2; ++core) assert(queue.GetFront(core) == nullptr);
    // Empty masks, start sentinel, last bit and exhaustion; includes >64 fallback.
    for (size_t bit = 0; bit < Priorities; ++bit) {
        std::bitset<Priorities> mask;
        assert(queue.GetNextSet(mask, size_t(-1)) == Priorities);
        mask.set(bit);
        assert(queue.GetNextSet(mask, size_t(-1)) == bit);
        for (size_t after = 0; after <= Priorities; ++after)
            assert(queue.GetNextSet(mask, after) == (after < bit ? bit : Priorities));
    }
    auto check_order = [&] {
        for (s32 core = 0; core < 2; ++core) {
            Member* actual = queue.GetFront(core);
            for (s32 priority = 0; priority < static_cast<s32>(Priorities); ++priority) {
                const auto& expected = model[core][priority];
                assert(queue.GetFront(priority, core) ==
                       (expected.empty() ? nullptr : expected.front()));
                for (Member* wanted : expected) {
                    assert(actual == wanted && "scheduler priority and FIFO order unchanged");
                    actual = queue.GetNext(core, actual);
                }
            }
            assert(actual == nullptr);
        }
    };
    for (u32 operation = 0; operation < 3000; ++operation) {
        const size_t index = random() % members.size();
        const s32 core = random() % 2;
        auto& member = members[index];
        auto& expected = model[core][member.priority];
        if (present[index][core]) {
            queue.Remove(member.priority, core, &member);
            std::erase(expected, &member);
        } else if ((random() & 1) != 0) {
            queue.PushFront(member.priority, core, &member);
            expected.insert(expected.begin(), &member);
        } else {
            queue.PushBack(member.priority, core, &member);
            expected.push_back(&member);
        }
        present[index][core] = !present[index][core];
        check_order();
    }
    // Drain all priorities and both cores, including the last remaining bit.
    for (size_t i = 0; i < members.size(); ++i) for (s32 core = 0; core < 2; ++core) {
        if (!present[i][core]) continue;
        auto& member = members[i];
        queue.Remove(member.priority, core, &member);
        std::erase(model[core][member.priority], &member);
        check_order();
    }
}
int main() {
    CheckSlots();
    CheckQueue<64, Kernel::KPriorityQueue<Member, 2, 63, 0>>();
    CheckQueue<128, Kernel::KPriorityQueue<Member, 2, 127, 0>>();
    // Same seed and exact pointer-order assertions with the pre-port scans.
    CheckQueue<64, LegacyKernel::KPriorityQueue<Member, 2, 63, 0>>();
    CheckQueue<128, LegacyKernel::KPriorityQueue<Member, 2, 127, 0>>();
    std::cout << "SKIP_EMPTY_BITMAP_WORDS_AND_SCHEDULER_ORDER_PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='skip-empty-bitmap-words-') as td:
    temp = Path(td)
    common = temp / 'common'
    common.mkdir()
    (common / 'common_types.h').write_text(
        '#pragma once\n#include <cstdint>\n'
        'using u32 = std::uint32_t; using u64 = std::uint64_t; using s32 = std::int32_t;\n')
    (common / 'assert.h').write_text(
        '#pragma once\n#include <cassert>\n#define ASSERT(x) assert(x)\n'
        '#define DEBUG_ASSERT(x) assert(x)\n')
    (temp / 'legacy_priority_queue.h').write_text(legacy)
    (temp / 'check.cpp').write_text(cpp)
    subprocess.run(['clang++-18', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I' + str(temp), '-I' + str(source),
                    str(temp / 'check.cpp'), '-o', str(temp / 'check')], check=True)
    subprocess.run([str(temp / 'check')], check=True)
