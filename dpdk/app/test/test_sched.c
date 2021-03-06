/*-
 *   BSD LICENSE
 * 
 *   Copyright(c) 2010-2013 Intel Corporation. All rights reserved.
 *   All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 * 
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <cmdline_parse.h>

#include "test.h"

#if defined(RTE_LIBRTE_SCHED) && defined(RTE_ARCH_X86_64)

#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_byteorder.h>
#include <rte_sched.h>


#define VERIFY(exp,fmt,args...)                    	                \
		if (!(exp)) {                                               \
			printf(fmt, ##args);                                    \
			return -1;                                              \
		}


#define SUBPORT 	0
#define PIPE 		1
#define TC 			2
#define QUEUE 		3

static struct rte_sched_subport_params subport_param[] = {
	{
		.tb_rate = 1250000000,
		.tb_size = 1000000,

		.tc_rate = {1250000000, 1250000000, 1250000000, 1250000000},
		.tc_period = 10,
	},
};

static struct rte_sched_pipe_params pipe_profile[] = {
	{ /* Profile #0 */
		.tb_rate = 305175,
		.tb_size = 1000000,

		.tc_rate = {305175, 305175, 305175, 305175},
		.tc_period = 40,

		.wrr_weights = {1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1},
	},
};

static struct rte_sched_port_params port_param = {
	.name = "port_0",
	.socket = 0, /* computed */
	.rate = 0, /* computed */
	.mtu = 1522,
	.frame_overhead = RTE_SCHED_FRAME_OVERHEAD_DEFAULT,
	.n_subports_per_port = 1,
	.n_pipes_per_subport = 4096,
	.qsize = {64, 64, 64, 64},
	.pipe_profiles = pipe_profile,
	.n_pipe_profiles = 1,
};

#define NB_MBUF          32
#define MAX_PACKET_SZ    2048
#define MBUF_SZ (MAX_PACKET_SZ + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define PKT_BURST_SZ     32
#define MEMPOOL_CACHE_SZ PKT_BURST_SZ
#define SOCKET           0


static struct rte_mempool *
create_mempool(void)
{
	struct rte_mempool * mp;

	mp = rte_mempool_lookup("test_sched");
	if (!mp)
		mp = rte_mempool_create("test_sched",
				NB_MBUF,
				MBUF_SZ,
				MEMPOOL_CACHE_SZ,
				sizeof(struct rte_pktmbuf_pool_private),
				rte_pktmbuf_pool_init,
				NULL,
				rte_pktmbuf_init,
				NULL,
				SOCKET,
				0);

	return mp;
}

static void
prepare_pkt(struct rte_mbuf *mbuf)
{
	struct ether_hdr *eth_hdr;
	struct vlan_hdr *vlan1, *vlan2;
	struct ipv4_hdr *ip_hdr;

	/* Simulate a classifier */
	eth_hdr = rte_pktmbuf_mtod(mbuf, struct ether_hdr *);
	vlan1 = (struct vlan_hdr *)(&eth_hdr->ether_type );
	vlan2 = (struct vlan_hdr *)((uintptr_t)&eth_hdr->ether_type + sizeof(struct vlan_hdr));
	eth_hdr = (struct ether_hdr *)((uintptr_t)&eth_hdr->ether_type + 2 *sizeof(struct vlan_hdr));
	ip_hdr = (struct ipv4_hdr *)((uintptr_t)eth_hdr +  sizeof(eth_hdr->ether_type));

	vlan1->vlan_tci = rte_cpu_to_be_16(SUBPORT);
	vlan2->vlan_tci = rte_cpu_to_be_16(PIPE);
	eth_hdr->ether_type =  rte_cpu_to_be_16(ETHER_TYPE_IPv4);
	ip_hdr->dst_addr = IPv4(0,0,TC,QUEUE);


	rte_sched_port_pkt_write(mbuf, SUBPORT, PIPE, TC, QUEUE, e_RTE_METER_YELLOW);

	/* 64 byte packet */
	mbuf->pkt.pkt_len  = 60;
	mbuf->pkt.data_len = 60;
}


/**
 * test main entrance for library sched
 */
int 
test_sched(void)
{
	struct rte_mempool *mp = NULL;
	struct rte_sched_port *port = NULL;
	uint32_t pipe;
	struct rte_mbuf *in_mbufs[10];
	struct rte_mbuf *out_mbufs[10];
	int i;

	int err;

	mp = create_mempool();

	port_param.socket = 0;
	port_param.rate = (uint64_t) 10000 * 1000 * 1000 / 8;
	port_param.name = "port_0";

	port = rte_sched_port_config(&port_param);
	VERIFY(port != NULL, "Error config sched port\n");

	
	err = rte_sched_subport_config(port, SUBPORT, subport_param);
	VERIFY(err == 0, "Error config sched, err=%d\n", err);

	for (pipe = 0; pipe < port_param.n_pipes_per_subport; pipe ++) {
		err = rte_sched_pipe_config(port, SUBPORT, pipe, 0);
		VERIFY(err == 0, "Error config sched pipe %u, err=%d\n", pipe, err);
	}

	for (i = 0; i < 10; i++) {
		in_mbufs[i] = rte_pktmbuf_alloc(mp);
		prepare_pkt(in_mbufs[i]);
	}


	err = rte_sched_port_enqueue(port, in_mbufs, 10);
	VERIFY(err == 10, "Wrong enqueue, err=%d\n", err);

	err = rte_sched_port_dequeue(port, out_mbufs, 10);
	VERIFY(err == 10, "Wrong dequeue, err=%d\n", err);

	for (i = 0; i < 10; i++) {
		enum rte_meter_color color;
		uint32_t subport, traffic_class, queue;

		color = rte_sched_port_pkt_read_color(out_mbufs[i]);
		VERIFY(color == e_RTE_METER_YELLOW, "Wrong color\n");

		rte_sched_port_pkt_read_tree_path(out_mbufs[i],
				&subport, &pipe, &traffic_class, &queue);

		VERIFY(subport == SUBPORT, "Wrong subport\n");
		VERIFY(pipe == PIPE, "Wrong pipe\n");
		VERIFY(traffic_class == TC, "Wrong traffic_class\n");
		VERIFY(queue == QUEUE, "Wrong queue\n");

	}


	struct rte_sched_subport_stats subport_stats;
	uint32_t tc_ov;
	rte_sched_subport_read_stats(port, SUBPORT, &subport_stats, &tc_ov);
	//VERIFY(subport_stats.n_pkts_tc[TC-1] == 10, "Wrong subport stats\n");

	struct rte_sched_queue_stats queue_stats;
	uint16_t qlen;
	rte_sched_queue_read_stats(port, QUEUE, &queue_stats, &qlen);
	//VERIFY(queue_stats.n_pkts == 10, "Wrong queue stats\n");

	rte_sched_port_free(port);

	return 0;
}

#else /* RTE_LIBRTE_SCHED */

int
test_sched(void)
{
	printf("The Scheduler library is not included in this build\n");
	return 0;
}
#endif /* RTE_LIBRTE_SCHED */
