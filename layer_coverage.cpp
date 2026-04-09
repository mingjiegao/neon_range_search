/// layer_coverage.cpp — LayerCoverage 实现
///
/// 严格对齐 Rust: pageserver/src/tenant/layer_map/layer_coverage.rs
///
/// 模板显式实例化在文件末尾，分别实例化:
///   - LayerCoverage<std::string>                        (用于 HistoricLayerCoverage 测试)
///   - LayerCoverage<std::shared_ptr<PersistentLayerDesc>> (用于真正的 layer_map)

#include "layer_coverage.h"
#include "layer_desc.h"
#include <memory>
#include <string>

/// ============================================================
/// add_node — 对应 Rust: LayerCoverage::add_node
/// ============================================================
///
/// Rust 原文:
///   fn add_node(&mut self, key: i128) {
///       let value = match self.nodes.range(..=key).next_back() {
///           Some((_, Some(v))) => Some(v.clone()),
///           Some((_, None)) => None,
///           None => None,
///       };
///       self.nodes.insert_mut(key, value);
///   }
///
/// 在 key 处创建一个节点，继承前一个节点的值。
/// 如果 key 处已有节点，则覆盖为前驱值（保持语义不变）。
template <typename Value>
void LayerCoverage<Value>::add_node(Key key) {
    // 找到 <= key 的最后一个节点
    // 对应 Rust: self.nodes.range(..=key).next_back()
    NodeValue value;  // 默认 std::nullopt
    auto it = nodes_.upper_bound(key);  // 第一个 > key 的
    if (it != nodes_.begin()) {
        --it;  // 现在指向 <= key 的最后一个
        // 对应 Rust 的 match:
        //   Some((_, Some(v))) => Some(v.clone()),
        //   Some((_, None)) => None,
        if (it->second.has_value()) {
            value = it->second;  // clone the pair<Lsn, Value>
        } else {
            value = std::nullopt;
        }
    }
    // 对应 Rust: self.nodes.insert_mut(key, value)
    nodes_[key] = value;
}

/// ============================================================
/// insert — 对应 Rust: LayerCoverage::insert
/// ============================================================
///
/// Rust 原文:
///   pub fn insert(&mut self, key: Range<i128>, lsn: Range<u64>, value: Value) {
///       // Add nodes at endpoints
///       self.add_node(key.start);
///       self.add_node(key.end);
///
///       // Raise the height where necessary
///       let mut to_update = Vec::new();
///       let mut to_remove = Vec::new();
///       let mut prev_covered = false;
///       for (k, node) in self.nodes.range(key) {
///           let needs_cover = match node {
///               None => true,
///               Some((h, _)) => h < &lsn.end,
///           };
///           if needs_cover {
///               match prev_covered {
///                   true => to_remove.push(*k),
///                   false => to_update.push(*k),
///               }
///           }
///           prev_covered = needs_cover;
///       }
///       for k in to_update { self.nodes.insert_mut(k, Some((lsn.end, value.clone()))); }
///       for k in to_remove { self.nodes.remove_mut(&k); }
///   }
///
/// 注意: Rust 中 range(key) 是 [key.start, key.end) 即左闭右开
template <typename Value>
void LayerCoverage<Value>::insert(Range<Key> key_range, Range<Lsn> lsn_range, Value value) {
    // Step 1: 在端点处锚定
    // "NOTE The order of lines is important. We add nodes at the start
    //  and end of the key range **before updating any nodes** in order
    //  to pin down the current coverage outside of the relevant key range."
    add_node(key_range.start);
    add_node(key_range.end);

    // Step 2: 收集需要更新和移除的节点
    // "Raise the height where necessary"
    std::vector<Key> to_update;
    std::vector<Key> to_remove;
    bool prev_covered = false;

    // 遍历 [key_range.start, key_range.end) 内的节点
    // 对应 Rust: for (k, node) in self.nodes.range(key)
    auto it_begin = nodes_.lower_bound(key_range.start);
    auto it_end = nodes_.lower_bound(key_range.end);

    for (auto it = it_begin; it != it_end; ++it) {
        Key k = it->first;
        const NodeValue& node = it->second;

        // 对应 Rust:
        //   let needs_cover = match node {
        //       None => true,
        //       Some((h, _)) => h < &lsn.end,
        //   };
        bool needs_cover;
        if (!node.has_value()) {
            needs_cover = true;
        } else {
            needs_cover = node->first < lsn_range.end;
        }

        if (needs_cover) {
            // 对应 Rust:
            //   match prev_covered {
            //       true => to_remove.push(*k),
            //       false => to_update.push(*k),
            //   }
            if (prev_covered) {
                to_remove.push_back(k);
            } else {
                to_update.push_back(k);
            }
        }
        prev_covered = needs_cover;
    }

    // 对应 Rust: for k in to_update { ... }
    for (Key k : to_update) {
        nodes_[k] = std::make_pair(lsn_range.end, value);
    }
    // 对应 Rust: for k in to_remove { ... }
    for (Key k : to_remove) {
        nodes_.erase(k);
    }
}

