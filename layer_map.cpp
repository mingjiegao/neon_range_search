/// layer_map.cpp — select_layer, RangeSearchCollector, range_search 实现
///
/// 严格对齐 Rust: pageserver/src/tenant/layer_map.rs

#include "layer_map.h"
#include <algorithm>
#include <cassert>

// ============================================================
// SearchResult 比较运算符
// ============================================================

bool SearchResult::operator==(const SearchResult& o) const {
    if (layer_type != o.layer_type) return false;
    if (lsn_floor != o.lsn_floor) return false;
    if (layer_type == LayerType::PersistentLayer) {
        // 比较 PersistentLayerDesc 的内容
        if (!persistent_layer && !o.persistent_layer) return true;
        if (!persistent_layer || !o.persistent_layer) return false;
        return persistent_layer->key_range == o.persistent_layer->key_range &&
               persistent_layer->lsn_range == o.persistent_layer->lsn_range &&
               persistent_layer->is_delta == o.persistent_layer->is_delta;
    } else {
        return in_memory_layer == o.in_memory_layer;
    }
}

bool SearchResult::operator<(const SearchResult& o) const {
    if (layer_type != o.layer_type) return layer_type < o.layer_type;
    if (lsn_floor != o.lsn_floor) return lsn_floor < o.lsn_floor;
    if (layer_type == LayerType::PersistentLayer) {
        if (!persistent_layer && !o.persistent_layer) return false;
        if (!persistent_layer) return true;
        if (!o.persistent_layer) return false;
        auto& a = *persistent_layer;
        auto& b = *o.persistent_layer;
        if (a.key_range != b.key_range) return a.key_range < b.key_range;
        if (a.lsn_range != b.lsn_range) return a.lsn_range < b.lsn_range;
        return a.is_delta < b.is_delta;
    } else {
        if (!in_memory_layer && !o.in_memory_layer) return false;
        if (!in_memory_layer) return true;
        if (!o.in_memory_layer) return false;
        return in_memory_layer->lsn_range < o.in_memory_layer->lsn_range;
    }
}

// ============================================================
// RangeSearchResult::map_to_in_memory_layer
// ============================================================

/// 对应 Rust:
///   fn map_to_in_memory_layer(in_memory_layer: Option<InMemoryLayerDesc>, range: Range<Key>)
///       -> RangeSearchResult
RangeSearchResult RangeSearchResult::map_to_in_memory_layer(
    std::optional<InMemoryLayerDesc> in_memory_layer,
    Range<Key> range)
{
    RangeSearchResult res;
    if (in_memory_layer.has_value()) {
        SearchResult sr;
        sr.layer_type = LayerType::InMemoryLayer;
        sr.in_memory_layer = in_memory_layer;
        sr.lsn_floor = in_memory_layer->get_lsn_range().start;

        KeySpaceAccum accum;
        accum.add_range(range);
        res.found[sr] = std::move(accum);
    }
    return res;
}

// ============================================================
// select_layer — 对应 Rust: LayerMap::select_layer
// ============================================================
///
/// Rust 原文 (简化版，完整见 layer_map.rs 第 473-594 行):
///
/// 8 个 case 根据 (delta, image, inmem) 三个 Option 的组合:
///
/// Case 1: (None, None, None) → None
/// Case 2: (None, Some(image), None) → image, lsn_floor = image.lsn.start
/// Case 3: (Some(delta), None, None) → delta, lsn_floor = delta.lsn.start
/// Case 4: (Some(delta), Some(image), None) →
///         image 更新 或 image exact match → 选 image
///         否则选 delta, lsn_floor = max(delta.lsn.start, image.lsn.start + 1)
/// Case 5: (None, None, Some(inmem)) → inmem, lsn_floor = inmem.lsn.start
/// Case 6: (None, Some(image), Some(inmem)) →
///         image 更新 或 exact match → 选 image
///         否则选 inmem, lsn_floor = max(inmem.lsn.start, image.lsn.start + 1)
/// Case 7: (Some(delta), None, Some(inmem)) →
///         delta 更新 或 exact match → 选 delta
///         否则选 inmem, lsn_floor = max(inmem.lsn.start, delta.lsn.end)
/// Case 8: (Some(delta), Some(image), Some(inmem)) →
///         先调 select_layer(delta, image, None) 得到最优持久化层
///         再调 select_layer(persistent, inmem) 和 inmem 比较
///         lsn_floor = max(两次的 lsn_floor)

