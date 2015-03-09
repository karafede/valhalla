
#include "mjolnir/graphbuilder.h"
#include "mjolnir/util.h"

#include <future>
#include <utility>
#include <thread>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>

#include <valhalla/midgard/logging.h>
#include <valhalla/midgard/aabb2.h>
#include <valhalla/midgard/pointll.h>
#include <valhalla/midgard/polyline2.h>
#include <valhalla/midgard/tiles.h>
#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/graphconstants.h>
#include <valhalla/baldr/signinfo.h>

#include "mjolnir/graphtilebuilder.h"
#include "mjolnir/edgeinfobuilder.h"

using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::mjolnir;


namespace {
// Number of tries when determining not thru edges
constexpr uint32_t kMaxNoThruTries = 256;

// Absurd classification.
constexpr uint32_t kAbsurdRoadClass = 777777;

/**
 * An edge in the graph. Connects 2 nodes that have 2 or more "uses" - meaning
 * the node forms an intersection (or is the end of an OSM way). OSM nodes
 * with less than 2 uses become a shape point (lat,lng) along the edge.
 */
struct Edge {
  // index of the source (start) node of the edge
  uint32_t sourcenode_;

  // Index into the list of OSM way information
  uint32_t wayindex_;

  // Index of the first lat,lng into the GraphBuilder latlngs
  uint32_t llindex_;

  // Attributes needed to sort the edges
  struct EdgeAttributes {
    uint32_t llcount          : 16;
    uint32_t importance       : 3;
    uint32_t driveableforward : 1;
    uint32_t driveablereverse : 1;
    uint32_t traffic_signal   : 1;
    uint32_t forward_signal   : 1;
    uint32_t backward_signal  : 1;
    uint32_t link             : 1;
    uint32_t spare            : 7;
  };
  EdgeAttributes attributes;

  // index of the target (end) node of the edge
  uint32_t targetnode_;


  /**
   * Construct a new edge. Target node and additional lat,lngs will
   * be filled in later.
   * @param sourcenode   Start node of the edge
   * @param wayindex     Index into list of OSM ways
   * @param ll           Lat,lng at the start of the edge.
   */
  static Edge make_edge(const uint32_t sourcenode, const uint32_t wayindex,
       const uint32_t llindex, const OSMWay& way) {
    Edge e{sourcenode, wayindex, llindex};
    e.attributes.llcount = 1;
    e.attributes.importance = static_cast<uint32_t>(way.road_class());
    e.attributes.driveableforward = way.auto_forward();
    e.attributes.driveablereverse = way.auto_backward();
    e.attributes.link = way.link();
    return e;
  }

};

/**
 * Node within the graph
 */
struct Node {
  //the underlying osm node and attributes
  OSMNode node;
  //the graph edge that this node starts
  uint32_t start_of;
  //the graph edge that this node ends
  uint32_t end_of;
  //the graphid of the node
  GraphId graph_id;

