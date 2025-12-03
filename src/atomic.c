#include <immintrin.h>
#include <stdio.h>

#include "rdma.h"

#define FAST_QUORUM(c) ((c->n * 3 + 3) / 4)
#define CLASSIC_QUORUM(c) (((c)->n / 2) + 1)

static int __thread_count = 0;
static __thread int __tid = -1;

/* Broadcast atomic RDMA CAS */
int rdma_bcas(struct rdma_ctx *r, uint32_t slot, uint64_t swp) {
    struct config *c = r->c;
    off_t offset = c->n * __tid;
    uint64_t *thread_results = r->results + (c->n + 1) * __tid;

    uint64_t local_result =
        __sync_val_compare_and_swap(&r->shared_mem->slots[slot], 0, swp);
    int local_won = (local_result == 0);
    int successes = local_won;

    for (int i = 0; i < c->n; ++i) {
        if (i != c->host_id) {
            struct ibv_send_wr *bad_wr = NULL, wr = {};
            struct remote_attr *a = r->ra + offset + i;
            uint64_t remote_slot_addr =
                a->addr + offsetof(typeof(*r->shared_mem), slots) +
                (slot * sizeof(uint64_t));

            struct ibv_sge list = {};
            list.addr = (uint64_t)(thread_results + i);
            list.length = sizeof(uint64_t);
            list.lkey = r->mr[1]->lkey;

            wr.wr_id = ((uint64_t)slot << 16) | i;
            wr.sg_list = &list;
            wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
            wr.num_sge = 1;
            wr.send_flags = IBV_SEND_SIGNALED;
            wr.wr.atomic.remote_addr = remote_slot_addr;
            wr.wr.atomic.rkey = a->rkey;
            wr.wr.atomic.compare_add = 0;
            wr.wr.atomic.swap = swp;
            if (ibv_post_send(r->qp[offset + i], &wr, &bad_wr)) {
                FAA_LOG("ibv_post_send failed for node %d, errno=%d", i, errno);
                perror("ibv_post_send");
            }
        }
    }

    struct ibv_wc wc[c->n];
    int left = c->n - 1, n = 0;
    while (1)
        if ((n = ibv_poll_cq(r->cq[__tid], left, wc)) > 0)
            for (int i = 0; i < n; ++i) {
                uint32_t completion_slot = (uint32_t)(wc[i].wr_id >> 16);
                int node_id = wc[i].wr_id & 0xFFFF;
                if (wc[i].status != IBV_WC_SUCCESS) {
                    FAA_LOG("[%hu:%d] Work completion error: %s", r->c->host_id,
                            __tid, ibv_wc_status_str(wc[i].status));
                } else if (completion_slot == slot) {
                    successes += (thread_results[node_id] == 0);
                    if (successes >= FAST_QUORUM(c)) return !local_won;
                    --left;
                }
            }

    return (successes < FAST_QUORUM(c)) ? -1 : !local_won;
}

