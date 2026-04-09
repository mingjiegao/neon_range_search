#pragma once
/// types.h — 基础类型: Lsn, Key, Range
///
/// 对应 Rust 中的:
///   - utils::lsn::Lsn(u64)   → uint64_t (typedef Lsn)
///   - pageserver_api::key::Key (i128) → int64_t (简化，测试值都很小)
///   - std::ops::Range<T>      → Range<T> { start, end } 左闭右开
///
/// 简化说明: Key 用 int64_t 代替 __int128，Lsn 直接用 uint64_t。

#include <cstdint>
#include <vector>
#include <optional>

/// Lsn — Log Sequence Number, 对应 Rust: Lsn(u64)
using Lsn = uint64_t;

/// Key — 页面键，对应 Rust: Key (i128)，简化为 int64_t
using Key = int64_t;

/// Range<T> — 左闭右开区间 [start, end)
/// 对应 Rust: std::ops::Range<T>
template <typename T>
struct Range {
    T start;
    T end;

    bool is_empty() const { return start >= end; }

    bool operator==(const Range& o) const {
        return start == o.start && end == o.end;
    }
    bool operator!=(const Range& o) const { return !(*this == o); }
    bool operator<(const Range& o) const {
        if (start != o.start) return start < o.start;
        return end < o.end;
    }
};

/// KeySpaceAccum — 简化版，用 vector<Range<Key>> 收集 key 范围
/// 对应 Rust: pageserver_api::keyspace::KeySpaceAccum
struct KeySpaceAccum {
    std::vector<Range<Key>> ranges;

    void add_range(Range<Key> r) {
        if (!r.is_empty()) {
            ranges.push_back(r);
        }
    }
};
