/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2016 Freescale Semiconductor, Inc. All rights reserved.
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
 *     * Neither the name of  Freescale Semiconductor, Inc nor the names of its
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

/* System headers */
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <limits.h>
#include <sched.h>
#include <pthread.h>

#include <rte_config.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_interrupts.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_pci.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_alarm.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_atomic.h>
#include <rte_malloc.h>
#include <rte_ring.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "dpaa_ethdev.h"
#include "dpaa_rxtx.h"

#include <usdpaa/fsl_usd.h>
#include <usdpaa/fsl_qman.h>
#include <usdpaa/fsl_bman.h>
#include <usdpaa/of.h>
#include <usdpaa/usdpaa_netcfg.h>

#define DPAA_MBUF_TO_CONTIG_FD(_mbuf, _fd, _bpid) \
	do { \
		(_fd)->cmd = 0; \
		(_fd)->opaque_addr = 0; \
		(_fd)->opaque = QM_FD_CONTIG << DPAA_FD_FORMAT_SHIFT; \
		(_fd)->opaque |= ((_mbuf)->data_off) << DPAA_FD_OFFSET_SHIFT; \
		(_fd)->opaque |= (_mbuf)->pkt_len; \
		(_fd)->addr = (_mbuf)->buf_physaddr; \
		(_fd)->bpid = _bpid; \
	} while (0);

void  dpaa_buf_free(struct pool_info_entry *bp_info,
		    uint64_t addr)
{
	struct bm_buffer buf;
	int ret;

	PMD_TX_FREE_LOG(DEBUG, "Free 0x%lx to bpid: %d", addr, bp_info->bpid);

	bm_buffer_set64(&buf, addr);
retry:
	ret = bman_release(bp_info->bp, &buf, 1, 0);
	if (ret) {
		PMD_TX_LOG(DEBUG, " BMAN busy. Retrying...");
		cpu_spin(CPU_SPIN_BACKOFF_CYCLES);
		goto retry;
	}
}

#if (defined RTE_LIBRTE_DPAA_DEBUG_DRIVER_DISPLAY)
void dpaa_display_frame(const struct qm_fd *fd)
{
	int ii;
	char *ptr;

	printf("%s::bpid %x addr %08x%08x, format %d off %d, len %d stat %x\n",
	       __func__, fd->bpid, fd->addr_hi, fd->addr_lo, fd->format,
		fd->offset, fd->length20, fd->status);

	ptr = (char *)dpaa_mem_ptov(fd->addr);
	ptr += fd->offset;
	printf("%02x ", *ptr);
	for (ii = 1; ii < fd->length20; ii++) {
		printf("%02x ", *ptr);
		if ((ii % 16) == 0)
			printf("\n");
		ptr++;
	}
	printf("\n");
}
#else
#define dpaa_display_frame(a)
#endif

static inline void dpaa_slow_parsing(struct rte_mbuf *m __rte_unused,
				     uint64_t prs __rte_unused)
{
	PMD_RX_LOG(DEBUG, " Slow parsing");

	/*TBD:XXX: to be implemented*/
}

