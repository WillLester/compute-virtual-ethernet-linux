// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2015-2019 Google, Inc.
 */

#include <linux/rtnetlink.h>
#include "gve.h"
#include "gve_adminq.h"

static void gve_get_drvinfo(struct net_device *netdev,
			    struct ethtool_drvinfo *info)
{
	struct gve_priv *priv = netdev_priv(netdev);

	strlcpy(info->driver, "gve", sizeof(info->driver));
	strlcpy(info->version, gve_version_str, sizeof(info->version));
	strlcpy(info->bus_info, pci_name(priv->pdev), sizeof(info->bus_info));
}

static void gve_set_msglevel(struct net_device *netdev, u32 value)
{
	struct gve_priv *priv = netdev_priv(netdev);

	priv->msg_enable = value;
}

static u32 gve_get_msglevel(struct net_device *netdev)
{
	struct gve_priv *priv = netdev_priv(netdev);

	return priv->msg_enable;
}

static const char gve_gstrings_main_stats[][ETH_GSTRING_LEN] = {
	"rx_packets", "rx_total_bytes", "rx_total_dropped_pkt",
	"rx_skb_alloc_fail", "rx_page_alloc_fail", "rx_dma_mapping_error",
	"rx_desc_err_dropped_pkt", "tx_packets", "tx_total_bytes",
	"tx_total_dropped_pkt", "tx_timeouts", "interface_up_cnt",
	"interface_down_cnt", "reset_cnt",
};

static const char gve_gstrings_rx_stats[][ETH_GSTRING_LEN] = {
	"rx_posted_desc[%u]", "rx_completed_desc[%u]", "rx_bytes[%u]",
	"rx_dropped_pkt[%u]", "rx_copybreak_pkt[%u]", "rx_copied_pkt[%u]",
};

static const char gve_gstrings_tx_stats[][ETH_GSTRING_LEN] = {
	"tx_posted_desc[%u]", "tx_completed_desc[%u]", "tx_bytes[%u]",
	"tx_wake[%u]", "tx_stop[%u]", "tx_event_counter[%u]",
};

static const char gve_gstrings_adminq_stats[][ETH_GSTRING_LEN] = {
	"adminq_prod_cnt", "adminq_cmd_fail", "adminq_timeouts",
	"adminq_describe_device_cnt", "adminq_cfg_device_resources_cnt",
	"adminq_register_page_list_cnt", "adminq_unregister_page_list_cnt",
	"adminq_create_tx_queue_cnt", "adminq_create_rx_queue_cnt",
	"adminq_destroy_tx_queue_cnt", "adminq_destroy_rx_queue_cnt",
	"adminq_dcfg_device_resources_cnt", "adminq_set_driver_parameter_cnt",
	"adminq_report_stats_cnt",
};

static const char gve_gstrings_priv_flags[][ETH_GSTRING_LEN] = {
	"report-stats",
};

#define GVE_MAIN_STATS_LEN  ARRAY_SIZE(gve_gstrings_main_stats)
#define GVE_ADMINQ_STATS_LEN  ARRAY_SIZE(gve_gstrings_adminq_stats)
#define NUM_GVE_TX_CNTS	ARRAY_SIZE(gve_gstrings_tx_stats)
#define NUM_GVE_RX_CNTS	ARRAY_SIZE(gve_gstrings_rx_stats)
#define GVE_PRIV_FLAGS_STR_LEN ARRAY_SIZE(gve_gstrings_priv_flags)

static void gve_get_strings(struct net_device *netdev, u32 stringset, u8 *data)
{
	struct gve_priv *priv = netdev_priv(netdev);
	char *s = (char *)data;
	int i, j;

	switch (stringset) {
	case ETH_SS_STATS:
		memcpy(s, *gve_gstrings_main_stats,
		       sizeof(gve_gstrings_main_stats));
		s += sizeof(gve_gstrings_main_stats);

		for (i = 0; i < priv->rx_cfg.num_queues; i++) {
			for (j = 0; j < NUM_GVE_RX_CNTS; j++) {
				snprintf(s, ETH_GSTRING_LEN,
					 gve_gstrings_rx_stats[j], i);
				s += ETH_GSTRING_LEN;
			}
		}

		for (i = 0; i < priv->tx_cfg.num_queues; i++) {
			for (j = 0; j < NUM_GVE_TX_CNTS; j++) {
				snprintf(s, ETH_GSTRING_LEN,
					 gve_gstrings_tx_stats[j], i);
				s += ETH_GSTRING_LEN;
			}
		}

		memcpy(s, *gve_gstrings_adminq_stats,
		       sizeof(gve_gstrings_adminq_stats));
		s += sizeof(gve_gstrings_adminq_stats);
		break;

	case ETH_SS_PRIV_FLAGS:
		memcpy(s, *gve_gstrings_priv_flags,
		       sizeof(gve_gstrings_priv_flags));
		s += sizeof(gve_gstrings_priv_flags);
		break;

	default:
		break;
	}
}