static SearchResult make_persistent_result(std::shared_ptr<PersistentLayerDesc> layer, Lsn lsn_floor) {
    SearchResult sr;
    sr.layer_type = LayerType::PersistentLayer;
    sr.persistent_layer = std::move(layer);
    sr.lsn_floor = lsn_floor;
    return sr;
}

static SearchResult make_inmem_result(InMemoryLayerDesc inmem, Lsn lsn_floor) {
    SearchResult sr;
    sr.layer_type = LayerType::InMemoryLayer;
    sr.in_memory_layer = std::move(inmem);
    sr.lsn_floor = lsn_floor;
    return sr;
}

std::optional<SearchResult> select_layer(
    std::shared_ptr<PersistentLayerDesc> delta_layer,
    std::shared_ptr<PersistentLayerDesc> image_layer,
    std::optional<InMemoryLayerDesc> in_memory_layer,
    Lsn end_lsn)
{
    bool has_delta = (delta_layer != nullptr);
    bool has_image = (image_layer != nullptr);
    bool has_inmem = in_memory_layer.has_value();

    // Case 1: (None, None, None) → None
    if (!has_delta && !has_image && !has_inmem) {
        return std::nullopt;
    }

    // Case 2: (None, Some(image), None)
    if (!has_delta && has_image && !has_inmem) {
        Lsn lsn_floor = image_layer->get_lsn_range().start;
        return make_persistent_result(image_layer, lsn_floor);
    }

    // Case 3: (Some(delta), None, None)
    if (has_delta && !has_image && !has_inmem) {
        Lsn lsn_floor = delta_layer->get_lsn_range().start;
        return make_persistent_result(delta_layer, lsn_floor);
    }

    // Case 4: (Some(delta), Some(image), None)
    /// 对应 Rust:
    ///   let img_lsn = image.get_lsn_range().start;
    ///   let image_is_newer = image.get_lsn_range().end >= delta.get_lsn_range().end;
    ///   let image_exact_match = img_lsn + 1 == end_lsn;
    ///   if image_is_newer || image_exact_match { 选 image }
    ///   else {
    ///       let lsn_floor = max(delta.lsn.start, image.lsn.start + 1);
    ///       选 delta with lsn_floor
    ///   }
    if (has_delta && has_image && !has_inmem) {
        Lsn img_lsn = image_layer->get_lsn_range().start;
        bool image_is_newer = image_layer->get_lsn_range().end >= delta_layer->get_lsn_range().end;
        bool image_exact_match = (img_lsn + 1 == end_lsn);
        if (image_is_newer || image_exact_match) {
            return make_persistent_result(image_layer, img_lsn);
        } else {
            Lsn lsn_floor = std::max(delta_layer->get_lsn_range().start,
                                     image_layer->get_lsn_range().start + 1);
            return make_persistent_result(delta_layer, lsn_floor);
        }
    }

    // Case 5: (None, None, Some(inmem))
    if (!has_delta && !has_image && has_inmem) {
        Lsn lsn_floor = in_memory_layer->get_lsn_range().start;
        return make_inmem_result(*in_memory_layer, lsn_floor);
    }

    // Case 6: (None, Some(image), Some(inmem))
    /// 对应 Rust:
    ///   let img_lsn = image.get_lsn_range().start;
    ///   let image_is_newer = image.lsn.end >= inmem.lsn.end;
    ///   let image_exact_match = img_lsn + 1 == end_lsn;
    ///   if image_is_newer || image_exact_match { 选 image }
    ///   else {
    ///       let lsn_floor = max(inmem.lsn.start, image.lsn.start + 1);
    ///       选 inmem with lsn_floor
    ///   }
    if (!has_delta && has_image && has_inmem) {
        Lsn img_lsn = image_layer->get_lsn_range().start;
        bool image_is_newer = image_layer->get_lsn_range().end >= in_memory_layer->get_lsn_range().end;
        bool image_exact_match = (img_lsn + 1 == end_lsn);
        if (image_is_newer || image_exact_match) {
            return make_persistent_result(image_layer, img_lsn);
        } else {
            Lsn lsn_floor = std::max(in_memory_layer->get_lsn_range().start,
                                     image_layer->get_lsn_range().start + 1);
            return make_inmem_result(*in_memory_layer, lsn_floor);
        }
    }

    // Case 7: (Some(delta), None, Some(inmem))
    /// 对应 Rust:
    ///   let delta_end = delta.get_lsn_range().end;
    ///   let delta_is_newer = delta_end >= inmem.lsn.end;
    ///   let delta_exact_match = delta_end == end_lsn;
    ///   if delta_is_newer || delta_exact_match { 选 delta }
    ///   else {
    ///       let lsn_floor = max(inmem.lsn.start, delta.lsn.end);
    ///       选 inmem
    ///   }
    if (has_delta && !has_image && has_inmem) {
        Lsn delta_end = delta_layer->get_lsn_range().end;
        bool delta_is_newer = delta_end >= in_memory_layer->get_lsn_range().end;
        bool delta_exact_match = (delta_end == end_lsn);
        if (delta_is_newer || delta_exact_match) {
            return make_persistent_result(delta_layer, delta_layer->get_lsn_range().start);
        } else {
            Lsn lsn_floor = std::max(in_memory_layer->get_lsn_range().start,
                                     delta_layer->get_lsn_range().end);
            return make_inmem_result(*in_memory_layer, lsn_floor);
        }
    }

    // Case 8: (Some(delta), Some(image), Some(inmem))
    /// 对应 Rust:
    ///   let persistent_res = Self::select_layer(Some(delta), Some(image), None, end_lsn).unwrap();
    ///   let persistent_l = persistent_res.layer (must be persistent);
    ///
    ///   let inmem_res = if persistent_l.is_delta() {
    ///       Self::select_layer(Some(persistent_l), None, Some(inmem), end_lsn)
    ///   } else {
    ///       Self::select_layer(None, Some(persistent_l), Some(inmem), end_lsn)
    ///   }.unwrap();
    ///
    ///   SearchResult {
    ///       layer: inmem_res.layer,
    ///       lsn_floor: max(persistent_res.lsn_floor, inmem_res.lsn_floor),
    ///   }
    if (has_delta && has_image && has_inmem) {
        // 先不考虑 inmem，从 delta 和 image 中选最优
        auto persistent_res = select_layer(delta_layer, image_layer, std::nullopt, end_lsn);
        assert(persistent_res.has_value());
        assert(persistent_res->layer_type == LayerType::PersistentLayer);
        auto persistent_l = persistent_res->persistent_layer;

        // 再和 inmem 比较
        std::optional<SearchResult> inmem_res;
        if (persistent_l->is_delta) {
            inmem_res = select_layer(persistent_l, nullptr, in_memory_layer, end_lsn);
        } else {
            inmem_res = select_layer(nullptr, persistent_l, in_memory_layer, end_lsn);
        }
        assert(inmem_res.has_value());

        // 取更严格的 lsn_floor
        inmem_res->lsn_floor = std::max(persistent_res->lsn_floor, inmem_res->lsn_floor);
        return inmem_res;
    }

    // 不应该到达这里
    return std::nullopt;
}

