/*   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 2 of the License
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   Copyright (C) 2018 John Crispin <john@phrozen.org>
 */

#include "mtk_offload.h"

#ifdef CONFIG_SOC_MT7620
#include "gsw_mt7620.h"
#include <linux/if_vlan.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/nf_conntrack_tuple.h>
#include <net/neighbour.h>
#endif

#define INVALID	0
#define UNBIND	1
#define BIND	2
#define FIN	3

#define IPV4_HNAPT			0
#define IPV4_HNAT			1

u32 mtk_rx_reason_cnt[MTK_RX_REASON_CNT];
u32 mtk_bind_hook_cnt;

#ifdef CONFIG_SOC_MT7620
/* MT7620 Frame Engine PPE block (RALINK_PPE_BASE = FE_BASE + 0xC00) */
#define MT7620_PPE_GDM2_FWD_CFG		0xD00
/* MT7620 ESW (GSW) registers used to wire the PPE port (port 7) */
#define MT7620_ESW_PFC			0x0004
#define MT7620_ESW_TPF(x)		(0x2030 + ((x) * 0x100))
#define MT7620_ESW_TPF_NUM		6
#define MT7620_ESW_PMCR_P7		0x3700
#define MT7620_ESW_PSC_P7		0x270C
/* TPF (to-PPE forwarding) bits, IPv4, exclude broadcast */
#define MT7620_TPF_IPV4_MYUC		BIT(0)
#define MT7620_TPF_IPV4_UC		BIT(4)
#define MT7620_TPF_IPV4_UN		BIT(5)

static void
mt7620_esw_ppe(bool enable, struct mtk_eth *eth)
{
	struct mt7620_gsw *gsw = (struct mt7620_gsw *)eth->soc->swpriv;
	u32 val;
	int i;

	if (enable) {
		/* PPE_PORT = 7, PPE_EN = 1 */
		mtk_switch_w32(gsw, mtk_switch_r32(gsw, MT7620_ESW_PFC) |
			       (1 << 3) | 0x7, MT7620_ESW_PFC);

		/* forward IPv4 ucast frames to the PPE */
		for (i = 0; i < MT7620_ESW_TPF_NUM; i++)
			mtk_switch_w32(gsw, mtk_switch_r32(gsw, MT7620_ESW_TPF(i)) |
				MT7620_TPF_IPV4_MYUC | MT7620_TPF_IPV4_UC |
				MT7620_TPF_IPV4_UN, MT7620_ESW_TPF(i));

		/* force port 7 link up, 1Gbps, full duplex */
		mtk_switch_w32(gsw, 0x5e33b, MT7620_ESW_PMCR_P7);

		/* disable SA learning on port 7 */
		mtk_switch_w32(gsw, mtk_switch_r32(gsw, MT7620_ESW_PSC_P7) |
			       BIT(4), MT7620_ESW_PSC_P7);
	} else {
		/* PPE_EN = 0 */
		mtk_switch_w32(gsw, mtk_switch_r32(gsw, MT7620_ESW_PFC) &
			       ~BIT(3), MT7620_ESW_PFC);

		/* clear all to-PPE forwarding */
		for (i = 0; i < MT7620_ESW_TPF_NUM; i++)
			mtk_switch_w32(gsw, 0, MT7620_ESW_TPF(i));

		/* force port 7 link down */
		mtk_switch_w32(gsw, 0x5e330, MT7620_ESW_PMCR_P7);
	}
}

static void
mt7620_ppe_gdm2_fwd(bool enable, struct mtk_eth *eth)
{
	u32 val = mtk_r32(eth, MT7620_PPE_GDM2_FWD_CFG);

	val &= ~0x7777;
	if (enable)
		/* U/B/M/O frames forward to the PPE */
		val |= 0x0;
	else
		/* discard U/B/M/O frames while the PPE is going down */
		val |= 0x7777;
	mtk_w32(eth, val, MT7620_PPE_GDM2_FWD_CFG);
}
#endif

static u32
mtk_flow_hash_v4(struct flow_offload_tuple *tuple)
{
	u32 ports = ntohs(tuple->src_port)  << 16 | ntohs(tuple->dst_port);
	u32 src = ntohl(tuple->dst_v4.s_addr);
	u32 dst = ntohl(tuple->src_v4.s_addr);
	u32 hash = (ports & src) | ((~ports) & dst);
	u32 hash_23_0 = hash & 0xffffff;
	u32 hash_31_24 = hash & 0xff000000;

	hash = ports ^ src ^ dst ^ ((hash_23_0 << 8) | (hash_31_24 >> 24));
	hash = ((hash & 0xffff0000) >> 16 ) ^ (hash & 0xfffff);
	hash &= 0x7ff;
	hash *= 2;;

	return hash;
}