/// ============================================================
/// query — 对应 Rust: LayerCoverage::query
/// ============================================================
///
/// Rust 原文:
///   pub fn query(&self, key: i128) -> Option<Value> {
///       self.nodes.range(..=key).next_back()?.1.as_ref().map(|(_, v)| v.clone())
///   }
///
/// 找 <= key 的最后一个节点，返回其 Value（如果有覆盖的话）。
template <typename Value>
std::optional<Value> LayerCoverage<Value>::query(Key key) const {
    // 对应 Rust: self.nodes.range(..=key).next_back()
    auto it = nodes_.upper_bound(key);
    if (it == nodes_.begin()) {
        return std::nullopt;
    }
    --it;
    // it->second 是 Optional<(Lsn, Value)>
    // 对应 Rust: ?.1.as_ref().map(|(_, v)| v.clone())
    if (it->second.has_value()) {
        return it->second->second;  // return the Value part
    }
    return std::nullopt;
}

/// ============================================================
/// range — 对应 Rust: LayerCoverage::range
/// ============================================================
///
/// Rust 原文:
///   pub fn range(&self, key: Range<i128>) -> impl Iterator<Item = (i128, Option<Value>)> {
///       self.nodes.range(key).map(|(k, v)| (*k, v.as_ref().map(|x| x.1.clone())))
///   }
///
/// 返回 [start, end) 内所有变化点，每个变化点包含 (key, optional<Value>)
template <typename Value>
std::vector<std::pair<Key, std::optional<Value>>>
LayerCoverage<Value>::range(Range<Key> key_range) const {
    std::vector<std::pair<Key, std::optional<Value>>> result;
    // 对应 Rust: self.nodes.range(key)
    auto it_begin = nodes_.lower_bound(key_range.start);
    auto it_end = nodes_.lower_bound(key_range.end);
    for (auto it = it_begin; it != it_end; ++it) {
        // 对应 Rust: map(|(k, v)| (*k, v.as_ref().map(|x| x.1.clone())))
        std::optional<Value> val;
        if (it->second.has_value()) {
            val = it->second->second;
        }
        result.emplace_back(it->first, val);
    }
    return result;
}

/// ============================================================
/// range_overlaps — 对应 Rust: LayerCoverage::range_overlaps
/// ============================================================
///
/// Rust 原文:
///   pub fn range_overlaps(&self, key_range: &Range<i128>) -> impl Iterator<...> {
///       let first_change = self.query(key_range.start);
///       match first_change {
///           Some(change) => {
///               let range = key_range.start..key_range.end;
///               let mut range_coverage = self.range(range).peekable();
///               if range_coverage.peek().is_some_and(|c| c.1.as_ref() == Some(&change)) {
///                   range_coverage.next();
///               }
///               Either::Left(std::iter::once((key_range.start, Some(change))).chain(range_coverage))
///           }
///           None => {
///               let range = key_range.start..key_range.end;
///               Either::Right(self.range(range))
///           }
///       }
///   }
///
/// 关键逻辑:
///   如果 range start 处已被覆盖，我们要在结果开头加上 (start, Some(layer))。
///   但如果 range() 的第一个元素已经是同一个 layer，就跳过它避免重复。
template <typename Value>
std::vector<std::pair<Key, std::optional<Value>>>
LayerCoverage<Value>::range_overlaps(const Range<Key>& key_range) const {
    auto first_change = query(key_range.start);
    if (first_change.has_value()) {
        // start 处有覆盖
        auto range_coverage = range({key_range.start, key_range.end});

        // 对应 Rust:
        //   if range_coverage.peek().is_some_and(|c| c.1.as_ref() == Some(&change)) {
        //       range_coverage.next();
        //   }
        size_t skip = 0;
        if (!range_coverage.empty() &&
            range_coverage[0].second.has_value() &&
            range_coverage[0].second.value() == first_change.value()) {
            skip = 1;
        }

        // 构造结果: once((start, Some(change))).chain(range_coverage[skip..])
        std::vector<std::pair<Key, std::optional<Value>>> result;
        result.emplace_back(key_range.start, first_change);
        for (size_t i = skip; i < range_coverage.size(); ++i) {
            result.push_back(std::move(range_coverage[i]));
        }
        return result;
    } else {
        // start 处无覆盖，直接返回 range()
        return range({key_range.start, key_range.end});
    }
}

// ============================================================
// 模板显式实例化
// ============================================================
// 实例化 string 版本（用于测试和 HistoricLayerCoverage<string>）
template class LayerCoverage<std::string>;

// 实例化 shared_ptr<PersistentLayerDesc> 版本（用于真正的 layer_map）
template class LayerCoverage<std::shared_ptr<PersistentLayerDesc>>;
