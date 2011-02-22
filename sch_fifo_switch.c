/*
 * net/sched/sch_fifo.c	The simplest FIFO queue.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <net/pkt_sched.h>

#include <linux/ip.h>
#include <linux/if_ether.h>
#include <asm/msr.h>

#define ETH_QCN                 0xA9A9

static int QCN_Q_EQ __read_mostly = 34000; /* 34KB */
static int QCN_W    __read_mostly = 2;

/* 1 band FIFO pseudo-"scheduler" */

struct fifo_sched_data
{
	u32 limit;

	/* QCN Variables */
	int qcn_qlen;				/* QCN Queue length */
	int qcn_qlen_old;			/* QCN Queue length at the time we
								   sent the last Fb */
	int sample;
	u32 generate_fb_frame;
};

struct qcn_frame {
	u32 DA;
	u32 SA;
	u32 Fb;
	int qoff;
	int qdelta;
};

static inline void qcn_init(struct fifo_sched_data *q)
{
	q->qcn_qlen = 0;
	q->qcn_qlen_old = 0;
	q->sample = 153600;
	q->generate_fb_frame = 0;
}

static inline int qcn_mark_table(u32 qntz_Fb) {
	switch (qntz_Fb >> 3) {
	case 0: return 153600;
	case 1:	return 76800;
	case 2:	return 51200;
	case 3:	return 38400;
	case 4:	return 30720;
	case 5: return 25600;
	case 6: return 22016;
	case 7: return 18944;
	}
	return 153600;
}

static struct sk_buff *qcnskb_create(struct sk_buff *skb, 
									 struct qcn_frame *frame)
{
	struct ethhdr *ethh;
	struct sk_buff *qcnskb;
	struct net_device *indev;

	memcpy(&indev, &skb->cb[24], sizeof(struct net_device *));
	if (strlen(indev->name) != 4) {
		printk("QCN err: qcnskb_create, indev->name size != 4");
		return NULL;
	}

	/* Initialization */
	if ((qcnskb = alloc_skb(64, GFP_ATOMIC)) == NULL)
		return NULL;	
	qcnskb->sk = NULL;
	qcnskb->pkt_type = PACKET_OTHERHOST;
	/* checksum: we dont need any checksum */
	qcnskb->ip_summed = CHECKSUM_NONE;

	/* eth */
	ethh = eth_hdr(skb);
	memcpy(skb_put(qcnskb, ETH_ALEN),ethh->h_source, ETH_ALEN);
	memcpy(skb_put(qcnskb, ETH_ALEN),ethh->h_dest, ETH_ALEN);
	*((__be16 *)skb_put(qcnskb, 2)) = htons(ETH_QCN);
	/* qcn */
	memcpy(skb_put(qcnskb, sizeof(struct qcn_frame)),
		   frame,
		   sizeof(struct qcn_frame));
	memcpy(&qcnskb->dev, &skb->cb[24], sizeof(struct net_device *));	

	return qcnskb;
}

static inline void qcn_algorithm(struct Qdisc* sch, struct fifo_sched_data *q,
								 struct sk_buff *skb, unsigned int len)
{
	struct sk_buff *qcnskb;		/* QCN Congestion Message skb */
	struct qcn_frame frame;
	struct iphdr *iph;
	u32 qntz_Fb, qntz_Fb_sent;
	int Fb;
	u64 tsc64;

	q->qcn_qlen += len;
	
	Fb = (QCN_Q_EQ - q->qcn_qlen) - QCN_W * (q->qcn_qlen - q->qcn_qlen_old);
	if (Fb < -QCN_Q_EQ * (2 * QCN_W +1)) {
		Fb = -QCN_Q_EQ * (2 * QCN_W +1);
	}
	else if (Fb > 0)
		Fb = 0;
	
	/* The maximum value of -Fb determines the number of bits that Fb
	   uses. Uniform quantization of -Fb, qntz_Fb, uses most
	   significant bits of -Fb. Note that now qntz_Fb has positive
	   values.  If Q_EQ = 32KB, W = 2, qcn_qlen = 160KB then the maximum
	   value for -Fb is 457728, which can be represented using 19bits
	   (110 1111 1100 0000 0000). To get the 6 most significant bits
	   --- considering that -Fb will use at most 19 bits ---, we need
	   to discard the 13 least significant bits (>> 13).
	*/
	qntz_Fb = 0x3F & (((u32) -Fb) >> 13);
	
