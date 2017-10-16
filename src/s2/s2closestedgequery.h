// Copyright 2013 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS-IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

// Author: ericv@google.com (Eric Veach)

#ifndef S2_S2CLOSESTEDGEQUERY_H_
#define S2_S2CLOSESTEDGEQUERY_H_

#include <memory>
#include <type_traits>
#include <vector>

#include <glog/logging.h>
#include "s2/third_party/absl/base/macros.h"
#include "s2/third_party/absl/container/inlined_vector.h"
#include "s2/util/btree/btree_set.h"
#include "s2/_fpcontractoff.h"
#include "s2/priority_queue_sequence.h"
#include "s2/s1angle.h"
#include "s2/s1chordangle.h"
#include "s2/s2cell.h"
#include "s2/s2cellid.h"
#include "s2/s2closestedgequery_base.h"
#include "s2/s2edge_distances.h"
#include "s2/s2shapeindex.h"

// S2ClosestEdgeQuery is a helper class for finding the closest edge(s) to a
// given point, edge, S2Cell, or geometry collection.  For example, given a
// set of polylines, the following code efficiently finds the closest 5 edges
// to a query point:
//
// void Test(vector<S2Polyline*> const& polylines, S2Point const& point) {
//   S2ShapeIndex index;
//   for (S2Polyline* polyline : polylines) {
//     index.Add(new S2Polyline::Shape(polyline));
//   }
//   S2ClosestEdgeQuery query(&index);
//   query.mutable_options()->set_max_edges(5);
//   S2ClosestEdgeQuery::PointTarget target(point);
//   for (auto const& result : query.FindClosestEdges(&target)) {
//     // The Result struct contains the following fields:
//     //   "distance" is the distance to the edge.
//     //   "shape_id" identifies the S2Shape containing the edge.
//     //   "edge_id" identifies the edge with the given shape.
//     // The following convenience methods may also be useful:
//     //   query.GetEdge(result) returns the endpoints of the edge.
//     //   query.Project(point, result) computes the closest point on the
//     //       result edge to the given target point.
//     int polyline_index = result.shape_id;
//     int edge_index = result.edge_id;
//     S1ChordAngle distance = result.distance;  // Use ToAngle() for S1Angle.
//     S2Shape::Edge edge = query.GetEdge(result);
//     S2Point closest_point = query.Project(point, result);
//   }
// }
//
// You can find either the k closest edges, or all edges within a given
// radius, or both (i.e., the k closest edges up to a given maximum radius).
// E.g. to find all the edges within 5 kilometers, call
//
//   query.mutable_options()->set_max_distance(
//       S2Earth::ToAngle(util::units::Kilometers(5)));
//
// By default *all* edges are returned, so you should always specify either
// max_edges() or max_distance() or both.  There is also a FindClosestEdge()
// convenience method that automatically sets max_edges() == 1 and returns
// only the closest edge.
//
// If you only need to test whether the distance is above or below a given
// threshold (e.g., 10 km), you can use the IsDistanceLess() method.  This is
// much faster than actually calculating the distance with FindClosestEdge(),
// since the implementation can stop as soon as it can prove that the minimum
// distance is either above or below the threshold.
//
// To find the closest edges to a query edge rather than a point, use:
//
//   S2ClosestEdgeQuery::EdgeTarget target(v0, v1);
//   query.FindClosestEdges(&target);
//
// Similarly you can find the closest edges to an S2Cell by using an
// S2ClosestEdgeQuery::CellTarget, and you can find the closest edges to an
// arbitrary collection of points, polylines, and polygons by using an
// S2ClosestEdgeQuery::ShapeIndexTarget.
//
// Note that by default, distances are measured to the boundaries of polygons.
// For example, if a point even is inside a polygon then its distance will be
// non-zero.  To change this behavior, call set_include_interiors(true).
//
// The implementation is designed to be fast for both simple and complex
// geometric objects.
class S2ClosestEdgeQuery {
 public:
  // See S2ClosestEdgeQueryBase for full documentation.