// ============================================================
// RangeSearchCollector — 对应 Rust: layer_map::RangeSearchCollector
// ============================================================
///
/// 双指针算法: 同时扫描 delta_coverage 和 image_coverage 的变化序列，
/// 在每个变化点调用 select_layer 选最优层，并收集结果。
///
/// Rust 原文:
///   "Collection is implemented via a two pointer algorithm.
///    One pointer tracks the start of the current range and the other tracks
///    the beginning of the next range which will overlap with the next change
///    in coverage across both image and delta."

class RangeSearchCollector {
    using Item = std::pair<Key, std::optional<std::shared_ptr<PersistentLayerDesc>>>;
public:
    /// 对应 Rust: RangeSearchCollector::new()
    RangeSearchCollector(
        Range<Key> key_range,
        Lsn end_lsn,
        std::optional<InMemoryLayerDesc> in_memory_layer,
        std::vector<Item> delta_changes,
        std::vector<Item> image_changes)
        : in_memory_layer_(std::move(in_memory_layer))
        , delta_coverage_(std::move(delta_changes))
        , image_coverage_(std::move(image_changes))
        , key_range_(key_range)
        , end_lsn_(end_lsn)
        , current_delta_(nullptr)
        , current_image_(nullptr)
    {}

    /// 对应 Rust: RangeSearchCollector::collect()
    ///
    /// Rust 原文 (layer_map.rs 第 276-318 行):
    ///   fn collect(mut self) -> RangeSearchResult {
    ///       let next_layer_type = self.choose_next_layer_type();
    ///       let mut current_range_start = match next_layer_type {
    ///           None => { self.pad_range(self.key_range.clone()); return self.result; }
    ///           Some(t) if self.key_range.end <= t.next_change_at_key() => {
    ///               self.pad_range(self.key_range.clone()); return self.result;
    ///           }
    ///           Some(t) => {
    ///               let coverage_start = t.next_change_at_key();
    ///               self.pad_range(self.key_range.start..coverage_start);
    ///               self.advance(&t);
    ///               coverage_start
    ///           }
    ///       };
    ///       while current_range_start < self.key_range.end {
    ///           match self.choose_next_layer_type() {
    ///               Some(t) => {
    ///                   self.add_range(current_range_start..t.next_change_at_key());
    ///                   current_range_start = t.next_change_at_key();
    ///                   self.advance(&t);
    ///               }
    ///               None => {
    ///                   self.add_range(current_range_start..self.key_range.end);
    ///                   current_range_start = self.key_range.end;
    ///               }
    ///           }
    ///       }
    ///       self.result
    ///   }
    RangeSearchResult collect() {
        auto next = choose_next_layer_type();
        Key current_range_start;

        if (!next.has_value()) {
            // 没有变化点
            pad_range(key_range_);
            return std::move(result_);
        }

        if (key_range_.end <= next->next_change_at_key()) {
            // 变化在范围外
            pad_range(key_range_);
            return std::move(result_);
        }

        // 变化在范围内
        Key coverage_start = next->next_change_at_key();
        pad_range({key_range_.start, coverage_start});
        advance(*next);
        current_range_start = coverage_start;

        while (current_range_start < key_range_.end) {
            auto next2 = choose_next_layer_type();
            if (next2.has_value()) {
                Key current_range_end = next2->next_change_at_key();
                add_range({current_range_start, current_range_end});
                current_range_start = current_range_end;
                advance(*next2);
            } else {
                add_range({current_range_start, key_range_.end});
                current_range_start = key_range_.end;
            }
        }

        return std::move(result_);
    }

private:
    /// 对应 Rust: RangeSearchCollector::pad_range
    ///
    /// "Map a range which does not intersect any persistent layers to
    ///  the in-memory layer candidate."
    void pad_range(Range<Key> key_range) {
        if (!key_range.is_empty() && in_memory_layer_.has_value()) {
            SearchResult sr;
            sr.layer_type = LayerType::InMemoryLayer;
            sr.in_memory_layer = in_memory_layer_;
            sr.lsn_floor = in_memory_layer_->get_lsn_range().start;
            result_.found[sr].add_range(key_range);
        }
    }