static inline void dpaa_eth_packet_info(struct rte_mbuf *m,
					uint64_t fd_virt_addr)
{
	struct annotations_t *annot = GET_ANNOTATIONS(fd_virt_addr);
	uint64_t prs = *((uint64_t *)(&annot->parse)) & DPAA_PARSE_MASK;

	PMD_RX_LOG(DEBUG, " Parsing mbuf: %p with annotations: %p", m, annot);

	switch (prs) {
	case DPAA_PKT_TYPE_NONE:
		m->packet_type = 0;
		break;
	case DPAA_PKT_TYPE_ETHER:
		m->packet_type = RTE_PTYPE_L2_ETHER;
		break;
	case DPAA_PKT_TYPE_IPV4:
		m->packet_type = RTE_PTYPE_L2_ETHER |
			RTE_PTYPE_L3_IPV4;
		break;
	case DPAA_PKT_TYPE_IPV6:
		m->packet_type = RTE_PTYPE_L2_ETHER |
			RTE_PTYPE_L3_IPV6;
		break;
	case DPAA_PKT_TYPE_IPV4_EXT:
		m->packet_type = RTE_PTYPE_L2_ETHER |
			RTE_PTYPE_L3_IPV4_EXT;
		break;
	case DPAA_PKT_TYPE_IPV6_EXT:
		m->packet_type = RTE_PTYPE_L2_ETHER |
			RTE_PTYPE_L3_IPV6_EXT;
		break;
	case DPAA_PKT_TYPE_IPV4_TCP:
		m->packet_type = RTE_PTYPE_L2_ETHER |
			RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_TCP;
		break;
	case DPAA_PKT_TYPE_IPV6_TCP:
		m->packet_type = RTE_PTYPE_L2_ETHER |
			RTE_PTYPE_L3_IPV6 | RTE_PTYPE_L4_TCP;
		break;
	case DPAA_PKT_TYPE_IPV4_UDP:
		m->packet_type = RTE_PTYPE_L2_ETHER |
			RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_UDP;
		break;
	case DPAA_PKT_TYPE_IPV6_UDP:
		m->packet_type = RTE_PTYPE_L2_ETHER |
			RTE_PTYPE_L3_IPV6 | RTE_PTYPE_L4_UDP;
		break;
	case DPAA_PKT_TYPE_IPV4_SCTP:
		m->packet_type = RTE_PTYPE_L2_ETHER |
			RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_SCTP;
		break;
	case DPAA_PKT_TYPE_IPV6_SCTP:
		m->packet_type = RTE_PTYPE_L2_ETHER |
			RTE_PTYPE_L3_IPV6 | RTE_PTYPE_L4_SCTP;
		break;
	/* More switch cases can be added */
	default:
		dpaa_slow_parsing(m, prs);
	}

	m->tx_offload = annot->parse.ip_off[0];
	m->tx_offload |= (annot->parse.l4_off - annot->parse.ip_off[0])
					<< DPAA_PKT_L3_LEN_SHIFT;

	/* Set the hash values */
	m->hash.rss = (uint32_t)(rte_be_to_cpu_64(annot->hash));
	m->ol_flags = PKT_RX_RSS_HASH;

	/* Check if Vlan is present */
	if (prs & DPAA_PARSE_VLAN_MASK)
		m->ol_flags |= PKT_RX_VLAN_PKT;
}

static inline void dpaa_checksum(struct rte_mbuf *mbuf)
{
	struct ether_hdr *eth_hdr = rte_pktmbuf_mtod(mbuf, struct ether_hdr *);
	char *l3_hdr = (char *)eth_hdr + mbuf->l2_len;
	struct ipv4_hdr *ipv4_hdr = (struct ipv4_hdr *)l3_hdr;
	struct ipv6_hdr *ipv6_hdr = (struct ipv6_hdr *)l3_hdr;

	PMD_TX_LOG(DEBUG, "Calculating checksum for mbuf: %p", mbuf);

	if (((mbuf->packet_type & RTE_PTYPE_L3_MASK) == RTE_PTYPE_L3_IPV4)
		|| ((mbuf->packet_type & RTE_PTYPE_L3_MASK)
			== RTE_PTYPE_L3_IPV4_EXT)) {
		ipv4_hdr = (struct ipv4_hdr *)l3_hdr;
		ipv4_hdr->hdr_checksum = 0;
		ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);
	} else if (((mbuf->packet_type & RTE_PTYPE_L3_MASK) == RTE_PTYPE_L3_IPV6)
		 || ((mbuf->packet_type & RTE_PTYPE_L3_MASK)
			== RTE_PTYPE_L3_IPV6_EXT))
		ipv6_hdr = (struct ipv6_hdr *)l3_hdr;

	if ((mbuf->packet_type & RTE_PTYPE_L4_MASK) == RTE_PTYPE_L4_TCP) {
		struct tcp_hdr *tcp_hdr = (struct tcp_hdr *)(l3_hdr + mbuf->l3_len);
		tcp_hdr->cksum = 0;
		if (eth_hdr->ether_type == htons(ETHER_TYPE_IPv4))
			tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, tcp_hdr);
		else /* assume ethertype == ETHER_TYPE_IPv6 */
			tcp_hdr->cksum = rte_ipv6_udptcp_cksum(ipv6_hdr, tcp_hdr);
	} else if ((mbuf->packet_type & RTE_PTYPE_L4_MASK) == RTE_PTYPE_L4_UDP) {
		struct udp_hdr *udp_hdr = (struct udp_hdr *)(l3_hdr + mbuf->l3_len);
		udp_hdr->dgram_cksum = 0;
		if (eth_hdr->ether_type == htons(ETHER_TYPE_IPv4))
			udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);
		else /* assume ethertype == ETHER_TYPE_IPv6 */
			udp_hdr->dgram_cksum = rte_ipv6_udptcp_cksum(ipv6_hdr, udp_hdr);
	}
}

