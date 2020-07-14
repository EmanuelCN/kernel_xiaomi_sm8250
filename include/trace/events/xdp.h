/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM xdp

#if !defined(_TRACE_XDP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_XDP_H

#include <linux/netdevice.h>
#include <linux/filter.h>
#include <linux/tracepoint.h>
#include <linux/bpf.h>

#define __XDP_ACT_MAP(FN)	\
	FN(ABORTED)		\
	FN(DROP)		\
	FN(PASS)		\
	FN(TX)			\
	FN(REDIRECT)

#define __XDP_ACT_TP_FN(x)	\
	TRACE_DEFINE_ENUM(XDP_##x);
#define __XDP_ACT_SYM_FN(x)	\
	{ XDP_##x, #x },
#define __XDP_ACT_SYM_TAB	\
	__XDP_ACT_MAP(__XDP_ACT_SYM_FN) { -1, 0 }
__XDP_ACT_MAP(__XDP_ACT_TP_FN)

TRACE_EVENT(xdp_exception,

	TP_PROTO(const struct net_device *dev,
		 const struct bpf_prog *xdp, u32 act),

	TP_ARGS(dev, xdp, act),

	TP_STRUCT__entry(
		__field(int, prog_id)
		__field(u32, act)
		__field(int, ifindex)
	),

	TP_fast_assign(
		__entry->prog_id	= xdp->aux->id;
		__entry->act		= act;
		__entry->ifindex	= dev->ifindex;
	),

	TP_printk("prog_id=%d action=%s ifindex=%d",
		  __entry->prog_id,
		  __print_symbolic(__entry->act, __XDP_ACT_SYM_TAB),
		  __entry->ifindex)
);

TRACE_EVENT(xdp_bulk_tx,

	TP_PROTO(const struct net_device *dev,
		 int sent, int drops, int err),

	TP_ARGS(dev, sent, drops, err),

	TP_STRUCT__entry(
		__field(int, ifindex)
		__field(u32, act)
		__field(int, drops)
		__field(int, sent)
		__field(int, err)
	),

	TP_fast_assign(
		__entry->ifindex	= dev->ifindex;
		__entry->act		= XDP_TX;
		__entry->drops		= drops;
		__entry->sent		= sent;
		__entry->err		= err;
	),

	TP_printk("ifindex=%d action=%s sent=%d drops=%d err=%d",
		  __entry->ifindex,
		  __print_symbolic(__entry->act, __XDP_ACT_SYM_TAB),
		  __entry->sent, __entry->drops, __entry->err)
);

#ifndef __DEVMAP_OBJ_TYPE
#define __DEVMAP_OBJ_TYPE
struct _bpf_dtab_netdev {
	struct net_device *dev;
};
#endif /* __DEVMAP_OBJ_TYPE */

#define devmap_ifindex(tgt, map)				\
	(((map->map_type == BPF_MAP_TYPE_DEVMAP ||	\
		  map->map_type == BPF_MAP_TYPE_DEVMAP_HASH)) ? \
	  ((struct _bpf_dtab_netdev *)tgt)->dev->ifindex : 0)

DECLARE_EVENT_CLASS(xdp_redirect_template,

	TP_PROTO(const struct net_device *dev,
		 const struct bpf_prog *xdp,
		 const void *tgt, int err,
		 const struct bpf_map *map, u32 index),

	TP_ARGS(dev, xdp, tgt, err, map, index),

	TP_STRUCT__entry(
		__field(int, prog_id)
		__field(u32, act)
		__field(int, ifindex)
		__field(int, err)
		__field(int, to_ifindex)
		__field(u32, map_id)
		__field(int, map_index)
	),

	TP_fast_assign(
		__entry->prog_id	= xdp->aux->id;
		__entry->act		= XDP_REDIRECT;
		__entry->ifindex	= dev->ifindex;
		__entry->err		= err;
		__entry->to_ifindex	= map ? devmap_ifindex(tgt, map) :
						index;
		__entry->map_id		= map ? map->id : 0;
		__entry->map_index	= map ? index : 0;
	),

	TP_printk("prog_id=%d action=%s ifindex=%d to_ifindex=%d err=%d"
		  " map_id=%d map_index=%d",
		  __entry->prog_id,
		  __print_symbolic(__entry->act, __XDP_ACT_SYM_TAB),
		  __entry->ifindex, __entry->to_ifindex,
		  __entry->err, __entry->map_id, __entry->map_index)
);

DEFINE_EVENT(xdp_redirect_template, xdp_redirect,
	TP_PROTO(const struct net_device *dev,
		 const struct bpf_prog *xdp,
		 const void *tgt, int err,
		 const struct bpf_map *map, u32 index),
	TP_ARGS(dev, xdp, tgt, err, map, index)
);

DEFINE_EVENT(xdp_redirect_template, xdp_redirect_err,
	TP_PROTO(const struct net_device *dev,
		 const struct bpf_prog *xdp,
		 const void *tgt, int err,
		 const struct bpf_map *map, u32 index),
	TP_ARGS(dev, xdp, tgt, err, map, index)
);

#define _trace_xdp_redirect(dev, xdp, to)		\
	 trace_xdp_redirect(dev, xdp, NULL, 0, NULL, to);

#define _trace_xdp_redirect_err(dev, xdp, to, err)	\
	 trace_xdp_redirect_err(dev, xdp, NULL, err, NULL, to);

#define _trace_xdp_redirect_map(dev, xdp, to, map, index)		\
	 trace_xdp_redirect(dev, xdp, to, 0, map, index);

#define _trace_xdp_redirect_map_err(dev, xdp, to, map, index, err)	\
	 trace_xdp_redirect_err(dev, xdp, to, err, map, index);

/* not used anymore, but kept around so as not to break old programs */
DEFINE_EVENT(xdp_redirect_template, xdp_redirect_map,
	TP_PROTO(const struct net_device *dev,
		 const struct bpf_prog *xdp,
		 const void *tgt, int err,
		 const struct bpf_map *map, u32 index),
	TP_ARGS(dev, xdp, tgt, err, map, index)
);

DEFINE_EVENT(xdp_redirect_template, xdp_redirect_map_err,
	TP_PROTO(const struct net_device *dev,
		 const struct bpf_prog *xdp,
		 const void *tgt, int err,
		 const struct bpf_map *map, u32 index),
	TP_ARGS(dev, xdp, tgt, err, map, index)
);

TRACE_EVENT(xdp_cpumap_kthread,

	TP_PROTO(int map_id, unsigned int processed,  unsigned int drops,
		 int sched, struct xdp_cpumap_stats *xdp_stats),

	TP_ARGS(map_id, processed, drops, sched, xdp_stats),

	TP_STRUCT__entry(
		__field(int, map_id)
		__field(u32, act)
		__field(int, cpu)
		__field(unsigned int, drops)
		__field(unsigned int, processed)
		__field(int, sched)
		__field(unsigned int, xdp_pass)
		__field(unsigned int, xdp_drop)
		__field(unsigned int, xdp_redirect)
	),

	TP_fast_assign(
		__entry->map_id		= map_id;
		__entry->act		= XDP_REDIRECT;
		__entry->cpu		= smp_processor_id();
		__entry->drops		= drops;
		__entry->processed	= processed;
		__entry->sched	= sched;
		__entry->xdp_pass	= xdp_stats->pass;
		__entry->xdp_drop	= xdp_stats->drop;
		__entry->xdp_redirect	= xdp_stats->redirect;
	),

	TP_printk("kthread"
		  " cpu=%d map_id=%d action=%s"
		  " processed=%u drops=%u"
		  " sched=%d"
		  " xdp_pass=%u xdp_drop=%u xdp_redirect=%u",
		  __entry->cpu, __entry->map_id,
		  __print_symbolic(__entry->act, __XDP_ACT_SYM_TAB),
		  __entry->processed, __entry->drops,
		  __entry->sched,
		  __entry->xdp_pass, __entry->xdp_drop, __entry->xdp_redirect)
);

TRACE_EVENT(xdp_cpumap_enqueue,

	TP_PROTO(int map_id, unsigned int processed,  unsigned int drops,
		 int to_cpu),

	TP_ARGS(map_id, processed, drops, to_cpu),

	TP_STRUCT__entry(
		__field(int, map_id)
		__field(u32, act)
		__field(int, cpu)
		__field(unsigned int, drops)
		__field(unsigned int, processed)
		__field(int, to_cpu)
	),

	TP_fast_assign(
		__entry->map_id		= map_id;
		__entry->act		= XDP_REDIRECT;
		__entry->cpu		= smp_processor_id();
		__entry->drops		= drops;
		__entry->processed	= processed;
		__entry->to_cpu		= to_cpu;
	),

	TP_printk("enqueue"
		  " cpu=%d map_id=%d action=%s"
		  " processed=%u drops=%u"
		  " to_cpu=%d",
		  __entry->cpu, __entry->map_id,
		  __print_symbolic(__entry->act, __XDP_ACT_SYM_TAB),
		  __entry->processed, __entry->drops,
		  __entry->to_cpu)
);

TRACE_EVENT(xdp_devmap_xmit,

	TP_PROTO(const struct net_device *from_dev,
		 const struct net_device *to_dev,
		 int sent, int drops, int err),

	TP_ARGS(from_dev, to_dev, sent, drops, err),

	TP_STRUCT__entry(
		__field(int, from_ifindex)
		__field(u32, act)
		__field(int, to_ifindex)
		__field(int, drops)
		__field(int, sent)
		__field(int, err)
	),

	TP_fast_assign(
		__entry->from_ifindex	= from_dev->ifindex;
		__entry->act		= XDP_REDIRECT;
		__entry->to_ifindex	= to_dev->ifindex;
		__entry->drops		= drops;
		__entry->sent		= sent;
		__entry->err		= err;
	),

	TP_printk("ndo_xdp_xmit"
		  " from_ifindex=%d to_ifindex=%d action=%s"
		  " sent=%d drops=%d"
		  " err=%d",
		  __entry->from_ifindex, __entry->to_ifindex,
		  __print_symbolic(__entry->act, __XDP_ACT_SYM_TAB),
		  __entry->sent, __entry->drops,
		  __entry->err)
);

#endif /* _TRACE_XDP_H */

#include <trace/define_trace.h>
