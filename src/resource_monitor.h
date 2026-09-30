#ifndef RESOURCE_MONITOR_H
#define RESOURCE_MONITOR_H

#include "process_info.h"

struct ResourceSnapshot
{
    struct ProcessInfo process;
    double cpu_usage;
};

int collect_resource_snapshot(int pid,
                              struct ResourceSnapshot *snapshot);

#endif
