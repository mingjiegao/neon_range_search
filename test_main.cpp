/// test_main.cpp — 单元测试
///
/// 对应 Rust 测试:
///   - historic_layer_coverage.rs 中的 test_persistent_simple 等
///   - layer_map.rs 中 select_layer 和 range_search 的行为
///
/// 测试列表:
///   T1: LayerCoverage 基础 insert + query
///   T2: LayerCoverage range_overlaps
///   T3: HistoricLayerCoverage 多版本查询 (复刻 Rust 的 test_persistent_simple)
///   T4: select_layer 8 Cases 各一个用例
///   T5: range_search 完整流程 — 单 key 搜索
///   T6: range_search 完整流程 — 多 key 范围搜索
///   T7: range_search 无任何层覆盖 → 返回 inmem 或空

#include "types.h"
#include "layer_desc.h"
#include "layer_coverage.h"
#include "historic_layer_coverage.h"
#include "layer_map.h"

#include <iostream>
#include <cassert>
#include <string>
#include <memory>
#include <cstdio>

// 简单测试框架
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    static struct Register_##name { \
        Register_##name() { tests.push_back({#name, test_##name}); } \
    } register_##name; \
    static void test_##name()

struct TestEntry { const char* name; void (*fn)(); };
static std::vector<TestEntry> tests;

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (!(_a == _b)) { \
        std::cerr << "  FAIL: " << #a << " == " << #b \
                  << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        throw std::runtime_error("assertion failed"); \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        std::cerr << "  FAIL: " << #x \
                  << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        throw std::runtime_error("assertion failed"); \
    } \
} while(0)

#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

// ============================================================
// 辅助函数
// ============================================================

static std::shared_ptr<PersistentLayerDesc> make_delta(
    Range<Key> kr, Range<Lsn> lr, const std::string& name = "")
{
    auto p = std::make_shared<PersistentLayerDesc>();
    p->key_range = kr;
    p->lsn_range = lr;
    p->is_delta = true;
    p->name = name;
    return p;
}

static std::shared_ptr<PersistentLayerDesc> make_image(
    Range<Key> kr, Range<Lsn> lr, const std::string& name = "")
{
    auto p = std::make_shared<PersistentLayerDesc>();
    p->key_range = kr;
    p->lsn_range = lr;
    p->is_delta = false;
    p->name = name;
    return p;
}

// ============================================================
// T1: LayerCoverage 基础 insert + query
// ============================================================
/// 对应 Rust: layer_coverage.rs 基本功能 + historic_layer_coverage.rs::test_persistent_simple 中 query 部分
///
/// 验证:
///   - insert 后 query 能找到覆盖层
///   - 高 lsn.end 的层覆盖低的
///   - 范围外 query 返回 nullopt
TEST(layer_coverage_basic) {
    LayerCoverage<std::string> cov;

    // 插入 Layer1 覆盖 key [0, 5), lsn [100, 101)
    cov.insert({0, 5}, {100, 101}, "Layer1");

    // key 1 在 [0,5) 内，应该返回 Layer1
    ASSERT_EQ(cov.query(1), std::optional<std::string>("Layer1"));
    ASSERT_EQ(cov.query(4), std::optional<std::string>("Layer1"));
    // key 5 在 [0,5) 外（左闭右开），应该返回 nullopt
    ASSERT_EQ(cov.query(5), std::nullopt);
    // key -1 不在范围内
    ASSERT_EQ(cov.query(-1), std::nullopt);

    // 插入 Layer2 覆盖 key [3, 9), lsn [110, 111) — lsn.end=111 > Layer1 的 101
    cov.insert({3, 9}, {110, 111}, "Layer2");

    // key 4 被 Layer2 覆盖（更高的 lsn.end）
    ASSERT_EQ(cov.query(4), std::optional<std::string>("Layer2"));
    // key 1 仍被 Layer1 覆盖（不在 Layer2 的 key 范围内）
    ASSERT_EQ(cov.query(1), std::optional<std::string>("Layer1"));
    // key 8 被 Layer2 覆盖
    ASSERT_EQ(cov.query(8), std::optional<std::string>("Layer2"));
    // key 9 不在范围内
    ASSERT_EQ(cov.query(9), std::nullopt);

    // 插入 Layer3 覆盖 key [5, 6), lsn [120, 121)
    cov.insert({5, 6}, {120, 121}, "Layer3");
    ASSERT_EQ(cov.query(5), std::optional<std::string>("Layer3"));
    ASSERT_EQ(cov.query(4), std::optional<std::string>("Layer2"));
    ASSERT_EQ(cov.query(7), std::optional<std::string>("Layer2"));
}