  bool is_start() const {
    return start_of != - 1;
  }
  bool is_end() const {
    return end_of != - 1;
  }

};

// collect all the edges that start or end at this node
struct node_bundle : Node {
  size_t node_count;
  std::list<std::pair<Edge, size_t> > edges;
  node_bundle(const Node& other):Node(other){node_count = 1;}
};
node_bundle collect_node_edges(const sequence<Node>::iterator& node_itr, sequence<Node>& nodes, sequence<Edge>& edges) {
  //for each node with the same id (duplicate)
  auto itr = node_itr;
  node_bundle bundle(*itr);
  Node node;
  for(++itr; itr != nodes.end() && (node = *itr).node.osmid == bundle.node.osmid; ++itr) {
    if(node.is_start()) {
      auto edge_itr = edges[node.start_of];
      auto edge = *edge_itr;
      bundle.edges.emplace_back(edge, edge_itr.position());
    }
    if(node.is_end()) {
      auto edge_itr = edges[node.end_of];
      auto edge = *edge_itr;
      bundle.edges.emplace_back(edge, edge_itr.position());
    }
  }
  bundle.node_count = itr - node_itr;
  return bundle;
}

/**
 * Get the best classification for any non-link edges from a node.
 * @param  edges The file backed list of edges in the graph.
 * @return  Returns the best (most important) classification
 */
// Gets the most important class among the node's edges
uint32_t GetBestNonLinkClass(const std::list<std::pair<Edge, size_t> >& edges) {
  uint32_t bestrc = kAbsurdRoadClass;
  for (const auto& edge : edges) {
    if (!edge.first.attributes.link) {
      if (edge.first.attributes.importance < bestrc)
        bestrc = edge.first.attributes.importance;
    }
  }
  return bestrc;
}

/**
 * we need the nodes to be sorted by graphid and then by osmid to make a set of tiles
 * we also need to then update the egdes that pointed to them
 *
 */
std::map<GraphId, size_t> SortGraph(const std::string& nodes_file, const std::string& edges_file, const TileHierarchy& tile_hierarchy, const uint8_t level) {
  LOG_INFO("Sorting graph...");

  // Sort nodes by graphid then by osmid, so its basically a set of tiles
  sequence<Node> nodes(nodes_file, false);
  nodes.sort(
    [&tile_hierarchy, &level](const Node& a, const Node& b) {
      if(a.graph_id == b.graph_id)
        return a.node.osmid < b.node.osmid;
      return a.graph_id < b.graph_id;
    }
  );
  //run through the sorted nodes, going back to the edges they reference and updating each edge
  //to point to the first (out of the duplicates) nodes index accumulate the edge link attributes
  //and apply them to the first node, at the end of this there will be tons of nodes that no edges
  //reference, but we need them because they are the means by which we know what edges connect to
  //a given node from the nodes perspective
  sequence<Edge> edges(edges_file, false);
  uint32_t run_index = 0;
  uint32_t node_index = 0;
  Node last_node{};
  std::map<GraphId, size_t> tiles;
  nodes.transform(
    [&nodes, &edges, &run_index, &node_index, &last_node, &tiles](Node& node) {
      bool runback = false;
      //remember if this was a new tile
      if(node_index == 0 || node.graph_id != (--tiles.end())->first) {
        tiles.insert({node.graph_id, node_index});
        node.graph_id.fields.id = 0;
        //we have just went through a run of duplicates run back over them
        if(node_index != 0)
          runback = true;
      }//but is it a new node
      else if(last_node.node.osmid != node.node.osmid) {
        node.graph_id.fields.id = last_node.graph_id.fields.id + 1;
        runback = true;
      }//its not new so just carry along the important stuff
      else {
        node.graph_id.fields.id = last_node.graph_id.fields.id;
        //if this node marks the start of an edge, go tell the edge where the node is
        if(node.is_start()) {
          auto element = edges[node.start_of];
          auto edge = *element;
          edge.sourcenode_ = run_index;
          element = edge;
          node.node.attributes_.link_edge = last_node.node.attributes_.link_edge || edge.attributes.link;
          node.node.attributes_.non_link_edge = last_node.node.attributes_.non_link_edge || !edge.attributes.link;
        }
        //if this node marks the end of an edge, go tell the dge where the node is
        if(node.is_end()) {
          auto element = edges[node.end_of];
          auto edge = *element;
          edge.targetnode_ = run_index;
          element = edge;
          node.node.attributes_.link_edge = last_node.node.attributes_.link_edge || edge.attributes.link;
          node.node.attributes_.non_link_edge = last_node.node.attributes_.non_link_edge || !edge.attributes.link;
        }
      }

      //if we just finished a run update the first nodes attributes and start another run
      if(runback) {
        auto run_itr = nodes[run_index];
        auto run_node = *run_itr;
        run_node = last_node;
        run_itr = run_node;
        run_index = node_index;
      }

      //next node
      last_node = node;
      ++node_index;
      return node;
    }
  );

  LOG_INFO("Finished");
  return tiles;
}

// Construct edges in the graph and assign nodes to tiles.
void ConstructEdges(const OSMData& osmdata, const std::string& nodes_file, const std::string& edges_file, const float tilesize,
    const std::function<GraphId (const OSMNode&)>& graph_id_predicate) {
  LOG_INFO("Creating graph edges from ways...")

  // Iterate through the OSM ways
  uint32_t edgeindex = 0;
  uint64_t startnodeid;
  GraphId graphid;
  //so we can read ways and nodes and write edges
  sequence<OSMWay> ways(osmdata.ways_file, false);
  sequence<OSMWayNode> way_nodes(osmdata.way_nodes_file, false);
  sequence<Edge> edges(edges_file, true);
  sequence<Node> nodes(nodes_file, true);

  //for each way traversed via the node refs
  size_t current_way_node_index = 0;
  while (current_way_node_index < way_nodes.size()) {
    //grab the way and its first node
    auto first_way_node = *way_nodes[current_way_node_index];
    const auto way = *ways[first_way_node.way_index];
    const auto first_way_node_index = current_way_node_index;
    const auto last_way_node_index = first_way_node_index + way.node_count() - 1;

    //remember this edge starts here
    Edge edge = Edge::make_edge(nodes.size(), first_way_node.way_index, current_way_node_index, way);

    //remember this node as starting this edge
    first_way_node.node.attributes_.link_edge = first_way_node.node.attributes_.link_edge || way.link();
    nodes.push_back({first_way_node.node, static_cast<uint32_t>(ways.size()), static_cast<uint32_t>(-1), graph_id_predicate(first_way_node.node)});

    // Iterate through the nodes of the way until we find an intersection
    while(current_way_node_index < way_nodes.size()) {
      //check if we are done with this way, ie we are started on the next way
      auto way_node = *way_nodes[++current_way_node_index];

      // Increment the count for this edge
      edge.attributes.llcount++;

      // If a is an intersection or the end of the way
      // it's a node of the road network graph
      if (way_node.node.intersection()) {
        //remember that this edge ends here
        edge.targetnode_ = nodes.size();

        //remember this node as ending this edge
        way_node.node.attributes_.link_edge = way_node.node.attributes_.link_edge || way.link();
        nodes.push_back({way_node.node, static_cast<uint32_t>(-1), static_cast<uint32_t>(ways.size()), graph_id_predicate(way_node.node)});

        // Add the edge to the list of edges
        edges.push_back(edge);

        // Start a new edge if this is not the last node in the way
        if (current_way_node_index != last_way_node_index) {
          startnodeid = way_node.node.osmid;
          edge = Edge::make_edge(nodes.size(), way_node.way_index, current_way_node_index, way);
          sequence<Node>::iterator element = --nodes.end();
          auto node = *element;
          node.start_of = ways.size();
          element = node;
        }// This was the last shape point in the way
        else {
          ++current_way_node_index;
          break;
        }
      }// if this edge has a signal not at a intersection
      else if (way_node.node.traffic_signal()) {
        edge.attributes.traffic_signal = true;
        edge.attributes.forward_signal = way_node.node.forward_signal();
        edge.attributes.backward_signal = way_node.node.backward_signal();
      }
    }
  }

  LOG_INFO("Finished with " + std::to_string(edges.size()) + " edges and " + std::to_string(nodes.size()) + " nodes");
}


// Reclassify links (ramps and turn channels).
void ReclassifyLinks(const std::string& ways_file, const std::string& nodes_file, const std::string& edges_file, DataQuality& stats) {
  LOG_INFO("Reclassifying link graph edges...")

  uint32_t count = 0;
  std::unordered_set<size_t> visitedset;  // Set of visited nodes
  std::unordered_set<size_t> expandset;   // Set of nodes to expand
  std::list<size_t> linkedgeindexes;     // Edge indexes to reclassify
  std::vector<uint32_t> endrc;           //
  sequence<OSMWay> ways(ways_file, false);
  sequence<Edge> edges(edges_file, false);
  sequence<Node> nodes(nodes_file, false);

  //try to expand
  auto expand = [&expandset, &endrc, &nodes, &edges] (const Edge& edge, const sequence<Node>::iterator& node_itr) {
    // If the first end node contains a non-link edge we compute the best
    // classification. If not we add the end node to the expand set.
    auto end_node_itr = (edge.sourcenode_ == node_itr.position() ? nodes[edge.targetnode_] : node_itr);
    auto end_node = (*end_node_itr).node;
    if (end_node.attributes_.non_link_edge) {
      endrc.push_back(GetBestNonLinkClass(collect_node_edges(end_node_itr, nodes, edges).edges));
    } else {
      expandset.insert(end_node_itr.position());
    }
  };

  //for each node
  sequence<Node>::iterator node_itr = nodes.begin();
  while (node_itr != nodes.end()) {
    //amalgamate all the node duplicates into one and the edges that connect to it
    //this moves the iterator for you
    auto bundle = collect_node_edges(node_itr, nodes, edges);
    const auto& node = bundle.node;
    if (node.attributes_.link_edge && node.attributes_.non_link_edge) {
      // Get the highest classification of non-link edges at this node
      endrc.clear();
      endrc.push_back(GetBestNonLinkClass(bundle.edges));

      // Expand from all link edges
      for (const auto& startedge : bundle.edges) {
        // Get the edge information. Skip non-link edges
        if (!startedge.first.attributes.link) {
          continue;
        }

        // Clear the sets and edge list
        visitedset.clear();
        expandset.clear();
        linkedgeindexes.clear();

        // Add start edge to list of links edges to potentially reclassify
        linkedgeindexes.push_back(startedge.second);

        expand(startedge.first, node_itr);

        // Expand edges until all paths reach a node that has a non-link
        for (uint32_t n = 0; n < 512; n++) {
          // Once expand list is empty - mark all link edges encountered
          // with the specified classification / importance and break out
          // of this loop (can still expand other ramp edges
          // from the starting node
          if (expandset.empty()) {
            // Make sure this connects...
            if (endrc.size() < 2)  {
              stats.AddIssue(kUnconnectedLinkEdge, GraphId(),
                             (*ways[startedge.first.wayindex_]).way_id(), 0);
            } else {
              // Set to the value of the 2nd best road class of all
              // connections. This protects against downgrading links
              // when branches occur.
              std::sort(endrc.begin(), endrc.end());
              uint32_t rc = endrc[1];
              for (auto idx : linkedgeindexes) {
                sequence<Edge>::iterator element = edges[idx];
                auto edge = *element;
                if (rc > edge.attributes.importance) {
                  edge.attributes.importance = rc;
                  element = edge;
                  count++;
                }
              }
            }
            break;
          }

          // Get the node off of the expand list and add it to the visited list
          auto expandnode = *expandset.begin();
          expandset.erase(expandset.begin());
          visitedset.insert(expandnode);

          // Expand all edges from this node
          auto expanded = collect_node_edges(nodes[expandnode], nodes, edges);
          for (const auto& expandededge : expanded.edges) {
            // Do not allow use of the start edge
            if (expandededge.second == startedge.second) {
              continue;
            }

            // Add this edge (it should be a link edge) and get its end node
            if (!expandededge.first.attributes.link) {
              LOG_ERROR("Expanding onto non-link edge!");
              continue;
            }
            expand(expandededge.first, nodes[expandnode]);
          }
        }
      }
    }

    //go to the next node
    node_itr += bundle.node_count;
  }

  LOG_INFO("Finished with " + std::to_string(count) + " reclassified.");
}

// Test if this is a "not thru" edge. These are edges that enter a region that
// has no exit other than the edge entering the region
bool IsNoThroughEdge(const size_t startnode, const size_t endnode,
             const size_t startedgeindex, sequence<Node>& nodes, sequence<Edge>& edges) {
  // Add the end node Id to the set of nodes to expand
  std::unordered_set<size_t> visitedset;
  std::unordered_set<size_t> expandset;
  expandset.insert(endnode);

  // Expand edges until exhausted, the maximum number of expansions occur,
  // or end up back at the starting node. No node can be visited twice.
  for (uint32_t n = 0; n < kMaxNoThruTries; n++) {
    // If expand list is exhausted this is "not thru"
    if (expandset.empty()) {
      return true;
    }

    // Get the node off of the expand list and add it to the visited list.
    // Expand edges from this node.
    const size_t node_index = *expandset.begin();
    expandset.erase(expandset.begin());
    visitedset.emplace(node_index);
    auto bundle = collect_node_edges(nodes[node_index], nodes, edges);
    for (const auto& edge_pair : bundle.edges) {
      if (edge_pair.second == startedgeindex) {
        // Do not allow use of the start edge
        continue;
      }

      // Return false if we have returned back to the start node or we
      // encounter a tertiary road (or better)
      auto nextendnode = static_cast<size_t>(edge_pair.first.sourcenode_ == node_index ?
          edge_pair.first.targetnode_ : edge_pair.first.sourcenode_);
      if (nextendnode == startnode ||
          edge_pair.first.attributes.importance <= static_cast<uint32_t>(RoadClass::kTertiary)) {
        return false;
      }

      // Add to the expand set if not in the visited set
      if (visitedset.find(nextendnode) == visitedset.end()) {
        expandset.insert(nextendnode);
      }
    }
  }
  return false;
}

// Test if a pair of one-way edges exist at the node. One must be
// inbound and one must be outbound. The current edge is skipped.

bool OnewayPairEdgesExist(const node_bundle& bundle,
                          const size_t node_index,
                          const uint32_t edgeindex,
                          const uint64_t wayid,
                          sequence<Edge>& edges,
                          sequence<OSMWay>& ways) {
  // Iterate through the edges from this node. Skip the one with
  // the specified edgeindex
  uint32_t idx;
  bool forward;
  bool inbound  = false;
  bool outbound = false;
  for (const auto edge_pair : bundle.edges) {
    if (edge_pair.second == edgeindex)
      continue;

    // Get the edge and way.
    const Edge& edge = edge_pair.first;
    const OSMWay &w = ways[edge.wayindex_];

    // Skip if this has matching way Id or a link (ramps/turn channel)
    if (w.way_id() == wayid || edge.attributes.link)
      continue;

    // Check if edge is forward or reverse
    forward = (edge.sourcenode_ == node_index);

    // Check if this is oneway inbound
    if ( (forward && !w.auto_forward() &&  w.auto_backward()) ||
        (!forward &&  w.auto_forward() && !w.auto_backward())) {
      inbound = true;
    }

    // Check if this is oneway outbound
    if ( (forward &&  w.auto_forward() && !w.auto_backward()) ||
        (!forward && !w.auto_forward() &&  w.auto_backward())) {
      outbound = true;
    }
  }
  return (inbound && outbound);
}

bool IsIntersectionInternal(const size_t startnode, const size_t endnode,
                const uint32_t edgeindex, const uint64_t wayid, const float length,
                sequence<Node>& nodes, sequence<Edge>& edges, sequence<OSMWay>& ways) {
  // Limit the length of intersection internal edges
  if (length > kMaxInternalLength)
    return false;

  // Both end nodes must connect to at least 3 edges
  auto bundle1 = collect_node_edges(nodes[startnode], nodes, edges);
  if (bundle1.edges.size() < 3)
    return false;
  auto bundle2 = collect_node_edges(nodes[endnode], nodes, edges);
  if (bundle2.edges.size() < 3)
    return false;

  // Each node must have a pair of oneways (one inbound and one outbound).
  // Exclude links (ramps/turn channels)
  if (!OnewayPairEdgesExist(bundle1, startnode, edgeindex, wayid, edges, ways) ||
      !OnewayPairEdgesExist(bundle2, endnode, edgeindex, wayid, edges, ways)) {
    return false;
  }

  // Assume this is an intersection internal edge
  return true;
}

/**
 * Get the use for a link (either a kRamp or kTurnChannel)
 * TODO - validate logic with some real world cases.
 */
Use GetLinkUse(const size_t edgeindex, const RoadClass rc,
               const float length, const size_t startnode,
               const size_t endnode,
               sequence<Node>& nodes,
               sequence<Edge>& edges) {
  // Assume link that has highway = motorway or trunk is a ramp.
  // Also, if length is > kMaxTurnChannelLength we assume this is a ramp
  if (rc == RoadClass::kMotorway || rc == RoadClass::kTrunk ||
      length > kMaxTurnChannelLength) {
    return Use::kRamp;
  }

  // TODO - if there is a exit sign or exit number present this is
  // considered kRamp. Do we have this information anywhere yet?

  // Both end nodes have to connect to a non-link edge. If either end node
  // connects only to "links" this likely indicates a split or fork,
  // which are not so prevalent in turn channels.
  auto startnode_itr = nodes[startnode];
  auto startnd = collect_node_edges(startnode_itr, nodes, edges);
  auto endnode_itr = nodes[endnode];
  auto endnd = collect_node_edges(endnode_itr, nodes, edges);
  if (startnd.node.attributes_.non_link_edge && endnd.node.attributes_.non_link_edge) {
    // If either end node connects to another link then still
    // call it a ramp. So turn channels are very short and ONLY connect
    // to non-link edges without any exit signs.
    for (const auto& edge : startnd.edges) {
      if (edge.second != edgeindex && edge.first.attributes.link) {
        return Use::kRamp;
      }
    }
    for (const auto& edge : endnd.edges) {
      if (edge.second != edgeindex && edge.first.attributes.link) {
        return Use::kRamp;
      }
    }
    return Use::kTurnChannel;
  }
  else {
    return Use::kRamp;
  }
}

float UpdateLinkSpeed(const Use use, const RoadClass rc, const float spd) {
  if (use == Use::kTurnChannel) {
    return spd * 0.9f;
  } else if (use == Use::kRamp) {
    if (rc == RoadClass::kMotorway) {
      return 95.0f;
    } else if (rc == RoadClass::kTrunk) {
      return 80.0f;
    } else if (rc == RoadClass::kPrimary) {
      return 65.0f;
    } else if (rc == RoadClass::kSecondary) {
      return 50.0f;
    } else if (rc == RoadClass::kTertiary) {
      return 40.0f;
    } else if (rc == RoadClass::kUnclassified) {
      return 35.0f;
    } else {
      return 25.0f;
    }
  }
  return spd;
}

struct DuplicateEdgeInfo {
  uint32_t edgeindex;
  uint32_t length;

