// Source Code: https://github.com/hit9/quadtree-pathfinding
// License: BSD. Chao Wang, hit9[At]icloud.com.

#include "pathfinder_flowfield.hpp"

#include <cassert>
#include <cstdlib>

#include "base.hpp"

namespace qdpf {
namespace internal {

// ffa1 is the flowfield pathfinder to work on the node graph.
// ffa2 is the flowfield pathfinder to work on the gate graph.
// In the constructor, we just initialized some lambda function for further reusing.
FlowFieldPathFinderImpl::FlowFieldPathFinderImpl(int n) : ffa1(FFA1(n)), ffa2(FFA2(n)) {
  // nodesOverlappingQueryRangeCollector: is to collect nodes overlapping with the query range.
  nodesOverlappingQueryRangeCollector = [this](const QdNode *node) {
    // we care about only leaf nodes with no obstacles
    if (node->isLeaf && node->objects.empty()) nodesOverlappingQueryRange.insert(node);
  };

  // gatesInNodesOverlappingQueryRangeCollector: is to collect gates inside a single node within
  // nodesOverlappingQueryRange.
  gatesInNodesOverlappingQueryRangeCollector = [this](const Gate *gate) {
    gatesInNodesOverlappingQueryRange.insert(gate->a);
  };

  // ffa1NeighborsCollector is for computing node flow field, it's used to visit every neighbour
  // vertex for given node.
  ffa1NeighborsCollector = [this](QdNode *u, NeighbourVertexVisitor<QdNode *> &visitor) {
    m->ForEachNeighbourNodes(u, visitor);
  };

  // ffa2NeighborsCollector is for computing gate flow field, it's used to visit every neighbour
  // gate cells for given gate cell u.
  // It collects neighbour on the { tmp + map } 's gate graph.
  ffa2NeighborsCollector = [this](int u, NeighbourVertexVisitor<int> &visitor) {
    ForEachNeighbourGateWithST(u, visitor);
  };
}

void FlowFieldPathFinderImpl::Reset(const QuadtreeMap *m, int x2, int y2,
                                    const Rectangle &qrange) {
  // debug mode, checks m, it's nullptr if mapx didn't find one.
  assert(m != nullptr);

  // resets the attributes.
  this->m = m;
  this->x2 = x2, this->y2 = y2;
  this->qrange = qrange;  // copy updated
  tNode = nullptr;

  // the given qrange is invalid.
  if (!(qrange.x1 <= qrange.x2 && qrange.y1 <= qrange.y2)) return;

  t = m->PackXY(x2, y2);
  tNode = m->FindNode(x2, y2);
  // tNode is not found, indicating that t is out of bounds.
  if (tNode == nullptr) return;

  // clears the old results.
  nodeFlowField.Clear();
  gateFlowField.Clear();
  finalFlowField.Clear();

  // find all nodes overlapping with qrange.
  nodesOverlappingQueryRange.clear();
  m->NodesInRange(qrange, nodesOverlappingQueryRangeCollector);

  // find all gates inside nodesOverlappingQueryRange.
  gatesInNodesOverlappingQueryRange.clear();
  for (auto node : nodesOverlappingQueryRange) {  // node is const QdNode*
    m->ForEachGateInNode(node, gatesInNodesOverlappingQueryRangeCollector);
  }

  gateCellsOnNodeFields.clear();

  // Rebuild the tmp graph.
  PathFinderHelper::Reset(this->m);

  // Add the target cell to the gate graph
  bool tIsGate = m->IsGateCell(tNode, t);

  if (!tIsGate) {
    AddCellToNodeOnTmpGraph(t, tNode);
    // t is a virtual gate cell now.
    // we should check if it is inside the qrange,
    // and add it to gatesInNodesOverlappingQueryRange if it is.
    if (x2 >= qrange.x1 && x2 <= qrange.x2 && y2 >= qrange.y1 && y2 <= qrange.y2) {
      gatesInNodesOverlappingQueryRange.insert(t);
    }
  }

  // Special case:
  // if the target node overlaps the query range, we should connects the overlapping cells to
  // the target, since the best path is a straight line then.
  Rectangle tNodeRectangle{tNode->x1, tNode->y1, tNode->x2, tNode->y2};
  Rectangle overlap;

  auto hasOverlap = GetOverlap(tNodeRectangle, qrange, overlap);

  if (hasOverlap) {
    for (int x = overlap.x1; x <= overlap.x2; ++x) {
      for (int y = overlap.y1; y <= overlap.y2; ++y) {
        int u = m->PackXY(x, y);
        // detail notice is: we should skip u if it's a gate cell on the map's graph,
        // since we already connect all gate cells with t.
        if (u != t && !m->IsGateCell(tNode, u)) {
          ConnectCellsOnTmpGraph(u, t);
          // We should consider u as a new tmp "gate" cell.
          //  we should add it to overlapping gates collection.
          gatesInNodesOverlappingQueryRange.insert(u);
        }
      }
    }
  }
}

// Computes node flow field.
// 1. Perform flowfield algorithm on the node graph.
// 2. Stops earlier if all nodes overlapping the query range are checked.
int FlowFieldPathFinderImpl::ComputeNodeFlowField() {
  // unreachable
  if (tNode == nullptr) return -1;
  // the target is an obstacle, unreachable
  if (m->IsObstacle(x2, y2)) return -1;

  // ensures that we can call it for multiple times
  if (nodeFlowField.costs.Size()) nodeFlowField.Clear();

  // stops earlier if all nodes overlapping with the query range are checked.

  // n counts the number of nodes marked by flowfield algorithm, for nodes inside
  // nodesOverlappingQueryRange.
  int n = 0;

  // stopf is a function to stop the flowfield algorithm from execution after given vertex (the
  // node) is marked.
  FFA1::StopAfterFunction stopf = [&n, this](QdNode *node) {
    if (nodesOverlappingQueryRange.find(node) != nodesOverlappingQueryRange.end()) ++n;
    // nodesOverlappingQueryRange's size will always > 0.
    return n >= nodesOverlappingQueryRange.size();
  };

  // Compute flowfield on the node graph.
  ffa1.Compute(tNode, nodeFlowField, ffa1NeighborsCollector, nullptr, stopf);
  return 0;
}

// collects the gate cells on the node flow field if ComputeNodeFlowField is successfully called
// and ComputeGateFlowField is called with useNodeFlowField is set true.
void FlowFieldPathFinderImpl::collectGateCellsOnNodeField() {
  gateCellsOnNodeFields.insert(t);

  // We have to add all non-gate neighbours of t on the tmp graph.
  NeighbourVertexVisitor<int> tmpNeighbourVisitor = [this](int v, int cost) {
    if (!m->IsGateCell(tNode, v)) gateCellsOnNodeFields.insert(v);
  };
  tmp.ForEachNeighbours(t, tmpNeighbourVisitor);

  // gateVisitor collects the gate between current node and nextNode.
  QdNode *node = nullptr, *nextNode = nullptr;

  // gateVisitor is to collect gates inside current node.
  GateVisitor gateVisitor = [this, &node, &nextNode](const Gate *gate) {
    // tNode has no next
    if (node == tNode || node == nullptr || nextNode == nullptr) return;
    // collect only the gates between current node and next node.
    if (gate->bNode == nextNode) {
      gateCellsOnNodeFields.insert(gate->a);
      gateCellsOnNodeFields.insert(gate->b);
    };
  };

  // nodeVisitor visits each node inside the nodeField.
  for (auto [v, cost] : nodeFlowField.costs.GetUnderlyingUnorderedMap()) {
    node = v;
    nextNode = nodeFlowField.nexts[v];
    m->ForEachGateInNode(node, gateVisitor);
  }
}

// Computes gate flow field.
// 1. Perform flowfield algorithm on the gate graph.
// 2. Stops earlier if all gate inside the nodes overlapping the query range are checked.
// 3. If there's a previous ComputeNodeFlowField() call, use only the gates on the node field.
int FlowFieldPathFinderImpl::ComputeGateFlowField(bool useNodeFlowField) {
  if (tNode == nullptr) return -1;
  if (m->IsObstacle(x2, y2)) return -1;

  // ensures that we can call it for multiple times
  if (gateFlowField.costs.Size()) gateFlowField.Clear();

  if (useNodeFlowField) {
    if (gateCellsOnNodeFields.size()) gateCellsOnNodeFields.clear();
    collectGateCellsOnNodeField();
  }

  // stops earlier if all gates inside the query range are checked.

  // n counts the number of gates marked by flowfield algorithm, for gates inside
  // gatesOverlappingQueryRange.
  int n = 0;

  FFA2::StopAfterFunction stopf = [this, &n](int u) {
    if (gatesInNodesOverlappingQueryRange.find(u) != gatesInNodesOverlappingQueryRange.end()) ++n;
    return n >= gatesInNodesOverlappingQueryRange.size();
  };

  // if useNodeFlowField is true, we visit only the gate cells on the node field.
  FFA2::NeighbourFilterTesterT neighbourTester = [this, useNodeFlowField](int v) {
    if (useNodeFlowField && gateCellsOnNodeFields.find(v) == gateCellsOnNodeFields.end())
      return false;
    return true;
  };

  ffa2.Compute(t, gateFlowField, ffa2NeighborsCollector, neighbourTester, stopf);
  return 0;
}

// Computes the final flow field via dynamic programming.
// Time Complexity O(dest.w * dest.h);
//
// DP in brief:
//
// 1. Suppose the cost to target for cell (x,y) is f[x][y].
// 2. For each node overlapping with the query range:
//
//     1. scan from left to right, up to bottom:
//      // directions: left-up, up, left, right-up
//      f[x][y] <= min(f[x][y], f[x-1][y-1], f[x-1][y], f[x][y-1], f[x-1][y+1]) + cost
//
//     2. scan from right to left, bottom to up:
//      // directions: right-bottom, bottom, right, left-bottom
//      f[x][y] <= min(f[x][y], f[x+1][y+1], f[x+1][y], f[x][y+1], f[x+1][y-1]) + cost
//
// This DP process is a bit faster than performing a Dijkstra on the dest rectangle.
// O(M*N) vs O(M*N*logMN), since the optimal path will always come from a cell on the
// node's borders. The optimal path should be a straight line, but there's no better
// algorithm than O(M*N).
int FlowFieldPathFinderImpl::ComputeFinalFlowFieldInQueryRange() {
  if (tNode == nullptr) return -1;
  if (m->IsObstacle(x2, y2)) return -1;

  // ensures that we can call it for multiple times
  if (finalFlowField.costs.Size()) finalFlowField.Clear();

  // f[x][y] is the cost from the cell (x,y) to the target.
  // all cells is initialized to inf.
  // for a cell on the gateFlowField, it's initialized to the cost value.
  // for an other cell inside the query range, it will be finally derived via DP.
  Final_F f;

  // from[x][y] stores which neighbour cell the min value comes from.
  // for a cell on the gateFlowField, it points to a neighbour cell on the direction to its next.
  // for an other cell inside qrange, it will finally point to a neighbour cell via DP.
  Final_From from;

  // b[x][y] indicates whether the (x,y) is on the computed gate flow field.
  Final_B b;

  // initialize f from computed gate flow field.
  for (auto [v, cost] : gateFlowField.costs.GetUnderlyingUnorderedMap()) {
    auto next = gateFlowField.nexts[v];

    auto [x, y] = m->UnpackXY(v);
    auto [x1, y1] = m->UnpackXY(next);

    f[x][y] = cost;

    // force it points to a neighbour on the direction to next,
    // if the (x,y) is inside the query range.
    if (x >= qrange.x1 && x <= qrange.x2 && y >= qrange.y1 && y <= qrange.y2) {
      int x2, y2;
      findNeighbourCellByNext(x, y, x1, y1, x2, y2);
      from[x][y] = m->PackXY(x2, y2);
    }

    // don't recompute the cells from gate flowfield.
    b[x][y] = true;
  }

  // cost unit on HV(horizonal and vertical) and diagonal directions.
  int c1 = m->Distance(0, 0, 0, 1), c2 = m->Distance(0, 0, 1, 1);

  // computes dp for each node, from node borders to inner.
  // why dp works: every node is empty (without obstacles inside it).
  for (auto node : nodesOverlappingQueryRange) {
    computeFinalFlowFieldDP1(node, f, from, b, c1, c2);
    computeFinalFlowFieldDP2(node, f, from, b, c1, c2);
  }

  // computes the flow field in the query range.
  // note: we only collect the results for cells inside the qrange.
  for (int x = qrange.x1; x <= qrange.x2; ++x) {
    for (int y = qrange.y1; y <= qrange.y2; ++y) {
      // (x1,y1) is the next cell to go.
      auto [x1, y1] = m->UnpackXY(from[x][y]);
      // f is inf: unreachable
      if (f[x][y] == inf || from[x][y] == inf) continue;

      int v = m->PackXY(x, y);
      finalFlowField.costs[v] = f[x][y];
      finalFlowField.nexts[v] = from[x][y];
    }
  }

  return 0;
}

// DP 1 of ComputeFinalFlowFieldInQueryRange inside a single leaf node.
// From left-top corner to right-bottom corner.
// c1 and c2 is the unit cost for HV and diagonal directions.
void FlowFieldPathFinderImpl::computeFinalFlowFieldDP1(const QdNode *node, Final_F &f,
                                                       Final_From &from, Final_B &b, int c1,
                                                       int c2) {
  int x1 = node->x1, y1 = node->y1, x2 = node->x2, y2 = node->y2;
  for (int x = x1; x <= x2; ++x) {
    for (int y = y1; y <= y2; ++y) {
      // skipping the cells that already computed in the gate flow field.
      if (b[x][y]) continue;

      int xfrom = -1, yfrom = -1;

      if (x > 0 && y > 0 && f[x][y] > f[x - 1][y - 1] + c2) {  // left-up
        f[x][y] = f[x - 1][y - 1] + c2;
        xfrom = x - 1, yfrom = y - 1;
      }
      if (x > 0 && f[x][y] > f[x - 1][y] + c1) {  // up
        f[x][y] = f[x - 1][y] + c1;
        xfrom = x - 1, yfrom = y;
      }
      if (y > 0 && f[x][y] > f[x][y - 1] + c1) {  // left
        f[x][y] = f[x][y - 1] + c1;
        xfrom = x, yfrom = y - 1;
      }
      if (x > 0 && y < y1 && f[x][y] > f[x - 1][y + 1] + c2) {  // right-up
        f[x][y] = f[x - 1][y + 1] + c2;
        xfrom = x - 1, yfrom = y + 1;
      }
      if (xfrom != -1) from[x][y] = m->PackXY(xfrom, yfrom);
    }
  }
}

// DP 2 of ComputeFinalFlowFieldInQueryRange  inside a single leaf node.
// From right-bottom corner to left-top corner.
// c1 and c2 is the unit cost for HV and diagonal directions.
void FlowFieldPathFinderImpl::computeFinalFlowFieldDP2(const QdNode *node, Final_F &f,
                                                       Final_From &from, Final_B &b, int c1,
                                                       int c2) {
  int x1 = node->x1, y1 = node->y1, x2 = node->x2, y2 = node->y2;
  for (int x = x2; x >= x1; --x) {
    for (int y = y2; y >= y1; --y) {
      // skipping the cells that already computed in the gate flow field.
      if (b[x][y]) continue;

      int xfrom = -1, yfrom = -1;

      if (x < x2 && y < y2 && f[x][y] > f[x + 1][y + 1] + c2) {  // right-bottom
        f[x][y] = f[x + 1][y + 1] + c2;
        xfrom = x + 1, yfrom = y + 1;
      }
      if (x < x2 && f[x][y] > f[x + 1][y] + c1) {  // bottom
        f[x][y] = f[x + 1][y] + c1;
        xfrom = x + 1, yfrom = y;
      }
      if (y < y2 && f[x][y] > f[x][y + 1] + c1) {  // right
        f[x][y] = f[x][y + 1] + c1;
        xfrom = x, yfrom = y + 1;
      }
      if (x < x2 && y > 0 && f[x][y] > f[x + 1][y - 1] + c2) {  // left-bottom
        f[x][y] = f[x + 1][y - 1] + c2;
        xfrom = x + 1, yfrom = y - 1;
      }
      if (xfrom != -1) from[x][y] = m->PackXY(xfrom, yfrom);
    }
  }
}

// (x,y) is a cell on the gateFlowField.
// (x1,y1) is the next cell that (x,y) points to.
// (x2,y2) is the result to compute, a neighbour cell of (x,y) on the direction to (x1,y1).
//
//  (x,y)
//     \
//      * (x2,y2)
//       \
//      (x1,y1)
//
void FlowFieldPathFinderImpl::findNeighbourCellByNext(int x, int y, int x1, int y1, int &x2,
                                                      int &y2) {
  int dx = x1 - x, dy = y1 - y;

  if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) {
    // fast check: (x1,y1) is the neighbour.
    x2 = x1;
    y2 = y1;
    return;
  }

  // We draw a straight line from (x,y) to (x1,y1)
  // but we just stop the draw until the second cell, that is the neighbour.
  CellCollector collector = [&x2, &y2, x, y](int x3, int y3) {
    if (x3 == x && y3 == y) return;
    x2 = x3;
    y2 = y3;
  };
  ComputeStraightLine(x, y, x1, y1, collector, 2);
}

void FlowFieldPathFinderImpl::VisitCellFlowField(const CellFlowField &cellFlowField,
                                                 UnpackedCellFlowFieldVisitor &visitor) const {
  for (auto [v, cost] : cellFlowField.costs.GetUnderlyingUnorderedMap()) {
    auto next = cellFlowField.nexts[v];
    auto [x, y] = m->UnpackXY(v);
    auto [xNext, yNext] = m->UnpackXY(next);
    visitor(x, y, xNext, yNext, cost);
  }
}

}  // namespace internal
}  // namespace qdpf