static int gve_get_sset_count(struct net_device *netdev, int sset)
{
	struct gve_priv *priv = netdev_priv(netdev);

	switch (sset) {
	case ETH_SS_STATS:
		return GVE_MAIN_STATS_LEN + GVE_ADMINQ_STATS_LEN +
		       (priv->rx_cfg.num_queues * NUM_GVE_RX_CNTS) +
		       (priv->tx_cfg.num_queues * NUM_GVE_TX_CNTS);
	case ETH_SS_PRIV_FLAGS:
		return GVE_PRIV_FLAGS_STR_LEN;
	default:
		return -EOPNOTSUPP;
	}
}

static void
gve_get_ethtool_stats(struct net_device *netdev,
		      struct ethtool_stats *stats, u64 *data)
{
	struct gve_priv *priv = netdev_priv(netdev);
	u64 rx_pkts, rx_bytes, rx_skb_alloc_fail, rx_page_alloc_fail,
		rx_dma_mapping_error, rx_desc_err_dropped_pkt, tx_pkts,
		tx_bytes;
	u64 tmp_rx_pkts, tmp_rx_bytes, tmp_rx_skb_alloc_fail,
		tmp_rx_page_alloc_fail, tmp_rx_dma_mapping_error,
		tmp_rx_desc_err_dropped_pkt, tmp_tx_pkts, tmp_tx_bytes;
	unsigned int start;
	int ring;
	int i;

	ASSERT_RTNL();

	for (rx_pkts = 0, rx_bytes = 0, rx_skb_alloc_fail = 0,
	     rx_page_alloc_fail = 0, rx_dma_mapping_error = 0,
	     rx_desc_err_dropped_pkt = 0, ring = 0;
	     ring < priv->rx_cfg.num_queues; ring++) {
		if (priv->rx) {
			do {
				struct gve_rx_ring *rx = &priv->rx[ring];
				start =
				  u64_stats_fetch_begin(&priv->rx[ring].statss);
				tmp_rx_pkts = rx->rpackets;
				tmp_rx_bytes = rx->rbytes;
				tmp_rx_skb_alloc_fail = rx->rx_skb_alloc_fail;
				tmp_rx_page_alloc_fail = rx->rx_page_alloc_fail;
				tmp_rx_dma_mapping_error =
					rx->rx_dma_mapping_error;
				tmp_rx_desc_err_dropped_pkt =
					rx->rx_desc_err_dropped_pkt;
			} while (u64_stats_fetch_retry(&priv->rx[ring].statss,
						       start));
			rx_pkts += tmp_rx_pkts;
			rx_bytes += tmp_rx_bytes;
			/* rx_page_alloc_fail and rx_dma_mapping_error are empty
			for now */
			rx_skb_alloc_fail += tmp_rx_skb_alloc_fail;
			rx_page_alloc_fail += tmp_rx_page_alloc_fail;
			rx_dma_mapping_error += tmp_rx_dma_mapping_error;
			rx_desc_err_dropped_pkt += tmp_rx_desc_err_dropped_pkt;
		}
	}
	for (tx_pkts = 0, tx_bytes = 0, ring = 0;
	     ring < priv->tx_cfg.num_queues; ring++) {
		if (priv->tx) {
			do {
				start =
				  u64_stats_fetch_begin(&priv->tx[ring].statss);
				tmp_tx_pkts = priv->tx[ring].pkt_done;
				tmp_tx_bytes = priv->tx[ring].bytes_done;
			} while (u64_stats_fetch_retry(&priv->tx[ring].statss,
						       start));
			tx_pkts += tmp_tx_pkts;
			tx_bytes += tmp_tx_bytes;
		}
	}

	i = 0;
	data[i++] = rx_pkts;
	data[i++] = rx_bytes;
	/* total rx dropped packets */
	data[i++] = rx_skb_alloc_fail + rx_page_alloc_fail +
		rx_dma_mapping_error + rx_desc_err_dropped_pkt;
	data[i++] = rx_skb_alloc_fail;
	data[i++] = rx_page_alloc_fail;
	data[i++] = rx_dma_mapping_error;
	data[i++] = rx_desc_err_dropped_pkt;
	data[i++] = tx_pkts;
	data[i++] = tx_bytes;
	/* Skip tx_dropped */
	i++;
	data[i++] = priv->tx_timeo_cnt;
	data[i++] = priv->interface_up_cnt;
	data[i++] = priv->interface_down_cnt;
	data[i++] = priv->reset_cnt;
	i = GVE_MAIN_STATS_LEN;