	q->sample -= len;
	if (q->sample < 0) {
		if (qntz_Fb > 0) {
			q->generate_fb_frame = 1;
		}
		q->qcn_qlen_old = q->qcn_qlen;
		/* TODO: random sampling */
		q->sample = qcn_mark_table(qntz_Fb);
	}
	
	if (q->generate_fb_frame && skb && skb->network_header &&
		(skb->protocol == __constant_htons(ETH_P_IP))) {
		/* Since we are using IP addresses, we cant sample non-IP
		   packets. */
		
		/* Filling the qcn_frame structure */
		memset(&frame, 0, sizeof(struct qcn_frame));
		iph = ip_hdr(skb);
		frame.DA = iph->daddr;	/* Already in network byte order */
		frame.SA = iph->saddr;	/* Already in network byte order */
		frame.Fb = htonl(qntz_Fb);
		frame.qoff = htonl(QCN_Q_EQ - q->qcn_qlen);
		frame.qdelta = htonl(q->qcn_qlen - q->qcn_qlen_old);

		if ((qcnskb = qcnskb_create(skb, &frame)) == NULL)
			printk (KERN_ALERT "QCN err: qcnskb_create");
		else if (dev_queue_xmit(qcnskb) != NET_XMIT_SUCCESS)
			printk(KERN_ALERT "QCN err: dev_queue_xmit");
		else {
			q->generate_fb_frame = 0;
			qntz_Fb_sent = qntz_Fb;
		}
	}
	/* End QCN Algorithm */
	else
		qntz_Fb_sent = 0;

	rdtscll(tsc64);
	printk(KERN_INFO "%llu %s: QLEN %d Fb %u",
		   tsc64, sch->dev_queue->dev->name, q->qcn_qlen, qntz_Fb_sent);

}

static int bfifo_enqueue(struct sk_buff *skb, struct Qdisc* sch)
{
	struct fifo_sched_data *q = qdisc_priv(sch);
	int ret;

	if (likely(sch->qstats.backlog + qdisc_pkt_len(skb) <= q->limit)) {
		if ((ret = qdisc_enqueue_tail(skb, sch)) == 0) {}
			/* qcn_algorithm(sch, q, skb, qdisc_pkt_len(skb)); */
		return ret;
	}

	return qdisc_reshape_fail(skb, sch);
}

static void bfifo_reset_queue(struct Qdisc *sch)
{
	qcn_init(qdisc_priv(sch));
	qdisc_reset_queue(sch);
}

static unsigned int bfifo_drop(struct Qdisc *sch)
{
	struct fifo_sched_data *q = qdisc_priv(sch);
	unsigned int len = 0;

	if ((len = qdisc_queue_drop(sch)) != 0)
		q->qcn_qlen -= len;
	return len;
}

static struct sk_buff *bfifo_dequeue(struct Qdisc *sch)
{
	struct fifo_sched_data *q = qdisc_priv(sch);
	struct sk_buff *skb = qdisc_peek_head(sch);

	if (skb) {
		unsigned int len = qdisc_pkt_len(skb);
		q->qcn_qlen -= len;
		return skb;
	}
	return NULL;
}


static int pfifo_enqueue(struct sk_buff *skb, struct Qdisc* sch)
{
	struct fifo_sched_data *q = qdisc_priv(sch);

	if (likely(skb_queue_len(&sch->q) < q->limit))
		return qdisc_enqueue_tail(skb, sch);

	return qdisc_reshape_fail(skb, sch);
}

static int pfifo_tail_enqueue(struct sk_buff *skb, struct Qdisc* sch)
{
	struct sk_buff *skb_head;
	struct fifo_sched_data *q = qdisc_priv(sch);

	if (likely(skb_queue_len(&sch->q) < q->limit))
		return qdisc_enqueue_tail(skb, sch);

	/* queue full, remove one skb to fulfill the limit */
	skb_head = qdisc_dequeue_head(sch);
	sch->bstats.bytes -= qdisc_pkt_len(skb_head);
	sch->bstats.packets--;
	sch->qstats.drops++;
	kfree_skb(skb_head);

	qdisc_enqueue_tail(skb, sch);

	return NET_XMIT_CN;
}

