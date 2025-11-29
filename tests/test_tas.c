#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "net_map.h"
#include "node.h"

#define NUM_LOCKS (1000)

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <host id>\n", argv[0]);
        return 1;
    }

    int host_id = atoi(argv[1]);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(host_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    struct node_ctx ctx;
    struct config c = {
        .n = sizeof(net_cfg) / sizeof(net_cfg[0]),
        .host_id = host_id,
        .rdma_device = 0,
        .c = (struct node_config *)net_cfg,
    };

    assert(!node_init(&ctx, &c));

    fprintf(stderr, "Host ID,Lock ID,Result,Latency\n");
    for (int lock_id = 0; lock_id < NUM_LOCKS; lock_id++) {
        uint64_t start = ts_us();
        int64_t result = test_and_set(&ctx, lock_id);
        uint64_t elapsed = ts_us() - start;
        if (result == 0)
            fprintf(stderr, "%d,%d,%ld,%lu\n", host_id, lock_id, result,
                    elapsed);
    }

    node_destroy(&ctx);
    return 0;
}