/* Slow path: paxos recovery */
int rdma_slow_path(struct rdma_ctx *r, uint32_t slot, uint64_t ballot,
                   uint64_t proposed_value) {
    struct config *c = r->c;
    off_t offset = c->n * __tid;
    uint64_t *thread_results = r->results + (c->n + 1) * __tid;
    struct prep_res *results = r->prepares + offset;
    memset(results, 0, sizeof(struct prep_res) * c->n);

    // Phase 2a (Prepare): Read current values
    results[c->host_id].ballot =
        *(volatile uint64_t *)&r->shared_mem->slots[slot];
    results[c->host_id].success = 1;
    for (int i = 0; i < c->n; ++i)
        if (i != c->host_id) {
            struct remote_attr *ra = r->ra + offset + i;
            uint64_t remote_slot_addr =
                ra->addr + offsetof(typeof(*r->shared_mem), slots) +
                (slot * sizeof(uint64_t));
            struct ibv_sge sge = {.addr = (uint64_t)(thread_results + i),
                                  .length = sizeof(uint64_t),
                                  .lkey = r->mr[1]->lkey};
            struct ibv_send_wr wr = {
                .wr_id = i,
                .sg_list = &sge,
                .num_sge = 1,
                .opcode = IBV_WR_RDMA_READ,
                .send_flags = IBV_SEND_SIGNALED,
                .wr.rdma = {.remote_addr = remote_slot_addr, .rkey = ra->rkey}};
            struct ibv_send_wr *bad_wr;
            if (ibv_post_send(r->qp[offset + i], &wr, &bad_wr)) {
                FAA_LOG("ibv_post_send failed for node %d, errno=%d", i, errno);
                perror("ibv_post_send");
            }
        }

    struct ibv_wc wc[c->n];
    int completed = 0;
    int num_posted = c->n - 1;
    while (completed < num_posted) {
        int n = ibv_poll_cq(r->cq[__tid], num_posted - completed, wc);
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                int remote_idx = wc[i].wr_id;
                if (wc[i].status != IBV_WC_SUCCESS) {
                    FAA_LOG("[%hu:%d] Work completion error: %s", r->c->host_id,
                            __tid, ibv_wc_status_str(wc[i].status));
                    results[remote_idx].success = 0;
                } else {
                    results[remote_idx].ballot = thread_results[remote_idx];
                    results[remote_idx].success = 1;
                }
            }
            completed += n;
        }
    }

    // Check if fast quorum already exists
    uint64_t ballot_counts[c->n];
    memset(ballot_counts, 0, sizeof(ballot_counts));
    int unique_ballots = 0;
    for (int i = 0; i < c->n; ++i)
        if (results[i].success && results[i].ballot > 0) {
            int found = 0;
            for (int j = 0; j < unique_ballots; ++j)
                if (ballot_counts[j] == results[i].ballot) {
                    found = 1;
                    break;
                }
            if (!found && unique_ballots < c->n)
                ballot_counts[unique_ballots++] = results[i].ballot;
        }
    for (int i = 0; i < unique_ballots; ++i) {
        int count = 0;
        uint16_t owner = ballot_counts[i] & 0xFFFF;
        for (int j = 0; j < c->n; ++j)
            if (results[j].success && results[j].ballot == ballot_counts[i])
                ++count;
        if (count >= FAST_QUORUM(c)) return (owner != c->host_id);
    }

    // Calculate promises
    int promises = 0;
    uint64_t highest_ballot = 0;
    uint64_t highest_value = 0;
    for (int i = 0; i < c->n; ++i)
        if (results[i].success && ballot >= results[i].ballot) {
            ++promises;
            if (results[i].ballot > highest_ballot) {
                highest_ballot = results[i].ballot;
                highest_value = results[i].ballot;
            }
        }
    if (promises < CLASSIC_QUORUM(c)) return -1;

    // Phase 2b (Accept)
    uint64_t proposal = (highest_ballot > 0) ? highest_value : proposed_value;
    uint64_t cmp = results[c->host_id].ballot;
    uint64_t res =
        __sync_val_compare_and_swap(&r->shared_mem->slots[slot], cmp, proposal);
    int accepts = (res == cmp);
    for (int i = 0; i < c->n; ++i)
        if (i != c->host_id) {
            struct remote_attr *ra = r->ra + offset + i;
            uint64_t remote_slot_addr =
                ra->addr + offsetof(typeof(*r->shared_mem), slots) +
                (slot * sizeof(uint64_t));
            uint64_t expected = results[i].ballot;
            struct ibv_sge sge = {.addr = (uint64_t)(thread_results + i),
                                  .length = sizeof(uint64_t),
                                  .lkey = r->mr[1]->lkey};
            struct ibv_send_wr wr = {
                .wr_id = i,
                .sg_list = &sge,
                .num_sge = 1,
                .opcode = IBV_WR_ATOMIC_CMP_AND_SWP,
                .send_flags = IBV_SEND_SIGNALED,
                .wr.atomic = {.remote_addr = remote_slot_addr,
                              .rkey = ra->rkey,
                              .compare_add = expected,
                              .swap = proposal}};
            struct ibv_send_wr *bad_wr;
            if (ibv_post_send(r->qp[offset + i], &wr, &bad_wr)) {
                FAA_LOG("ibv_post_send failed for node %d, errno=%d", i, errno);
                perror("ibv_post_send");
            }
        }

    completed = 0;
    num_posted = c->n - 1;
    while (completed < num_posted) {
        int n = ibv_poll_cq(r->cq[__tid], num_posted - completed, wc);
        if (n > 0) {
            for (int i = 0; i < n; ++i)
                if (wc[i].status != IBV_WC_SUCCESS)
                    FAA_LOG("[%hu:%d] Work completion error: %s", r->c->host_id,
                            __tid, ibv_wc_status_str(wc[i].status));
                else {
                    int remote_idx = wc[i].wr_id;
                    uint64_t returned = thread_results[remote_idx];
                    uint64_t expected = results[remote_idx].ballot;
                    if (returned == expected) ++accepts;
                }
            completed += n;
        }
    }

    uint16_t winner = proposal & 0xffff;
    return (accepts >= CLASSIC_QUORUM(c)) ? c->host_id != winner : -1;
}

/* Read next slot from frontier node */
uint64_t rdma_get_next_slot(struct rdma_ctx *r) {
    if (__tid == -1) __tid = __sync_fetch_and_add(&__thread_count, 1);
    if (__tid < 0 || __tid >= MAX_THREADS) {
        FAA_LOG("MAX_THREADS exceeded");
        return -1;
    }

    uint64_t *result_ptr = r->results + (r->c->n + 1) * __tid + r->c->n;
    off_t offset = r->c->n * __tid + FRONTIER_NODE;
    struct remote_attr *ra = r->ra + offset;
    uint64_t remote_frontier_addr =
        ra->addr + offsetof(typeof(*r->shared_mem), frontier);

    struct ibv_sge sge = {.addr = (uint64_t)result_ptr,
                          .length = sizeof(uint64_t),
                          .lkey = r->mr[1]->lkey};

    struct ibv_send_wr wr = {.wr_id = 0,
                             .sg_list = &sge,
                             .num_sge = 1,
                             .opcode = IBV_WR_ATOMIC_FETCH_AND_ADD,
                             .send_flags = IBV_SEND_SIGNALED,
                             .wr.atomic = {.remote_addr = remote_frontier_addr,
                                           .rkey = ra->rkey,
                                           .compare_add = 1}};

    struct ibv_send_wr *bad_wr;
    if (ibv_post_send(r->fqp[offset], &wr, &bad_wr)) {
        FAA_LOG("ibv_post_send failed for node errno=%d", errno);
        perror("ibv_post_send");
        return -1;
    }

    struct ibv_wc wc;
    while (1)
        if (ibv_poll_cq(r->fcq[__tid], 1, &wc) > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                FAA_LOG("[%hu:%d] Work completion error: %s", r->c->host_id,
                        __tid, ibv_wc_status_str(wc.status));
                return (uint64_t)-1;
            }
            return *result_ptr;
        }

    return -1;
}
