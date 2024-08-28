// Source Code: https://github.com/hit9/quadtree-pathfinding
// License: BSD. Chao Wang, hit9[At]icloud.com.

#include "pathfinder_flowfield.hpp"

#include <cassert>
#include <functional>
#include <vector>

namespace qdpf {
namespace internal {

FlowFieldPathFinderImpl::FlowFieldPathFinderImpl(int n) : ffa1(FFA1(n)), ffa2(FFA2(n)) {
  // nodesInDestCollector is used to collect all nodes locating (overlaping) in the dest rectangle.
  nodesInDestCollector = [this](const QdNode *node) {
    // we care about only leaf nodes with no obstacles
    if (node->isLeaf && node->objects.empty()) nodesInDest.insert(node);
  };
  // gatesInDestCollector is used to collect all gate cells locating in the dest rectangle, for a
  // single node.
  gatesInDestCollector = [this](const Gate *gate) {
    auto [x, y] = this->m->UnpackXY(gate->a);
    if (x >= this->dest.x1 && x <= this->dest.x2 && y >= this->dest.y1 && y <= this->dest.y2) {
      gatesInDest.insert(gate->a);
    }
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

void FlowFieldPathFinderImpl::Reset(const QuadtreeMap *m, int x2, int y2, const Rectangle &dest) {
  // debug mode, checks m, it's nullptr if mapx didn't find one.
  assert(m != nullptr);

  // resets the attributes.
  this->m = m;
  this->x2 = x2, this->y2 = y2;
  this->dest = dest;  // copy updated
  tNode = nullptr;

  // the given rectangle dest is invalid.
  if (!(dest.x1 <= dest.x2 && dest.y1 <= dest.y2)) return;

  t = m->PackXY(x2, y2);
  tNode = m->FindNode(x2, y2);
  // tNode is not found, indicating that t is out of bounds.
  if (tNode == nullptr) return;

  // clears the old results.
  nodeFlowField.Clear();
  gateFlowField.Clear();
  finalFlowField.Clear();

  // find all nodes in the dest range.
  nodesInDest.clear();
  m->NodesInRange(dest, nodesInDestCollector);

  // find all gates in the dest range.
  gatesInDest.clear();
  for (auto node : nodesInDest) {  // node is const QdNode*
    m->ForEachGateInNode(node, gatesInDestCollector);
  }

  gateCellsOnNodeFields.clear();

  // Rebuild the tmp graph.
  PathFinderHelper::Reset(this->m);
  // Add the target cell to the gate graph
  bool tIsGate = m->IsGateCell(tNode, t);
  if (!tIsGate) {
    AddCellToNodeOnTmpGraph(t, tNode);
    // t is a virtual gate cell now.
    // we should check if it is inside the dest rectangle,
    // and add it to gatesInDest if it is.
    if (x2 >= dest.x1 && x2 <= dest.x2 && y2 >= dest.y1 && y2 <= dest.y2) {
      gatesInDest.insert(t);
    }
  }

  // Special case:
  // if the target node overlaps the dest rectangle, we should connects the overlaping cells to
  // the target, since the best path is a straight line then.
  Rectangle tNodeRectangle{tNode->x1, tNode->y1, tNode->x2, tNode->y2};
  Rectangle overlap;
  auto hasOverlap = GetOverlap(tNodeRectangle, dest, overlap);
  if (hasOverlap) {
    for (int x = overlap.x1; x <= overlap.x2; ++x) {
      for (int y = overlap.y1; y <= overlap.y2; ++y) {
        int u = m->PackXY(x, y);
        // detail notice is: u should not be a gate cell,
        // since we already connect all gate cells with t.
        if (u != t && m->IsGateCell(tNode, u)) {
          ConnectCellsOnTmpGraph(u, t);
          // We should consider u as a new tmp "gate" cell.
          gatesInDest.insert(u);
        }
      }
    }
  }
}

int FlowFieldPathFinderImpl::ComputeNodeFlowField() {
  // unreachable
  if (tNode == nullptr) return -1;
  // the target is an obstacle, unreachable
  if (m->IsObstacle(x2, y2)) return -1;

  // stops earlier if all nodes inside the dest rectangle are checked.
  int numNodesInDestChecked = 0;
  FFA1::StopAfterFunction stopf = [&numNodesInDestChecked, this](QdNode *node) {
    if (nodesInDest.find(node) != nodesInDest.end()) ++numNodesInDestChecked;
    return numNodesInDestChecked >= nodesInDest.size();
  };

  // Compute flowfield on the node graph.
  ffa1.Compute(tNode, nodeFlowField, ffa1NeighborsCollector, nullptr, stopf);
  return 0;
}

// collects the gate cells on the node flow field if ComputeNodeFlowField is successfully called
// and ComputeGateFlowField is called with useNodeFlowField is set true.
void FlowFieldPathFinderImpl::collectGateCellsOnNodeField() {
  gateCellsOnNodeFields.insert(t);

  // gateVisitor collects the gate between node and nextNode.
  QdNode *node = nullptr, *nextNode = nullptr;

  GateVisitor gateVisitor = [this, &node, &nextNode](const Gate *gate) {
    // tNode has no next.
    if (node == tNode || node == nullptr || nextNode == nullptr) return;
    // collect only the gates between node and next node.
    if (gate->bNode == nextNode) {
      gateCellsOnNodeFields.insert(gate->a);
      gateCellsOnNodeFields.insert(gate->b);
    };
  };

  // nodeVisitor visits each node inside the nodeField.
  NodeFlowField::Visitor nodeVisitor = [this, &node, &nextNode, &gateVisitor](
                                           QdNode *v, QdNode *next, int cost) {
    node = v;
    nextNode = next;
    m->ForEachGateInNode(node, gateVisitor);
  };
  nodeFlowField.ForEach(nodeVisitor);
}

int FlowFieldPathFinderImpl::ComputeGateFlowField(bool useNodeFlowField) {
  if (tNode == nullptr) return -1;
  if (m->IsObstacle(x2, y2)) return -1;

  if (useNodeFlowField) collectGateCellsOnNodeField();

  // stops earlier if all gates inside the dest rectangle are checked.
  int numGatesInDestChecked = 0;
  FFA2::StopAfterFunction stopf = [this, &numGatesInDestChecked](int u) {
    if (gatesInDest.find(u) != gatesInDest.end()) ++numGatesInDestChecked;
    return numGatesInDestChecked >= gatesInDest.size();
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
// 1. Suppose the cost to target for cell (x,y) is f[x][y].
// 2. For each node in dest rectangle (excepts the target's node):
//     1. scan from left to right, up to bottom:
//      // directions: left-up, up, left, right-up
//      f[x][y] <= min(f[x][y], f[x-1][y-1], f[x-1][y], f[x][y-1], f[x-1][y+1]) + cost
//     2. scan from right to left, bottom to up:
//      // directions: right-bottom, bottom, right, left-bottom
//      f[x][y] <= min(f[x][y], f[x+1][y+1], f[x+1][y], f[x][y+1], f[x+1][y-1]) + cost
// 3. Then calculates the flow field via f.
//
// This DP process is a bit faster than performing a Dijkstra on the dest rectangle.
// O(M*N) vs O(M*N*logMN), since the optimal path must come from a cell on the node's borders.
// The optimal path should be a straight line, but there's no better algorithm than O(M*N).
int FlowFieldPathFinderImpl::ComputeCellFlowFieldInDestRectangle() {
  if (tNode == nullptr) return -1;
  if (m->IsObstacle(x2, y2)) return -1;

  // width and height of rectangle.
  int w = dest.y2 - dest.y1 + 1, h = dest.x2 - dest.x1 + 1;

  // f[x][y] is the cost from (x+dest.x1, y+dest.x2) to target.
  // the size of f is : h x w
  Final_F f(h, std::vector<int>(w, inf));

  // from[x][y] stores which gate cell the min value comes from.
  Final_From from(h, std::vector<int>(w, inf));

  // initialize f from computed gate flow field.
  CellFlowField::Visitor visitor = [this, &f, &from](int v, int next, int cost) {
    auto [x, y] = m->UnpackXY(v);
    x -= dest.x1;
    y -= dest.y1;
    f[x][y] = cost;
    from[x][y] = next;
  };

  // cost unit on HV(horizonal and vertical) and diagonal directions.
  int c1 = m->Distance(0, 0, 0, 1), c2 = m->Distance(0, 0, 1, 1);

  // computes dp for each node.
  // because every node is empty (without obstacles inside it),
  // so the dp works there.
  for (auto node : nodesInDest) {
    // Excludes the tNode, since all cells within the overlaping area are already computed via
    // Dijkstra in the previous ComputeGateFlowField() call.
    if (node != tNode) {
      computeFinalFlowFieldDP1(node, f, from, c1, c2);
      computeFinalFlowFieldDP2(node, f, from, c1, c2);
    }
  }

  // computes the flow field.
  for (int x = 0; x < h; ++x) {
    for (int y = 0; y < w; ++y) {
      if (from[x][y] == inf) continue;
      int v = m->PackXY(x + dest.x1, y + dest.y1);
      finalFlowField.SetCost(v, f[x][y]);
      finalFlowField.SetNext(v, from[x][y]);
    }
  }

  return 0;
}

// DP 1 of ComputeCellFlowFieldInDestRectangle inside a single leaf node.
// From left-top corner to right-bottom corner.
// c1 and c2 is the unit cost for HV and diagonal directions.
void FlowFieldPathFinderImpl::computeFinalFlowFieldDP1(const QdNode *node, Final_F &f,
                                                       Final_From &from, int c1, int c2) {
  // x,y's bounds in this node.
  int x0 = std::max(0, node->x1 - dest.x1);
  int y0 = std::max(0, node->y1 - dest.y1);
  int x1 = std::min(node->x2, dest.x2) - dest.x1;
  int y1 = std::min(node->y2, dest.y2) - dest.y1;

  for (int x = x0; x <= x1; ++x) {
    for (int y = y0; y <= y1; ++y) {
      if (x > 0 && y > 0 && f[x][y] > f[x - 1][y - 1] + c2) {  // left-up
        f[x][y] = f[x - 1][y - 1] + c2;
        from[x][y] = from[x - 1][y - 1];
      }
      if (y > 0 && f[x][y] > f[x - 1][y] + c1) {  // up
        f[x][y] = f[x - 1][y] + c1;
        from[x][y] = from[x - 1][y];
      }
      if (x > 0 && f[x][y] > f[x][y - 1] + c1) {  // left
        f[x][y] = f[x][y - 1] + c1;
        from[x][y] = from[x][y - 1];
      }
      if (x > 0 && y < y1 && f[x][y] > f[x - 1][y + 1] + c2) {  // right-up
        f[x][y] = f[x - 1][y + 1] + c2;
        from[x][y] = from[x - 1][y + 1];
      }
    }
  }
}

// DP 2 of ComputeCellFlowFieldInDestRectangle  inside a single leaf node.
// From right-bottom corner to left-top corner.
// c1 and c2 is the unit cost for HV and diagonal directions.
void FlowFieldPathFinderImpl::computeFinalFlowFieldDP2(const QdNode *node, Final_F &f,
                                                       Final_From &from, int c1, int c2) {
  // x,y's bounds in this node.
  int x0 = std::max(0, node->x1 - dest.x1);
  int y0 = std::max(0, node->y1 - dest.y1);
  int x1 = std::min(node->x2, dest.x2) - dest.x1;
  int y1 = std::min(node->y2, dest.y2) - dest.y1;

  for (int x = x1; x >= x0; --x) {
    for (int y = y1; y >= y0; --y) {
      if (x < x1 && y < y1 && f[x][y] > f[x + 1][y + 1] + c2) {  // right-bottom
        f[x][y] = f[x + 1][y + 1] + c2;
        from[x][y] = from[x + 1][y + 1];
      }
      if (x < x1 && f[x][y] > f[x + 1][y] + c1) {  // bottom
        f[x][y] = f[x + 1][y] + c1;
        from[x][y] = from[x + 1][y];
      }
      if (y < y1 && f[x][y] > f[x][y + 1] + c1) {  // right
        f[x][y] = f[x][y + 1] + c1;
        from[x][y] = from[x][y + 1];
      }
      if (x < x1 && y > 0 && f[x][y] > f[x + 1][y - 1] + c2) {  // left-bottom
        f[x][y] = f[x + 1][y - 1] + c2;
        from[x][y] = from[x + 1][y - 1];
      }
    }
  }
}

void FlowFieldPathFinderImpl::VisitCellFlowField(const CellFlowField &cellFlowField,
                                                 UnpackedCellFlowFieldVisitor &visitor) const {
  CellFlowField::Visitor visitor1 = [&visitor, this](int v, int next, int cost) {
    auto [x, y] = m->UnpackXY(v);
    auto [xNext, yNext] = m->UnpackXY(next);
    visitor(x, y, xNext, yNext, cost);
  };
  cellFlowField.ForEach(visitor1);
}

}  // namespace internal
}  // namespace qdpf