// ============================================================
// T2: LayerCoverage range_overlaps
// ============================================================
/// 验证 range_overlaps 返回正确的变化序列
TEST(layer_coverage_range_overlaps) {
    LayerCoverage<std::string> cov;

    cov.insert({0, 5}, {100, 101}, "Layer1");
    cov.insert({3, 9}, {110, 111}, "Layer2");

    // range_overlaps([2, 7)) 应该包含:
    //   (2, Some("Layer1"))   — 初始覆盖
    //   (3, Some("Layer2"))   — 在 key=3 处变为 Layer2
    //   (5, Some("Layer2"))   — key=5 仍是 Layer2（如果有节点的话）
    auto changes = cov.range_overlaps({2, 7});

    // 第一个变化应该是初始覆盖 (2, Layer1)
    ASSERT_TRUE(changes.size() >= 2);
    ASSERT_EQ(changes[0].first, (Key)2);
    ASSERT_EQ(changes[0].second, std::optional<std::string>("Layer1"));

    // 应该有一个变化点在 key=3，变为 Layer2
    bool found_layer2 = false;
    for (auto& [k, v] : changes) {
        if (k == 3 && v.has_value() && *v == "Layer2") {
            found_layer2 = true;
        }
    }
    ASSERT_TRUE(found_layer2);

    // range_overlaps 在范围外
    auto changes2 = cov.range_overlaps({10, 20});
    // key 10 处没覆盖
    ASSERT_TRUE(changes2.empty() || !changes2[0].second.has_value());
}

// ============================================================
// T3: HistoricLayerCoverage 多版本查询
// ============================================================
/// 复刻 Rust: historic_layer_coverage.rs::test_persistent_simple
///
/// Rust 原文:
///   map.insert(LayerKey { key: 0..5, lsn: 100..101, is_image: true }, "Layer 1");
///   map.insert(LayerKey { key: 3..9, lsn: 110..111, is_image: true }, "Layer 2");
///   map.insert(LayerKey { key: 5..6, lsn: 120..121, is_image: true }, "Layer 3");
///
///   // After Layer 1 insertion
///   version = map.get_version(105).unwrap();
///   assert_eq!(version.image_coverage.query(1), Some("Layer 1"));
///
///   // After Layer 2 insertion
///   version = map.get_version(115).unwrap();
///   assert_eq!(version.image_coverage.query(4), Some("Layer 2"));
///   assert_eq!(version.image_coverage.query(11), None);
///
///   // After Layer 3 insertion
///   version = map.get_version(125).unwrap();
///   assert_eq!(version.image_coverage.query(5), Some("Layer 3"));
TEST(historic_layer_coverage_multi_version) {
    HistoricLayerCoverage<std::string> map;

    map.insert(
        LayerKey{{0, 5}, {100, 101}, true},
        "Layer 1");
    map.insert(
        LayerKey{{3, 9}, {110, 111}, true},
        "Layer 2");
    map.insert(
        LayerKey{{5, 6}, {120, 121}, true},
        "Layer 3");

    // After Layer 1 insertion (version 105 > 100)
    auto* v = map.get_version(105);
    ASSERT_TRUE(v != nullptr);
    ASSERT_EQ(v->image_coverage.query(1), std::optional<std::string>("Layer 1"));
    ASSERT_EQ(v->image_coverage.query(4), std::optional<std::string>("Layer 1"));

    // After Layer 2 insertion (version 115 > 110)
    v = map.get_version(115);
    ASSERT_TRUE(v != nullptr);
    ASSERT_EQ(v->image_coverage.query(4), std::optional<std::string>("Layer 2"));
    ASSERT_EQ(v->image_coverage.query(8), std::optional<std::string>("Layer 2"));
    ASSERT_EQ(v->image_coverage.query(11), std::nullopt);

    // After Layer 3 insertion (version 125 > 120)
    v = map.get_version(125);
    ASSERT_TRUE(v != nullptr);
    ASSERT_EQ(v->image_coverage.query(4), std::optional<std::string>("Layer 2"));
    ASSERT_EQ(v->image_coverage.query(5), std::optional<std::string>("Layer 3"));
    ASSERT_EQ(v->image_coverage.query(7), std::optional<std::string>("Layer 2"));

    // Before any insertion
    v = map.get_version(99);
    ASSERT_TRUE(v == nullptr);
}