static int
mtk_foe_prepare_v4(struct mtk_foe_entry *entry,
		   struct flow_offload_tuple *tuple,
		   struct flow_offload_tuple *dest_tuple,
		   struct flow_offload_hw_path *src,
		   struct flow_offload_hw_path *dest)
{
	int is_mcast = !!is_multicast_ether_addr(dest->eth_dest);

#ifdef CONFIG_SOC_MT7620
	/* keep reference to avoid "unused variable" warning */
	(void)is_mcast;
#endif

	if (tuple->l4proto == IPPROTO_UDP)
		entry->ipv4_hnapt.bfib1.udp = 1;

	entry->ipv4_hnapt.etype = htons(ETH_P_IP);
	entry->ipv4_hnapt.bfib1.pkt_type = IPV4_HNAPT;
	entry->ipv4_hnapt.bfib1.ttl = 1;
	entry->ipv4_hnapt.bfib1.cah = 1;
	entry->ipv4_hnapt.bfib1.ka = 1;
	entry->ipv4_hnapt.iblk2.port_mg = 0x3f;
	entry->ipv4_hnapt.iblk2.port_ag = 0x1f;
#ifdef CONFIG_SOC_MT7620
	/* no force port - let the switch pick the egress port by DA */
	entry->ipv4_hnapt.iblk2.fpidx = 8;
	entry->ipv4_hnapt.iblk2.fp = 0;
	entry->ipv4_hnapt.iblk2.up = 0;
	entry->ipv4_hnapt.iblk2.fdq = 0;
	/* keep VPRI/DSCP on egress */
	entry->ipv4_hnapt.bfib1.dvp = 1;
	entry->ipv4_hnapt.bfib1.drm = 1;
#else
	entry->ipv4_hnapt.iblk2.fqos = 0;
	entry->ipv4_hnapt.iblk2.mcast = is_mcast;
	entry->ipv4_hnapt.iblk2.dscp = 0;
#ifdef CONFIG_NET_MEDIATEK_HW_QOS
	entry->ipv4_hnapt.iblk2.qid = 1;
	entry->ipv4_hnapt.iblk2.fqos = 1;
#endif
#ifdef CONFIG_RALINK
	entry->ipv4_hnapt.iblk2.dp = 1;
	if ((dest->flags & FLOW_OFFLOAD_PATH_VLAN) && (dest->vlan_id > 1))
		entry->ipv4_hnapt.iblk2.qid += 8;
#else
	entry->ipv4_hnapt.iblk2.dp = (dest->dev->name[3] - '0') + 1;
#endif
#endif

	entry->ipv4_hnapt.sip = ntohl(tuple->src_v4.s_addr);
	entry->ipv4_hnapt.dip = ntohl(tuple->dst_v4.s_addr);
	entry->ipv4_hnapt.sport = ntohs(tuple->src_port);
	entry->ipv4_hnapt.dport = ntohs(tuple->dst_port);

	entry->ipv4_hnapt.new_sip = ntohl(dest_tuple->dst_v4.s_addr);
	entry->ipv4_hnapt.new_dip = ntohl(dest_tuple->src_v4.s_addr);
	entry->ipv4_hnapt.new_sport = ntohs(dest_tuple->dst_port);
	entry->ipv4_hnapt.new_dport = ntohs(dest_tuple->src_port);

	entry->bfib1.state = BIND;

	if (dest->flags & FLOW_OFFLOAD_PATH_PPPOE) {
		entry->bfib1.psn = 1;
		entry->ipv4_hnapt.etype = htons(ETH_P_PPP_SES);
		entry->ipv4_hnapt.pppoe_id = dest->pppoe_sid;
	}

	if (dest->flags & FLOW_OFFLOAD_PATH_VLAN) {
		entry->ipv4_hnapt.vlan1 = dest->vlan_id;
		entry->bfib1.vlan_layer = 1;

#ifndef CONFIG_SOC_MT7620
		switch (dest->vlan_proto) {
		case htons(ETH_P_8021Q):
			entry->ipv4_hnapt.bfib1.vpm = 1;
			break;
		case htons(ETH_P_8021AD):
			entry->ipv4_hnapt.bfib1.vpm = 2;
			break;
		default:
			return -EINVAL;
		}
#else
		if (dest->vlan_proto != htons(ETH_P_8021Q))
			return -EINVAL;
#endif
	}

