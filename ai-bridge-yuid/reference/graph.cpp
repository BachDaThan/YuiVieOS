// yui_graph implementation.
#include "yui/graph.hpp"

#include <algorithm>
#include <deque>
#include <set>

#include "yui/text.hpp"

namespace yui::graph {
namespace {
using yui::text::fold;
}  // namespace

const char* kind_name(NodeKind k) {
  switch (k) {
    case NodeKind::File: return "file";
    case NodeKind::App: return "app";
    case NodeKind::Entity: return "entity";
    case NodeKind::Concept: return "concept";
    case NodeKind::Device: return "device";
    case NodeKind::Capability: return "capability";
    case NodeKind::Person: return "person";
    case NodeKind::Place: return "place";
    case NodeKind::Note: return "note";
    case NodeKind::Unknown: break;
  }
  return "unknown";
}

NodeKind kind_from(const std::string& s) {
  if (s == "file") return NodeKind::File;
  if (s == "app") return NodeKind::App;
  if (s == "entity") return NodeKind::Entity;
  if (s == "concept") return NodeKind::Concept;
  if (s == "device") return NodeKind::Device;
  if (s == "capability") return NodeKind::Capability;
  if (s == "person") return NodeKind::Person;
  if (s == "place") return NodeKind::Place;
  if (s == "note") return NodeKind::Note;
  return NodeKind::Unknown;
}

void Graph::reindex() {
  index_.clear();
  out_.clear();
  for (size_t i = 0; i < nodes_.size(); ++i) index_[nodes_[i].id] = i;
  for (size_t i = 0; i < edges_.size(); ++i) out_[edges_[i].from].push_back(i);
}

bool Graph::upsert(const std::string& id, const std::string& label, NodeKind kind, const json::Value& props) {
  auto it = index_.find(id);
  if (it != index_.end()) {
    Node& n = nodes_[it->second];
    n.label = label;
    n.kind = kind;
    if (props.is_object())
      for (const auto& m : props.obj()) n.props.set(m.first, m.second);
    return false;
  }
  Node n;
  n.id = id;
  n.label = label;
  n.kind = kind;
  n.props = props.is_object() ? props : json::Value(json::Object{});
  index_[id] = nodes_.size();
  nodes_.push_back(std::move(n));
  return true;
}

bool Graph::remove(const std::string& id) {
  auto it = index_.find(id);
  if (it == index_.end()) return false;
  nodes_.erase(nodes_.begin() + long(it->second));
  edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                              [&](const Edge& e) { return e.from == id || e.to == id; }),
               edges_.end());
  reindex();
  return true;
}

const Node* Graph::find(const std::string& id) const {
  auto it = index_.find(id);
  return it == index_.end() ? nullptr : &nodes_[it->second];
}

bool Graph::link(const std::string& from, const std::string& rel, const std::string& to, double weight,
                 const json::Value& props) {
  if (!find(from) || !find(to) || rel.empty()) return false;
  for (auto& e : edges_) {
    if (e.from == from && e.rel == rel && e.to == to) {
      e.weight = weight;
      if (props.is_object())
        for (const auto& m : props.obj()) e.props.set(m.first, m.second);
      return true;
    }
  }
  Edge e;
  e.from = from;
  e.rel = rel;
  e.to = to;
  e.weight = weight;
  e.props = props.is_object() ? props : json::Value(json::Object{});
  out_[from].push_back(edges_.size());
  edges_.push_back(std::move(e));
  return true;
}

bool Graph::unlink(const std::string& from, const std::string& rel, const std::string& to) {
  const size_t before = edges_.size();
  edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                              [&](const Edge& e) { return e.from == from && e.rel == rel && e.to == to; }),
               edges_.end());
  if (edges_.size() == before) return false;
  reindex();
  return true;
}

std::vector<Edge> Graph::out_edges(const std::string& id) const {
  std::vector<Edge> res;
  auto it = out_.find(id);
  if (it == out_.end()) return res;
  for (size_t i : it->second) res.push_back(edges_[i]);
  return res;
}

std::vector<Edge> Graph::in_edges(const std::string& id) const {
  std::vector<Edge> res;
  for (const auto& e : edges_)
    if (e.to == id) res.push_back(e);
  return res;
}