  // A thin wrapper around S1ChordAngle that implements the Distance concept
  // required by S2ClosestEdgeQueryBase.
  struct Distance : public S1ChordAngle {
    Distance() : S1ChordAngle() {}
    explicit Distance(S1Angle x) : S1ChordAngle(x) {}
    explicit Distance(S1ChordAngle x) : S1ChordAngle(x) {}
    static Distance Zero() { return Distance(S1ChordAngle::Zero()); }
    static Distance Infinity() { return Distance(S1ChordAngle::Infinity()); }
    static Distance Negative() { return Distance(S1ChordAngle::Negative()); }
    friend Distance operator-(Distance x, Distance y) {
      return Distance(S1ChordAngle(x) - y);
    }
    static S1Angle GetAngleBound(Distance x) {
      return x.PlusError(x.GetS1AngleConstructorMaxError()).ToAngle();
    }
  };
  using Base = S2ClosestEdgeQueryBase<Distance>;
  using Result = Base::Result;

  // See S2ClosestEdgeQueryBase for full documentation of the available options.
  class Options : public Base::Options {
   public:
    // Versions of set_max_distance() that accept S1ChordAngle / S1Angle.
    //
    // Note that only edges whose distance is *less than* "max_distance" are
    // returned.  Normally this doesn't matter, because distances are not
    // computed exactly in the first place, but if such edges are needed then
    // you can retrieve them by specifying max_distance.Successor() instead.
    void set_max_distance(S1ChordAngle max_distance);
    void set_max_distance(S1Angle max_distance);

    // Versions of set_max_error() that accept S1ChordAngle / S1Angle.
    void set_max_error(S1ChordAngle max_error);
    void set_max_error(S1Angle max_error);

    // Like set_max_distance(), except that "max_distance" is increased by the
    // maximum error in the distance calculation.  This ensures that all edges
    // whose true distance is less than "max_distance" will be returned (along
    // with some edges whose true distance is slightly greater).
    //
    // Algorithms that need to do exact distance comparisons can use this
    // option to find a set of candidate edges that can then be filtered
    // further (e.g., using s2pred::CompareEdgeDistance).
    void set_conservative_max_distance(S1ChordAngle max_distance);
  };

  // "Target" represents the geometry that the distance is measured to.  There
  // are subtypes for measuring the distance to a point, an edge, an S2Cell,
  // or an S2ShapeIndex representing an arbitrary collection of geometry.
  using Target = Base::Target;

  // Target subtype that computes the closest distance to a point.
  class PointTarget final : public Target {
   public:
    explicit PointTarget(S2Point const& point);
    int max_brute_force_edges() const override;
    S2Cap GetCapBound() const override;
    bool UpdateMinDistance(S2Point const& v0, S2Point const& v1,
                           Distance* min_dist) const override;
    bool UpdateMinDistance(S2Cell const& cell,
                           Distance* min_dist) const override;
    std::vector<int> GetContainingShapes(S2ShapeIndexBase const& index,
                                         int max_shapes) const override;

   private:
    S2Point point_;
  };

  // Target subtype that computes the closest distance to an edge.
  class EdgeTarget final : public Target {
   public:
    EdgeTarget(S2Point const& a, S2Point const& b);
    int max_brute_force_edges() const override;
    S2Cap GetCapBound() const override;
    bool UpdateMinDistance(S2Point const& v0, S2Point const& v1,
                           Distance* min_dist) const override;
    bool UpdateMinDistance(S2Cell const& cell,
                           Distance* min_dist) const override;
    std::vector<int> GetContainingShapes(S2ShapeIndexBase const& index,
                                         int max_shapes) const override;

   private:
    S2Point a_, b_;
  };

  // Target subtype that computes the closest distance to an S2Cell
  // (including the interior of the cell).
  class CellTarget final : public Target {
   public:
    explicit CellTarget(S2Cell const& cell);
    int max_brute_force_edges() const override;
    S2Cap GetCapBound() const override;
    bool UpdateMinDistance(S2Point const& v0, S2Point const& v1,
                           Distance* min_dist) const override;
    bool UpdateMinDistance(S2Cell const& cell,
                           Distance* min_dist) const override;
    std::vector<int> GetContainingShapes(S2ShapeIndexBase const& index,
                                         int max_shapes) const override;

   private:
    S2Cell cell_;
  };

