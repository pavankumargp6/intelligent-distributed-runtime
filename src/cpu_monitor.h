#ifndef CPU_MONITOR_H
#define CPU_MONITOR_H

int read_cpu_ticks(int pid, long *total_ticks);

double calculate_cpu_usage(long start_ticks,
                           long end_ticks,
                           double elapsed_seconds);

#endif