static int fifo_init(struct Qdisc *sch, struct nlattr *opt)
{
	struct fifo_sched_data *q = qdisc_priv(sch);

	if (opt == NULL) {
		u32 limit = qdisc_dev(sch)->tx_queue_len ? : 1;

		if (sch->ops == &bfifo_qdisc_ops)
			limit *= psched_mtu(qdisc_dev(sch));

		q->limit = limit;
	} else {
		struct tc_fifo_qopt *ctl = nla_data(opt);

		if (nla_len(opt) < sizeof(*ctl))
			return -EINVAL;

		q->limit = ctl->limit;
	}
	/* Initializing QCN CP Variables */
	qcn_init(q);
	printk(KERN_INFO "%s: init", sch->dev_queue->dev->name);

	return 0;
}

static int fifo_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct fifo_sched_data *q = qdisc_priv(sch);
	struct tc_fifo_qopt opt = { .limit = q->limit };

	NLA_PUT(skb, TCA_OPTIONS, sizeof(opt), &opt);
	return skb->len;

nla_put_failure:
	return -1;
}

struct Qdisc_ops pfifo_qdisc_ops __read_mostly = {
	.id		=	"pfifo",
	.priv_size	=	sizeof(struct fifo_sched_data),
	.enqueue	=	pfifo_enqueue,
	.dequeue	=	qdisc_dequeue_head,
	.peek		=	qdisc_peek_head,
	.drop		=	qdisc_queue_drop,
	.init		=	fifo_init,
	.reset		=	qdisc_reset_queue,
	.change		=	fifo_init,
	.dump		=	fifo_dump,
	.owner		=	THIS_MODULE,
};
EXPORT_SYMBOL(pfifo_qdisc_ops);

struct Qdisc_ops bfifo_qdisc_ops __read_mostly = {
	.id		=	"bfifo",
	.priv_size	=	sizeof(struct fifo_sched_data),
	.enqueue	=	bfifo_enqueue,
	.dequeue    =   bfifo_dequeue,
	.peek		=	qdisc_peek_head,
	.drop		=	bfifo_drop,
	.init		=	fifo_init,
	.reset		=	bfifo_reset_queue,
	.change		=	fifo_init,
	.dump		=	fifo_dump,
	.owner		=	THIS_MODULE,
};
EXPORT_SYMBOL(bfifo_qdisc_ops);

struct Qdisc_ops pfifo_head_drop_qdisc_ops __read_mostly = {
	.id		=	"pfifo_head_drop",
	.priv_size	=	sizeof(struct fifo_sched_data),
	.enqueue	=	pfifo_tail_enqueue,
	.dequeue	=	qdisc_dequeue_head,
	.peek		=	qdisc_peek_head,
	.drop		=	qdisc_queue_drop_head,
	.init		=	fifo_init,
	.reset		=	qdisc_reset_queue,
	.change		=	fifo_init,
	.dump		=	fifo_dump,
	.owner		=	THIS_MODULE,
};

/* Pass size change message down to embedded FIFO */
int fifo_set_limit(struct Qdisc *q, unsigned int limit)
{
	struct nlattr *nla;
	int ret = -ENOMEM;

	/* Hack to avoid sending change message to non-FIFO */
	if (strncmp(q->ops->id + 1, "fifo", 4) != 0)
		return 0;

	nla = kmalloc(nla_attr_size(sizeof(struct tc_fifo_qopt)), GFP_KERNEL);
	if (nla) {
		nla->nla_type = RTM_NEWQDISC;
		nla->nla_len = nla_attr_size(sizeof(struct tc_fifo_qopt));
		((struct tc_fifo_qopt *)nla_data(nla))->limit = limit;

		ret = q->ops->change(q, nla);
		kfree(nla);
	}
	return ret;
}
EXPORT_SYMBOL(fifo_set_limit);

struct Qdisc *fifo_create_dflt(struct Qdisc *sch, struct Qdisc_ops *ops,
			       unsigned int limit)
{
	struct Qdisc *q;
	int err = -ENOMEM;

	q = qdisc_create_dflt(qdisc_dev(sch), sch->dev_queue,
			      ops, TC_H_MAKE(sch->handle, 1));
	if (q) {
		err = fifo_set_limit(q, limit);
		if (err < 0) {
			qdisc_destroy(q);
			q = NULL;
		}
	}

	return q ? : ERR_PTR(err);
}
EXPORT_SYMBOL(fifo_create_dflt);
