// yui_graph — Đồ thị Tri thức (knowledge graph) backing yuid's long-term memory.
//
// Deliberately dependency-free: an in-memory directed labelled multigraph with
// stable ids, JSON (de)serialisation, substring search and BFS utilities.  The
// daemon keeps one Graph per "vault namespace" so a user profile's memories are
// isolated from an app sandbox's.
#ifndef YUI_GRAPH_HPP
#define YUI_GRAPH_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "yui/json.hpp"

namespace yui::graph {

enum class NodeKind { Unknown, File, App, Entity, Concept, Device, Capability, Person, Place, Note };

const char* kind_name(NodeKind k);
NodeKind kind_from(const std::string& s);

struct Node {
  std::string id;     // caller-chosen stable id, e.g. "app:flutter.yui"
  std::string label;  // human label, may contain Vietnamese
  NodeKind kind = NodeKind::Unknown;
  json::Value props = json::Value(json::Object{});
};

struct Edge {
  std::string from;
  std::string rel;  // relationship name, e.g. "depends_on"
  std::string to;
  double weight = 1.0;
  json::Value props = json::Value(json::Object{});
};

struct Stats {
  size_t nodes = 0;
  size_t edges = 0;
  size_t kinds[10] = {0};
};

class Graph {
 public:
  // Insert or update.  Returns true when a new node was created.
  bool upsert(const std::string& id, const std::string& label, NodeKind kind = NodeKind::Unknown,
              const json::Value& props = json::Value(json::Object{}));
  bool remove(const std::string& id);
  const Node* find(const std::string& id) const;

  // Adds (or replaces, matching from+rel+to) a directed edge.  Both endpoints
  // must exist.  Returns false when an endpoint is missing.
  bool link(const std::string& from, const std::string& rel, const std::string& to, double weight = 1.0,
            const json::Value& props = json::Value(json::Object{}));
  bool unlink(const std::string& from, const std::string& rel, const std::string& to);

  std::vector<Edge> out_edges(const std::string& id) const;
  std::vector<Edge> in_edges(const std::string& id) const;
  // Neighbour ids, optionally restricted to one relationship name.
  std::vector<std::string> neighbours(const std::string& id, const std::string& rel = "") const;

  // Breadth-first expansion with a depth cap; deterministic ordering.
  std::vector<std::string> bfs(const std::string& start, int max_depth = 2,
                               const std::string& rel = "") const;
  // Returns the shortest path (inclusive endpoints) or an empty vector.
  std::vector<std::string> shortest_path(const std::string& from, const std::string& to) const;

  // Case-insensitive substring match over id/label plus optional kind filter.
  std::vector<std::string> search(const std::string& needle, bool kind_filter_set = false,
                                  NodeKind kind = NodeKind::Unknown) const;

  Stats stats() const;

  json::Value to_json() const;
  bool from_json(const json::Value& v, std::string* error = nullptr);
  std::string dump(bool pretty = true) const;

 private:
  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  std::unordered_map<std::string, size_t> index_;
  std::unordered_map<std::string, std::vector<size_t>> out_;  // id -> edge indices
  void reindex();
};

}  // namespace yui::graph

#endif  // YUI_GRAPH_HPP
