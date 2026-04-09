/// historic_layer_coverage.cpp — HistoricLayerCoverage 实现
///
/// 严格对齐 Rust: pageserver/src/tenant/layer_map/historic_layer_coverage.rs

#include "historic_layer_coverage.h"
#include "layer_desc.h"
#include <cassert>
#include <memory>
#include <string>

/// ============================================================
/// insert — 对应 Rust: HistoricLayerCoverage::insert
/// ============================================================
///
/// Rust 原文:
///   pub fn insert(&mut self, layer_key: LayerKey, value: Value) {
///       // It's only a persistent map, not a retroactive one
///       if let Some(last_entry) = self.historic.iter().next_back() {
///           let last_lsn = last_entry.0;
///           if layer_key.lsn.start < *last_lsn {
///               panic!("unexpected retroactive insert");
///           }
///       }
///
///       let target = if layer_key.is_image {
///           &mut self.head.image_coverage
///       } else {
///           &mut self.head.delta_coverage
///       };
///       target.insert(layer_key.key, layer_key.lsn.clone(), value);
///
///       // Remember history. Clone is O(1)
///       self.historic.insert(layer_key.lsn.start, self.head.clone());
///   }
template <typename Value>
void HistoricLayerCoverage<Value>::insert(const LayerKey& layer_key, Value value) {
    // 检查不是逆序插入
    if (!historic_.empty()) {
        Lsn last_lsn = historic_.rbegin()->first;
        assert(layer_key.lsn.start >= last_lsn && "unexpected retroactive insert");
    }

    // 选择 image 或 delta 覆盖树
    // 对应 Rust: let target = if layer_key.is_image { image } else { delta }
    auto& target = layer_key.is_image ? head_.image_coverage : head_.delta_coverage;
    target.insert(layer_key.key, layer_key.lsn, value);

    // 记录快照 — Rust 中 clone 是 O(1)，C++ 中是深拷贝
    // 对应 Rust: self.historic.insert(layer_key.lsn.start, self.head.clone())
    historic_[layer_key.lsn.start] = head_.clone();
}

/// ============================================================
/// get_version — 对应 Rust: HistoricLayerCoverage::get_version
/// ============================================================
///
/// Rust 原文:
///   pub fn get_version(&self, lsn: u64) -> Option<&LayerCoverageTuple<Value>> {
///       match self.historic.range(..=lsn).next_back() {
///           Some((_, v)) => Some(v),
///           None => None,
///       }
///   }
///
/// 返回 <= lsn 的最新版本的指针，如果没有则返回 nullptr
template <typename Value>
const LayerCoverageTuple<Value>*
HistoricLayerCoverage<Value>::get_version(Lsn lsn) const {
    // 对应 Rust: self.historic.range(..=lsn).next_back()
    auto it = historic_.upper_bound(lsn);
    if (it == historic_.begin()) {
        return nullptr;
    }
    --it;
    return &it->second;
}

/// ============================================================
/// trim — 对应 Rust: HistoricLayerCoverage::trim
/// ============================================================
///
/// Rust 原文:
///   pub fn trim(&mut self, begin: &u64) {
///       self.historic.split_off(begin);
///       self.head = self.historic.iter().next_back()
///           .map(|(_, v)| v.clone())
///           .unwrap_or_default();
///   }
///
/// 删除 >= begin 的所有版本，head 回退到剩余最后一个版本
template <typename Value>
void HistoricLayerCoverage<Value>::trim(Lsn begin) {
    // 对应 Rust: self.historic.split_off(begin) — 删除 >= begin 的部分
    auto it = historic_.lower_bound(begin);
    historic_.erase(it, historic_.end());

    // 对应 Rust: self.head = ... .unwrap_or_default()
    if (historic_.empty()) {
        head_ = LayerCoverageTuple<Value>{};
    } else {
        head_ = historic_.rbegin()->second.clone();
    }
}

// ============================================================
// 模板显式实例化
// ============================================================
template class HistoricLayerCoverage<std::string>;
template class HistoricLayerCoverage<std::shared_ptr<PersistentLayerDesc>>;
