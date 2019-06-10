#include "connection_box_lookahead_map.h"

#include <vector>
#include <queue>

#include "connection_box.h"
#include "rr_node.h"
#include "router_lookahead_map_utils.h"
#include "globals.h"
#include "vtr_math.h"
#include "vtr_time.h"
#include "echo_files.h"

/* we're profiling routing cost over many tracks for each wire type, so we'll
 * have many cost entries at each |dx|,|dy| offset. There are many ways to
 * "boil down" the many costs at each offset to a single entry for a given
 * (wire type, chan_type) combination we can take the smallest cost, the
 * average, median, etc. This define selects the method we use.
 *
 * See e_representative_entry_method */
#define REPRESENTATIVE_ENTRY_METHOD SMALLEST

#define REF_X 25
#define REF_Y 23

static int signum(int x) {
    if (x > 0) return 1;
    if (x < 0)
        return -1;
    else
        return 0;
}

typedef std::vector<std::pair<std::pair<int, int>, Cost_Entry>> t_routing_cost_map;
static void run_dijkstra(int start_node_ind,
                         t_routing_cost_map* cost_map);

class CostMap {
  public:
    void set_segment_count(size_t seg_count) {
        cost_map_.clear();
        offset_.clear();
        cost_map_.resize(seg_count);
        offset_.resize(seg_count);

        const auto& device_ctx = g_vpr_ctx.device();
        segment_map_.resize(device_ctx.rr_nodes.size());
        for (size_t i = 0; i < segment_map_.size(); ++i) {
            auto& from_node = device_ctx.rr_nodes[i];

            int from_cost_index = from_node.cost_index();
            int from_seg_index = device_ctx.rr_indexed_data[from_cost_index].seg_index;

            segment_map_[i] = from_seg_index;
        }
    }

    int node_to_segment(int from_node_ind) {
        return segment_map_[from_node_ind];
    }

    Cost_Entry find_cost(int from_seg_index, int delta_x, int delta_y) const {
        VTR_ASSERT(from_seg_index >= 0 && from_seg_index < (ssize_t)offset_.size());
        int dx = delta_x - offset_[from_seg_index].first;
        int dy = delta_y - offset_[from_seg_index].second;
        const auto& cost_map = cost_map_[from_seg_index];

        if (dx < 0) {
            dx = 0;
        }
        if (dy < 0) {
            dy = 0;
        }

        if (dx >= (ssize_t)cost_map.dim_size(0)) {
            dx = cost_map.dim_size(0) - 1;
        }
        if (dy >= (ssize_t)cost_map.dim_size(1)) {
            dy = cost_map.dim_size(1) - 1;
        }

        return cost_map_[from_seg_index][dx][dy];
    }

    void set_cost_map(int from_seg_index,
                      const t_routing_cost_map& cost_map,
                      e_representative_entry_method method) {
        VTR_ASSERT(from_seg_index >= 0 && from_seg_index < (ssize_t)offset_.size());

        // Find coordinate offset for this segment.
        int min_dx = 0;
        int min_dy = 0;
        int max_dx = 0;
        int max_dy = 0;
        for (const auto& entry : cost_map) {
            min_dx = std::min(entry.first.first, min_dx);
            min_dy = std::min(entry.first.second, min_dy);

            max_dx = std::max(entry.first.first, max_dx);
            max_dy = std::max(entry.first.second, max_dy);
        }

        offset_[from_seg_index].first = min_dx;
        offset_[from_seg_index].second = min_dy;
        size_t dim_x = max_dx - min_dx + 1;
        size_t dim_y = max_dy - min_dy + 1;

        vtr::NdMatrix<Expansion_Cost_Entry, 2> expansion_cost_map(
            {dim_x, dim_y});

        for (const auto& entry : cost_map) {
            int x = entry.first.first - min_dx;
            int y = entry.first.second - min_dy;
            expansion_cost_map[x][y].add_cost_entry(
                method, entry.second.delay,
                entry.second.congestion);
        }

        cost_map_[from_seg_index] = vtr::NdMatrix<Cost_Entry, 2>(
            {dim_x, dim_y});

        /* set the lookahead cost map entries with a representative cost
         * entry from routing_cost_map */
        for (unsigned ix = 0; ix < expansion_cost_map.dim_size(0); ix++) {
            for (unsigned iy = 0; iy < expansion_cost_map.dim_size(1); iy++) {
                cost_map_[from_seg_index][ix][iy] = expansion_cost_map[ix][iy].get_representative_cost_entry(method);
            }
        }

        /* find missing cost entries and fill them in by copying a nearby cost entry */
        for (unsigned ix = 0; ix < expansion_cost_map.dim_size(0); ix++) {
            for (unsigned iy = 0; iy < expansion_cost_map.dim_size(1); iy++) {
                Cost_Entry cost_entry = cost_map_[from_seg_index][ix][iy];

                if (!cost_entry.valid()) {
                    Cost_Entry copied_entry = get_nearby_cost_entry(
                        from_seg_index,
                        offset_[from_seg_index].first + ix,
                        offset_[from_seg_index].second + iy);
                    cost_map_[from_seg_index][ix][iy] = copied_entry;
                }
            }
        }
    }

