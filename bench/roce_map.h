#ifndef NET_MAP_H
#define NET_MAP_H

#include "config.h"

/* Static definition of the network map */
#define TCP_PORT (8888)
#define IB_PORT (1)
#define GID_IDX (0)

static const struct node_config net_cfg[] = {
    {
        .ip = {2, 2, 168, 192},
        .id = 0,
        .tcp_port = TCP_PORT,
        .ib_port = IB_PORT,
        .gid_index = GID_IDX,
    },
    {
        .ip = {3, 2, 168, 192},
        .id = 1,
        .tcp_port = TCP_PORT + 1,
        .ib_port = IB_PORT,
        .gid_index = GID_IDX,
    },
};

#endif /* NET_MAP_H */
