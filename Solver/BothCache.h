#ifndef BOTHCACHE_H
#define BOTHCACHE_H

#include "Solver.h"

class BothCache
{
    public:
        BothCache(Solver& solver);
        const bool tryBoth(const vector<TransCache>& cache);

    private:
        Solver& solver;
};

#endif //BOTHCACHE_H