static inline void dpaa_checksum_offload(struct rte_mbuf *mbuf,
					 struct qm_fd *fd, char *prs_buf)
{
	struct dpaa_eth_parse_results_t *prs;

	PMD_TX_LOG(DEBUG, " Offloading checksum for mbuf: %p", mbuf);

	prs = GET_TX_PRS(prs_buf);
	prs->l3r = 0;
	prs->l4r = 0;
	if (((mbuf->packet_type & RTE_PTYPE_L3_MASK) == RTE_PTYPE_L3_IPV4)
		|| ((mbuf->packet_type & RTE_PTYPE_L3_MASK)
			== RTE_PTYPE_L3_IPV4_EXT))
		prs->l3r = DPAA_L3_PARSE_RESULT_IPV4;
	else if (((mbuf->packet_type & RTE_PTYPE_L3_MASK) == RTE_PTYPE_L3_IPV6)
		 || ((mbuf->packet_type & RTE_PTYPE_L3_MASK)
			== RTE_PTYPE_L3_IPV6_EXT))
		prs->l3r = DPAA_L3_PARSE_RESULT_IPV6;

	if ((mbuf->packet_type & RTE_PTYPE_L4_MASK) == RTE_PTYPE_L4_TCP)
		prs->l4r = DPAA_L4_PARSE_RESULT_TCP;
	else if ((mbuf->packet_type & RTE_PTYPE_L4_MASK) == RTE_PTYPE_L4_UDP)
		prs->l4r = DPAA_L4_PARSE_RESULT_UDP;

	prs->ip_off[0] = mbuf->l2_len;
	prs->l4_off = mbuf->l3_len + mbuf->l2_len;
	/* Enable L3 (and L4, if TCP or UDP) HW checksum*/
	fd->cmd = DPAA_FD_CMD_RPD | DPAA_FD_CMD_DTC;
}

struct rte_mbuf *dpaa_eth_sg_to_mbuf(struct qm_fd *fd, uint32_t ifid)
{
	struct pool_info_entry *bp_info = DPAA_BPID_TO_POOL_INFO(fd->bpid);
	struct rte_mbuf *first_seg, *prev_seg, *cur_seg, *temp;
	struct qm_sg_entry *sgt, *sg_temp;
	void *vaddr, *sg_vaddr;
	int i = 0;
	uint8_t fd_offset = fd->offset;

	PMD_RX_LOG(DEBUG, "Received an SG frame");

	vaddr = dpaa_mem_ptov(qm_fd_addr(fd));
	if (!vaddr) {
		PMD_DRV_LOG(ERR, "unable to convert physical address");
		return NULL;
	}
	sgt = vaddr + fd_offset;
	sg_temp = &sgt[i++];
	hw_sg_to_cpu(sg_temp);
	temp = (struct rte_mbuf *)((char *)vaddr - bp_info->meta_data_size);
	sg_vaddr = dpaa_mem_ptov(qm_sg_entry_get64(sg_temp));