	return 0;
}

static void
mtk_foe_set_mac(struct mtk_foe_entry *entry, u8 *smac, u8 *dmac)
{
	entry->ipv4_hnapt.dmac_hi = swab32(*((u32*) dmac));
	entry->ipv4_hnapt.dmac_lo = swab16(*((u16*) &dmac[4]));
	entry->ipv4_hnapt.smac_hi = swab32(*((u32*) smac));
	entry->ipv4_hnapt.smac_lo = swab16(*((u16*) &smac[4]));
}

static int
mtk_check_entry_available(struct mtk_eth *eth, u32 hash)
{
	struct mtk_foe_entry entry = ((struct mtk_foe_entry *)eth->foe_table)[hash];

	return (entry.bfib1.state == BIND)? 0:1;
}

static void
mtk_foe_write(struct mtk_eth *eth, u32 hash,
	      struct mtk_foe_entry *entry)
{
	struct mtk_foe_entry *table = (struct mtk_foe_entry *)eth->foe_table;

	memcpy(&table[hash], entry, sizeof(*entry));
}

int mtk_flow_offload(struct mtk_eth *eth,
		     enum flow_offload_type type,
		     struct flow_offload *flow,
		     struct flow_offload_hw_path *src,
		     struct flow_offload_hw_path *dest)
{
	struct flow_offload_tuple *otuple = &flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].tuple;
	struct flow_offload_tuple *rtuple = &flow->tuplehash[FLOW_OFFLOAD_DIR_REPLY].tuple;
	u32 time_stamp = mtk_r32(eth, 0x0010) & (0x7fff);
	u32 ohash, rhash;
	struct mtk_foe_entry orig = {
		.bfib1.time_stamp = time_stamp,
		.bfib1.psn = 0,
	};
	struct mtk_foe_entry reply = {
		.bfib1.time_stamp = time_stamp,
		.bfib1.psn = 0,
	};

	if (otuple->l4proto != IPPROTO_TCP && otuple->l4proto != IPPROTO_UDP)
		return -EINVAL;
	
	if (type == FLOW_OFFLOAD_DEL) {
		flow = NULL;
		synchronize_rcu();
		return 0;
	}

	switch (otuple->l3proto) {
	case AF_INET:
		if (mtk_foe_prepare_v4(&orig, otuple, rtuple, src, dest) ||
		    mtk_foe_prepare_v4(&reply, rtuple, otuple, dest, src))
			return -EINVAL;

		ohash = mtk_flow_hash_v4(otuple);
		rhash = mtk_flow_hash_v4(rtuple);
		break;

	case AF_INET6:
		return -EINVAL;

	default:
		return -EINVAL;
	}

	/* Two-way hash: when hash collision occurs, the hash value will be shifted to the next position. */
	if (!mtk_check_entry_available(eth, ohash)){       
		if (!mtk_check_entry_available(eth, ohash + 1))
			return -EINVAL;
                ohash += 1;
        }
        if (!mtk_check_entry_available(eth, rhash)){
		if (!mtk_check_entry_available(eth, rhash + 1))
                        return -EINVAL;
                rhash += 1;
	}

	mtk_foe_set_mac(&orig, dest->eth_src, dest->eth_dest);
	mtk_foe_set_mac(&reply, src->eth_src, src->eth_dest);
	mtk_foe_write(eth, ohash, &orig);
	mtk_foe_write(eth, rhash, &reply);
	rcu_assign_pointer(eth->foe_flow_table[ohash], flow);
	rcu_assign_pointer(eth->foe_flow_table[rhash], flow);

	return 0;
}

#ifdef CONFIG_NET_MEDIATEK_HW_QOS

#define QDMA_TX_SCH_TX	  0x1a14

static void mtk_ppe_scheduler(struct mtk_eth *eth, int id, u32 rate)
{
	int exp = 0, shift = 0;
	u32 reg = mtk_r32(eth, QDMA_TX_SCH_TX);
	u32 val = 0;

	if (rate)
		val = BIT(11);

	while (rate > 127) {
		rate /= 10;
		exp++;
	}

	val |= (rate & 0x7f) << 4;
	val |= exp & 0xf;
	if (id)
		shift = 16;
	reg &= ~(0xffff << shift);
	reg |= val << shift;
	mtk_w32(eth, val, QDMA_TX_SCH_TX);
}