// ============================================================
// T4: select_layer 8 Cases
// ============================================================
/// 对应 Rust: LayerMap::select_layer 的 8 个 match 分支
TEST(select_layer_8_cases) {
    // Case 1: (None, None, None) → None
    {
        auto r = select_layer(nullptr, nullptr, std::nullopt, 200);
        ASSERT_FALSE(r.has_value());
    }

    // Case 2: (None, Some(image), None) → image
    {
        auto img = make_image({0, 10}, {100, 101});
        auto r = select_layer(nullptr, img, std::nullopt, 200);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->layer_type, LayerType::PersistentLayer);
        ASSERT_EQ(r->lsn_floor, (Lsn)100);
    }

    // Case 3: (Some(delta), None, None) → delta
    {
        auto dlt = make_delta({0, 10}, {100, 150});
        auto r = select_layer(dlt, nullptr, std::nullopt, 200);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->layer_type, LayerType::PersistentLayer);
        ASSERT_EQ(r->lsn_floor, (Lsn)100);
    }

    // Case 4: (Some(delta), Some(image), None)
    // Sub-case: image is newer → 选 image
    {
        auto dlt = make_delta({0, 10}, {100, 150});
        auto img = make_image({0, 10}, {140, 200});  // lsn.end=200 >= 150
        auto r = select_layer(dlt, img, std::nullopt, 250);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->layer_type, LayerType::PersistentLayer);
        ASSERT_EQ(r->lsn_floor, (Lsn)140);
        ASSERT_FALSE(r->persistent_layer->is_delta);  // 选了 image
    }
    // Sub-case: delta is newer → 选 delta, lsn_floor = max(delta.start, image.start+1)
    {
        auto dlt = make_delta({0, 10}, {100, 200});
        auto img = make_image({0, 10}, {90, 91});  // lsn.end=91 < 200
        auto r = select_layer(dlt, img, std::nullopt, 250);
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->persistent_layer->is_delta);
        ASSERT_EQ(r->lsn_floor, std::max((Lsn)100, (Lsn)91));  // max(100, 91) = 100
    }

    // Case 5: (None, None, Some(inmem)) → inmem
    {
        InMemoryLayerDesc inmem{{150, 200}};
        auto r = select_layer(nullptr, nullptr, inmem, 250);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->layer_type, LayerType::InMemoryLayer);
        ASSERT_EQ(r->lsn_floor, (Lsn)150);
    }

    // Case 6: (None, Some(image), Some(inmem)) — image newer → 选 image
    {
        auto img = make_image({0, 10}, {140, 201});
        InMemoryLayerDesc inmem{{150, 200}};
        auto r = select_layer(nullptr, img, inmem, 250);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->layer_type, LayerType::PersistentLayer);
    }

    // Case 7: (Some(delta), None, Some(inmem)) — delta newer → 选 delta
    {
        auto dlt = make_delta({0, 10}, {100, 200});
        InMemoryLayerDesc inmem{{150, 180}};
        auto r = select_layer(dlt, nullptr, inmem, 250);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->layer_type, LayerType::PersistentLayer);
    }

    // Case 8: (Some(delta), Some(image), Some(inmem))
    {
        auto dlt = make_delta({0, 10}, {100, 150});
        auto img = make_image({0, 10}, {90, 91});
        InMemoryLayerDesc inmem{{160, 200}};
        auto r = select_layer(dlt, img, inmem, 250);
        ASSERT_TRUE(r.has_value());
        // inmem is newest (lsn.end=200), should be selected
        ASSERT_EQ(r->layer_type, LayerType::InMemoryLayer);
    }
}

// ============================================================
// T5: range_search 完整流程 — 单 key 搜索
// ============================================================
/// 构建一个简单场景: 一个 image 层覆盖 [0, 10)，搜索 [3, 4)
TEST(range_search_single_key) {
    HistoricLayerCoverage<std::shared_ptr<PersistentLayerDesc>> historic;

    auto img = make_image({0, 10}, {100, 101}, "Image1");
    historic.insert(
        LayerKey{{0, 10}, {100, 101}, true},  // is_image=true
        img);

    // 搜索 key [3,4) at end_lsn=200
    auto result = range_search(historic, std::nullopt, {3, 4}, 200);

    // 应该找到一个层
    ASSERT_EQ(result.found.size(), (size_t)1);
    auto it = result.found.begin();
    ASSERT_EQ(it->first.layer_type, LayerType::PersistentLayer);
    ASSERT_EQ(it->first.persistent_layer->name, std::string("Image1"));
    ASSERT_EQ(it->second.ranges.size(), (size_t)1);
    ASSERT_EQ(it->second.ranges[0].start, (Key)3);
    ASSERT_EQ(it->second.ranges[0].end, (Key)4);
}