	/* walk RX rings */
	if (priv->rx) {
		for (ring = 0; ring < priv->rx_cfg.num_queues; ring++) {
			struct gve_rx_ring *rx = &priv->rx[ring];

			data[i++] = rx->fill_cnt;
			data[i++] = rx->cnt;
			do {
				start =
				  u64_stats_fetch_begin(&priv->rx[ring].statss);
				tmp_rx_bytes = rx->rbytes;
				tmp_rx_skb_alloc_fail = rx->rx_skb_alloc_fail;
				tmp_rx_page_alloc_fail = rx->rx_page_alloc_fail;
				tmp_rx_dma_mapping_error =
					rx->rx_dma_mapping_error;
				tmp_rx_desc_err_dropped_pkt =
					rx->rx_desc_err_dropped_pkt;
			} while (u64_stats_fetch_retry(&priv->rx[ring].statss,
						       start));
			data[i++] = tmp_rx_bytes;
			/* rx dropped packets */
			data[i++] = tmp_rx_skb_alloc_fail +
				tmp_rx_page_alloc_fail +
				tmp_rx_dma_mapping_error +
				tmp_rx_desc_err_dropped_pkt;
			data[i++] = rx->rx_copybreak_pkt;
			data[i++] = rx->rx_copied_pkt;
		}
	} else {
		i += priv->rx_cfg.num_queues * NUM_GVE_RX_CNTS;
	}
	/* walk TX rings */
	if (priv->tx) {
		for (ring = 0; ring < priv->tx_cfg.num_queues; ring++) {
			struct gve_tx_ring *tx = &priv->tx[ring];

			data[i++] = tx->req;
			data[i++] = tx->done;
			do {
				start =
				  u64_stats_fetch_begin(&priv->tx[ring].statss);
				tmp_tx_bytes = tx->bytes_done;
			} while (u64_stats_fetch_retry(&priv->tx[ring].statss,
						       start));
			data[i++] = tmp_tx_bytes;
			data[i++] = tx->wake_queue;
			data[i++] = tx->stop_queue;
			data[i++] = be32_to_cpu(gve_tx_load_event_counter(priv,
									  tx));
		}
	} else {
		i += priv->tx_cfg.num_queues * NUM_GVE_TX_CNTS;
	}
	/* AQ Stats */
	data[i++] = priv->adminq_prod_cnt;
	data[i++] = priv->adminq_cmd_fail;
	data[i++] = priv->adminq_timeouts;
	data[i++] = priv->adminq_describe_device_cnt;
	data[i++] = priv->adminq_cfg_device_resources_cnt;
	data[i++] = priv->adminq_register_page_list_cnt;
	data[i++] = priv->adminq_unregister_page_list_cnt;
	data[i++] = priv->adminq_create_tx_queue_cnt;
	data[i++] = priv->adminq_create_rx_queue_cnt;
	data[i++] = priv->adminq_destroy_tx_queue_cnt;
	data[i++] = priv->adminq_destroy_rx_queue_cnt;
	data[i++] = priv->adminq_dcfg_device_resources_cnt;
	data[i++] = priv->adminq_set_driver_parameter_cnt;
	data[i++] = priv->adminq_report_stats_cnt;
}

static void gve_get_channels(struct net_device *netdev,
			     struct ethtool_channels *cmd)
{
	struct gve_priv *priv = netdev_priv(netdev);

	cmd->max_rx = priv->rx_cfg.max_queues;
	cmd->max_tx = priv->tx_cfg.max_queues;
	cmd->max_other = 0;
	cmd->max_combined = 0;
	cmd->rx_count = priv->rx_cfg.num_queues;
	cmd->tx_count = priv->tx_cfg.num_queues;
	cmd->other_count = 0;
	cmd->combined_count = 0;
}

static int gve_set_channels(struct net_device *netdev,
			    struct ethtool_channels *cmd)
{
	struct gve_priv *priv = netdev_priv(netdev);
	struct gve_queue_config new_tx_cfg = priv->tx_cfg;
	struct gve_queue_config new_rx_cfg = priv->rx_cfg;
	struct ethtool_channels old_settings;
	int new_tx = cmd->tx_count;
	int new_rx = cmd->rx_count;

	gve_get_channels(netdev, &old_settings);