    Cost_Entry get_nearby_cost_entry(int segment_index, int x, int y) {
        /* compute the slope from x,y to 0,0 and then move towards 0,0 by one
         * unit to get the coordinates of the cost entry to be copied */

        float slope;
        int copy_x, copy_y;
        if (x == 0 || y == 0) {
            slope = std::numeric_limits<float>::infinity();
            copy_x = x - signum(x);
            copy_y = y - signum(y);
        } else {
            slope = (float)y / (float)x;
            if (slope >= 1.0) {
                copy_y = y - signum(y);
                copy_x = vtr::nint((float)y / slope);
            } else {
                copy_x = x - signum(x);
                copy_y = vtr::nint((float)x * slope);
            }
        }

        Cost_Entry copy_entry = find_cost(segment_index, copy_x, copy_y);

        /* if the entry to be copied is also empty, recurse */
        if (copy_entry.valid()) {
            return copy_entry;
        } else if (copy_x == 0 && copy_y == 0) {
            return Cost_Entry();
        }

        return get_nearby_cost_entry(segment_index, copy_x, copy_y);
    }

    void print_cost_map(const std::vector<t_segment_inf>& segment_inf,
                        const char* fname) {
        FILE* fp = vtr::fopen(fname, "w");
        for (size_t iseg = 0; iseg < cost_map_.size(); iseg++) {
            fprintf(fp, "Seg %s(%zu) (%d, %d)\n", segment_inf.at(iseg).name.c_str(),
                    iseg,
                    offset_[iseg].first,
                    offset_[iseg].second);
            for (size_t iy = 0; iy < cost_map_[iseg].dim_size(1); iy++) {
                for (size_t ix = 0; ix < cost_map_[iseg].dim_size(0); ix++) {
                    fprintf(fp, "%.4g,\t",
                            cost_map_[iseg][ix][iy].delay);
                }
                fprintf(fp, "\n");
            }
            fprintf(fp, "\n\n");
        }

        fclose(fp);
    }

  private:
    std::vector<vtr::NdMatrix<Cost_Entry, 2>> cost_map_;
    std::vector<std::pair<int, int>> offset_;
    std::vector<int> segment_map_;
};

static CostMap g_cost_map;

class StartNode {
  public:
    StartNode(int start_x, int start_y, t_rr_type rr_type, int seg_index)
        : start_x_(start_x)
        , start_y_(start_y)
        , rr_type_(rr_type)
        , seg_index_(seg_index)
        , index_(0) {}
    int get_next_node() {
        const auto& device_ctx = g_vpr_ctx.device();
        const std::vector<int>& channel_node_list = device_ctx.rr_node_indices[rr_type_][start_x_][start_y_][0];

        for (; index_ < channel_node_list.size(); index_++) {
            int node_ind = channel_node_list[index_];

            if (node_ind == OPEN || device_ctx.rr_nodes[node_ind].capacity() == 0) {
                continue;
            }

            const std::pair<size_t, size_t>* loc = device_ctx.connection_boxes.find_canonical_loc(node_ind);
            if (loc == nullptr) {
                continue;
            }

            int node_cost_ind = device_ctx.rr_nodes[node_ind].cost_index();
            int node_seg_ind = device_ctx.rr_indexed_data[node_cost_ind].seg_index;
            if (node_seg_ind == seg_index_) {
                index_ += 1;
                return node_ind;
            }
        }

        return UNDEFINED;
    }