	first_seg = (struct rte_mbuf *)((char *)sg_vaddr - bp_info->meta_data_size);
	first_seg->data_off = sg_temp->offset;
	first_seg->data_len = sg_temp->length;
	first_seg->pkt_len = sg_temp->length;
	rte_mbuf_refcnt_set(first_seg, 1);

	first_seg->port = ifid;
	first_seg->nb_segs = 1;
	first_seg->ol_flags = 0;
	prev_seg = first_seg;
	while (i < DPA_SGT_MAX_ENTRIES) {
		sg_temp = &sgt[i++];
		hw_sg_to_cpu(sg_temp);
		sg_vaddr = dpaa_mem_ptov(qm_sg_entry_get64(sg_temp));
		cur_seg = (struct rte_mbuf *)((char *)sg_vaddr - bp_info->meta_data_size);
		cur_seg->data_off = sg_temp->offset;
		cur_seg->data_len = sg_temp->length;
		first_seg->pkt_len += sg_temp->length;
		first_seg->nb_segs += 1;
		rte_mbuf_refcnt_set(cur_seg, 1);
		prev_seg->next = cur_seg;
		if (sg_temp->final) {
			cur_seg->next = NULL;
			break;
		} else
			prev_seg = cur_seg;
	}

	dpaa_eth_packet_info(first_seg, (uint64_t)vaddr);
	rte_pktmbuf_free_seg(temp);

	return first_seg;
}

static inline struct rte_mbuf *dpaa_eth_fd_to_mbuf(struct qm_fd *fd,
							uint32_t ifid)
{
	struct pool_info_entry *bp_info = DPAA_BPID_TO_POOL_INFO(fd->bpid);
	struct rte_mbuf *mbuf;
	void *ptr;
	uint8_t format = (fd->opaque & DPAA_FD_FORMAT_MASK) >> DPAA_FD_FORMAT_SHIFT;
	uint16_t offset = (fd->opaque & DPAA_FD_OFFSET_MASK) >> DPAA_FD_OFFSET_SHIFT;
	uint32_t length = fd->opaque & DPAA_FD_LENGTH_MASK;

	PMD_RX_LOG(DEBUG, " FD--->MBUF");

	if (unlikely(format == qm_fd_sg))
		return dpaa_eth_sg_to_mbuf(fd, ifid);
	else if (unlikely(format != qm_fd_contig)) {
		PMD_DRV_LOG(ERR, "dropping packet in sg form");
		goto errret;
	}
	dpaa_display_frame(fd);
	ptr = dpaa_mem_ptov(fd->addr);
	if (!ptr) {
		PMD_DRV_LOG(ERR, "unable to convert physical address");
		goto errret;
	}
	mbuf = (struct rte_mbuf *)((char *)ptr - bp_info->meta_data_size);
	/* Prefetch the Parse results and packet data to L1 */
	rte_prefetch0((void *)((uint8_t *)ptr + DEFAULT_RX_ICEOF));
	rte_prefetch0((void *)((uint8_t *)ptr + offset));

	mbuf->data_off = offset;
	mbuf->data_len = length;
	mbuf->pkt_len = length;

	mbuf->port = ifid;
	mbuf->nb_segs = 1;
	mbuf->ol_flags = 0;
	mbuf->next = NULL;
	rte_mbuf_refcnt_set(mbuf, 1);
	dpaa_eth_packet_info(mbuf, (uint64_t)mbuf->buf_addr);

	return mbuf;
errret:
	dpaa_buf_free(bp_info, qm_fd_addr(fd));
	return NULL;
}