  DuplicateEdgeInfo() : edgeindex(0), length(0) { }
  DuplicateEdgeInfo(const uint32_t idx, const uint32_t l)
      : edgeindex(idx),
        length(l) {
  }
};
/*
void CheckForDuplicates(const GraphId& nodeid, const Node& node,
                const std::vector<uint32_t>& edgelengths,
                const std::unordered_map<GraphId, std::vector<Node>>& nodes,
                const std::vector<Edge>& edges,
                const std::vector<OSMWay>& ways, std::atomic<DataQuality*>& stats) {
  uint32_t edgeindex;
  GraphId endnode;
  std::unordered_map<GraphId, DuplicateEdgeInfo> endnodes;
  uint32_t n = 0;
  for (auto edgeindex : node.edges) {
    const Edge& edge = edges[edgeindex];
    if (edge.sourcenode_ == nodeid) {
      endnode = edge.targetnode_;
    } else {
      endnode = edge.sourcenode_;
    }

    // Check if the end node is already in the set of edges from this node
    const auto en = endnodes.find(endnode);
    if (en != endnodes.end() && en->second.length == edgelengths[n]) {
      uint64_t wayid1 = ways[edges[en->second.edgeindex].wayindex_].way_id();
      uint64_t wayid2 = ways[edges[edgeindex].wayindex_].way_id();
      (*stats).AddIssue(kDuplicateWays, GraphId(), wayid1, wayid2);
    } else {
      endnodes.emplace(std::piecewise_construct,
                       std::forward_as_tuple(endnode),
                       std::forward_as_tuple(edgeindex, edgelengths[n]));
    }
    n++;
  }
}
*/
uint32_t CreateSimpleTurnRestriction(const uint64_t wayid, const uint32_t edgeindex,
                 GraphTileBuilder& graphtile,
                 const size_t endnode, sequence<Node>& nodes, sequence<Edge>& edges,
                 const OSMData& osmdata, sequence<OSMWay>& ways,
                 DataQuality& stats) {
  auto res = osmdata.restrictions.equal_range(wayid);
  if (res.first == osmdata.restrictions.end()) {
    return 0;
  }

  // Edge is the from edge of a restriction. Find all TRs (if any)
  // through the target (end) node of this directed edge.
  std::vector<OSMRestriction> trs;
  for (auto r = res.first; r != res.second; ++r) {
    if (r->second.via_graphid() == endnode) {
      if (r->second.day_on() != DOW::kNone) {
        stats.timedrestrictions++;
      } else {
        trs.push_back(r->second);
      }
    }
  }
  if (trs.empty()) {
    return 0;
  }

  // Get the way Ids of the edges at the endnode
  std::vector<uint64_t> wayids;
  auto bundle = collect_node_edges(nodes[endnode], nodes, edges);
  for (const auto& edge : bundle.edges) {
    wayids.push_back((*ways[edge.first.wayindex_]).osmwayid_);
  }

  // There are some cases where both ONLY and NO restriction types are
  // present. Allow this. Iterate through all restrictions and set the
  // restriction mask to include the indexes of restricted turns.
  uint32_t mask = 0;
  for (const auto& tr : trs) {
    switch (tr.type()) {
    case RestrictionType::kNoLeftTurn:
    case RestrictionType::kNoRightTurn:
    case RestrictionType::kNoStraightOn:
    case RestrictionType::kNoUTurn:
      // Iterate through the edge wayIds until the matching to way Id is found
      for (uint32_t idx = 0, n = wayids.size(); idx < n; idx++) {
        if (wayids[idx] == tr.to()) {
          mask |= (1 << idx);
          break;
        }
      }
      break;

    case RestrictionType::kOnlyRightTurn:
    case RestrictionType::kOnlyLeftTurn:
    case RestrictionType::kOnlyStraightOn:
      // Iterate through the edge wayIds - any non-matching edge is added
      // to the turn restriction
      for (uint32_t idx = 0, n = wayids.size(); idx < n; idx++) {
        if (wayids[idx] != tr.to()) {
          mask |= (1 << idx);
        }
      }
      break;
    }
  }

  // Return the restriction mask
  return mask;
}

void BuildTileSet(const std::string& nodes_file, const std::string& edges_file,
    const TileHierarchy& hierarchy, const OSMData& osmdata,
    std::map<GraphId, size_t>::const_iterator tile_start, std::map<GraphId, size_t>::const_iterator tile_end,
    std::promise<DataQuality>& result) {

  std::string thread_id = static_cast<std::ostringstream&>(std::ostringstream() << std::this_thread::get_id()).str();
  LOG_INFO("Thread " + thread_id + " started");

  sequence<OSMWay> ways(osmdata.ways_file, false);
  sequence<OSMWayNode> way_nodes(osmdata.way_nodes_file, false);
  sequence<Edge> edges(edges_file, false);
  sequence<Node> nodes(nodes_file, false);

  // Method to get the shape for an edge - since LL is stored as a pair of
  // floats we need to change into PointLL to get length of an edge
  const auto EdgeShape = [&way_nodes](size_t idx, const size_t count) {
    std::vector<PointLL> shape;
    shape.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      auto node = (*way_nodes[idx++]).node;
      shape.emplace_back(node.lng, node.lat);
    }
    return shape;
  };