	/* Changing combined is not allowed allowed */
	if (cmd->combined_count != old_settings.combined_count)
		return -EINVAL;

	if (!new_rx || !new_tx)
		return -EINVAL;

	if (!netif_carrier_ok(netdev)) {
		priv->tx_cfg.num_queues = new_tx;
		priv->rx_cfg.num_queues = new_rx;
		return 0;
	}

	new_tx_cfg.num_queues = new_tx;
	new_rx_cfg.num_queues = new_rx;

	return gve_adjust_queues(priv, new_rx_cfg, new_tx_cfg);
}

static void gve_get_ringparam(struct net_device *netdev,
			      struct ethtool_ringparam *cmd)
{
	struct gve_priv *priv = netdev_priv(netdev);

	cmd->rx_max_pending = priv->rx_desc_cnt;
	cmd->tx_max_pending = priv->tx_desc_cnt;
	cmd->rx_pending = priv->rx_desc_cnt;
	cmd->tx_pending = priv->tx_desc_cnt;
}

static int gve_user_reset(struct net_device *netdev, u32 *flags)
{
	struct gve_priv *priv = netdev_priv(netdev);

	if (*flags == ETH_RESET_ALL) {
		*flags = 0;
		return gve_reset(priv, true);
	}

	return -EOPNOTSUPP;
}

static int gve_get_tunable(struct net_device *netdev,
			   const struct ethtool_tunable *etuna, void *value)
{
	struct gve_priv *priv = netdev_priv(netdev);

	switch (etuna->id) {
	case ETHTOOL_RX_COPYBREAK:
		*(u32 *)value = priv->rx_copybreak;
		return 0;
	default:
		return -EINVAL;
	}
}

static int gve_set_tunable(struct net_device *netdev,
			   const struct ethtool_tunable *etuna,
			   const void *value)
{
	struct gve_priv *priv = netdev_priv(netdev);
	u32 len;

	switch(etuna->id) {
	case ETHTOOL_RX_COPYBREAK:
		len = *(u32 *)value;
		if (len > priv->dev->mtu) {
			return -EINVAL;
		}
		priv->rx_copybreak = len;
		return 0;
	default:
		return -EINVAL;
	}
}

static u32 gve_get_priv_flags(struct net_device *netdev)
{
	struct gve_priv *priv = netdev_priv(netdev);
	u32 i, ret_flags = 0;

	for (i = 0; i < GVE_PRIV_FLAGS_STR_LEN; i++) {
		if (priv->ethtool_flags & BIT(i)) {
			ret_flags |= BIT(i);
		}
	}
	return ret_flags;
}

static int gve_set_priv_flags(struct net_device *netdev, u32 flags)
{
	struct gve_priv *priv = netdev_priv(netdev);
	u64 ori_flags, new_flags;
	int err;
	u32 i;

	ori_flags = READ_ONCE(priv->ethtool_flags);
	new_flags = ori_flags;

	for (i = 0; i < GVE_PRIV_FLAGS_STR_LEN; i++) {
		err = 0;
		if (flags & BIT(i))
			new_flags |= BIT(i);
		else
			new_flags &= ~(BIT(i));
		/* set report-stats */
		if (strcmp(gve_gstrings_priv_flags[i], "report-stats") == 0) {
			/* update the stats when user turns report-stats on */
			if (flags & BIT(i))
				gve_handle_report_stats(priv);
			/* execute command when flag changed */
			if ((flags & BIT(i)) && !(ori_flags & BIT(i)))
				err = gve_adminq_report_stats(
					priv, priv->stats_report_len,
					priv->stats_report_bus);
			else if (!(flags & BIT(i)) && (ori_flags & BIT(i)))
				err = gve_adminq_report_stats(priv, 0, 0x0);
		}
		if (err)
			return EOPNOTSUPP;
		priv->ethtool_flags = new_flags;
	}

	return 0;
}

const struct ethtool_ops gve_ethtool_ops = {
	.get_drvinfo = gve_get_drvinfo,
	.get_strings = gve_get_strings,
	.get_sset_count = gve_get_sset_count,
	.get_ethtool_stats = gve_get_ethtool_stats,
	.set_msglevel = gve_set_msglevel,
	.get_msglevel = gve_get_msglevel,
	.set_channels = gve_set_channels,
	.get_channels = gve_get_channels,
	.get_link = ethtool_op_get_link,
	.get_ringparam = gve_get_ringparam,
	.reset = gve_user_reset,
	.get_tunable = gve_get_tunable,
	.set_tunable = gve_set_tunable,
	.get_priv_flags = gve_get_priv_flags,
	.set_priv_flags = gve_set_priv_flags,
};
