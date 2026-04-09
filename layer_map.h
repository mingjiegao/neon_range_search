#pragma once
/// layer_map.h — SearchResult, RangeSearchResult, RangeSearchCollector, select_layer, range_search
///
/// 对应 Rust 文件: pageserver/src/tenant/layer_map.rs
///
/// 这是整个搜索机制的核心:
///   range_search(key_range, end_lsn) 在层映射中搜索覆盖给定 key 范围的所有层。
///
/// 算法概览:
///   1. 找到 end_lsn 对应的历史版本（LayerCoverageTuple）
///   2. 获取 delta 和 image 覆盖在 key_range 内的变化序列（range_overlaps）
///   3. RangeSearchCollector 用双指针算法遍历这两个变化序列:
///      - 从左到右扫描 key 范围
///      - 在每个变化点，用 select_layer 从候选层中选最优层
///      - 将 key 子范围分配给选中的层
///   4. 没有持久化层覆盖的区域，fallback 到内存层

#include "types.h"
#include "layer_desc.h"
#include "layer_coverage.h"
#include "historic_layer_coverage.h"
#include <memory>
#include <map>
#include <optional>
#include <vector>

// ============================================================
// SearchResult — 搜索结果
// ============================================================

/// 对应 Rust: layer_map::ReadableLayerWeak
/// 简化为枚举 + 数据
enum class LayerType {
    PersistentLayer,
    InMemoryLayer,
};

/// 对应 Rust: layer_map::SearchResult
///
/// Rust 原文:
///   pub struct SearchResult {
///       pub layer: ReadableLayerWeak,
///       pub lsn_floor: Lsn,
///   }
///
/// lsn_floor 是下一次搜索的 end_lsn 上界:
///   当返回 delta 层时，lsn_floor 告诉调用者应该从哪个 LSN 继续往下搜索。
///   如果 delta 层下面有 image 层，lsn_floor 会设在 image 层上方，以免跳过它。
struct SearchResult {
    LayerType layer_type;
    std::shared_ptr<PersistentLayerDesc> persistent_layer;  // if PersistentLayer
    std::optional<InMemoryLayerDesc> in_memory_layer;       // if InMemoryLayer
    Lsn lsn_floor;

    bool operator==(const SearchResult& o) const;
    bool operator<(const SearchResult& o) const;
};

// ============================================================
// RangeSearchResult — 范围搜索结果
// ============================================================

/// 对应 Rust: layer_map::RangeSearchResult
///
/// Rust 原文:
///   "Contains a mapping from a layer description to a keyspace
///    accumulator that contains all the keys which intersect the layer
///    from the original search space."
struct RangeSearchResult {
    std::map<SearchResult, KeySpaceAccum> found;

    RangeSearchResult() = default;

    /// 对应 Rust: RangeSearchResult::map_to_in_memory_layer
    static RangeSearchResult map_to_in_memory_layer(
        std::optional<InMemoryLayerDesc> in_memory_layer,
        Range<Key> range);
};

// ============================================================
// NextLayerType — 下一个变化点类型
// ============================================================

/// 对应 Rust: layer_map::NextLayerType
///
/// 在双指针扫描中，标记下一个变化点来自 delta、image 还是两者同时
enum class NextLayerKind { Delta, Image, Both };

struct NextLayerType {
    NextLayerKind kind;
    Key at;  // 变化发生的 key

    /// 对应 Rust: NextLayerType::next_change_at_key()
    Key next_change_at_key() const { return at; }
};

// ============================================================
// PeekableIter — 支持 peek 的迭代器
// ============================================================

/// 对应 Rust: std::iter::Peekable<Iter>
/// C++ 中用 vector + index 实现
template <typename T>
class PeekableIter {
public:
    PeekableIter() : pos_(0) {}
    explicit PeekableIter(std::vector<T> data) : data_(std::move(data)), pos_(0) {}

    /// 对应 Rust: Peekable::peek()
    const T* peek() const {
        if (pos_ < data_.size()) return &data_[pos_];
        return nullptr;
    }

    /// 对应 Rust: Iterator::next()
    T next() {
        return data_[pos_++];
    }

    bool has_next() const { return pos_ < data_.size(); }

private:
    std::vector<T> data_;
    size_t pos_;
};

// ============================================================
// 核心函数声明
// ============================================================

/// 对应 Rust: LayerMap::select_layer (静态方法)
///
/// Rust 原文:
///   "Select a layer from three potential candidates (in-memory, delta and image layer).
///    The candidates represent the first layer of each type which intersect a key range."
///   "Layer types have an implicit priority (image > delta > in-memory)."
///
/// 8 Cases (3 bool 变量: delta存在?, image存在?, inmem存在?):
///   Case 1: (None, None, None) → None
///   Case 2: (None, Some(image), None) → image
///   Case 3: (Some(delta), None, None) → delta
///   Case 4: (Some(delta), Some(image), None) → 比较谁更新，image 优先
///   Case 5: (None, None, Some(inmem)) → inmem
///   Case 6: (None, Some(image), Some(inmem)) → 比较，image 优先
///   Case 7: (Some(delta), None, Some(inmem)) → 比较，delta 优先
///   Case 8: (Some(delta), Some(image), Some(inmem)) → 先选最优持久化层，再和 inmem 比
std::optional<SearchResult> select_layer(
    std::shared_ptr<PersistentLayerDesc> delta_layer,
    std::shared_ptr<PersistentLayerDesc> image_layer,
    std::optional<InMemoryLayerDesc> in_memory_layer,
    Lsn end_lsn);

/// 对应 Rust: LayerMap::range_search
///
/// 入口函数: 在 HistoricLayerCoverage 中搜索覆盖 key_range 的所有层。
///
/// 参数:
///   - historic: 历史层覆盖数据
///   - in_memory_layer: 可选的内存层候选
///   - key_range: 要搜索的 key 范围
///   - end_lsn: 搜索的 LSN 上界（不包含）
RangeSearchResult range_search(
    const HistoricLayerCoverage<std::shared_ptr<PersistentLayerDesc>>& historic,
    std::optional<InMemoryLayerDesc> in_memory_layer,
    Range<Key> key_range,
    Lsn end_lsn);