#define QTX_CFG(x)	(0x1800 + (x * 0x10))
#define QTX_SCH(x)	(0x1804 + (x * 0x10))

static void mtk_ppe_queue(struct mtk_eth *eth, int id, int sched, int weight, int resv, u32 min_rate, u32 max_rate)
{
	int max_exp = 0, min_exp = 0;
	u32 reg;

	if (id >= 16)
		return;

	reg = mtk_r32(eth, QTX_SCH(id));
	reg &= 0x70000000;

	if (sched)
		reg |= BIT(31);

	if (min_rate)
		reg |= BIT(27);

	if (max_rate)
		reg |= BIT(11);

	while (max_rate > 127) {
		max_rate /= 10;
		max_exp++;
	}

	while (min_rate > 127) {
		min_rate /= 10;
		min_exp++;
	}

	reg |= (min_rate & 0x7f) << 20;
	reg |= (min_exp & 0xf) << 16;
	reg |= (weight & 0xf) << 12;
	reg |= (max_rate & 0x7f) << 4;
	reg |= max_exp & 0xf;
	mtk_w32(eth, reg, QTX_SCH(id));

	resv &= 0xff;
	reg = mtk_r32(eth, QTX_CFG(id));
	reg &= 0xffff0000;
	reg |= (resv << 8) | resv;
	mtk_w32(eth, reg, QTX_CFG(id));
}
#endif

static int mtk_init_foe_table(struct mtk_eth *eth)
{
	if (eth->foe_table)
		return 0;

	eth->foe_flow_table = devm_kcalloc(eth->dev, MTK_PPE_ENTRY_CNT,
					   sizeof(*eth->foe_flow_table),
					   GFP_KERNEL);
	if (!eth->foe_flow_table)
		return -EINVAL;

	/* map the FOE table */
	eth->foe_table = dmam_alloc_coherent(eth->dev, MTK_PPE_TBL_SZ,
					     &eth->foe_table_phys, GFP_KERNEL);
	if (!eth->foe_table) {
		dev_err(eth->dev, "failed to allocate foe table\n");
		kfree(eth->foe_flow_table);
		return -ENOMEM;
	}


	return 0;
}