  private:
    int start_x_;
    int start_y_;
    t_rr_type rr_type_;
    int seg_index_;
    size_t index_;
};

// Minimum size of search for channels to profile.  kMinProfile results
// in searching x = [0, kMinProfile], and y = [0, kMinProfile[.
//
// Making this value larger will increase the sample size, but also the runtime
// to produce the lookahead.
static constexpr int kMinProfile = 1;

// Maximum size of search for channels to profile.  Once search is outside of
// kMinProfile distance, lookahead will stop searching once:
//  - At least one channel has been profiled
//  - kMaxProfile is exceeded.
static constexpr int kMaxProfile = 7;

void compute_connection_box_lookahead(
    const std::vector<t_segment_inf>& segment_inf) {
    size_t num_segments = segment_inf.size();
    vtr::ScopedStartFinishTimer timer("Computing connection box lookahead map");

    /* free previous delay map and allocate new one */
    g_cost_map.set_segment_count(segment_inf.size());

    /* run Dijkstra's algorithm for each segment type & channel type combination */
    for (int iseg = 0; iseg < (ssize_t)num_segments; iseg++) {
        VTR_LOG("Creating cost map for %s(%d)\n",
                segment_inf[iseg].name.c_str(), iseg);
        /* allocate the cost map for this iseg/chan_type */
        t_routing_cost_map cost_map;

        int count = 0;

        int dx = 0;
        int dy = 0;
        //int start_x = vtr::nint(device_ctx.grid.width()/2);
        //int start_y = vtr::nint(device_ctx.grid.height()/2);
        int start_x = REF_X;
        int start_y = REF_Y;
        while ((count == 0 && dx < kMaxProfile) || dy <= kMinProfile) {
            for (e_rr_type chan_type : {CHANX, CHANY}) {
                StartNode start_node(start_x + dx, start_y + dy, chan_type, iseg);

                for (int start_node_ind = start_node.get_next_node();
                     start_node_ind != UNDEFINED;
                     start_node_ind = start_node.get_next_node()) {
                    count += 1;

                    /* run Dijkstra's algorithm */
                    run_dijkstra(start_node_ind, &cost_map);
                }
            }

            if (dy < dx) {
                dy += 1;
            } else {
                dx += 1;
            }
        }

        if (count == 0) {
            VTR_LOG_WARN("Segment %s(%d) found no start_node_ind\n",
                         segment_inf[iseg].name.c_str(), iseg);
        }

        /* boil down the cost list in routing_cost_map at each coordinate to a
         * representative cost entry and store it in the lookahead cost map */
        g_cost_map.set_cost_map(iseg, cost_map,
                                REPRESENTATIVE_ENTRY_METHOD);
    }

    if (getEchoEnabled() && isEchoFileEnabled(E_ECHO_LOOKAHEAD_MAP)) {
        g_cost_map.print_cost_map(segment_inf, getEchoFileName(E_ECHO_LOOKAHEAD_MAP));
    }
}

