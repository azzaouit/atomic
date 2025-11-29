#ifndef NODE_H
#define NODE_H

#include "rdma.h"

/* Per-node context */
struct node_ctx {
  uint16_t id;
  uint32_t seed;
  struct rdma_ctx r;
  pthread_mutex_t lock;
};

/* Initialize node context */
int node_init(struct node_ctx *ctx, struct config *c);

/* Destroy context */
void node_destroy(struct node_ctx *ctx);

/* Distributed atomic operations */
int64_t fetch_and_add(struct node_ctx *ctx);
int64_t test_and_set(struct node_ctx *ctx, uint32_t slot);

#endif /* NODE_H */
