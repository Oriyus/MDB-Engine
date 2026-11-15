#pragma once
#include <algorithm>
#include <cstddef>
#include <thread>
#include <vector>

namespace Engine {

    // How many parts ParallelFor splits `count` items into: at least `minPerPart` items each, at most one per
    // hardware thread (1 when it isn't worth starting threads). Callers size per-part result buffers with it.
    inline size_t ParallelParts(size_t count, size_t minPerPart) {
        const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
        return std::clamp<size_t>(count / std::max<size_t>(minPerPart, 1), 1, hw);
    }

    // Runs fn(part, begin, end) over [0, count) in `parts` contiguous ranges, in order (part k covers
    // [k * count / parts, (k + 1) * count / parts)), on short-lived threads; the calling thread runs part 0 and
    // waits for the rest. For one-off heavy loops (grass scatter), not per-frame work: each call starts threads.
    // fn must not throw and must only touch shared data read-only (or its own part's slot).
    template <typename Fn>
    void ParallelFor(size_t count, size_t parts, Fn&& fn) {
        parts = std::max<size_t>(parts, 1);
        auto range = [&](size_t k) { fn(k, k * count / parts, (k + 1) * count / parts); };
        if (parts == 1) { range(0); return; }
        std::vector<std::thread> threads;
        threads.reserve(parts - 1);
        for (size_t k = 1; k < parts; k++) threads.emplace_back(range, k);
        range(0);
        for (std::thread& t : threads) t.join();
    }

} // namespace Engine