    /// 对应 Rust: RangeSearchCollector::add_range
    ///
    /// "Select the appropriate layer for the given range and update the collector."
    void add_range(Range<Key> covered_range) {
        auto selected = select_layer(
            current_delta_, current_image_, in_memory_layer_, end_lsn_);

        if (selected.has_value()) {
            result_.found[*selected].add_range(covered_range);
        } else {
            pad_range(covered_range);
        }
    }

    /// 对应 Rust: RangeSearchCollector::advance
    ///
    /// "Move to the next coverage change."
    ///
    /// Rust 原文:
    ///   fn advance(&mut self, layer_type: &NextLayerType) {
    ///       match layer_type {
    ///           Delta(_)  => { let (_, layer) = self.delta_coverage.next().unwrap(); self.current_delta = layer; }
    ///           Image(_)  => { let (_, layer) = self.image_coverage.next().unwrap(); self.current_image = layer; }
    ///           Both(_)   => { /* next both */ }
    ///       }
    ///   }
    void advance(const NextLayerType& layer_type) {
        switch (layer_type.kind) {
        case NextLayerKind::Delta: {
            auto item = delta_coverage_.next();
            current_delta_ = item.second.value_or(nullptr);
            break;
        }
        case NextLayerKind::Image: {
            auto item = image_coverage_.next();
            current_image_ = item.second.value_or(nullptr);
            break;
        }
        case NextLayerKind::Both: {
            auto img_item = image_coverage_.next();
            auto dlt_item = delta_coverage_.next();
            current_image_ = img_item.second.value_or(nullptr);
            current_delta_ = dlt_item.second.value_or(nullptr);
            break;
        }
        }
    }

