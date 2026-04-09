模拟neon range_search逻辑

文件结构
```
  a1/range_search_cpp/
  ├── CMakeLists.txt              # CMake 构建配置
  ├── Makefile                    # 直接 make 构建
  ├── types.h                     # Lsn, Key, Range<T>, KeySpaceAccum
  ├── layer_desc.h                # PersistentLayerDesc, InMemoryLayerDesc
  ├── layer_coverage.h/cpp        # LayerCoverage + LayerCoverageTuple
  ├── historic_layer_coverage.h/cpp # LayerKey + HistoricLayerCoverage
  ├── layer_map.h/cpp             # SearchResult, select_layer, RangeSearchCollector, range_search
  └── test_main.cpp               # 10 个单元测试
```
  函数对齐清单 (23 个函数全部实现)
```
  ┌───────┬──────────────────────────────────────────────────────────────────────────────────────┬───────────────────────────────────┐
  │   #   │                                         函数                                          │        对应 Rust 源码位置           │
  ├───────┼──────────────────────────────────────────────────────────────────────────────────────┼───────────────────────────────────┤
  │ 1-6   │ LayerCoverage::new/add_node/insert/query/range/range_overlaps                        │ layer_coverage.rs                 │
  ├───────┼──────────────────────────────────────────────────────────────────────────────────────┼───────────────────────────────────┤
  │ 7     │ LayerCoverageTuple                                                                   │ layer_coverage.rs:177-198         │
  ├───────┼──────────────────────────────────────────────────────────────────────────────────────┼───────────────────────────────────┤
  │ 8     │ LayerKey + 排序                                                                       │ historic_layer_coverage.rs:17-43  │
  ├───────┼──────────────────────────────────────────────────────────────────────────────────────┼───────────────────────────────────┤
  │ 9-11  │ HistoricLayerCoverage::insert/get_version/trim                                       │ historic_layer_coverage.rs:62-130 │
  ├───────┼──────────────────────────────────────────────────────────────────────────────────────┼───────────────────────────────────┤
  │ 12-13 │ SearchResult / RangeSearchResult + map_to_in_memory_layer                            │ layer_map.rs:168-211              │
  ├───────┼──────────────────────────────────────────────────────────────────────────────────────┼───────────────────────────────────┤
  │ 14    │ NextLayerType enum                                                                   │ layer_map.rs:232-247              │
  ├───────┼──────────────────────────────────────────────────────────────────────────────────────┼───────────────────────────────────┤
  │ 15-20 │ RangeSearchCollector::new/collect/pad_range/add_range/advance/choose_next_layer_type │ layer_map.rs:216-398              │
  ├───────┼──────────────────────────────────────────────────────────────────────────────────────┼───────────────────────────────────┤
  │ 21    │ select_layer() — 完整 8 Cases                                                         │ layer_map.rs:473-594              │
  ├───────┼──────────────────────────────────────────────────────────────────────────────────────┼───────────────────────────────────┤
  │ 22    │ range_search() 入口                                                                   │ layer_map.rs:596-618              │
  ├───────┼──────────────────────────────────────────────────────────────────────────────────────┼───────────────────────────────────┤
  │ 23    │ search_in_memory_layer (简化为参数传入)                                                 │ layer_map.rs:723-746              │
  └───────┴──────────────────────────────────────────────────────────────────────────────────────┴───────────────────────────────────┘
```
  测试结果: 10/10 PASS
```
  - T1-T2: LayerCoverage 基础功能 + range_overlaps
  - T3: HistoricLayerCoverage 多版本查询（复刻 Rust test_persistent_simple）
  - T4: select_layer 8 Cases
  - T5-T6: range_search 单 key / 多 key 范围搜索
  - T7: 无覆盖 fallback
  - T8: delta+image 混合场景
  - T9: trim 裁剪
  - T10: 部分覆盖 + inmem fallback
```
neon version
```
commit 39e4f234633fa480cebcdf09993628779d73c094 (HEAD -> main, origin/main, origin/HEAD)
Author: John G. Crowley <53502854+johngcrowley@users.noreply.github.com>
Date:   Tue Feb 17 19:25:51 2026 -0600

    GCS Provider Bytes Range Headers (#12855)

    ## Problem

    Bytes range headers are not yet implemented for the GCS JSON API
    interface in Neon,
    [affecting](https://github.com/neondatabase/neon/blob/489c7a20f4ee23ae017d48ab18a9c24123d2b0ec/safekeeper/src/wal_backup.rs#L623)
    `read_object` in SafeKeepers' `wal_backup.rs`, when reading partial
    segments back from remote storage.

    ## Summary of changes
     * Handle bytes range header for GCS JSON API
     * Testing
```