uint16_t dpaa_eth_queue_rx(void *q,
			   struct rte_mbuf **bufs,
		uint16_t nb_bufs)
{
	struct qman_fq *fq = q;
	struct qm_dqrr_entry *dq;
	uint32_t num_rx = 0, ifid = ((struct dpaa_if *)fq->dpaa_intf)->ifid;
	int ret;

	if (unlikely(!RTE_PER_LCORE(_dpaa_io))) {
		ret = dpaa_portal_init((void *)0);
		if (ret) {
			PMD_DRV_LOG(ERR, "Failure in affining portal");
			return 0;
		}
	}
	ret = qman_set_vdq(fq, (nb_bufs > DPAA_MAX_DEQUEUE_NUM_FRAMES) ?
				DPAA_MAX_DEQUEUE_NUM_FRAMES : nb_bufs);
	if (ret)
		return 0;

	do {
		dq = qman_dequeue(fq);
		if (!dq)
			continue;
		bufs[num_rx++] = dpaa_eth_fd_to_mbuf(&dq->fd, ifid);
		qman_dqrr_consume(fq, dq);
	} while (fq->flags & QMAN_FQ_STATE_VDQCR);

	return num_rx;
}

static void *dpaa_get_pktbuf(struct pool_info_entry *bp_info)
{
	int ret;
	uint64_t buf = 0;
	struct bm_buffer bufs;

	ret = bman_acquire(bp_info->bp, &bufs, 1, 0);
	if (ret <= 0) {
		PMD_DRV_LOG(WARNING, "Failed to allocate buffers %d", ret);
		return (void *)buf;
	}

	PMD_RX_LOG(DEBUG, "got buffer 0x%llx from pool %d",
		    bufs.addr, bufs.bpid);

	buf = (uint64_t)dpaa_mem_ptov(bufs.addr) - bp_info->meta_data_size;
	if (!buf)
		goto out;

out:
	return (void *)buf;
}

static struct rte_mbuf *dpaa_get_dmable_mbuf(struct rte_mbuf *mbuf,
					     struct dpaa_if *dpaa_intf)
{
	struct rte_mbuf *dpaa_mbuf;

	/* allocate pktbuffer on bpid for dpaa port */
	dpaa_mbuf = dpaa_get_pktbuf(dpaa_intf->bp_info);
	if (!dpaa_mbuf)
		return NULL;

	memcpy((uint8_t *)(dpaa_mbuf->buf_addr) + mbuf->data_off, (void *)
		((uint8_t *)(mbuf->buf_addr) + mbuf->data_off), mbuf->pkt_len);

	/* Copy only the required fields */
	dpaa_mbuf->data_off = mbuf->data_off;
	dpaa_mbuf->pkt_len = mbuf->pkt_len;
	dpaa_mbuf->ol_flags = mbuf->ol_flags;
	dpaa_mbuf->packet_type = mbuf->packet_type;
	dpaa_mbuf->tx_offload = mbuf->tx_offload;
	rte_pktmbuf_free(mbuf);
	return dpaa_mbuf;
}

