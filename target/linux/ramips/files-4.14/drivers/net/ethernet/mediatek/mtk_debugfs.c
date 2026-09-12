/*   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 2 of the License
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   Copyright (C) 2014-2016 Sean Wang <sean.wang@mediatek.com>
 *   Copyright (C) 2016-2017 John Crispin <blogic@openwrt.org>
 */

#include "mtk_offload.h"
#include <linux/if_vlan.h>
#ifdef CONFIG_SOC_MT7620
#include "gsw_mt7620.h"
#endif

static const char *mtk_foe_entry_state_str[] = {
	"INVALID",
	"UNBIND",
	"BIND",
	"FIN"
};

static const char *mtk_foe_packet_type_str[] = {
	"IPV4_HNAPT",
	"IPV4_HNAT",
	"IPV6_1T_ROUTE",
	"IPV4_DSLITE",
	"IPV6_3T_ROUTE",
	"IPV6_5T_ROUTE",
	"IPV6_6RD",
};

#define IPV4_HNAPT                      0
#define IPV4_HNAT                       1
#define IS_IPV4_HNAPT(x)	(((x)->bfib1.pkt_type == IPV4_HNAPT) ? 1: 0)
struct mtk_eth *_eth;
#define es(entry)		(mtk_foe_entry_state_str[entry->bfib1.state])
//#define ei(entry, end)		(MTK_PPE_TBL_SZ - (int)(end - entry))
#define ei(entry, end)		(MTK_PPE_ENTRY_CNT - (int)(end - entry))
#define pt(entry)		(mtk_foe_packet_type_str[entry->ipv4_hnapt.bfib1.pkt_type])

static int mtk_ppe_debugfs_foe_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = _eth;
	struct mtk_foe_entry *entry, *end;
	int i = 0;

	entry = eth->foe_table;
	end = eth->foe_table + MTK_PPE_ENTRY_CNT;

	while (entry < end) {
		if (IS_IPV4_HNAPT(entry)) {
			__be32 saddr = htonl(entry->ipv4_hnapt.sip);
			__be32 daddr = htonl(entry->ipv4_hnapt.dip);
			__be32 nsaddr = htonl(entry->ipv4_hnapt.new_sip);
			__be32 ndaddr = htonl(entry->ipv4_hnapt.new_dip);
			unsigned char h_dest[ETH_ALEN];
			unsigned char h_source[ETH_ALEN];

			*((u32*) h_source) = swab32(entry->ipv4_hnapt.smac_hi);
			*((u16*) &h_source[4]) = swab16(entry->ipv4_hnapt.smac_lo);
			*((u32*) h_dest) = swab32(entry->ipv4_hnapt.dmac_hi);
			*((u16*) &h_dest[4]) = swab16(entry->ipv4_hnapt.dmac_lo);
			seq_printf(m,
				   "(%x)0x%05x|state=%s|type=%s|"
				   "%pI4:%d->%pI4:%d=>%pI4:%d->%pI4:%d|%pM=>%pM|"
				   "etype=0x%04x|info1=0x%x|info2=0x%x|"
				   "vlan1=%d|vlan2=%d\n",
				   i,
				   ei(entry, end), es(entry), pt(entry),
				   &saddr, entry->ipv4_hnapt.sport,
				   &daddr, entry->ipv4_hnapt.dport,
				   &nsaddr, entry->ipv4_hnapt.new_sport,
				   &ndaddr, entry->ipv4_hnapt.new_dport, h_source,
				   h_dest, ntohs(entry->ipv4_hnapt.etype),
				   entry->ipv4_hnapt.info_blk1,
				   entry->ipv4_hnapt.info_blk2,
				   entry->ipv4_hnapt.vlan1,
				   entry->ipv4_hnapt.vlan2);
		} else
			seq_printf(m, "0x%05x state=%s\n",
				   ei(entry, end), es(entry));
		entry++;
		i++;
	}

	return 0;
}

static int mtk_ppe_debugfs_foe_open(struct inode *inode, struct file *file)
{
	return single_open(file, mtk_ppe_debugfs_foe_show, file->private_data);
}