static int mtk_ppe_start(struct mtk_eth *eth)
{
	int ret;

	ret = mtk_init_foe_table(eth);
	if (ret)
		return ret;

	/* tell the PPE about the tables base address */
	mtk_w32(eth, eth->foe_table_phys, MTK_REG_PPE_TB_BASE);

	/* flush the table */
	memset(eth->foe_table, 0, MTK_PPE_TBL_SZ);

	/* setup hashing */
	mtk_m32(eth,
		MTK_PPE_TB_CFG_HASH_MODE_MASK | MTK_PPE_TB_CFG_TBL_SZ_MASK,
		MTK_PPE_TB_CFG_HASH_MODE1 | MTK_PPE_TB_CFG_TBL_SZ_4K,
		MTK_REG_PPE_TB_CFG);

	/* set the default hashing seed */
	mtk_w32(eth, MTK_PPE_HASH_SEED, MTK_REG_PPE_HASH_SEED);

	/* each foe entry is 64bytes and is setup by cpu forwarding*/
	mtk_m32(eth, MTK_PPE_CAH_CTRL_X_MODE | MTK_PPE_TB_CFG_ENTRY_SZ_MASK |
		MTK_PPE_TB_CFG_SMA_MASK,
		MTK_PPE_TB_CFG_ENTRY_SZ_64B |  MTK_PPE_TB_CFG_SMA_FWD_CPU,
		MTK_REG_PPE_TB_CFG);

	/* set ip proto */
	mtk_w32(eth, 0xFFFFFFFF, MTK_REG_PPE_IP_PROT_CHK);

	/* setup caching */
	mtk_m32(eth, 0, MTK_PPE_CAH_CTRL_X_MODE, MTK_REG_PPE_CAH_CTRL);
	mtk_m32(eth, MTK_PPE_CAH_CTRL_X_MODE, MTK_PPE_CAH_CTRL_EN,
		MTK_REG_PPE_CAH_CTRL);

	/* enable FOE */
	mtk_m32(eth, 0, MTK_PPE_FLOW_CFG_IPV4_NAT_FRAG_EN |
		MTK_PPE_FLOW_CFG_IPV4_NAPT_EN | MTK_PPE_FLOW_CFG_IPV4_NAT_EN |
		MTK_PPE_FLOW_CFG_IPV4_GREK_EN,
		MTK_REG_PPE_FLOW_CFG);

	/* setup flow entry un/bind aging */
	mtk_m32(eth, 0,
		MTK_PPE_TB_CFG_UNBD_AGE | MTK_PPE_TB_CFG_NTU_AGE |
		MTK_PPE_TB_CFG_FIN_AGE | MTK_PPE_TB_CFG_UDP_AGE |
		MTK_PPE_TB_CFG_TCP_AGE,
		MTK_REG_PPE_TB_CFG);

	mtk_m32(eth, MTK_PPE_UNB_AGE_MNP_MASK | MTK_PPE_UNB_AGE_DLTA_MASK,
		MTK_PPE_UNB_AGE_MNP | MTK_PPE_UNB_AGE_DLTA,
		MTK_REG_PPE_UNB_AGE);
	mtk_m32(eth, MTK_PPE_BND_AGE0_NTU_DLTA_MASK |
		MTK_PPE_BND_AGE0_UDP_DLTA_MASK,
		MTK_PPE_BND_AGE0_NTU_DLTA | MTK_PPE_BND_AGE0_UDP_DLTA,
		MTK_REG_PPE_BND_AGE0);
	mtk_m32(eth, MTK_PPE_BND_AGE1_FIN_DLTA_MASK |
		MTK_PPE_BND_AGE1_TCP_DLTA_MASK,
		MTK_PPE_BND_AGE1_FIN_DLTA | MTK_PPE_BND_AGE1_TCP_DLTA,
		MTK_REG_PPE_BND_AGE1);

	/* setup flow entry keep alive */
	mtk_m32(eth, MTK_PPE_TB_CFG_KA_MASK, MTK_PPE_TB_CFG_KA,
		MTK_REG_PPE_TB_CFG);
	mtk_w32(eth, MTK_PPE_KA_UDP | MTK_PPE_KA_TCP | MTK_PPE_KA_T, MTK_REG_PPE_KA);

	/* setup flow entry rate limit */
	mtk_w32(eth, (0x3fff << 16) | 0x3fff, MTK_REG_PPE_BIND_LMT_0);
	mtk_w32(eth, MTK_PPE_NTU_KA | 0x3fff, MTK_REG_PPE_BIND_LMT_1);
	mtk_m32(eth, MTK_PPE_BNDR_RATE_MASK, 1, MTK_REG_PPE_BNDR);

#ifdef CONFIG_SOC_MT7620
	/*
	 * MT7620: PSE "force port" bitmap table used by the FOE fpidx
	 * (0:force port0 ... 8:no force port / 9:force to all ports).
	 * Required for FOE entries with fpidx=8 (no force port).
	 */
	mtk_w32(eth, 0x00020001, MTK_REG_PPE_DFT_CPORT + 0x0);
	mtk_w32(eth, 0x00080004, MTK_REG_PPE_DFT_CPORT + 0x4);
	mtk_w32(eth, 0x00200010, MTK_REG_PPE_DFT_CPORT + 0x8);
	mtk_w32(eth, 0x00800040, MTK_REG_PPE_DFT_CPORT + 0xc);
	mtk_w32(eth, 0x003F0000, MTK_REG_PPE_DFT_CPORT + 0x10);

	/* wire the PPE port (port 7) on the ESW side */
	mt7620_esw_ppe(true, eth);

	/* send all traffic from gdm to the ppe */
	mt7620_ppe_gdm2_fwd(true, eth);
#endif

	/* enable the PPE */
	mtk_m32(eth, 0, MTK_PPE_GLO_CFG_EN, MTK_REG_PPE_GLO_CFG);

#ifndef CONFIG_SOC_MT7620
#ifdef CONFIG_RALINK
	/* set the default forwarding port to QDMA */
	mtk_w32(eth, 0x0, MTK_REG_PPE_DFT_CPORT);
#else
	/* set the default forwarding port to QDMA */
	mtk_w32(eth, 0x55555555, MTK_REG_PPE_DFT_CPORT);
#endif
#endif

	/* allow packets with TTL=0 */
	mtk_m32(eth, MTK_PPE_GLO_CFG_TTL0_DROP, 0, MTK_REG_PPE_GLO_CFG);

#ifndef CONFIG_SOC_MT7620
	/* send all traffic from gmac to the ppe */
	mtk_m32(eth, 0xffff, 0x4444, MTK_GDMA_FWD_CFG(0));
	mtk_m32(eth, 0xffff, 0x4444, MTK_GDMA_FWD_CFG(1));
#endif

	dev_info(eth->dev, "PPE started\n");

#ifdef CONFIG_NET_MEDIATEK_HW_QOS
	mtk_ppe_scheduler(eth, 0, 500000);
	mtk_ppe_scheduler(eth, 1, 500000);
	mtk_ppe_queue(eth, 0, 0, 7, 32, 250000, 0);
	mtk_ppe_queue(eth, 1, 0, 7, 32, 250000, 0);
	mtk_ppe_queue(eth, 8, 1, 7, 32, 250000, 0);
	mtk_ppe_queue(eth, 9, 1, 7, 32, 250000, 0);
#endif

	return 0;
}

