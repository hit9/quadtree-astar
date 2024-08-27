// Source Code: https://github.com/hit9/quadtree-pathfinding
// License: BSD. Chao Wang, hit9[At]icloud.com.

#include "qdpf.hpp"

namespace qdpf {

//////////////////////////////////////
/// QuadtreeMapX
//////////////////////////////////////

QuadtreeMapX::QuadtreeMapX(int w, int h, DistanceCalculator distance,
                           TerrainTypesChecker terrainChecker, QuadtreeMapXSettings settings,
                           int step, StepFunction stepf, int maxNodeWidth, int maxNodeHeight)
    : impl(internal::QuadtreeMapXImpl(w, h, distance, terrainChecker, settings, step, stepf,
                                      maxNodeWidth, maxNodeHeight)) {}

void QuadtreeMapX::Build() { impl.Build(); }
void QuadtreeMapX::Update(int x, int y) { impl.Update(x, y); }
void QuadtreeMapX::Compute() { impl.Compute(); }
const internal::QuadtreeMap *QuadtreeMapX::Get(int agentSize, int terrainTypes) const {
  return impl.Get(agentSize, terrainTypes);
}

//////////////////////////////////////
/// AStarPathFinder
//////////////////////////////////////

AStarPathFinder::AStarPathFinder(const QuadtreeMapX &mx)
    : mx(mx), impl(internal::AStarPathFinderImpl(mx.impl.N())) {}

int AStarPathFinder::Reset(int x1, int y1, int x2, int y2, int agentSize, int terrainTypes) {
  auto m = mx.Get(agentSize, terrainTypes);
  if (m == nullptr) return -1;
  impl.Reset(m, x1, y1, x2, y2);
  return 0;
}

int AStarPathFinder::ComputeNodeRoutes() { return impl.ComputeNodeRoutes(); }
std::size_t AStarPathFinder::NodePathSize() const { return impl.NodePath().size(); }

void AStarPathFinder::VisitComputedNodeRoutes(NodeVisitor &visitor) const {
  for (auto [node, _] : impl.NodePath()) visitor(node);
}

int AStarPathFinder::ComputeGateRoutes(CellCollector &collector, bool useNodePath) {
  return impl.ComputeGateRoutes(collector, useNodePath);
}

void AStarPathFinder::ComputePathToNextRouteCell(int x1, int y1, int x2, int y2,
                                                 CellCollector &collector) const {
  impl.ComputePathToNextRouteCell(x1, y1, x2, y2, collector);
}

//////////////////////////////////////
/// FlowFieldPathFinder
//////////////////////////////////////

FlowFieldPathFinder::FlowFieldPathFinder(const QuadtreeMapX &mx)
    : mx(mx), impl(internal::FlowFieldPathFinderImpl(mx.impl.N())) {}

int FlowFieldPathFinder::Reset(int x2, int y2, const Rectangle &dest, int agentSize,
                               int walkableterrainTypes) {
  auto m = mx.Get(agentSize, walkableterrainTypes);
  if (m == nullptr) return -1;
  impl.Reset(m, x2, y2, dest);
  return 0;
}

void FlowFieldPathFinder::ComputeNodeFlowField() { impl.ComputeNodeFlowField(); }
void FlowFieldPathFinder::VisitComputedNodeFlowField(NodeFlowFieldVisitor &visitor) {
  // TODO
}

}  // namespace qdpf