  // For each tile in the task
  bool added = false;
  DataQuality stats;
  for(; tile_start != tile_end; ++tile_start) {
    try {
      // What actually writes the tile
      GraphTileBuilder graphtile;

      // Iterate through the nodes
      uint32_t idx = 0;                 // Current directed edge index
      uint32_t directededgecount = 0;
      GraphId nodeid = tile_start->first.Tile_Base();
      //for each node in the tile
      sequence<Node>::iterator node_itr = nodes[tile_start->second];
      while (node_itr != nodes.end()) {
        //amalgamate all the node duplicates into one and the edges that connect to it
        //this moves the iterator for you
        auto bundle = collect_node_edges(node_itr, nodes, edges);
        const auto& node = bundle.node;
        PointLL node_ll{node.lng, node.lat};

        // Look for potential duplicates
        //CheckForDuplicates(nodeid, node, edgelengths, nodes, edges, osmdata.ways, stats);

        // Build directed edges. Track the best classification/importance
        // of outbound edges from this node.
        uint32_t n = 0;
        uint32_t driveable = 0;
        RoadClass bestclass = RoadClass::kServiceOther;
        std::vector<DirectedEdgeBuilder> directededges;
        for (const auto& edge_pair : bundle.edges) {
          // Get the edge and way
          const Edge& edge = edge_pair.first;
          const OSMWay w = *ways[edge.wayindex_];

          // Get the shape for the edge and compute its length
          auto shape = EdgeShape(edge.llindex_, edge.attributes.llcount);
          uint32_t length = static_cast<uint32_t>(PointLL::Length(shape) + .5f);

          // Determine orientation along the edge (forward or reverse between
          // the 2 nodes). Check for edge error.
          bool forward;
          size_t source, target;
          if (edge.sourcenode_ == node_itr.position()) {
            forward = true;
            source = edge.sourcenode_;
            target = edge.targetnode_;
          } else {
            forward = false;
            source = edge.targetnode_;
            target = edge.sourcenode_;
          }

          // Increment driveable count if edge is driveable in either direction
          if (edge.attributes.driveableforward || edge.attributes.driveablereverse)
            driveable++;

          // Check for not_thru edge (only on low importance edges)
          bool not_thru = false;
          if (edge.attributes.importance > static_cast<uint32_t>(RoadClass::kTertiary)) {
            not_thru = IsNoThroughEdge(source, target, edge_pair.second, nodes, edges);
            if (not_thru)
              stats.not_thru_count++;
          }

          // Test if an internal intersection edge
          bool internal = IsIntersectionInternal(source, target, edge_pair.second,
                         w.way_id(), length, nodes, edges, ways);
          if (internal)
            stats.internalcount++;

          // If link is set test to see if we can infer that the edge
          // is a turn channel. Update speed for link edges.
          float speed = w.speed();
          RoadClass rc = static_cast<RoadClass>(edge.attributes.importance);
          Use use = w.use();
          if (w.link()) {
            use = GetLinkUse(edge_pair.second, rc, length, edge.sourcenode_, edge.targetnode_, nodes, edges);
            if (use == Use::kTurnChannel)
              stats.turnchannelcount++;
            speed = UpdateLinkSpeed(use, rc, w.speed());
          }

          // Infer cul-de-sac if a road edge is a loop and is low
          // classification. TODO - do we need length limit?
          if (use == Use::kRoad && source == target && rc > RoadClass::kTertiary) {
            use = Use::kCuldesac;
            stats.culdesaccount++;
          }

          // Handle simple turn restrictions that originate from this
          // directed edge
          uint32_t restrictions = CreateSimpleTurnRestriction(w.way_id(), idx,
                      graphtile, target, nodes, edges, osmdata, ways, stats);
          if (restrictions != 0)
            stats.simplerestrictions++;


          // traffic signal exists at an intersection node
          // OR
          // traffic signal exists at a non-intersection node
          // forward signal must exist if forward direction and vice versa.
          // if forward and backward signal flags are not set then only set for oneways.
          bool has_signal = (!forward && node.traffic_signal()) ||
            ((edge.attributes.traffic_signal) &&
            ((forward && edge.attributes.forward_signal) || (!forward && edge.attributes.backward_signal) ||
            (w.oneway() && !edge.attributes.forward_signal && !edge.attributes.backward_signal)));

          // Add a directed edge and get a reference to it
          directededges.emplace_back(w, (*nodes[target]).graph_id, forward, length,
                        speed, use, not_thru, internal, rc, n, has_signal, restrictions);
          DirectedEdgeBuilder& directededge = directededges.back();

          // Update the node's best class
          bestclass = std::min(bestclass, directededge.classification());

          // Check for updated ref from relations.
          std::string ref;
          auto iter = osmdata.way_ref.find(w.way_id());
          if (iter != osmdata.way_ref.end()) {
            if (w.ref_index() != 0)
              ref = GraphBuilder::GetRef(osmdata.ref_offset_map.name(w.ref_index()),iter->second);
          }

          // Add edge info to the tile and set the offset in the directed edge
          uint32_t edge_info_offset = graphtile.AddEdgeInfo(
            edge_pair.second, (*nodes[source]).graph_id, (*nodes[target]).graph_id, shape,
            w.GetNames(ref, osmdata.ref_offset_map, osmdata.name_offset_map),
            added);
          directededge.set_edgeinfo_offset(edge_info_offset);

          // TODO - update logic so we limit the CreateExitSignInfoList calls
          // TODO - Also, we will have to deal with non ramp signs
          // Any exits for this directed edge? is auto and oneway?
          std::vector<SignInfo> exits = GraphBuilder::CreateExitSignInfoList(node, w, osmdata);
          if (!exits.empty() && directededge.forwardaccess()
               && directededge.use() == Use::kRamp) {
            graphtile.AddSigns(idx, exits);
            directededge.set_exitsign(true);
          }

          // Increment the directed edge index within the tile
          idx++;
          n++;
        }

        // Set the node lat,lng, index of the first outbound edge, and the
        // directed edge count from this edge and the best road class
        // from the node. Increment directed edge count.
        NodeInfoBuilder nodebuilder(node_ll, directededgecount,
                                    bundle.edges.size(), driveable,
                                    bestclass, node.access_mask(), node.type(),
                                    (bundle.edges.size() == 1),
                                    node.traffic_signal());

        directededgecount += bundle.edges.size();

        // Add node and directed edge information to the tile
        graphtile.AddNodeAndDirectedEdges(nodebuilder, directededges);

        // Increment nodeid
        nodeid++;

        // Increment the counts in the histogram
        stats.node_counts[directededges.size()]++;
      }

      // Write the actual tile to disk
      graphtile.StoreTileData(hierarchy, tile_start->first);

      // Made a tile
      LOG_INFO((boost::format("Thread %1% wrote tile %2%: %3% bytes") % thread_id % tile_start->first % graphtile.size()).str());
    }// Whatever happens in Vegas..
    catch(std::exception& e) {
      // ..gets sent back to the main thread
      result.set_exception(std::current_exception());
      LOG_ERROR((boost::format("Thread %1% failed tile %2%: %3%") % thread_id % tile_start->first % e.what()).str());
      return;
    }
  }
  // Let the main thread see how this thread faired
  result.set_value(stats);
}