std::vector<std::string> Graph::neighbours(const std::string& id, const std::string& rel) const {
  std::vector<std::string> res;
  for (const auto& e : out_edges(id))
    if (rel.empty() || e.rel == rel) res.push_back(e.to);
  return res;
}

std::vector<std::string> Graph::bfs(const std::string& start, int max_depth, const std::string& rel) const {
  std::vector<std::string> order;
  if (!find(start)) return order;
  std::set<std::string> seen{start};
  std::deque<std::pair<std::string, int>> q{{start, 0}};
  while (!q.empty()) {
    auto [id, depth] = q.front();
    q.pop_front();
    order.push_back(id);
    if (depth >= max_depth) continue;
    for (const auto& nb : neighbours(id, rel))
      if (seen.insert(nb).second) q.emplace_back(nb, depth + 1);
  }
  return order;
}

std::vector<std::string> Graph::shortest_path(const std::string& from, const std::string& to) const {
  std::vector<std::string> path;
  if (!find(from) || !find(to)) return path;
  if (from == to) return {from};
  std::unordered_map<std::string, std::string> parent;
  std::deque<std::string> q{from};
  std::set<std::string> seen{from};
  while (!q.empty()) {
    const std::string cur = q.front();
    q.pop_front();
    for (const auto& nb : neighbours(cur)) {
      if (!seen.insert(nb).second) continue;
      parent[nb] = cur;
      if (nb == to) {
        std::string step = to;
        while (true) {
          path.push_back(step);
          if (step == from) break;
          step = parent[step];
        }
        std::reverse(path.begin(), path.end());
        return path;
      }
      q.push_back(nb);
    }
  }
  return path;
}

std::vector<std::string> Graph::search(const std::string& needle, bool kind_filter_set, NodeKind kind) const {
  std::vector<std::string> res;
  const std::string n = fold(needle);
  for (const auto& node : nodes_) {
    if (kind_filter_set && node.kind != kind) continue;
    if (n.empty() || fold(node.id).find(n) != std::string::npos ||
        fold(node.label).find(n) != std::string::npos)
      res.push_back(node.id);
  }
  return res;
}

Stats Graph::stats() const {
  Stats s;
  s.nodes = nodes_.size();
  s.edges = edges_.size();
  for (const auto& n : nodes_) s.kinds[size_t(n.kind)]++;
  return s;
}

json::Value Graph::to_json() const {
  json::Value root(json::Object{});
  root.set("format", "yui-graph/1");
  json::Value arr(json::Array{});
  for (const auto& n : nodes_) {
    json::Value o(json::Object{});
    o.set("id", n.id);
    o.set("label", n.label);
    o.set("kind", kind_name(n.kind));
    o.set("props", n.props);
    arr.push(o);
  }
  root.set("nodes", arr);
  json::Value earr(json::Array{});
  for (const auto& e : edges_) {
    json::Value o(json::Object{});
    o.set("from", e.from);
    o.set("rel", e.rel);
    o.set("to", e.to);
    o.set("weight", e.weight);
    o.set("props", e.props);
    earr.push(o);
  }
  root.set("edges", earr);
  return root;
}

bool Graph::from_json(const json::Value& v, std::string* error) {
  auto fail = [&](const char* m) {
    if (error) *error = m;
    return false;
  };
  if (!v.is_object()) return fail("graph document must be an object");
  if (v["format"].as_string("yui-graph/1") != "yui-graph/1") return fail("unsupported graph format");
  nodes_.clear();
  edges_.clear();
  index_.clear();
  out_.clear();
  for (const auto& n : v["nodes"].arr()) {
    if (!n.is_object() || !n["id"].is_string()) return fail("node requires a string id");
    upsert(n["id"].as_string(), n["label"].as_string(), kind_from(n["kind"].as_string("unknown")),
           n["props"]);
  }
  for (const auto& e : v["edges"].arr()) {
    if (!e.is_object()) return fail("edge must be an object");
    if (!link(e["from"].as_string(), e["rel"].as_string(), e["to"].as_string(), e["weight"].as_double(1.0),
              e["props"]))
      return fail("edge references a missing node");
  }
  reindex();
  return true;
}

std::string Graph::dump(bool pretty) const { return to_json().dump(pretty); }

}  // namespace yui::graph
