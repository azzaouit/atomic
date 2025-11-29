#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "net_map.h"
#include "node.h"

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

    struct node_ctx n;
    struct config c = {
        .n = sizeof(net_cfg) / sizeof(net_cfg[0]),
        .host_id = host_id,
        .rdma_device = 0,
        .c = (struct node_config *)net_cfg,
    };

    assert(!node_init(&n, &c));

    fprintf(stderr, "Host ID,Slot,Elapsed\n");
    int64_t ret = 0;
    while (ret != -ENOMEM) {
        uint64_t start_time = ts_us();
        ret = fetch_and_add(&n);
        uint64_t elapsed = ts_us() - start_time;
        if (ret >= 0) fprintf(stderr, "%hu,%ld,%lu\n", host_id, ret, elapsed);
    }

#ifdef TRACK_SLOTS
    DUMP_CSV(stdout, &n);
#endif

    node_destroy(&n);
    return 0;
}
