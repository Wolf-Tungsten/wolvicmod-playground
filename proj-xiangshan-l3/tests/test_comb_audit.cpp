// P5 集成前提审计：WolvicZjTop 边界不存在「根 In → 根 Out」的纯组合穿透路径。
//
// SV DPI 薄壳采用单调用方案（posedge 提交 + 输出寄存）的正确性前提：模型所有
// 边界输出都是状态的函数，不同拍内随边界输入组合变化。若本测试报告路径，
// 薄壳必须改用双调用（组合 eval + posedge 提交）方案或对相应端口特殊处理。
//
// 方法：复用框架展平结果（§4.2），在「entity → 读它的 Assign → 被驱动 entity」
// 组合图上从每个根 In 做 DFS，Reg/Mem 为周期边界不穿越，Update 不参与组合
// 驱动。到达根 Out 即报告一条见证路径。

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/elab/flatten.h>
#include <model/wolvic_zj_top.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

using namespace wolvicmod;
using namespace wolvicmod::detail;

namespace {

using Node = std::variant<const Entity*, const Action*>;

std::string nodeName(const Node& n) {
    if (auto* e = std::get_if<const Entity*>(&n)) return (*e)->hierPath();
    const Action* a = std::get<const Action*>(n);
    return "assign@" + a->ctx()->hierPath();
}

}  // namespace

TEST_CASE("WolvicZjTop boundary has no combinational pass-through") {
    zj::WolvicZjTop top;
    FlatModel flat;
    flattenInto(&top, flat);

    std::unordered_map<const Entity*, std::vector<const Action*>> readers;
    for (const Action* a : flat.actions) {
        if (a->kind() != Action::Kind::Assign) continue;
        for (const Entity* r : a->reads()) readers[r].push_back(a);
    }

    std::vector<const Entity*> rootIns;
    for (const Entity* e : flat.entities) {
        if (e->owner() == &top && e->kind() == EntityKind::In) rootIns.push_back(e);
    }
    CHECK(!rootIns.empty());

    std::vector<std::string> report;
    for (const Entity* in : rootIns) {
        std::unordered_set<const void*> visited;
        std::unordered_map<const void*, Node> parent;
        std::vector<Node> stack{in};
        visited.insert(in);
        while (!stack.empty()) {
            const Node node = stack.back();
            stack.pop_back();
            if (auto* ep = std::get_if<const Entity*>(&node)) {
                const Entity* e = *ep;
                if (e->owner() == &top && e->kind() == EntityKind::Out) {
                    // 重建见证路径
                    std::string chain;
                    for (Node cur = node;;) {
                        chain = nodeName(cur) + (chain.empty() ? "" : "  <-  ") + chain;
                        const void* key =
                            std::holds_alternative<const Entity*>(cur)
                                ? static_cast<const void*>(std::get<const Entity*>(cur))
                                : static_cast<const void*>(std::get<const Action*>(cur));
                        auto it = parent.find(key);
                        if (it == parent.end()) break;
                        cur = it->second;
                    }
                    report.push_back(chain);
                    continue;
                }
                // Reg/Mem 是周期边界，不穿越
                if (e->kind() == EntityKind::Reg || e->kind() == EntityKind::Mem) continue;
                auto it = readers.find(e);
                if (it == readers.end()) continue;
                for (const Action* a : it->second) {
                    if (visited.insert(a).second) {
                        parent[a] = node;
                        stack.push_back(a);
                    }
                }
            } else {
                const Action* a = std::get<const Action*>(node);
                const Entity* t = a->target();
                if (visited.insert(t).second) {
                    parent[t] = node;
                    stack.push_back(t);
                }
            }
        }
    }

    for (const auto& chain : report) fprintf(stderr, "[comb-audit] %s\n", chain.c_str());
    CHECK(report.empty());
}