  // Target subtype that computes the closest distance to an S2ShapeIndex
  // (an arbitrary collection of points, polylines, and/or polygons).
  //
  // Note that ShapeIndexTarget has its own options:
  //
  //   include_interiors()
  //     - specifies that distance will be measures to the interior of the
  //       S2ShapeIndex rather than only to its boundary.
  //
  //   brute_force()
  //     - specifies that the distances should be computed by examining every
  //       edge in the S2ShapeIndex (for testing and debugging purposes).
  //
  // These options are specified independently of the corresponding
  // S2ClosestEdgeQuery options.  For example, if include_interiors is true
  // for a ShapeIndexTarget but false for the S2ClosestEdgeQuery where the
  // target is used, then distances will be measured from the boundary of one
  // S2ShapeIndex to the boundary and interior of the other.
  //
  // The remaining S2ClosestEdgeQuery::Options are instead handled as follows:
  //
  //  - max_error() is copied from the current S2ClosestEdgeQuery, i.e. if you
  //    set query.options().max_error() then this value is automatically
  //    propagated to the ShapeIndexTarget.
  //
  //    Note that unlike the other Target subtypes, this option can affect the
  //    "distance" field of the results.  Suppose that max_edges() == 1 and
  //    max_error() == 0.01, and let the result edge be E with "distance"
  //    field d.  Then the implementation guarantees that the true distance
  //    from E to the target S2ShapeIndex is at least (d - 0.01), and
  //    furthermore no other edge E' of the query S2ShapeIndex is closer to
  //    the target S2ShapeIndex than (d - 0.01).
  //
  //    As always, this option does not affect max_distance().  Continuing the
  //    example above, if max_distance() == M then the "distance" field of the
  //    result edge satisfies (d < M) no matter how max_error() is set.
  //
  //  - max_edges() and max_distance() are set internally on every method call
  //    in order to implement the Target API.
  class ShapeIndexTarget final : public Target {
   public:
    explicit ShapeIndexTarget(S2ShapeIndex const* index);

    // Specifies that distance will be measures to the interior of the
    // S2ShapeIndex rather than only to its boundary.
    //
    // DEFAULT: false
    // TODO(ericv): Implement this, and possibly make true by default.
    bool include_interiors() const;
    void set_include_interiors(bool include_interiors);

    // Specifies that the distances should be computed by examining every edge
    // in the S2ShapeIndex (for testing and debugging purposes).
    //
    // DEFAULT: false
    bool use_brute_force() const;
    void set_use_brute_force(bool use_brute_force);

    bool set_max_error(Distance const& max_error) override;
    int max_brute_force_edges() const override;
    S2Cap GetCapBound() const override;
    bool UpdateMinDistance(S2Point const& v0, S2Point const& v1,
                           Distance* min_dist) const override;
    bool UpdateMinDistance(S2Cell const& cell,
                           Distance* min_dist) const override;
    std::vector<int> GetContainingShapes(S2ShapeIndexBase const& query_index,
                                         int max_shapes) const override;

   private:
    S2ShapeIndex const* index_;
    std::unique_ptr<S2ClosestEdgeQuery> query_;
  };

  // Convenience constructor that calls Init().  Options may be specified here
  // or changed at any time using the mutable_options() accessor method.
  explicit S2ClosestEdgeQuery(S2ShapeIndexBase const* index,
                              Options const& options = Options());

  // Default constructor; requires Init() to be called.
  S2ClosestEdgeQuery();
  ~S2ClosestEdgeQuery();

  // Initializes the query.  Options may be specified here or changed at any
  // time using the mutable_options() accessor method.
  //
  // REQUIRES: Reset() must be called if "index" is modified.
  void Init(S2ShapeIndexBase const* index, Options const& options = Options());

  // Reset the query state.  This method must be called whenever the
  // underlying S2ShapeIndex is modified.
  void Reset();

  // Return a reference to the underlying S2ShapeIndex.
  S2ShapeIndexBase const& index() const;

  // Returns the query options.  Options can be modifed between queries.
  Options const& options() const;
  Options* mutable_options();

  // Returns the closest edges to the given target that satisfy the given
  // options.  This method may be called multiple times.
  std::vector<Result> FindClosestEdges(Target* target);

  // This version can be more efficient when this method is called many times,
  // since it does not require allocating a new vector on each call.
  void FindClosestEdges(Target* target, std::vector<Result>* results);

  //////////////////////// Convenience Methods ////////////////////////

  // Returns the closest edge to the target.  If no edge satisfies the search
  // criteria, then the Result object will have distance == Infinity() and
  // shape_id == edge_id == -1.
  //
  // SIDE EFFECT: Calls mutable_options()->set_max_edges(1).
  //              All other options are unchanged.
  Result FindClosestEdge(Target* target);

  // Returns the minimum distance to the target.  If the target has no edges,
  // returns S1ChordAngle::Infinity().
  //
  // SIDE EFFECT: Calls mutable_options()->set_max_edges(1).
  //              All other options are unchanged.
  S1ChordAngle GetDistance(Target* target);

