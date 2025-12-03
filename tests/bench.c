#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "net_map.h"
#include "node.h"

struct thread_args {
    struct node_ctx *ctx;
    int thread_id;
    int requests_per_thread;
    int host_id;
};

void *worker_thread(void *arg) {
    struct thread_args *args = (struct thread_args *)arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(args->host_id * 16 + args->thread_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    for (int i = 0; i < args->requests_per_thread; ++i) {
        uint64_t start_time = ts_us();
        int64_t ret = fetch_and_add(args->ctx);
        uint64_t elapsed = ts_us() - start_time;
        if (ret >= 0)
            fprintf(stderr, "%d,%d,%ld,%lu\n", args->host_id, args->thread_id,
                    ret, elapsed);
        else if (ret == -ENOMEM)
            break;
    }

    FAA_LOG("[%d] Worker thread %d exiting", args->ctx->id, args->thread_id);

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr,
                "Usage: %s <host_id> <num_threads> <requests_per_thread>\n",
                argv[0]);
        return 1;
    }

    int npeers = sizeof(net_cfg) / sizeof(net_cfg[0]);
    int host_id = atoi(argv[1]);
    int num_threads = atoi(argv[2]);
    int requests_per_thread = atoi(argv[3]);

    struct config c = {
        .n = npeers,
        .host_id = host_id,
        .rdma_device = 0,
        .c = (struct node_config *)net_cfg,
    };

    struct node_ctx n;
    assert(!node_init(&n, &c));

    if (num_threads > 0) {
        pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
        struct thread_args *args =
            malloc(num_threads * sizeof(struct thread_args));
        for (int i = 0; i < num_threads; i++) {
            args[i].ctx = &n;
            args[i].thread_id = i;
            args[i].requests_per_thread = requests_per_thread;
            args[i].host_id = host_id;
            pthread_create(threads + i, NULL, worker_thread, args + i);
        }
        for (int i = 0; i < num_threads; i++) pthread_join(threads[i], NULL);
        free(threads);
        free(args);
    }

    // Wait for last slot to fill
    while (!((volatile uint64_t *)n.r.shared_mem->slots)[MAX_SLOTS - 1])
        sleep(1);

    FAA_LOG("Node %d exiting", n.id);

    node_destroy(&n);
    return 0;
}