// ============================================================
// T6: range_search 完整流程 — 多 key 范围搜索
// ============================================================
/// 两个 image 层覆盖不同 key 范围，搜索跨越两个层
TEST(range_search_multi_key) {
    HistoricLayerCoverage<std::shared_ptr<PersistentLayerDesc>> historic;

    auto img1 = make_image({0, 5}, {100, 101}, "Image1");
    auto img2 = make_image({5, 10}, {110, 111}, "Image2");

    historic.insert(LayerKey{{0, 5}, {100, 101}, true}, img1);
    historic.insert(LayerKey{{5, 10}, {110, 111}, true}, img2);

    // 搜索 [2, 8) at end_lsn=200
    auto result = range_search(historic, std::nullopt, {2, 8}, 200);

    // 应该有两个层
    ASSERT_EQ(result.found.size(), (size_t)2);

    // 验证每个层覆盖了正确的 key 子范围
    bool found_img1 = false, found_img2 = false;
    for (auto& [sr, accum] : result.found) {
        if (sr.persistent_layer && sr.persistent_layer->name == "Image1") {
            found_img1 = true;
            // Image1 应覆盖 [2, 5)
            ASSERT_EQ(accum.ranges.size(), (size_t)1);
            ASSERT_EQ(accum.ranges[0].start, (Key)2);
            ASSERT_EQ(accum.ranges[0].end, (Key)5);
        }
        if (sr.persistent_layer && sr.persistent_layer->name == "Image2") {
            found_img2 = true;
            // Image2 应覆盖 [5, 8)
            ASSERT_EQ(accum.ranges.size(), (size_t)1);
            ASSERT_EQ(accum.ranges[0].start, (Key)5);
            ASSERT_EQ(accum.ranges[0].end, (Key)8);
        }
    }
    ASSERT_TRUE(found_img1);
    ASSERT_TRUE(found_img2);
}

// ============================================================
// T7: range_search 无任何层覆盖 → 返回 inmem 或空
// ============================================================
TEST(range_search_no_coverage) {
    HistoricLayerCoverage<std::shared_ptr<PersistentLayerDesc>> historic;

    // 场景 1: 无层 + 无 inmem → 空结果
    {
        auto result = range_search(historic, std::nullopt, {0, 10}, 200);
        ASSERT_TRUE(result.found.empty());
    }

    // 场景 2: 无持久化层 + 有 inmem → fallback 到 inmem
    {
        InMemoryLayerDesc inmem{{150, 200}};
        auto result = range_search(historic, inmem, {0, 10}, 200);
        ASSERT_EQ(result.found.size(), (size_t)1);
        auto it = result.found.begin();
        ASSERT_EQ(it->first.layer_type, LayerType::InMemoryLayer);
    }

    // 场景 3: 有持久化层但搜索范围外 + 有 inmem → fallback 到 inmem
    {
        auto img = make_image({0, 5}, {100, 101}, "Img");
        historic.insert(LayerKey{{0, 5}, {100, 101}, true}, img);

        InMemoryLayerDesc inmem{{150, 200}};
        auto result = range_search(historic, inmem, {20, 30}, 200);
        ASSERT_EQ(result.found.size(), (size_t)1);
        ASSERT_EQ(result.found.begin()->first.layer_type, LayerType::InMemoryLayer);
    }
}