// Build tiles for the local graph hierarchy
void BuildLocalTiles(const unsigned int thread_count, const OSMData& osmdata, const std::string& nodes_file, const std::string& edges_file,
  const std::map<GraphId, size_t>& tiles, const TileHierarchy& tile_hierarchy, DataQuality& stats) {

  LOG_INFO("Building " + std::to_string(tiles.size()) + " tiles with " + std::to_string(thread_count) + " threads...");

  // A place to hold worker threads and their results, be they exceptions or otherwise
  std::vector<std::shared_ptr<std::thread> > threads(thread_count);

  // Hold the results (DataQuality/stats) for the threads
  std::vector<std::promise<DataQuality> > results(threads.size());

  // Divvy up the work
  size_t floor = tiles.size() / threads.size();
  size_t at_ceiling = tiles.size() - (threads.size() * floor);
  std::map<GraphId, size_t>::const_iterator tile_start, tile_end = tiles.begin();

  // Atomically pass around stats info
  LOG_INFO(std::to_string(tiles.size()) + " tiles");
  for (size_t i = 0; i < threads.size(); ++i) {
    // Figure out how many this thread will work on (either ceiling or floor)
    size_t tile_count = (i < at_ceiling ? floor + 1 : floor);
    // Where the range begins
    tile_start = tile_end;
    // Where the range ends
    std::advance(tile_end, tile_count);
    // Make the thread
    threads[i].reset(
      new std::thread(BuildTileSet,  std::cref(nodes_file), std::cref(edges_file), std::cref(tile_hierarchy),
                      std::cref(osmdata), tile_start, tile_end, std::ref(results[i]))
    );
  }

  // Join all the threads to wait for them to finish up their work
  for (auto& thread : threads) {
    thread->join();
  }

  LOG_INFO("Finished");

  // Check all of the outcomes and accumulate stats
  for (auto& result : results) {
    // If something bad went down this will rethrow it
    try {
      // Add statistics and log issues on this thread
      const auto& stat = result.get_future().get();
      stats.AddStatistics(stat);
      stat.LogIssues();
    }
    catch(std::exception& e) {
      //TODO: throw further up the chain?
    }
  }
}

}