static const struct file_operations mtk_ppe_debugfs_foe_fops = {
	.open = mtk_ppe_debugfs_foe_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int mtk_ppe_debugfs_rx_reasons_show(struct seq_file *m, void *private)
{
	int i;

	for (i = 0; i < MTK_RX_REASON_CNT; i++)
		seq_printf(m, "0x%02x %u\n", i, mtk_rx_reason_cnt[i]);

	seq_printf(m, "bind_hook %u\n", mtk_bind_hook_cnt);
	seq_printf(m, "lan_vid %u\n", mtk_lan_vid);
	seq_printf(m, "sdk_bind_enabled %u\n", sdk_bind_enabled);
	seq_printf(m, "sdk_bind_alg_enforce %u\n", sdk_bind_alg_enforce);

	seq_printf(m, "sdk_bind_hint_rx %u\n", sdk_bind_hint_rx_cnt);
	seq_printf(m, "sdk_bind_hint_pre %u\n", sdk_bind_hint_pr_cnt);
	seq_printf(m, "sdk_bind_hint_fwd %u\n", sdk_bind_hint_fwd_cnt);
	seq_printf(m, "sdk_bind_hint_post %u\n", sdk_bind_hint_post_cnt);
	seq_printf(m, "sdk_bind_hint_tx %u\n", sdk_bind_hint_tx_cnt);
	seq_printf(m, "sdk_bind_ok %u\n", sdk_bind_ok_cnt);
	seq_printf(m, "sdk_bind_skip_state %u\n", sdk_bind_skip_state_cnt);
	seq_printf(m, "sdk_bind_skip_alg %u\n", sdk_bind_skip_alg_cnt);
	seq_printf(m, "sdk_bind_fail %u\n", sdk_bind_fail_cnt);
	seq_printf(m, "last_tag_alg %u\n", last_tag_alg_val);

	return 0;
}

static int mtk_ppe_debugfs_rx_reasons_open(struct inode *inode,
					   struct file *file)
{
	return single_open(file, mtk_ppe_debugfs_rx_reasons_show,
			   file->private_data);
}

static const struct file_operations mtk_ppe_debugfs_rx_reasons_fops = {
	.open = mtk_ppe_debugfs_rx_reasons_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int mtk_ppe_debugfs_lan_vid_open(struct inode *inode,
					struct file *file)
{
	return single_open(file, mtk_ppe_debugfs_rx_reasons_show,
			   file->private_data);
}

static ssize_t mtk_ppe_debugfs_lan_vid_write(struct file *file,
					     const char __user *user_buf,
					     size_t count, loff_t *ppos)
{
	char buf[8];
	unsigned long val;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	mtk_lan_vid = val & VLAN_VID_MASK;

	return count;
}

static const struct file_operations mtk_ppe_debugfs_lan_vid_fops = {
	.open = mtk_ppe_debugfs_lan_vid_open,
	.read = seq_read,
	.write = mtk_ppe_debugfs_lan_vid_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t mtk_ppe_debugfs_sdk_u32_write(struct file *file,
					     const char __user *user_buf,
					     size_t count, loff_t *ppos)
{
	char buf[16];
	unsigned long val;
	u32 *p = file->private_data;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	*p = val;

	return count;
}

static int mtk_ppe_debugfs_sdk_u32_show(struct seq_file *m, void *private)
{
	seq_printf(m, "%u\n", *(u32 *)m->private);

	return 0;
}

static int mtk_ppe_debugfs_sdk_u32_open(struct inode *inode,
					struct file *file)
{
	return single_open(file, mtk_ppe_debugfs_sdk_u32_show,
			   file->private_data);
}

static const struct file_operations mtk_ppe_debugfs_sdk_u32_fops = {
	.open = mtk_ppe_debugfs_sdk_u32_open,
	.read = seq_read,
	.write = mtk_ppe_debugfs_sdk_u32_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t mtk_ppe_debugfs_ppe_reset_write(struct file *file,
					       const char __user *user_buf,
					       size_t count, loff_t *ppos)
{
	char buf[8];
	unsigned long val;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	if (val) {
		ret = mtk_ppe_reset();
		if (ret)
			return ret;
	}

	return count;
}

static const struct file_operations mtk_ppe_debugfs_ppe_reset_fops = {
	.write = mtk_ppe_debugfs_ppe_reset_write,
};

#ifdef CONFIG_SOC_MT7620
#define ESW_VLAN_VTCR		0x90
#define ESW_VLAN_VAWD1		0x94
#define ESW_VLAN_VAWD2		0x98
#define ESW_VLAN_VTIM(x)	(0x100 + 4 * ((x) / 2))
#define ESW_PORT_PCR(x)		(0x2004 | ((x) << 8))
#define ESW_PORT_PVC(x)		(0x2010 | ((x) << 8))
#define ESW_PORT_PPBV1(x)	(0x2014 | ((x) << 8))
#define ESW_TPF(x)		(0x2030 + ((x) * 0x100))
#define ESW_PSC_P7		0x270C
#define ESW_PMCR_P7		0x3700

static void mtk_esw_dump_vlan(struct seq_file *m, struct mt7620_gsw *gsw,
			      int idx)
{
	u32 a1, a2, vtim;
	int i;

	/* read the VLAN CAM entry by table index (cmd 0) */
	mtk_switch_w32(gsw, ESW_VLAN_VTCR, BIT(31) | (idx & 0xfff));
	for (i = 0; i < 20 && (mtk_switch_r32(gsw, ESW_VLAN_VTCR) & BIT(31));
	     i++)
		udelay(1000);

	a1 = mtk_switch_r32(gsw, ESW_VLAN_VAWD1);
	a2 = mtk_switch_r32(gsw, ESW_VLAN_VAWD2);
	vtim = mtk_switch_r32(gsw, ESW_VLAN_VTIM(idx));
	if (idx & 1)
		vtim >>= 12;
	vtim &= 0xfff;

	seq_printf(m, "vlan[%d] vid=%u valid=%u vtag_en=%u member=0x%02x\n",
		   idx, vtim, a1 & 1, (a1 >> 28) & 1, (a1 >> 16) & 0xff);
	seq_printf(m, "  ports:");
	for (i = 0; i < 8; i++) {
		int etag = (a2 >> (i * 2)) & 0x3;

		if ((a1 >> 16) & BIT(i))
			seq_printf(m, " %d%s", i, etag == 2 ? "t" : "");
	}
	seq_printf(m, "\n");
}

static int mtk_ppe_debugfs_esw_regs_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = _eth;
	struct mt7620_gsw *gsw =
			(struct mt7620_gsw *)eth->soc->swpriv;
	int i;

	seq_printf(m, "pfc 0x%08x\n", mtk_switch_r32(gsw, 0x0004));
	/*
	 * NOTE: only read registers that exist on MT7620N.  Reading the
	 * port-5 block (0x2500) or the port-7 PMCR (0x3700) wedges the
	 * eSwitch bus and hangs the router (watchdog reboot).
	 */
	for (i = 0; i < 5; i++) {
		seq_printf(m, "pcr[%d] 0x%08x pvc[%d] 0x%08x ppbv1[%d] 0x%08x\n",
			   i, mtk_switch_r32(gsw, ESW_PORT_PCR(i)),
			   i, mtk_switch_r32(gsw, ESW_PORT_PVC(i)),
			   i, mtk_switch_r32(gsw, ESW_PORT_PPBV1(i)));
	}
	seq_printf(m, "pcr[6] 0x%08x pvc[6] 0x%08x ppbv1[6] 0x%08x\n",
		   mtk_switch_r32(gsw, ESW_PORT_PCR(6)),
		   mtk_switch_r32(gsw, ESW_PORT_PVC(6)),
		   mtk_switch_r32(gsw, ESW_PORT_PPBV1(6)));
	seq_printf(m, "pcr[7] 0x%08x pvc[7] 0x%08x ppbv1[7] 0x%08x\n",
		   mtk_switch_r32(gsw, ESW_PORT_PCR(7)),
		   mtk_switch_r32(gsw, ESW_PORT_PVC(7)),
		   mtk_switch_r32(gsw, ESW_PORT_PPBV1(7)));
	for (i = 0; i < 6; i++)
		seq_printf(m, "tpf[%d] 0x%08x\n", i,
			   mtk_switch_r32(gsw, ESW_TPF(i)));
	seq_printf(m, "psc_p7 0x%08x\n", mtk_switch_r32(gsw, ESW_PSC_P7));
	seq_printf(m, "vtcr 0x%08x\n", mtk_switch_r32(gsw, ESW_VLAN_VTCR));

	mtk_esw_dump_vlan(m, gsw, 1);
	mtk_esw_dump_vlan(m, gsw, 2);

	return 0;
}

static int mtk_ppe_debugfs_esw_regs_open(struct inode *inode,
					 struct file *file)
{
	return single_open(file, mtk_ppe_debugfs_esw_regs_show,
			   file->private_data);
}

static const struct file_operations mtk_ppe_debugfs_esw_regs_fops = {
	.open = mtk_ppe_debugfs_esw_regs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

int mtk_ppe_debugfs_init(struct mtk_eth *eth)
{
	struct dentry *root;

	_eth = eth;

	root = debugfs_create_dir("mtk_ppe", NULL);
	if (IS_ERR(root)) {
		/* fe_open()/fe_stop() cycles keep the directory; the files
		 * below were created on the first run, so -EEXIST is fine */
		if (PTR_ERR(root) == -EEXIST)
			return 0;
		return PTR_ERR(root);
	}

	debugfs_create_file("all_entry", S_IRUGO, root, eth, &mtk_ppe_debugfs_foe_fops);
	debugfs_create_file("rx_reasons", S_IRUGO, root, eth, &mtk_ppe_debugfs_rx_reasons_fops);
	debugfs_create_file("lan_vid", S_IRUGO | S_IWUSR, root, eth,
			    &mtk_ppe_debugfs_lan_vid_fops);
	debugfs_create_file("bind_en", S_IRUGO | S_IWUSR, root,
			    &sdk_bind_enabled, &mtk_ppe_debugfs_sdk_u32_fops);
	debugfs_create_file("alg_enforce", S_IRUGO | S_IWUSR, root,
			    &sdk_bind_alg_enforce, &mtk_ppe_debugfs_sdk_u32_fops);
	debugfs_create_file("ppe_reset", S_IWUSR, root, NULL,
			    &mtk_ppe_debugfs_ppe_reset_fops);
#ifdef CONFIG_SOC_MT7620
	debugfs_create_file("esw_regs", S_IRUGO, root, eth,
			    &mtk_ppe_debugfs_esw_regs_fops);
#endif

	return 0;
}