// ============================================================
// T8: range_search delta + image 混合场景
// ============================================================
/// delta 和 image 层都存在，验证 select_layer 在 range_search 中正确工作
TEST(range_search_mixed_delta_image) {
    HistoricLayerCoverage<std::shared_ptr<PersistentLayerDesc>> historic;

    // image 层 [0, 10) at lsn [100, 101)
    auto img = make_image({0, 10}, {100, 101}, "Image1");
    historic.insert(LayerKey{{0, 10}, {100, 101}, true}, img);

    // delta 层 [0, 10) at lsn [100, 200)  — lsn.end=200 > image的101
    auto dlt = make_delta({0, 10}, {100, 200}, "Delta1");
    historic.insert(LayerKey{{0, 10}, {100, 200}, false}, dlt);

    // 搜索 [3, 7) at end_lsn=250
    // select_layer(delta, image): delta lsn.end=200 > image lsn.end=101
    //   → 选 delta, lsn_floor = max(100, 101) = 101
    auto result = range_search(historic, std::nullopt, {3, 7}, 250);
    ASSERT_EQ(result.found.size(), (size_t)1);
    auto it = result.found.begin();
    ASSERT_EQ(it->first.layer_type, LayerType::PersistentLayer);
    ASSERT_TRUE(it->first.persistent_layer->is_delta);
    ASSERT_EQ(it->first.lsn_floor, (Lsn)101);
}

// ============================================================
// T9: HistoricLayerCoverage::trim
// ============================================================
/// 对应 Rust: HistoricLayerCoverage::trim
TEST(historic_trim) {
    HistoricLayerCoverage<std::string> map;

    map.insert(LayerKey{{0, 5}, {100, 101}, true}, "Layer 1");
    map.insert(LayerKey{{3, 9}, {110, 111}, true}, "Layer 2");
    map.insert(LayerKey{{5, 6}, {120, 121}, true}, "Layer 3");

    // 验证 version 125 存在
    ASSERT_TRUE(map.get_version(125) != nullptr);

    // trim 掉 >= 110 的版本
    map.trim(110);

    // 现在只剩 version 100
    ASSERT_TRUE(map.get_version(105) != nullptr);
    ASSERT_EQ(map.get_version(105)->image_coverage.query(1),
              std::optional<std::string>("Layer 1"));

    // version >= 110 应该没有了
    ASSERT_TRUE(map.get_version(115) == nullptr ||
                map.get_version(115)->image_coverage.query(4) !=
                    std::optional<std::string>("Layer 2"));
}

// ============================================================
// T10: range_search with partial gap + inmem fallback
// ============================================================
/// 部分范围有持久化层覆盖，部分没有 → 没覆盖的部分 fallback 到 inmem
TEST(range_search_partial_coverage) {
    HistoricLayerCoverage<std::shared_ptr<PersistentLayerDesc>> historic;

    // image 层覆盖 [0, 5), lsn.end=301 比 inmem 的 200 更新
    auto img = make_image({0, 5}, {100, 301}, "Image1");
    historic.insert(LayerKey{{0, 5}, {100, 301}, true}, img);

    // 搜索 [0, 10) 但只有 [0,5) 有持久化覆盖
    // inmem lsn.end=200 < image lsn.end=301, 所以 [0,5) 选 image, [5,10) 选 inmem
    InMemoryLayerDesc inmem{{150, 200}};
    auto result = range_search(historic, inmem, {0, 10}, 350);

    // 应该有两个结果: Image1 覆盖 [0,5), inmem 覆盖 [5,10)
    ASSERT_EQ(result.found.size(), (size_t)2);

    bool found_img = false, found_inmem = false;
    for (auto& [sr, accum] : result.found) {
        if (sr.layer_type == LayerType::PersistentLayer) {
            found_img = true;
            ASSERT_EQ(accum.ranges[0].start, (Key)0);
            ASSERT_EQ(accum.ranges[0].end, (Key)5);
        }
        if (sr.layer_type == LayerType::InMemoryLayer) {
            found_inmem = true;
            // inmem 应该覆盖 [5, 10)
            ASSERT_EQ(accum.ranges[0].start, (Key)5);
            ASSERT_EQ(accum.ranges[0].end, (Key)10);
        }
    }
    ASSERT_TRUE(found_img);
    ASSERT_TRUE(found_inmem);
}

// ============================================================
// main
// ============================================================
int main() {
    std::cout << "=== Neon range_search C++ 最小用例测试 ===" << std::endl;
    std::cout << "共 " << tests.size() << " 个测试" << std::endl;
    std::cout << std::endl;

    for (auto& t : tests) {
        std::cout << "  运行: " << t.name << " ... ";
        try {
            t.fn();
            std::cout << "✓ PASS" << std::endl;
            tests_passed++;
        } catch (const std::exception& e) {
            std::cout << "✗ FAIL: " << e.what() << std::endl;
            tests_failed++;
        }
    }

    std::cout << std::endl;
    std::cout << "结果: " << tests_passed << " passed, "
              << tests_failed << " failed" << std::endl;

    return tests_failed > 0 ? 1 : 0;
}