int dpaa_eth_mbuf_to_sg_fd(struct rte_mbuf *mbuf,
		struct qm_fd *fd,
		uint32_t bpid)
{
	struct rte_mbuf *cur_seg = mbuf, *prev_seg = NULL;
	struct pool_info_entry *bp_info = DPAA_BPID_TO_POOL_INFO(bpid);
	struct rte_mbuf *temp, *mi;
	struct qm_sg_entry *sg_temp, *sgt;
	int i = 0;

	PMD_TX_LOG(DEBUG, "Creating SG FD to transmit");

	temp = rte_pktmbuf_alloc(bp_info->mp);
	if (!temp) {
		PMD_DRV_LOG(ERR, "Failure in allocation mbuf");
		return -1;
	}
	if (temp->buf_len < ((mbuf->nb_segs * sizeof(struct qm_sg_entry))
				+ temp->data_off)) {
		PMD_DRV_LOG(ERR, "Insufficient space in mbuf for SG entries");
		return -1;
	}

	fd->cmd = 0;
	fd->opaque_addr = 0;

	if (mbuf->ol_flags & DPAA_TX_CKSUM_OFFLOAD_MASK) {
		if (temp->data_off < DEFAULT_TX_ICEOF
			+ sizeof(struct dpaa_eth_parse_results_t))
			temp->data_off = DEFAULT_TX_ICEOF
				+ sizeof(struct dpaa_eth_parse_results_t);
		dcbz_64(temp->buf_addr);
		dpaa_checksum_offload(mbuf, fd, temp->buf_addr);
	}

	sgt = temp->buf_addr + temp->data_off;
	fd->format = QM_FD_SG;
	fd->addr = temp->buf_physaddr;
	fd->offset = temp->data_off;
	fd->bpid = bpid;
	fd->length20 = mbuf->pkt_len;


	while (i < DPA_SGT_MAX_ENTRIES) {
		sg_temp = &sgt[i++];
		sg_temp->opaque = 0;
		sg_temp->val = 0;
		sg_temp->addr = cur_seg->buf_physaddr;
		sg_temp->offset = cur_seg->data_off;
		sg_temp->length = cur_seg->data_len;
		if (RTE_MBUF_DIRECT(cur_seg)) {
			if (rte_mbuf_refcnt_read(cur_seg) > 1) {
				/*If refcnt > 1, invalid bpid is set to ensure buffer is not freed by HW */
				sg_temp->bpid = 0xff;
				rte_mbuf_refcnt_update(cur_seg, -1);
			} else
				sg_temp->bpid = DPAA_MEMPOOL_TO_BPID(cur_seg->pool);
			cur_seg = cur_seg->next;
		} else {
			/* Get owner MBUF from indirect buffer */
			mi = rte_mbuf_from_indirect(cur_seg);
			if (rte_mbuf_refcnt_read(mi) > 1) {
				/*If refcnt > 1, invalid bpid is set to ensure owner buffer is not freed by HW */
				sg_temp->bpid = 0xff;
			} else {
				sg_temp->bpid = DPAA_MEMPOOL_TO_BPID(mi->pool);
				rte_mbuf_refcnt_update(mi, 1);
			}
			prev_seg = cur_seg;
			cur_seg = cur_seg->next;
			prev_seg->next = NULL;
			rte_pktmbuf_free(prev_seg);
		}
		if (cur_seg == NULL) {
			sg_temp->final = 1;
			cpu_to_hw_sg(sg_temp);
			break;
		}
		cpu_to_hw_sg(sg_temp);
	}
	return 0;
}