    /// 对应 Rust: RangeSearchCollector::choose_next_layer_type
    ///
    /// "Pick the next coverage change: the one at the lesser key or both if they're aligned."
    ///
    /// Rust 原文:
    ///   fn choose_next_layer_type(&mut self) -> Option<NextLayerType> {
    ///       let next_delta_at = self.delta_coverage.peek().map(|(key, _)| key);
    ///       let next_image_at = self.image_coverage.peek().map(|(key, _)| key);
    ///       match (next_delta_at, next_image_at) {
    ///           (None, None) => None,
    ///           (Some(d), None) => Some(Delta(*d)),
    ///           (None, Some(i)) => Some(Image(*i)),
    ///           (Some(d), Some(i)) if i < d => Some(Image(*i)),
    ///           (Some(d), Some(i)) if d < i => Some(Delta(*d)),
    ///           (Some(d), Some(_)) => Some(Both(*d)),
    ///       }
    ///   }
    std::optional<NextLayerType> choose_next_layer_type() {
        auto* delta_peek = delta_coverage_.peek();
        auto* image_peek = image_coverage_.peek();

        bool has_delta = (delta_peek != nullptr);
        bool has_image = (image_peek != nullptr);

        if (!has_delta && !has_image) return std::nullopt;
        if (has_delta && !has_image) return NextLayerType{NextLayerKind::Delta, delta_peek->first};
        if (!has_delta && has_image) return NextLayerType{NextLayerKind::Image, image_peek->first};

        Key d = delta_peek->first;
        Key i = image_peek->first;
        if (i < d) return NextLayerType{NextLayerKind::Image, i};
        if (d < i) return NextLayerType{NextLayerKind::Delta, d};
        return NextLayerType{NextLayerKind::Both, d};
    }

    std::optional<InMemoryLayerDesc> in_memory_layer_;
    PeekableIter<Item> delta_coverage_;
    PeekableIter<Item> image_coverage_;
    Range<Key> key_range_;
    Lsn end_lsn_;

    std::shared_ptr<PersistentLayerDesc> current_delta_;
    std::shared_ptr<PersistentLayerDesc> current_image_;

    RangeSearchResult result_;
};

// ============================================================
// range_search — 对应 Rust: LayerMap::range_search
// ============================================================
///
/// Rust 原文 (layer_map.rs 第 596-618 行):
///   pub fn range_search(&self, key_range: Range<Key>, end_lsn: Lsn) -> RangeSearchResult {
///       let in_memory_layer = self.search_in_memory_layer(end_lsn);
///
///       let version = match self.historic.get().unwrap().get_version(end_lsn.0 - 1) {
///           Some(version) => version,
///           None => { return RangeSearchResult::map_to_in_memory_layer(in_memory_layer, key_range); }
///       };
///
///       let raw_range = key_range.start.to_i128()..key_range.end.to_i128();
///       let delta_changes = version.delta_coverage.range_overlaps(&raw_range);
///       let image_changes = version.image_coverage.range_overlaps(&raw_range);
///
///       let collector = RangeSearchCollector::new(
///           key_range, end_lsn, in_memory_layer, delta_changes, image_changes);
///       collector.collect()
///   }
///
/// 注意: Rust 中用 end_lsn.0 - 1 查版本，因为 get_version 是 inclusive 的，
///       而 end_lsn 是 exclusive 的上界。

RangeSearchResult range_search(
    const HistoricLayerCoverage<std::shared_ptr<PersistentLayerDesc>>& historic,
    std::optional<InMemoryLayerDesc> in_memory_layer,
    Range<Key> key_range,
    Lsn end_lsn)
{
    // 对应 Rust: self.historic.get().unwrap().get_version(end_lsn.0 - 1)
    auto* version = historic.get_version(end_lsn - 1);
    if (!version) {
        return RangeSearchResult::map_to_in_memory_layer(in_memory_layer, key_range);
    }

    // 对应 Rust:
    //   let raw_range = key_range.start.to_i128()..key_range.end.to_i128();
    //   let delta_changes = version.delta_coverage.range_overlaps(&raw_range);
    //   let image_changes = version.image_coverage.range_overlaps(&raw_range);
    Range<Key> raw_range = key_range;
    auto delta_changes = version->delta_coverage.range_overlaps(raw_range);
    auto image_changes = version->image_coverage.range_overlaps(raw_range);

    // 对应 Rust: RangeSearchCollector::new(...).collect()
    RangeSearchCollector collector(
        key_range, end_lsn, in_memory_layer,
        std::move(delta_changes), std::move(image_changes));
    return collector.collect();
}