static int mtk_ppe_busy_wait(struct mtk_eth *eth)
{
	unsigned long t_start = jiffies;
	u32 r = 0;

	while (1) {
		r = mtk_r32(eth, MTK_REG_PPE_GLO_CFG);
		if (!(r & MTK_PPE_GLO_CFG_BUSY))
			return 0;
		if (time_after(jiffies, t_start + HZ))
			break;
		usleep_range(10, 20);
	}

	dev_err(eth->dev, "ppe: table busy timeout - resetting\n");
	if (eth->rst_ppe)
		reset_control_reset(eth->rst_ppe);

	return -ETIMEDOUT;
}

static int mtk_ppe_stop(struct mtk_eth *eth)
{
	u32 r1 = 0, r2 = 0;
	int i;

	/* discard all traffic while we disable the PPE */
#ifdef CONFIG_SOC_MT7620
	mt7620_ppe_gdm2_fwd(false, eth);
#else
	mtk_m32(eth, 0xffff, 0x7777, MTK_GDMA_FWD_CFG(0));
	mtk_m32(eth, 0xffff, 0x7777, MTK_GDMA_FWD_CFG(1));
#endif

	if (mtk_ppe_busy_wait(eth))
		return -ETIMEDOUT;

	/* invalidate all flow table entries */
	for (i = 0; i < MTK_PPE_ENTRY_CNT; i++)
		eth->foe_table[i].bfib1.state = FOE_STATE_INVALID;

	/* disable caching */
	mtk_m32(eth, 0, MTK_PPE_CAH_CTRL_X_MODE, MTK_REG_PPE_CAH_CTRL);
	mtk_m32(eth, MTK_PPE_CAH_CTRL_X_MODE | MTK_PPE_CAH_CTRL_EN, 0,
		MTK_REG_PPE_CAH_CTRL);

	/* flush cache has to be ahead of hnat diable --*/
	mtk_m32(eth, MTK_PPE_GLO_CFG_EN, 0, MTK_REG_PPE_GLO_CFG);

	/* disable FOE */
	mtk_m32(eth,
		MTK_PPE_FLOW_CFG_IPV4_NAT_FRAG_EN |
		MTK_PPE_FLOW_CFG_IPV4_NAPT_EN | MTK_PPE_FLOW_CFG_IPV4_NAT_EN |
		MTK_PPE_FLOW_CFG_FUC_FOE | MTK_PPE_FLOW_CFG_FMC_FOE,
		0, MTK_REG_PPE_FLOW_CFG);

	/* disable FOE aging */
	mtk_m32(eth, 0,
		MTK_PPE_TB_CFG_FIN_AGE | MTK_PPE_TB_CFG_UDP_AGE |
		MTK_PPE_TB_CFG_TCP_AGE | MTK_PPE_TB_CFG_UNBD_AGE |
		MTK_PPE_TB_CFG_NTU_AGE, MTK_REG_PPE_TB_CFG);

#ifdef CONFIG_SOC_MT7621
	r1 = mtk_r32(eth, 0x100);
	r2 = mtk_r32(eth, 0x10c);

	dev_info(eth->dev, "0x100 = 0x%x, 0x10c = 0x%x\n", r1, r2);

	if (((r1 & 0xff00) >> 0x8) >= (r1 & 0xff) ||
	    ((r1 & 0xff00) >> 0x8) >= (r2 & 0xff)) {
		dev_info(eth->dev, "reset pse\n");
		mtk_w32(eth, 0x1, 0x4);
	}
#endif

	/* set the foe entry base address to 0 */
	mtk_w32(eth, 0, MTK_REG_PPE_TB_BASE);

	if (mtk_ppe_busy_wait(eth))
		return -ETIMEDOUT;

	/* send all traffic back to the DMA engine */
#ifdef CONFIG_SOC_MT7620
	mt7620_esw_ppe(false, eth);
	/* keep the PPE ingress path disabled while the PPE is off */
	mt7620_ppe_gdm2_fwd(false, eth);
#else
#ifdef CONFIG_RALINK
	mtk_m32(eth, 0xffff, 0x0, MTK_GDMA_FWD_CFG(0));
	mtk_m32(eth, 0xffff, 0x0, MTK_GDMA_FWD_CFG(1));
#else
	mtk_m32(eth, 0xffff, 0x5555, MTK_GDMA_FWD_CFG(0));
	mtk_m32(eth, 0xffff, 0x5555, MTK_GDMA_FWD_CFG(1));
#endif
#endif
	return 0;
}

