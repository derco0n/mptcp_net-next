/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2019 Facebook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * Include file for sample Host Bandwidth Manager (HBM) BPF programs
 */
#define KBUILD_MODNAME "foo"
#include <stddef.h>
#include <stdbool.h>
#include <uapi/linux/bpf.h>
#include <uapi/linux/if_ether.h>
#include <uapi/linux/if_packet.h>
#include <uapi/linux/ip.h>
#include <uapi/linux/ipv6.h>
#include <uapi/linux/in.h>
#include <uapi/linux/tcp.h>
#include <uapi/linux/filter.h>
#include <uapi/linux/pkt_cls.h>
#include <net/ipv6.h>
#include <net/inet_ecn.h>
#include "bpf_endian.h"
#include "bpf_helpers.h"
#include "hbm.h"

#define DROP_PKT	0
#define ALLOW_PKT	1
#define TCP_ECN_OK	1

#ifndef HBM_DEBUG  // Define HBM_DEBUG to enable debugging
#undef bpf_printk
#define bpf_printk(fmt, ...)
#endif

#define INITIAL_CREDIT_PACKETS	100
#define MAX_BYTES_PER_PACKET	1500
#define MARK_THRESH		(40 * MAX_BYTES_PER_PACKET)
#define DROP_THRESH		(80 * 5 * MAX_BYTES_PER_PACKET)
#define LARGE_PKT_DROP_THRESH	(DROP_THRESH - (15 * MAX_BYTES_PER_PACKET))
#define MARK_REGION_SIZE	(LARGE_PKT_DROP_THRESH - MARK_THRESH)
#define LARGE_PKT_THRESH	120
#define MAX_CREDIT		(100 * MAX_BYTES_PER_PACKET)
#define INIT_CREDIT		(INITIAL_CREDIT_PACKETS * MAX_BYTES_PER_PACKET)

// rate in bytes per ns << 20
#define CREDIT_PER_NS(delta, rate) ((((u64)(delta)) * (rate)) >> 20)

struct bpf_map_def SEC("maps") queue_state = {
	.type = BPF_MAP_TYPE_CGROUP_STORAGE,
	.key_size = sizeof(struct bpf_cgroup_storage_key),
	.value_size = sizeof(struct hbm_vqueue),
};
BPF_ANNOTATE_KV_PAIR(queue_state, struct bpf_cgroup_storage_key,
		     struct hbm_vqueue);

struct bpf_map_def SEC("maps") queue_stats = {
	.type = BPF_MAP_TYPE_ARRAY,
	.key_size = sizeof(u32),
	.value_size = sizeof(struct hbm_queue_stats),
	.max_entries = 1,
};
BPF_ANNOTATE_KV_PAIR(queue_stats, int, struct hbm_queue_stats);

struct hbm_pkt_info {
	int	cwnd;
	int	rtt;
	bool	is_ip;
	bool	is_tcp;
	short	ecn;
};

static int get_tcp_info(struct __sk_buff *skb, struct hbm_pkt_info *pkti)
{
	struct bpf_sock *sk;
	struct bpf_tcp_sock *tp;

	sk = skb->sk;
	if (sk) {
		sk = bpf_sk_fullsock(sk);
		if (sk) {
			if (sk->protocol == IPPROTO_TCP) {
				tp = bpf_tcp_sock(sk);
				if (tp) {
					pkti->cwnd = tp->snd_cwnd;
					pkti->rtt = tp->srtt_us >> 3;
					return 0;
				}
			}
		}
	}
	return 1;
}

static __always_inline void hbm_get_pkt_info(struct __sk_buff *skb,
					     struct hbm_pkt_info *pkti)
{
	struct iphdr iph;
	struct ipv6hdr *ip6h;

	pkti->cwnd = 0;
	pkti->rtt = 0;
	bpf_skb_load_bytes(skb, 0, &iph, 12);
	if (iph.version == 6) {
		ip6h = (struct ipv6hdr *)&iph;
		pkti->is_ip = true;
		pkti->is_tcp = (ip6h->nexthdr == 6);
		pkti->ecn = (ip6h->flow_lbl[0] >> 4) & INET_ECN_MASK;
	} else if (iph.version == 4) {
		pkti->is_ip = true;
		pkti->is_tcp = (iph.protocol == 6);
		pkti->ecn = iph.tos & INET_ECN_MASK;
	} else {
		pkti->is_ip = false;
		pkti->is_tcp = false;
		pkti->ecn = 0;
	}
	if (pkti->is_tcp)
		get_tcp_info(skb, pkti);
}

static __always_inline void hbm_init_vqueue(struct hbm_vqueue *qdp, int rate)
{
		bpf_printk("Initializing queue_state, rate:%d\n", rate * 128);
		qdp->lasttime = bpf_ktime_get_ns();
		qdp->credit = INIT_CREDIT;
		qdp->rate = rate * 128;
}

static __always_inline void hbm_update_stats(struct hbm_queue_stats *qsp,
					     int len,
					     unsigned long long curtime,
					     bool congestion_flag,
					     bool drop_flag,
					     bool cwr_flag,
					     bool ecn_ce_flag,
					     struct hbm_pkt_info *pkti,
					     int credit)
{
	int rv = ALLOW_PKT;

	if (qsp != NULL) {
		// Following is needed for work conserving
		__sync_add_and_fetch(&(qsp->bytes_total), len);
		if (qsp->stats) {
			// Optionally update statistics
			if (qsp->firstPacketTime == 0)
				qsp->firstPacketTime = curtime;
			qsp->lastPacketTime = curtime;
			__sync_add_and_fetch(&(qsp->pkts_total), 1);
			if (congestion_flag) {
				__sync_add_and_fetch(&(qsp->pkts_marked), 1);
				__sync_add_and_fetch(&(qsp->bytes_marked), len);
			}
			if (drop_flag) {
				__sync_add_and_fetch(&(qsp->pkts_dropped), 1);
				__sync_add_and_fetch(&(qsp->bytes_dropped),
						     len);
			}
			if (ecn_ce_flag)
				__sync_add_and_fetch(&(qsp->pkts_ecn_ce), 1);
			if (pkti->cwnd) {
				__sync_add_and_fetch(&(qsp->sum_cwnd),
						     pkti->cwnd);
				__sync_add_and_fetch(&(qsp->sum_cwnd_cnt), 1);
			}
			if (pkti->rtt)
				__sync_add_and_fetch(&(qsp->sum_rtt),
						     pkti->rtt);
			__sync_add_and_fetch(&(qsp->sum_credit), credit);

			if (drop_flag)
				rv = DROP_PKT;
			if (cwr_flag)
				rv |= 2;
			if (rv == DROP_PKT)
				__sync_add_and_fetch(&(qsp->returnValCount[0]),
						     1);
			else if (rv == ALLOW_PKT)
				__sync_add_and_fetch(&(qsp->returnValCount[1]),
						     1);
			else if (rv == 2)
				__sync_add_and_fetch(&(qsp->returnValCount[2]),
						     1);
			else if (rv == 3)
				__sync_add_and_fetch(&(qsp->returnValCount[3]),
						     1);
		}
	}
}
