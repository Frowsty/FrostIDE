// SPDX-License-Identifier: Apache-2.0
// Bounded actual-parent forest projection for the engine JSON Structure panel.
#include "frost/lang/json_entity_hierarchy.hpp"
#include <algorithm>
#include <map>
#include <vector>
namespace frost::lang {
Result<Json> jsonEntityHierarchy(std::span<const JsonEntityStructureNode> nodes) {
    if(nodes.size()>10000)return fail(ErrorCode::OutOfRange,"Engine entity structure exceeds 10000 captured entities");
    const auto absent=nodes.size();std::map<std::uint64_t,std::size_t> ids;
    for(std::size_t i=0;i<nodes.size();++i) {
        const auto& node=nodes[i];
        if(!node.id||!ids.emplace(node.id,i).second||node.range.end<node.range.start||node.selectionRange.end<node.selectionRange.start||
            node.selectionRange.start<node.range.start||node.range.end<node.selectionRange.end)
            return fail(ErrorCode::Corrupt,"Engine entity structure needs unique nonzero IDs and actual contained selection ranges");
    }
    std::vector<std::size_t> parents(nodes.size(),absent),depth(nodes.size(),0),roots;
    std::vector<std::vector<std::size_t>> children(nodes.size());
    for(std::size_t i=0;i<nodes.size();++i)if(nodes[i].parent) {
        const auto parent=ids.find(nodes[i].parent);if(parent==ids.end())return fail(ErrorCode::Corrupt,"Captured entity Parent does not exist in this document");
        parents[i]=parent->second;children[parent->second].push_back(i);
    } else roots.push_back(i);
    std::vector<unsigned char> color(nodes.size(),0);
    for(std::size_t i=0;i<nodes.size();++i)if(!color[i]) {
        std::vector<std::size_t> path;auto current=i;
        while(current!=absent&&!color[current]) {color[current]=1;path.push_back(current);current=parents[current];}
        if(current!=absent&&color[current]==1)return fail(ErrorCode::Corrupt,"Captured entity Parent values contain a cycle");
        for(auto it=path.rbegin();it!=path.rend();++it) {
            const auto parent=parents[*it];depth[*it]=parent==absent?0:depth[parent]+1;
            if(depth[*it]>64)return fail(ErrorCode::OutOfRange,"Captured entity hierarchy exceeds 64 Parent levels");color[*it]=2;
        }
    }
    std::vector<LspRange> whole;whole.reserve(nodes.size());for(const auto& node:nodes)whole.push_back(node.range);
    std::vector<std::size_t> order;order.reserve(nodes.size());for(std::size_t i=0;i<nodes.size();++i)order.push_back(i);
    std::ranges::sort(order,[&](auto a,auto b){return depth[a]>depth[b];});
    for(const auto i:order)if(parents[i]!=absent) {
        auto& parent=whole[parents[i]];parent.start=std::min(parent.start,whole[i].start);parent.end=std::max(parent.end,whole[i].end);
    }
    const auto emit=[&](auto& self,std::size_t i)->Json {
        auto nested=Json::array();for(const auto child:children[i])nested.push(self(self,child));
        const auto& node=nodes[i];return Json::object().set("name",node.name.empty()?std::to_string(node.id):node.name)
            .set("detail","Entity "+std::to_string(node.id)).set("kind",5).set("range",toJson(whole[i]))
            .set("selectionRange",toJson(node.selectionRange)).set("children",nested);
    };
    auto result=Json::array();for(const auto root:roots)result.push(emit(emit,root));return result;
}
}
