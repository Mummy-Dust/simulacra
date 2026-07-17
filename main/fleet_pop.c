#include "fleet_pop.h"

int fleet_pop_size(void)
{
    int k = SIMULACRA_FLEET_SIZE;
    return k < 1 ? 1 : k;
}

int fleet_pop_share_k(int target, int k)
{
    if (target <= 0 || k <= 1) return target;
    int s = (target + k / 2) / k;   // round to nearest
    return s < 1 ? 1 : s;           // never drop a whole population class to zero
}

int fleet_pop_share(int target)
{
    return fleet_pop_share_k(target, fleet_pop_size());
}
