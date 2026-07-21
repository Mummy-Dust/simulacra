#pragma once
#include <stdint.h>
// Fleet population share. In a mesh of K decoy nodes sharing one environment, each node runs 1/K of
// the fleet-wide population so the aggregate matches observed density (design law 4: population-match)
// and the crowd originates from K physical points instead of one. K is static: -DSIMULACRA_FLEET_SIZE=K,
// default 1 (standalone). Pure: no radio, no timers -> host-unit-tested like the other pure cores.

#ifndef SIMULACRA_FLEET_SIZE
#define SIMULACRA_FLEET_SIZE 1
#endif

// Fleet size K, clamped to >= 1 so the divisor is always safe (a bad/zero config becomes 1).
int fleet_pop_size(void);

// round(target / k), floored at 1 for target > 0 (a node never zeroes a whole population class).
// Pure in k -> testable at any k without recompiling. target <= 0 or k <= 1 returns target unchanged.
int fleet_pop_share_k(int target, int k);

// This node's share of a fleet-wide target: fleet_pop_share_k(target, fleet_pop_size()).
int fleet_pop_share(int target);

// Live fleet size: distinct peer NODES heard from recently (fleet_node_count), + this node.
// Falls back to 1 (standalone) with no peers heard -- the correct, safe default, achieved for
// free (fleet_node_count returns 0 when nothing has been noted yet).
int fleet_pop_live_size(uint32_t now_ms);
