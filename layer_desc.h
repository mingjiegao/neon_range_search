#pragma once
/// layer_desc.h — PersistentLayerDesc, InMemoryLayerDesc
///
/// 对应 Rust:
///   - crate::tenant::storage_layer::PersistentLayerDesc
///   - layer_map::InMemoryLayerDesc + InMemoryLayerHandle
///
/// PersistentLayerDesc 描述一个已经持久化到磁盘的层文件。
/// InMemoryLayerDesc 描述一个还在内存中的层。

#include "types.h"
#include <string>
#include <memory>

/// PersistentLayerDesc — 持久化层描述
/// 对应 Rust: PersistentLayerDesc
///
/// 在 Neon 中，每个持久化层文件有：
///   - key_range: 该层覆盖的 key 范围 [start, end)
///   - lsn_range: 该层覆盖的 LSN 范围 [start, end)
///   - is_delta:  是增量层(delta)还是镜像层(image)
///
/// Image 层: lsn_range 高度为 1 (lsn..lsn+1), 存某个 LSN 的完整页面快照
/// Delta 层: lsn_range 可以跨多个 LSN, 存 WAL 增量记录
struct PersistentLayerDesc {
    Range<Key> key_range;
    Range<Lsn> lsn_range;
    bool is_delta;       // true=delta层, false=image层
    std::string name;    // 调试用名称

    Range<Key> get_key_range() const { return key_range; }
    Range<Lsn> get_lsn_range() const { return lsn_range; }

    /// 对应 Rust: PersistentLayerDesc::is_incremental() / is_delta()
    bool is_incremental() const { return is_delta; }
};

/// InMemoryLayerDesc — 内存层描述（简化版）
/// 对应 Rust: layer_map::InMemoryLayerDesc
///
/// 在 Neon 中，新写入的 WAL 先放到 InMemoryLayer 中，
/// 定期 checkpoint 时才冻结并写入磁盘变成 PersistentLayer。
struct InMemoryLayerDesc {
    Range<Lsn> lsn_range;

    Range<Lsn> get_lsn_range() const { return lsn_range; }

    bool operator==(const InMemoryLayerDesc& o) const {
        return lsn_range == o.lsn_range;
    }
};
