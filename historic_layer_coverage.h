#pragma once
/// historic_layer_coverage.h — LayerKey + HistoricLayerCoverage
///
/// 对应 Rust 文件: pageserver/src/tenant/layer_map/historic_layer_coverage.rs
///
/// 核心思想:
///   LayerCoverage 只处理 key 维度的覆盖。
///   HistoricLayerCoverage 在此基础上引入 LSN 维度:
///     每次插入一个层（按 lsn.start 递增顺序），都记录一个「快照」，
///     这样就能查询任意历史 LSN 时刻的覆盖状态。
///
///   在 Rust 中，快照是 O(1) 的（因为 rpds 持久化树 clone 是 O(1)）。
///   在 C++ 中，我们用 std::map 深拷贝（测试规模小，OK）。

#include "types.h"
#include "layer_coverage.h"
#include <map>

/// LayerKey — 层的索引键
/// 对应 Rust: historic_layer_coverage::LayerKey
///
/// Rust 原文:
///   "Layers in this module are identified and indexed by this data."
///   "This is a helper struct to enable sorting layers by lsn.start."
///
/// 排序规则: lsn.start → lsn.end → key.start → key.end → is_image
struct LayerKey {
    Range<Key> key;
    Range<Lsn> lsn;
    bool is_image;

    /// 对应 Rust: impl Ord for LayerKey
    ///
    /// Rust 原文:
    ///   "NOTE we really care about comparing by lsn.start first"
    ///   self.lsn.start.cmp(&other.lsn.start)
    ///       .then(self.lsn.end.cmp(&other.lsn.end))
    ///       .then(self.key.start.cmp(&other.key.start))
    ///       .then(self.key.end.cmp(&other.key.end))
    ///       .then(self.is_image.cmp(&other.is_image))
    bool operator<(const LayerKey& o) const {
        if (lsn.start != o.lsn.start) return lsn.start < o.lsn.start;
        if (lsn.end != o.lsn.end) return lsn.end < o.lsn.end;
        if (key.start != o.key.start) return key.start < o.key.start;
        if (key.end != o.key.end) return key.end < o.key.end;
        return is_image < o.is_image;
    }

    bool operator==(const LayerKey& o) const {
        return lsn.start == o.lsn.start && lsn.end == o.lsn.end &&
               key.start == o.key.start && key.end == o.key.end &&
               is_image == o.is_image;
    }
};

/// HistoricLayerCoverage<Value>
/// 对应 Rust: historic_layer_coverage::HistoricLayerCoverage<Value>
///
/// Rust 原文:
///   "Efficiently queryable layer coverage for each LSN."
///   "Allows answering layer map queries very efficiently,
///    but doesn't allow retroactive insertion."
///
/// 内部结构:
///   - head: 当前最新的 LayerCoverageTuple (delta + image 两棵树)
///   - historic: BTreeMap<Lsn, LayerCoverageTuple>, 存储每个版本的快照
template <typename Value>
class HistoricLayerCoverage {
public:
    /// 对应 Rust: HistoricLayerCoverage::new()
    HistoricLayerCoverage() = default;

    /// 对应 Rust: HistoricLayerCoverage::insert(layer_key, value)
    ///
    /// Rust 原文:
    ///   "Add a layer"
    ///   "Panics if new layer has older lsn.start than an existing layer."
    ///
    /// 步骤:
    ///   1. 检查不是逆序插入
    ///   2. 根据 is_image 选择 image_coverage 或 delta_coverage
    ///   3. 调用 LayerCoverage::insert
    ///   4. 快照 head 到 historic[lsn.start]
    void insert(const LayerKey& layer_key, Value value);

    /// 对应 Rust: HistoricLayerCoverage::get_version(lsn)
    ///
    /// Rust 原文:
    ///   "Query at a particular LSN, inclusive"
    ///   match self.historic.range(..=lsn).next_back() { ... }
    ///
    /// 返回 <= lsn 的最新版本快照
    const LayerCoverageTuple<Value>* get_version(Lsn lsn) const;

    /// 对应 Rust: HistoricLayerCoverage::trim(begin)
    ///
    /// Rust 原文:
    ///   "Remove all entries after a certain LSN (inclusive)"
    ///   self.historic.split_off(begin);
    ///   self.head = self.historic.iter().next_back()...
    void trim(Lsn begin);

private:
    /// 当前最新状态
    LayerCoverageTuple<Value> head_;
    /// 所有历史版本: lsn → 快照
    std::map<Lsn, LayerCoverageTuple<Value>> historic_;
};
