#ifndef NAT_PROCESSING_H
#define NAT_PROCESSING_H

#include <rte_mbuf.h>

int process_packet(struct rte_mbuf *m, uint16_t port_id);
void aging_sessions(void);

#endif