  // Returns true if the distance to the target is less than "limit".
  //
  // This method is usually *much* faster than calling GetDistance(), since it
  // is much less work to determine whether the minimum distance is above or
  // below a threshold than it is to calculate the actual minimum distance.
  //
  // SIDE EFFECT: Calls mutable_options()->set_max_distance(limit).
  // SIDE EFFECT: Calls mutable_options()->set_max_error(limit).
  // SIDE EFFECT: Calls mutable_options()->set_max_edges(1).
  bool IsDistanceLess(Target* target, S1ChordAngle limit);

  // Returns the endpoints of the given result edge.
  S2Shape::Edge GetEdge(Result const& result) const;

  // Returns the point on given result edge that is closest to "point".
  S2Point Project(S2Point const& point, Result const& result) const;

 private:
  Options options_;
  Base base_;

  S2ClosestEdgeQuery(S2ClosestEdgeQuery const&) = delete;
  void operator=(S2ClosestEdgeQuery const&) = delete;
};


//////////////////   Implementation details follow   ////////////////////


inline void S2ClosestEdgeQuery::Options::set_max_distance(
    S1ChordAngle max_distance) {
  Base::Options::set_max_distance(Distance(max_distance));
}

inline void S2ClosestEdgeQuery::Options::set_max_distance(
    S1Angle max_distance) {
  Base::Options::set_max_distance(Distance(max_distance));
}

inline void S2ClosestEdgeQuery::Options::set_max_error(S1ChordAngle max_error) {
  Base::Options::set_max_error(Distance(max_error));
}

inline void S2ClosestEdgeQuery::Options::set_max_error(S1Angle max_error) {
  Base::Options::set_max_error(Distance(max_error));
}

inline bool S2ClosestEdgeQuery::ShapeIndexTarget::include_interiors() const {
  return query_->options().include_interiors();
}

inline void S2ClosestEdgeQuery::ShapeIndexTarget::set_include_interiors(
    bool include_interiors) {
  query_->mutable_options()->set_include_interiors(include_interiors);
}

inline bool S2ClosestEdgeQuery::ShapeIndexTarget::use_brute_force() const {
  return query_->options().use_brute_force();
}

inline void S2ClosestEdgeQuery::ShapeIndexTarget::set_use_brute_force(
    bool use_brute_force) {
  query_->mutable_options()->set_use_brute_force(use_brute_force);
}

inline S2ClosestEdgeQuery::S2ClosestEdgeQuery(S2ShapeIndexBase const* index,
                                              Options const& options) {
  Init(index, options);
}

inline void S2ClosestEdgeQuery::Init(S2ShapeIndexBase const* index,
                                     Options const& options) {
  options_ = options;
  base_.Init(index);
}

inline void S2ClosestEdgeQuery::Reset() {
  base_.Reset();
}

inline S2ShapeIndexBase const& S2ClosestEdgeQuery::index() const {
  return base_.index();
}

inline S2ClosestEdgeQuery::Options const& S2ClosestEdgeQuery::options() const {
  return options_;
}

inline S2ClosestEdgeQuery::Options* S2ClosestEdgeQuery::mutable_options() {
  return &options_;
}

inline std::vector<S2ClosestEdgeQuery::Result>
S2ClosestEdgeQuery::FindClosestEdges(Target* target) {
  return base_.FindClosestEdges(target, options_);
}

inline void S2ClosestEdgeQuery::FindClosestEdges(Target* target,
                                                 std::vector<Result>* results) {
  base_.FindClosestEdges(target, options_, results);
}

inline S2ClosestEdgeQuery::Result S2ClosestEdgeQuery::FindClosestEdge(
    Target* target) {
  options_.set_max_edges(1);
  return base_.FindClosestEdge(target, options_);
}

inline S1ChordAngle S2ClosestEdgeQuery::GetDistance(Target* target) {
  return FindClosestEdge(target).distance;
}

inline S2Shape::Edge S2ClosestEdgeQuery::GetEdge(Result const& result) const {
  return index().shape(result.shape_id)->edge(result.edge_id);
}

inline S2Point S2ClosestEdgeQuery::Project(S2Point const& point,
                                           Result const& result) const {
  if (result.edge_id < 0) return point;
  auto edge = GetEdge(result);
  return S2::Project(point, edge.v0, edge.v1);
}

#endif  // S2_S2CLOSESTEDGEQUERY_H_