static void mtk_offload_keepalive(struct fe_priv *eth, unsigned int hash)
{
	struct flow_offload *flow;

	rcu_read_lock();
	flow = rcu_dereference(eth->foe_flow_table[hash]);
	if (flow)
		flow->timeout = jiffies + 30 * HZ;
	rcu_read_unlock();
}

int mtk_offload_check_rx(struct fe_priv *eth, struct sk_buff *skb, u32 rxd4)
{
	u32 reason = FIELD_GET(MTK_RXD4_CPU_REASON, rxd4);
	unsigned int hash;

	mtk_rx_reason_cnt[reason]++;

	switch (reason) {
	case MTK_CPU_REASON_KEEPALIVE_UC_OLD_HDR:
	case MTK_CPU_REASON_KEEPALIVE_MC_NEW_HDR:
	case MTK_CPU_REASON_KEEPALIVE_DUP_OLD_HDR:
		hash = FIELD_GET(MTK_RXD4_FOE_ENTRY, rxd4);
		mtk_offload_keepalive(eth, hash);
		return -1;
	case MTK_CPU_REASON_PACKET_SAMPLING:
		return -1;
#ifdef CONFIG_SOC_MT7620
	case MTK_CPU_REASON_HIT_UNBIND_RATE_REACHED:
	case MTK_CPU_REASON_HIT_UNBIND:
		/*
		 * MT7620 Variant 2 (SDK-model): pass the UNBIND sample to the
		 * stack but remember the FOE slot index in skb->cb.  The
		 * netfilter POSTROUTING hook binds the slot by index (that is
		 * how the SDK turns PPE-built UNBIND entries into BIND).
		 */
		mtk_offload_put_hint(skb, FIELD_GET(MTK_RXD4_FOE_ENTRY, rxd4));
		return 0;
#endif
	default:
		return 0;
	}
}

#ifdef CONFIG_SOC_MT7620
/*
 * MT7620 Variant 2: bind a PPE FOE slot from the sample packet.
 *
 * The skb carries the slot index in skb->cb (set by mtk_offload_check_rx).
 * The conntrack tuples are already final here (the hook runs after NAT), so:
 *   - the packet side (sip/dip/sport/dport)  = ct->tuplehash[dir]
 *   - the translation (new_*)               = ct->tuplehash[!dir], reversed
 * just like mtk_foe_prepare_v4() does for the SW path.
 *
 * Returns NF_DROP on success - per the SDK the sample is a duplicate, the
 * PPE has already forwarded the original frame.  Every non-bindable case
 * falls back to NF_ACCEPT so the packet is never lost.
 */
static struct mtk_eth *mtk_offload_eth;

