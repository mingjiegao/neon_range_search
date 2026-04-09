#pragma once
/// layer_coverage.h — LayerCoverage + LayerCoverageTuple
///
/// 对应 Rust 文件: pageserver/src/tenant/layer_map/layer_coverage.rs
///
/// 核心思想:
///   用一棵有序映射 (Rust 中用 rpds::RedBlackTreeMapSync<i128, Option<(u64, Value)>>)
///   来记录 key 维度上的「覆盖变化」。
///
///   每个节点的 key 表示一个变化点: 从这个 key 开始，覆盖情况变成了节点存的值。
///   - Some((lsn_end, value)): 从此 key 开始，被 value 层覆盖，该层 lsn.end = lsn_end
///   - None: 从此 key 开始，没有层覆盖
///
///   这类似于一维的「扫描线」数据结构。
///
/// C++ 中用 std::map<Key, std::optional<std::pair<Lsn, Value>>> 代替持久化红黑树。
/// clone() 做深拷贝（测试规模小，不影响正确性）。
///
/// 对应 Rust 原文注释:
///   "For every change in coverage (as we sweep the key space) we store (lsn.end, value)."

#include "types.h"
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include <iterator>

/// LayerCoverage<Value>
/// 对应 Rust: layer_coverage::LayerCoverage<Value>
///
/// 功能：
///   1. insert(key_range, lsn_range, value) — 插入一个层
///   2. query(key) — 查询 key 处最高（lsn.end 最大）的层
///   3. range(key_range) — 迭代 key 范围内的覆盖变化
///   4. range_overlaps(key_range) — 返回与范围相交的所有变化（含首元素修正）
template <typename Value>
class LayerCoverage {
public:
    /// 节点类型: key → optional<(lsn_end, value)>
    /// None 表示此处无覆盖，Some 表示被 value 覆盖且 lsn.end = lsn_end
    using NodeValue = std::optional<std::pair<Lsn, Value>>;
    using MapType = std::map<Key, NodeValue>;

    /// 对应 Rust: LayerCoverage::new()
    LayerCoverage() = default;

    /// 对应 Rust: LayerCoverage::add_node(key)
    ///
    /// "Helper function to subdivide the key range without changing any values"
    /// "This operation has no semantic effect by itself. It only helps us pin in
    ///  place the part of the coverage we don't want to change when inserting."
    ///
    /// 在 key 处插入一个节点，值继承自前一个节点（即当前覆盖情况）。
    /// 这样后续修改不会影响到 key 之外的区域。
    void add_node(Key key);

    /// 对应 Rust: LayerCoverage::insert(key_range, lsn_range, value)
    ///
    /// 插入一个层，只在需要的地方提升覆盖高度（lsn.end）。
    /// 算法:
    ///   1. 先在 key_range.start 和 key_range.end 处 add_node 锚定边界
    ///   2. 遍历 [start, end) 内的节点，如果当前覆盖的 lsn.end < 新层的 lsn.end,
    ///      则更新为新层。相邻的相同更新可以合并（移除中间节点）。
    void insert(Range<Key> key_range, Range<Lsn> lsn_range, Value value);

    /// 对应 Rust: LayerCoverage::query(key)
    ///
    /// "Get the latest (by lsn.end) layer at a given key"
    /// 找 key 处的覆盖: 查找 <= key 的最后一个节点。
    std::optional<Value> query(Key key) const;

    /// 对应 Rust: LayerCoverage::range(key_range)
    ///
    /// "Iterate the changes in layer coverage in a given range"
    /// 返回 [key_range.start, key_range.end) 内的所有变化点。
    std::vector<std::pair<Key, std::optional<Value>>> range(Range<Key> key_range) const;

    /// 对应 Rust: LayerCoverage::range_overlaps(key_range)
    ///
    /// "Returns an iterator which includes all coverage changes for layers
    ///  that intersect with the provided range."
    ///
    /// 与 range() 的区别:
    ///   如果 key_range.start 已经被某层覆盖（即 query(start) 有值），
    ///   则结果的第一个元素是 (start, Some(该层))，这样调用者能看到
    ///   从 start 开始就有覆盖。
    ///   如果 range() 的第一个元素恰好和这个初始覆盖相同，则跳过以避免重复。
    std::vector<std::pair<Key, std::optional<Value>>> range_overlaps(const Range<Key>& key_range) const;

    /// 对应 Rust: LayerCoverage::clone() — "O(1) clone"
    /// C++ 中做深拷贝（std::map 没有持久化性质）
    LayerCoverage clone() const {
        LayerCoverage copy;
        copy.nodes_ = nodes_;
        return copy;
    }

    /// 方便测试
    const MapType& nodes() const { return nodes_; }

private:
    MapType nodes_;
};

/// LayerCoverageTuple<Value>
/// 对应 Rust: layer_coverage::LayerCoverageTuple<Value>
///
/// "Image and delta coverage at a specific LSN."
/// 一个 LSN 版本下的 delta + image 两棵覆盖树。
template <typename Value>
struct LayerCoverageTuple {
    LayerCoverage<Value> image_coverage;
    LayerCoverage<Value> delta_coverage;

    LayerCoverageTuple clone() const {
        LayerCoverageTuple copy;
        copy.image_coverage = image_coverage.clone();
        copy.delta_coverage = delta_coverage.clone();
        return copy;
    }
};