float get_connection_box_lookahead_map_cost(int from_node_ind,
                                            int to_node_ind,
                                            float criticality_fac) {
    if (from_node_ind == to_node_ind) {
        return 0.f;
    }

    auto& device_ctx = g_vpr_ctx.device();

    std::pair<size_t, size_t> from_location;
    std::pair<size_t, size_t> to_location;
    auto to_node_type = device_ctx.rr_nodes[to_node_ind].type();

    if (to_node_type == SINK) {
        const auto& sink_to_ipin = device_ctx.connection_boxes.find_sink_connection_boxes(to_node_ind);
        if (sink_to_ipin.ipin_count > 1) {
            float cost = std::numeric_limits<float>::infinity();
            // Find cheapest cost from from_node_ind to IPINs for this SINK.
            for (int i = 0; i < sink_to_ipin.ipin_count; ++i) {
                cost = std::min(cost,
                                get_connection_box_lookahead_map_cost(
                                    from_node_ind,
                                    sink_to_ipin.ipin_nodes[i], criticality_fac));
            }

            return cost;
        } else if (sink_to_ipin.ipin_count == 1) {
            to_node_ind = sink_to_ipin.ipin_nodes[0];
            if (from_node_ind == to_node_ind) {
                return 0.f;
            }
        } else {
            return std::numeric_limits<float>::infinity();
        }
    }

    if (device_ctx.rr_nodes[to_node_ind].type() == IPIN) {
        ConnectionBoxId box_id;
        std::pair<size_t, size_t> box_location;
        bool found = device_ctx.connection_boxes.find_connection_box(
            to_node_ind, &box_id, &box_location);
        if (!found) {
            VPR_THROW(VPR_ERROR_ROUTE, "No connection box for IPIN %d", to_node_ind);
        }

        to_location = box_location;
    } else {
        const std::pair<size_t, size_t>* to_canonical_loc = device_ctx.connection_boxes.find_canonical_loc(to_node_ind);
        if (!to_canonical_loc) {
            VPR_THROW(VPR_ERROR_ROUTE, "No canonical loc for %d", to_node_ind);
        }

        to_location = *to_canonical_loc;
    }

    const std::pair<size_t, size_t>* from_canonical_loc = device_ctx.connection_boxes.find_canonical_loc(from_node_ind);
    if (from_canonical_loc == nullptr) {
        VPR_THROW(VPR_ERROR_ROUTE, "No canonical loc for %d (to %d)",
                  from_node_ind, to_node_ind);
    }

    ssize_t dx = ssize_t(from_canonical_loc->first) - ssize_t(to_location.first);
    ssize_t dy = ssize_t(from_canonical_loc->second) - ssize_t(to_location.second);

    int from_seg_index = g_cost_map.node_to_segment(from_node_ind);
    Cost_Entry cost_entry = g_cost_map.find_cost(from_seg_index, dx, dy);
    float expected_delay = cost_entry.delay;
    float expected_congestion = cost_entry.congestion;

    float expected_cost = criticality_fac * expected_delay + (1.0 - criticality_fac) * expected_congestion;
    return expected_cost;
}

/* runs Dijkstra's algorithm from specified node until all nodes have been
 * visited. Each time a pin is visited, the delay/congestion information
 * to that pin is stored to an entry in the routing_cost_map */
static void run_dijkstra(int start_node_ind,
                         t_routing_cost_map* routing_cost_map) {
    auto& device_ctx = g_vpr_ctx.device();

    /* a list of boolean flags (one for each rr node) to figure out if a
     * certain node has already been expanded */
    std::vector<bool> node_expanded(device_ctx.rr_nodes.size(), false);
    /* for each node keep a list of the cost with which that node has been
     * visited (used to determine whether to push a candidate node onto the
     * expansion queue */
    std::vector<float> node_visited_costs(device_ctx.rr_nodes.size(), -1.0);
    /* a priority queue for expansion */
    std::priority_queue<PQ_Entry> pq;

    /* first entry has no upstream delay or congestion */
    PQ_Entry first_entry(start_node_ind, UNDEFINED, 0, 0, 0, true);

    pq.push(first_entry);

    const std::pair<size_t, size_t>* from_canonical_loc = device_ctx.connection_boxes.find_canonical_loc(start_node_ind);
    if (from_canonical_loc == nullptr) {
        VPR_THROW(VPR_ERROR_ROUTE, "No canonical location of node %d",
                  start_node_ind);
    }

    /* now do routing */
    while (!pq.empty()) {
        PQ_Entry current = pq.top();
        pq.pop();

        int node_ind = current.rr_node_ind;

        /* check that we haven't already expanded from this node */
        if (node_expanded[node_ind]) {
            continue;
        }

        /* if this node is an ipin record its congestion/delay in the routing_cost_map */
        if (device_ctx.rr_nodes[node_ind].type() == IPIN) {
            ConnectionBoxId box_id;
            std::pair<size_t, size_t> box_location;
            bool found = device_ctx.connection_boxes.find_connection_box(
                node_ind, &box_id, &box_location);
            if (!found) {
                VPR_THROW(VPR_ERROR_ROUTE, "No connection box for IPIN %d", node_ind);
            }

            int delta_x = ssize_t(from_canonical_loc->first) - ssize_t(box_location.first);
            int delta_y = ssize_t(from_canonical_loc->second) - ssize_t(box_location.second);

            routing_cost_map->push_back(std::make_pair(
                std::make_pair(delta_x, delta_y),
                Cost_Entry(
                    current.delay,
                    current.congestion_upstream)));
        }

        expand_dijkstra_neighbours(current, node_visited_costs, node_expanded, pq);
        node_expanded[node_ind] = true;
    }
}
