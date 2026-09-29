#ifndef PROCESS_INFO_H
#define PROCESS_INFO_H

struct ProcessInfo
{
    int pid;
    int parent_pid;
    char name[64];
    char state[64];
    long memory_kb;
    int threads;
};

int read_process_info(int pid, struct ProcessInfo *process);

#endif