uint16_t dpaa_eth_queue_tx(void *q,
			   struct rte_mbuf **bufs,
		uint16_t nb_bufs)
{
	struct rte_mbuf *mbuf, *mi = NULL;
	struct rte_mempool *mp;
	struct pool_info_entry *bp_info;
	struct qm_fd fd_arr[MAX_TX_RING_SLOTS];
	uint32_t frames_to_send, loop, i = 0;
	int ret;

	if (unlikely(!RTE_PER_LCORE(_dpaa_io))) {
		ret = dpaa_portal_init((void *)0);
		if (ret) {
			PMD_DRV_LOG(ERR, "Failure in affining portal");
			return 0;
		}
	}

	PMD_TX_LOG(DEBUG, "Transmitting %d buffers on queue: %p", nb_bufs, q);

	while (nb_bufs) {
		frames_to_send = (nb_bufs >> 3) ? MAX_TX_RING_SLOTS : nb_bufs;
		for (loop = 0; loop < frames_to_send; loop++, i++) {

			mbuf = bufs[i];
			if (RTE_MBUF_DIRECT(mbuf))
				mp = mbuf->pool;
			else {
				mi = rte_mbuf_from_indirect(mbuf);
				mp = mi->pool;
			}
			if (mp && (mp->flags & MEMPOOL_F_HW_PKT_POOL)) {
				PMD_TX_LOG(DEBUG, "BMAN offloaded buffer, "
					"mbuf: %p", mbuf);
				bp_info = DPAA_MEMPOOL_TO_POOL_INFO(mp);
				if (mbuf->nb_segs == 1) {
					if (RTE_MBUF_DIRECT(mbuf)) {
						if (rte_mbuf_refcnt_read(mbuf) > 1) {
							DPAA_MBUF_TO_CONTIG_FD(mbuf,
								&fd_arr[loop], 0xff);
							rte_mbuf_refcnt_update(mbuf, -1);
						} else {
							DPAA_MBUF_TO_CONTIG_FD(mbuf,
								&fd_arr[loop], bp_info->bpid);
						}
					} else {
						if (rte_mbuf_refcnt_read(mi) > 1) {
							DPAA_MBUF_TO_CONTIG_FD(mbuf,
								&fd_arr[loop], 0xff);
						} else {
							rte_mbuf_refcnt_update(mi, 1);
							DPAA_MBUF_TO_CONTIG_FD(mbuf,
								&fd_arr[loop], bp_info->bpid);
						}
						rte_pktmbuf_free(mbuf);
					}
					if (mbuf->ol_flags & DPAA_TX_CKSUM_OFFLOAD_MASK) {
						if (mbuf->data_off < DEFAULT_TX_ICEOF +
							sizeof(struct dpaa_eth_parse_results_t)) {
							PMD_DRV_LOG(ERR, "Checksum offload Err: "
								"Not enough Headroom "
								"space for correct Checksum offload."
								"So Calculating checksum in Software.");
							dpaa_checksum(mbuf);
						} else
							dpaa_checksum_offload(mbuf, &fd_arr[loop],
								mbuf->buf_addr);
					}
				} else if (mbuf->nb_segs > 1 && mbuf->nb_segs <= DPA_SGT_MAX_ENTRIES) {
					if (dpaa_eth_mbuf_to_sg_fd(mbuf,
						&fd_arr[loop], bp_info->bpid)) {
						PMD_DRV_LOG(DEBUG, "Unable to create Scatter Gather FD");
						frames_to_send = loop;
						nb_bufs = loop;
						goto send_pkts;
					}
				} else {
					PMD_DRV_LOG(DEBUG, "Number of Segments not supported");
					/* Set frames_to_send & nb_bufs so that
					 * packets are transmitted till
					 * previous frame */
					frames_to_send = loop;
					nb_bufs = loop;
					goto send_pkts;
				}
			} else {
				struct qman_fq *txq = q;
				struct dpaa_if *dpaa_intf = txq->dpaa_intf;

				PMD_TX_LOG(DEBUG, "Non-BMAN offloaded buffer."
					"Allocating an offloaded buffer");
				mbuf = dpaa_get_dmable_mbuf(mbuf, dpaa_intf);
				if (!mbuf) {
					PMD_DRV_LOG(DEBUG, "no dpaa buffers.");
					/* Set frames_to_send & nb_bufs so that
					 * packets are transmitted till
					 * previous frame */
					frames_to_send = loop;
					nb_bufs = loop;
					goto send_pkts;
				}

				DPAA_MBUF_TO_CONTIG_FD(mbuf, &fd_arr[loop],
						dpaa_intf->bp_info->bpid);
			}
		}

send_pkts:
		loop = 0;
		while (loop < frames_to_send) {
			loop += qman_enqueue_multi(q, &fd_arr[loop],
					frames_to_send - loop);
		}
		nb_bufs -= frames_to_send;
	}

	PMD_TX_LOG(DEBUG, "Transmitted %d buffers on queue: %p", i, q);

	return i;
}

uint16_t dpaa_eth_tx_drop_all(void *q  __rte_unused,
			      struct rte_mbuf **bufs __rte_unused,
		uint16_t nb_bufs __rte_unused)
{
	PMD_TX_LOG(DEBUG, "Drop all packets");

	/* Drop all incoming packets. No need to free packets here
	 * because the rte_eth f/w frees up the packets through tx_buffer
	 * callback in case this functions returns count less than nb_bufs
	 */
	return 0;
}