static unsigned int
mtk_offload_bind_hook(void *priv, struct sk_buff *skb,
		      const struct nf_hook_state *state)
{
	struct nf_conntrack_tuple *t_this, *t_other;
	struct mtk_foe_entry *entry;
	enum ip_conntrack_info ctinfo;
	enum ip_conntrack_dir dir;
	struct net_device *outdev;
	struct neighbour *n;
	struct dst_entry *dst;
	struct nf_conn *ct;
	u32 idx;

	if (skb->protocol != htons(ETH_P_IP))
		return NF_ACCEPT;

	if (!mtk_offload_skb_has_hint(skb))
		return NF_ACCEPT;

	idx = mtk_offload_get_hint(skb);
	mtk_offload_clear_hint(skb);

	if (!mtk_offload_eth || !mtk_offload_eth->foe_table)
		return NF_ACCEPT;

	if (idx >= MTK_PPE_ENTRY_CNT)
		return NF_ACCEPT;

	ct = nf_ct_get(skb, &ctinfo);
	if (!ct || nf_ct_is_untracked(ct) || nf_ct_is_dying(ct))
		return NF_ACCEPT;

	dst = skb_dst(skb);
	if (!dst || !dst->dev)
		return NF_ACCEPT;

	outdev = dst->dev;

	dir = CTINFO2DIR(ctinfo);
	t_this = &ct->tuplehash[dir].tuple;
	t_other = &ct->tuplehash[!dir].tuple;

	if (t_this->dst.protonum != IPPROTO_TCP &&
	    t_this->dst.protonum != IPPROTO_UDP)
		return NF_ACCEPT;

	/* The egress neighbor (far end of this flow) must already be resolved,
	 * otherwise the PPE would forward with an empty destination MAC. */
	n = dst_neigh_lookup(dst, &t_other->src.u3.ip);
	if (!n || !(n->nud_state & NUD_VALID)) {
		if (n)
			neigh_release(n);
		return NF_ACCEPT;
	}

	entry = &mtk_offload_eth->foe_table[idx];
	memset(entry, 0, sizeof(*entry));

	entry->ipv4_hnapt.etype = htons(ETH_P_IP);
	entry->ipv4_hnapt.bfib1.pkt_type = IPV4_HNAPT;
	entry->ipv4_hnapt.bfib1.ttl = 1;
	entry->ipv4_hnapt.bfib1.cah = 1;
	entry->ipv4_hnapt.bfib1.ka = 1;
	entry->ipv4_hnapt.bfib1.dvp = 1;
	entry->ipv4_hnapt.bfib1.drm = 1;
	entry->ipv4_hnapt.bfib1.udp = (t_this->dst.protonum == IPPROTO_UDP);
	entry->ipv4_hnapt.bfib1.time_stamp =
		mtk_r32(mtk_offload_eth, 0x0010) & 0x7fff;
	entry->ipv4_hnapt.iblk2.port_mg = 0x3f;
	entry->ipv4_hnapt.iblk2.port_ag = 0x1f;
	entry->ipv4_hnapt.iblk2.fpidx = 8;

	entry->ipv4_hnapt.sip = ntohl(t_this->src.u3.ip);
	entry->ipv4_hnapt.dip = ntohl(t_this->dst.u3.ip);
	entry->ipv4_hnapt.sport = ntohs(t_this->src.u.tcp.port);
	entry->ipv4_hnapt.dport = ntohs(t_this->dst.u.tcp.port);
	entry->ipv4_hnapt.new_sip = ntohl(t_other->dst.u3.ip);
	entry->ipv4_hnapt.new_dip = ntohl(t_other->src.u3.ip);
	entry->ipv4_hnapt.new_sport = ntohs(t_other->dst.u.tcp.port);
	entry->ipv4_hnapt.new_dport = ntohs(t_other->src.u.tcp.port);

	mtk_foe_set_mac(entry, outdev->dev_addr, n->ha);
	neigh_release(n);

	if (is_vlan_dev(outdev)) {
		entry->ipv4_hnapt.vlan1 = vlan_dev_priv(outdev)->vlan_id;
		entry->ipv4_hnapt.bfib1.vlan_layer = 1;
	}

	entry->ipv4_hnapt.bfib1.state = BIND;

	mtk_bind_hook_cnt++;

	return NF_DROP;
}

static struct nf_hook_ops mtk_offload_bind_ops = {
	.hook		= mtk_offload_bind_hook,
	.pf		= NFPROTO_IPV4,
	.hooknum	= NF_INET_POST_ROUTING,
	/* after NAT (NF_IP_PRI_NAT_SRC = 100), so the conntrack tuples are
	 * final and the packet 5-tuple already reflects the mapping */
	.priority	= NF_IP_PRI_NAT_SRC + 10,
};
#endif

int mtk_ppe_probe(struct mtk_eth *eth)
{
	int err;

	err = mtk_ppe_start(eth);
	if (err)
		return err;

	err = mtk_ppe_debugfs_init(eth);
	if (err)
		return err;

#ifdef CONFIG_SOC_MT7620
	/*
	 * Variant 2 bind: the PPE builds UNBIND entries by itself and samples
	 * a packet to the CPU once the counters reach the rate limit.  That
	 * sample (reason 0x0f/0x0e, FOE slot index in skb->cb) is bound here
	 * in POSTROUTING and dropped (the PPE already forwarded the original).
	 */
	mtk_offload_eth = eth;
	nf_register_hook(&mtk_offload_bind_ops);
#endif

	return 0;
}

void mtk_ppe_remove(struct mtk_eth *eth)
{
#ifdef CONFIG_SOC_MT7620
	nf_unregister_hook(&mtk_offload_bind_ops);
	mtk_offload_eth = NULL;
#endif
	mtk_ppe_stop(eth);
}