namespace valhalla {
namespace mjolnir {

// Build the graph from the input
void GraphBuilder::Build(const boost::property_tree::ptree& pt, OSMData& osmdata) {
  std::string nodes_file = "nodes.bin";
  std::string edges_file = "edges.bin";
  TileHierarchy tile_hierarchy(pt.get_child("hierarchy"));
  unsigned int threads = std::max(static_cast<unsigned int>(1),
    pt.get<unsigned int>("concurrency", std::thread::hardware_concurrency()));
  const auto& tl = tile_hierarchy.levels().rbegin();
  uint8_t level = tl->second.level;

  // Make the edges and nodes in the graph
  ConstructEdges(osmdata, nodes_file, edges_file, tl->second.tiles.TileSize(),
    [&tile_hierarchy, &level](const OSMNode& node) {
      return tile_hierarchy.GetGraphId({node.lng, node.lat}, level);
    }
  );

  // Line up the nodes and then re-map the edges that the edges to them
  auto tiles = SortGraph(nodes_file, edges_file, tile_hierarchy, level);
  //TODO: a bunch of node osm id associations for restrictions, node_ref, node_exit_to, node_name

  // Reclassify links (ramps). Cannot do this when building tiles since the
  // edge list needs to be modified
  DataQuality stats;
  ReclassifyLinks(osmdata.ways_file, nodes_file, edges_file, stats);

  // Build tiles at the local level. Form connected graph from nodes and edges.
  BuildLocalTiles(level, osmdata, nodes_file, edges_file, tiles, tile_hierarchy, stats);

  stats.LogStatistics();
}


// Get highway refs from relations
std::string GraphBuilder::GetRef(const std::string& way_ref, const std::string& relation_ref) {
  bool found = false;
  std::string refs;
  std::vector<std::string> way_refs = GetTagTokens(way_ref); // US 51;I 57
  std::vector<std::string> refdirs = GetTagTokens(relation_ref);// US 51|north;I 57|north
  for (auto& ref : way_refs) {
    found = false;
    for (const auto& refdir : refdirs) {
      std::vector<std::string> tmp = GetTagTokens(refdir,'|'); // US 51|north
      if (tmp.size() == 2) {
        if (tmp[0] == ref) { // US 51 == US 51
          if (!refs.empty())
            refs += ";" + ref + " " + tmp[1];// ref order of the way wins.
          else
            refs = ref + " " + tmp[1];
          found = true;
          break;
        }
      }
    }

    if (!found) {   // no direction found in relations for this ref
      if (!refs.empty())
        refs += ";" + ref;
      else
        refs = ref;
    }
  }
  return refs;
}

std::vector<SignInfo> GraphBuilder::CreateExitSignInfoList(const OSMNode& node, const OSMWay& way, const OSMData& osmdata) {

  std::vector<SignInfo> exit_list;

  ////////////////////////////////////////////////////////////////////////////
  // NUMBER

  // Exit sign number
  if (way.junction_ref_index() != 0) {
    exit_list.emplace_back(Sign::Type::kExitNumber,
            osmdata.ref_offset_map.name(way.junction_ref_index()));
  }  else if (node.ref()) {
    exit_list.emplace_back(Sign::Type::kExitNumber,
            osmdata.node_ref.find(node.osmid)->second);
  }

  ////////////////////////////////////////////////////////////////////////////
  // BRANCH

  bool has_branch = false;

  // Exit sign branch refs
  if (way.destination_ref_index() != 0) {
    has_branch = true;
    std::vector<std::string> branch_refs = GetTagTokens(
        osmdata.ref_offset_map.name(way.destination_ref_index()));
    for (auto& branch_ref : branch_refs) {
      exit_list.emplace_back(Sign::Type::kExitBranch, branch_ref);
    }
  }

  // Exit sign branch road names
  if (way.destination_street_index() != 0) {
    has_branch = true;
    std::vector<std::string> branch_streets = GetTagTokens(
        osmdata.name_offset_map.name(way.destination_street_index()));
    for (auto& branch_street : branch_streets) {
      exit_list.emplace_back(Sign::Type::kExitBranch, branch_street);
    }
  }

  ////////////////////////////////////////////////////////////////////////////
  // TOWARD

  bool has_toward = false;

  // Exit sign toward refs
  if (way.destination_ref_to_index() != 0) {
    has_toward = true;
    std::vector<std::string> toward_refs = GetTagTokens(
        osmdata.ref_offset_map.name(way.destination_ref_to_index()));
    for (auto& toward_ref : toward_refs) {
      exit_list.emplace_back(Sign::Type::kExitToward, toward_ref);
    }
  }

  // Exit sign toward streets
  if (way.destination_street_to_index() != 0) {
    has_toward = true;
    std::vector<std::string> toward_streets = GetTagTokens(
        osmdata.name_offset_map.name(way.destination_street_to_index()));
    for (auto& toward_street : toward_streets) {
      exit_list.emplace_back(Sign::Type::kExitToward, toward_street);
    }
  }

  // Exit sign toward locations
  if (way.destination_index() != 0) {
    has_toward = true;
    std::vector<std::string> toward_names = GetTagTokens(
        osmdata.name_offset_map.name(way.destination_index()));
    for (auto& toward_name : toward_names) {
      exit_list.emplace_back(Sign::Type::kExitToward, toward_name);
    }
  }

  ////////////////////////////////////////////////////////////////////////////
  // Process exit_to only if other branch or toward info does not exist
  if (!has_branch && !has_toward) {
    if (node.exit_to()) {

      std::string tmp;
      std::size_t pos;
      std::vector<std::string> exit_tos = GetTagTokens(
          osmdata.node_exit_to.find(node.osmid)->second);
      for (auto& exit_to : exit_tos) {

        tmp = exit_to;

        boost::algorithm::to_lower(tmp);

        //remove the "To" For example:  US 11;To I 81;Carlisle;Harrisburg
        if (boost::starts_with(tmp, "to ")) {
            exit_list.emplace_back(Sign::Type::kExitToward, exit_to.substr(3));
            continue;
        }
        //remove the "Toward" For example:  US 11;Toward I 81;Carlisle;Harrisburg
        if (boost::starts_with(tmp, "toward ")) {
            exit_list.emplace_back(Sign::Type::kExitToward, exit_to.substr(7));
            continue;
        }

        std::size_t found = tmp.find(" to ");

        //Default to kToward if found twice or "toward" found as well; otherwise, <branch> to <toward>
        //For example:  I 95 to I 695
        if (found != std::string::npos &&
           (tmp.find(" to ",found+4) == std::string::npos && tmp.find(" toward ") == std::string::npos)) {

            exit_list.emplace_back(Sign::Type::kExitBranch, exit_to.substr(0,found));

            exit_list.emplace_back(Sign::Type::kExitToward, exit_to.substr(found+4));
            continue;
        }

        found = tmp.find(" toward ");

        //Default to kToward if found twice or "to" found as well; otherwise, <branch> toward <toward>
        //For example:  I 95 to I 695
        if (found != std::string::npos &&
            (tmp.find(" toward ",found+8) == std::string::npos && tmp.find(" to ") == std::string::npos)) {

          exit_list.emplace_back(Sign::Type::kExitBranch, exit_to.substr(0,found));

          exit_list.emplace_back(Sign::Type::kExitToward, exit_to.substr(found+8));
          continue;
        }

        //default to toward.
        exit_list.emplace_back(Sign::Type::kExitToward, exit_to);
      }
    }
  }

  ////////////////////////////////////////////////////////////////////////////
  // NAME

  // Exit sign name
  if (node.name()) {
    std::vector<std::string> names = GetTagTokens(
            osmdata.node_name.find(node.osmid)->second);
    for (auto& name : names) {
      exit_list.emplace_back(Sign::Type::kExitName, name);
    }
  }

  return exit_list;
}

}
}
