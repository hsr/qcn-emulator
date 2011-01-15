/*
 * net/sched/sch_tbf.c	Token Bucket Filter queue.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 *		Dmitry Torokhov <dtor@mail.ru> - allow attaching inner qdiscs -
 *						 original idea by Martin Devera
 *
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>

#include <linux/ip.h>
#include <linux/list.h>
#include <linux/semaphore.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/kfifo.h>

#define LISTEN_PORT             6660
#define THREAD_NAME             "QCNFbSender%x"
#define FRAME_KFIFO_SIZE        32
#define QCN_Q_EQ                33792 /* 33KB */
#define QCN_W                   2

/*	Simple Token Bucket Filter.
	=======================================

	SOURCE.
	-------

	None.

	Description.
	------------

	A data flow obeys TBF with rate R and depth B, if for any
	time interval t_i...t_f the number of transmitted bits
	does not exceed B + R*(t_f-t_i).

	Packetized version of this definition:
	The sequence of packets of sizes s_i served at moments t_i
	obeys TBF, if for any i<=k:

	s_i+....+s_k <= B + R*(t_k - t_i)

	Algorithm.
	----------

	Let N(t_i) be B/R initially and N(t) grow continuously with time as:

	N(t+delta) = min{B/R, N(t) + delta}

	If the first packet in queue has length S, it may be
	transmitted only at the time t_* when S/R <= N(t_*),
	and in this case N(t) jumps:

	N(t_* + 0) = N(t_* - 0) - S/R.



	Actually, QoS requires two TBF to be applied to a data stream.
	One of them controls steady state burst size, another
	one with rate P (peak rate) and depth M (equal to link MTU)
	limits bursts at a smaller time scale.

	It is easy to see that P>R, and B>M. If P is infinity, this double
	TBF is equivalent to a single one.

	When TBF works in reshaping mode, latency is estimated as:

	lat = max ((L-B)/R, (L-M)/P)


	NOTES.
	------

	If TBF throttles, it starts a watchdog timer, which will wake it up
	when it is ready to transmit.
	Note that the minimal timer resolution is 1/HZ.
	If no new packets arrive during this period,
	or if the device is not awaken by EOI for some previous packet,
	TBF can stop its activity for 1/HZ.


	This means, that with depth B, the maximal rate is

	R_crit = B*HZ

	F.e. for 10Mbit ethernet and HZ=100 the minimal allowed B is ~10Kbytes.

	Note that the peak rate TBF is much more tough: with MTU 1500
	P_crit = 150Kbytes/sec. So, if you need greater peak
	rates, use alpha with HZ=1000 :-)

	With classful TBF, limit is just kept for backwards compatibility.
	It is passed to the default bfifo qdisc - if the inner qdisc is
	changed the limit is not effective anymore.
*/

struct qcn_frame {
	u32 DA;
	u32 SA;
	u32 Fb;
	int qoff;
	int qdelta;
};

/* struct qcn_frame_list { */
/* 	struct qcn_frame frame; */
/* 	struct list_head list; */
/* }; */

struct qcn_kthread_t {
	struct task_struct *tsk;
	struct sockaddr_in addr;
	struct socket *sock;		/* Socket used to send Fb */
	struct Qdisc *sch;			/* The scheduler itself */

	/* struct qcn_frame_list frame_list; */

	struct semaphore sem_frame_fifo;
	/* DECLARE_KFIFO_PTR is available on kernels >= 2.6.37 */
	DECLARE_KFIFO_PTR(frame_fifo, struct qcn_frame);
	u32 frame_fifo_alloc;

};


struct tbf_sched_data {
    /* Parameters */
	u32		limit;		/* Maximal length of backlog: bytes */
	u32		buffer;		/* Token bucket depth/rate: MUST BE >= MTU/B */
	u32		mtu;
	u32		max_size;
	struct qdisc_rate_table	*R_tab;
	struct qdisc_rate_table	*P_tab;

    /* Variables */
	long	tokens;			/* Current number of B tokens */
	long	ptokens;		/* Current number of P tokens */
	psched_time_t	t_c;		/* Time check-point */
	struct Qdisc	*qdisc;		/* Inner qdisc, default - bfifo queue */
	struct qdisc_watchdog watchdog;	/* Watchdog timer */

	/* QCN Parameters */
	int W;
	int Q_EQ;
 
	/* QCN Variables */
	int qlen;
	int qlen_old;
	int sample;

	struct qcn_kthread_t th;	/* Feedback Sender Thread */

};

#define L2T(q,L)   qdisc_l2t((q)->R_tab,L)
#define L2T_P(q,L) qdisc_l2t((q)->P_tab,L)


/* The right way to do the task of sending Fb is by using one queue
   and one thread (producer-consumer). Don't use this qdisc on a
   normal interface. It's developed to be attached on a bridged
   iface. */
static int qcn_send_fb (struct socket *sock, struct sockaddr_in *addr,
						struct qcn_frame *frame, long ip_dst)
{
	struct msghdr msg;
	struct iovec iov;
	mm_segment_t oldfs;
	int size = 0;


	/* Connecting and sending the packet */
	memset(addr, 0, sizeof(struct sockaddr));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(LISTEN_PORT);
	addr->sin_addr.s_addr = htonl(0x7f000001);
	/* addr->sin_addr.s_addr = ip_dst & htonl(0xFFFF00FF); */
	/* The "AND 0xFFFF00FF" is a "workaround" to send the packet
	   to the physical machine's IP instead of the virtual
	   machine's IP (which was sampled). This code assumes that
	   the VMs' IP addresses are assigned in such a way that each
	   VM is placed on a PM which has an IP address equals to the
	   VM IP address with the bits from 8 to 15 set to 0 (hence
	   the AND 0xFFFF00FF). Such "workaround" is needed because we
	   dont have a host directory that maps VMs->PMs within the
	   kernel. */
	if (sock->ops->connect(sock, (struct sockaddr *) addr,
						   sizeof(struct sockaddr), 0) < 0) {
		printk(KERN_EMERG "Could not connect to 0x%x qntz_Fb 0x%x", 
			   addr->sin_addr.s_addr, frame->Fb);
		return 0;
	}
	printk(KERN_EMERG "Connected to 0x%x qntz_Fb 0x%x", 
		   addr->sin_addr.s_addr, frame->Fb);

	if (sock->sk == NULL) 
		return 0;
		
	iov.iov_base = frame;
	iov.iov_len = sizeof(struct qcn_frame);
		
	msg.msg_name = addr;
	msg.msg_namelen  = sizeof(struct sockaddr_in);
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = NULL;
	msg.msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	size = sock_sendmsg(sock, &msg, sizeof(struct qcn_frame));
	set_fs(oldfs);

	return size;
}

int qcn_feedback_sender(void *arg) {
	struct Qdisc *sch = arg;
	struct tbf_sched_data *q = qdisc_priv(sch);
	struct qcn_kthread_t *th = &q->th;
	struct qcn_frame frame;

	printk(KERN_EMERG "%s: success!\n", th->tsk->comm);
	
	/* Creating a socket to recv udp messages */
	if (sock_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &th->sock) < 0) {
		printk(KERN_EMERG "%s: could not create the recv socket, error = %d\n",
			   th->tsk->comm, -ENXIO);
		goto out;
	} else {
		printk(KERN_EMERG "%s: socket created!", th->tsk->comm);
	}

	while (!kthread_should_stop()) {
		if (down_timeout(&th->sem_frame_fifo, HZ) == 0) {
			printk(KERN_EMERG "%s: acquired the lock!", th->tsk->comm);

			if (!kfifo_get(&th->frame_fifo, &frame))
				printk(KERN_EMERG "%s: err while removing from kfifo",
					   th->tsk->comm);
			else if (qcn_send_fb(th->sock, &th->addr, &frame, frame.SA) <= 0)
				printk(KERN_EMERG "%s: err while sending fb",
					   th->tsk->comm);
			else
				printk(KERN_EMERG "%s: fb sent to %x",
					   th->tsk->comm, frame.SA);

		} else {
			/* printk(KERN_EMERG "%s: down_timeout!", th->tsk->comm); */
		}
	}

out:
	printk(KERN_EMERG "%s: releasing socket\n", th->tsk->comm);
	sock_release(th->sock);
	th->sock = NULL;
	th->tsk = NULL;
	return 0;
}

static int tbf_enqueue(struct sk_buff *skb, struct Qdisc* sch)
{
	struct tbf_sched_data *q = qdisc_priv(sch);
	int ret;
	unsigned int len = qdisc_pkt_len(skb);

	int Fb;
	struct iphdr *iph;
	struct qcn_frame frame;
	u32 qntz_Fb, generate_fb_frame;

	if (len > q->max_size)
		return qdisc_reshape_fail(skb, sch);

	/* QCN Algorithm */
	q->qlen += len;
	
	Fb = (QCN_Q_EQ - q->qlen) - QCN_W * (q->qlen - q->qlen_old);
	if (Fb < -QCN_Q_EQ * (2 * QCN_W +1)) {
		Fb = -QCN_Q_EQ * (2 * QCN_W +1);
	}
	else if (Fb > 0)
		Fb = 0;
	
	/* The maximum value of -Fb determines the number of bits that Fb
	   uses. Uniform quantization of -Fb, qntz_Fb, uses most
	   significant bits of -Fb. Note that now qntz_Fb has positive
	   values.  If Q_EQ = 32KB, W = 2, qlen = 160KB then the maximum
	   value for -Fb is 457728, which can be represented using 19bits
	   (110 1111 1100 0000 0000). To get the 6 most significant bits
	   --- considering that -Fb will use at most 19 bits ---, we need
	   to discard the 13 least significant bits (>> 13).
	*/
	qntz_Fb = ((u32) -Fb) >> 13;
	
	generate_fb_frame = 0;
	q->sample -= len;
	if (q->sample < 0) {
		if (qntz_Fb > 0) {
			generate_fb_frame = 1;
			printk(KERN_WARNING "Generate feedback, Fb 0x%x, qntz_Fb 0x%x",
				   Fb, qntz_Fb);
		}
		q->qlen_old = q->qlen;
		/* TODO: random sampling */
		q->sample = 153600;
	}
	
	if (generate_fb_frame && skb && skb->network_header &&
		(skb->protocol == __constant_htons(ETH_P_IP))) {
		/* Since we are using IP addresses, we cant sample non-IP
		   packets. */
		
		printk(KERN_EMERG "Sending Fb...");
				
		/* Filling the qcn_frame structure */
		memset(&frame, 0, sizeof(struct qcn_frame));
		iph = ip_hdr(skb);
		frame.DA = iph->daddr;	/* Already in network byte order */
		frame.SA = iph->saddr;	/* Already in network byte order */
		frame.Fb = htonl(qntz_Fb);
		frame.qoff = htonl(QCN_Q_EQ - q->qlen);
		frame.qdelta = htonl(q->qlen - q->qlen_old);

		if (!kfifo_put(&q->th.frame_fifo, &frame))
			printk(KERN_EMERG "kfifo is full!");
		else
			up(&q->th.sem_frame_fifo);

	}
	/* End QCN Algorithm */

	ret = qdisc_enqueue(skb, q->qdisc);
	if (ret != 0) {
		if (net_xmit_drop_count(ret))
			sch->qstats.drops++;
		return ret;
	}

	sch->q.qlen++;
	sch->bstats.bytes += qdisc_pkt_len(skb);
	sch->bstats.packets++;
	return 0;
}

static unsigned int tbf_drop(struct Qdisc* sch)
{
	struct tbf_sched_data *q = qdisc_priv(sch);
	unsigned int len = 0;

	if (q->qdisc->ops->drop && (len = q->qdisc->ops->drop(q->qdisc)) != 0) {
		sch->q.qlen--;
		sch->qstats.drops++;
	}
	return len;
}

static struct sk_buff *tbf_dequeue(struct Qdisc* sch)
{
	struct tbf_sched_data *q = qdisc_priv(sch);
	struct sk_buff *skb;

	skb = q->qdisc->ops->peek(q->qdisc);

	if (skb) {
		psched_time_t now;
		long toks;
		long ptoks = 0;
		unsigned int len = qdisc_pkt_len(skb);

		now = psched_get_time();
		toks = psched_tdiff_bounded(now, q->t_c, q->buffer);

		if (q->P_tab) {
			ptoks = toks + q->ptokens;
			if (ptoks > (long)q->mtu)
				ptoks = q->mtu;
			ptoks -= L2T_P(q, len);
		}
		toks += q->tokens;
		if (toks > (long)q->buffer)
			toks = q->buffer;
		toks -= L2T(q, len);

		if ((toks|ptoks) >= 0) {
			skb = qdisc_dequeue_peeked(q->qdisc);
			if (unlikely(!skb))
				return NULL;

			q->t_c = now;
			q->tokens = toks;
			q->ptokens = ptoks;
			sch->q.qlen--;
			sch->flags &= ~TCQ_F_THROTTLED;

			/* QCN: Update the queue len */
			q->qlen -= len;

			return skb;
		}

		qdisc_watchdog_schedule(&q->watchdog,
								now + max_t(long, -toks, -ptoks));

		/* Maybe we have a shorter packet in the queue,
		   which can be sent now. It sounds cool,
		   but, however, this is wrong in principle.
		   We MUST NOT reorder packets under these circumstances.

		   Really, if we split the flow into independent
		   subflows, it would be a very good solution.
		   This is the main idea of all FQ algorithms
		   (cf. CSZ, HPFQ, HFSC)
		*/

		sch->qstats.overlimits++;
	}
	return NULL;
}

static void tbf_reset(struct Qdisc* sch)
{
	struct tbf_sched_data *q = qdisc_priv(sch);

	qdisc_reset(q->qdisc);
	sch->q.qlen = 0;
	q->t_c = psched_get_time();
	q->tokens = q->buffer;
	q->ptokens = q->mtu;
	qdisc_watchdog_cancel(&q->watchdog);
}

static const struct nla_policy tbf_policy[TCA_TBF_MAX + 1] = {
	[TCA_TBF_PARMS]	= { .len = sizeof(struct tc_tbf_qopt) },
	[TCA_TBF_RTAB]	= { .type = NLA_BINARY, .len = TC_RTAB_SIZE },
	[TCA_TBF_PTAB]	= { .type = NLA_BINARY, .len = TC_RTAB_SIZE },
};

static int tbf_change(struct Qdisc* sch, struct nlattr *opt)
{
	int err;
	struct tbf_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_TBF_PTAB + 1];
	struct tc_tbf_qopt *qopt;
	struct qdisc_rate_table *rtab = NULL;
	struct qdisc_rate_table *ptab = NULL;
	struct Qdisc *child = NULL;
	int max_size,n;

	err = nla_parse_nested(tb, TCA_TBF_PTAB, opt, tbf_policy);
	if (err < 0)
		return err;

	err = -EINVAL;
	if (tb[TCA_TBF_PARMS] == NULL)
		goto done;

	qopt = nla_data(tb[TCA_TBF_PARMS]);
	rtab = qdisc_get_rtab(&qopt->rate, tb[TCA_TBF_RTAB]);
	if (rtab == NULL)
		goto done;

	if (qopt->peakrate.rate) {
		if (qopt->peakrate.rate > qopt->rate.rate)
			ptab = qdisc_get_rtab(&qopt->peakrate, tb[TCA_TBF_PTAB]);
		if (ptab == NULL)
			goto done;
	}

	for (n = 0; n < 256; n++)
		if (rtab->data[n] > qopt->buffer) break;
	max_size = (n << qopt->rate.cell_log)-1;
	if (ptab) {
		int size;

		for (n = 0; n < 256; n++)
			if (ptab->data[n] > qopt->mtu) break;
		size = (n << qopt->peakrate.cell_log)-1;
		if (size < max_size) max_size = size;
	}
	if (max_size < 0)
		goto done;

	if (qopt->limit > 0) {
		child = fifo_create_dflt(sch, &bfifo_qdisc_ops, qopt->limit);
		if (IS_ERR(child)) {
			err = PTR_ERR(child);
			goto done;
		}
	}

	sch_tree_lock(sch);
	if (child) {
		qdisc_tree_decrease_qlen(q->qdisc, q->qdisc->q.qlen);
		qdisc_destroy(q->qdisc);
		q->qdisc = child;
	}
	q->limit = qopt->limit;
	q->mtu = qopt->mtu;
	q->max_size = max_size;
	q->buffer = qopt->buffer;
	q->tokens = q->buffer;
	q->ptokens = q->mtu;

	swap(q->R_tab, rtab);
	swap(q->P_tab, ptab);

	sch_tree_unlock(sch);
	err = 0;
done:
	if (rtab)
		qdisc_put_rtab(rtab);
	if (ptab)
		qdisc_put_rtab(ptab);
	return err;
}

static int tbf_init(struct Qdisc* sch, struct nlattr *opt)
{
	struct tbf_sched_data *q = qdisc_priv(sch);

	if (opt == NULL)
		return -EINVAL;

	q->t_c = psched_get_time();
	qdisc_watchdog_init(&q->watchdog, sch);
	q->qdisc = &noop_qdisc;

	/* QCN Parameters */
	printk(KERN_EMERG "Initializing QCN CP Variables");

	/* Initializing variables */
	q->qlen = 0;
	q->qlen_old = 0;
	q->sample = 153600;

	/* Creating the Feedback Sender Thread */
	memset(&q->th, 0, sizeof(struct qcn_kthread_t));
	q->th.tsk = NULL;
	q->th.sock = NULL;
	/* Thread's fifo queue, for qcn frames */
	sema_init(&q->th.sem_frame_fifo, 0);
	q->th.frame_fifo_alloc = 0;
	if (kfifo_alloc(&q->th.frame_fifo, FRAME_KFIFO_SIZE, GFP_KERNEL)) {
		printk(KERN_EMERG "error kfifo_alloc\n");
		q->th.frame_fifo_alloc = 1;
	}

	q->th.tsk = kthread_create(qcn_feedback_sender, sch,
							   THREAD_NAME, sch->handle);
	if (IS_ERR(q->th.tsk)) {
		printk(KERN_EMERG "Unable to start the Feedback Sender Thread\n");
		kfree(q->th.tsk);
		q->th.tsk = NULL;
	} else {
		wake_up_process(q->th.tsk);
	}

	return tbf_change(sch, opt);
}

static void tbf_destroy(struct Qdisc *sch)
{
	struct tbf_sched_data *q = qdisc_priv(sch);

	/* Destroying the Feedback Sender Thread */
	if (q->th.tsk == NULL)
		printk(KERN_EMERG "Feedback Sender Thread is not running!");
	else
		kthread_stop(q->th.tsk);
	if (q->th.frame_fifo_alloc)
		kfifo_free(&q->th.frame_fifo);

	qdisc_watchdog_cancel(&q->watchdog);

	if (q->P_tab)
		qdisc_put_rtab(q->P_tab);
	if (q->R_tab)
		qdisc_put_rtab(q->R_tab);

	qdisc_destroy(q->qdisc);
}

static int tbf_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct tbf_sched_data *q = qdisc_priv(sch);
	struct nlattr *nest;
	struct tc_tbf_qopt opt;

	nest = nla_nest_start(skb, TCA_OPTIONS);
	if (nest == NULL)
		goto nla_put_failure;

	opt.limit = q->limit;
	opt.rate = q->R_tab->rate;
	if (q->P_tab)
		opt.peakrate = q->P_tab->rate;
	else
		memset(&opt.peakrate, 0, sizeof(opt.peakrate));
	opt.mtu = q->mtu;
	opt.buffer = q->buffer;
	NLA_PUT(skb, TCA_TBF_PARMS, sizeof(opt), &opt);

	nla_nest_end(skb, nest);
	return skb->len;

nla_put_failure:
	nla_nest_cancel(skb, nest);
	return -1;
}

static int tbf_dump_class(struct Qdisc *sch, unsigned long cl,
						  struct sk_buff *skb, struct tcmsg *tcm)
{
	struct tbf_sched_data *q = qdisc_priv(sch);

	tcm->tcm_handle |= TC_H_MIN(1);
	tcm->tcm_info = q->qdisc->handle;

	return 0;
}

static int tbf_graft(struct Qdisc *sch, unsigned long arg, struct Qdisc *new,
					 struct Qdisc **old)
{
	struct tbf_sched_data *q = qdisc_priv(sch);

	if (new == NULL)
		new = &noop_qdisc;

	sch_tree_lock(sch);
	*old = q->qdisc;
	q->qdisc = new;
	qdisc_tree_decrease_qlen(*old, (*old)->q.qlen);
	qdisc_reset(*old);
	sch_tree_unlock(sch);

	return 0;
}

static struct Qdisc *tbf_leaf(struct Qdisc *sch, unsigned long arg)
{
	struct tbf_sched_data *q = qdisc_priv(sch);
	return q->qdisc;
}

static unsigned long tbf_get(struct Qdisc *sch, u32 classid)
{
	return 1;
}

static void tbf_put(struct Qdisc *sch, unsigned long arg)
{
}

static void tbf_walk(struct Qdisc *sch, struct qdisc_walker *walker)
{
	if (!walker->stop) {
		if (walker->count >= walker->skip)
			if (walker->fn(sch, 1, walker) < 0) {
				walker->stop = 1;
				return;
			}
		walker->count++;
	}
}

static const struct Qdisc_class_ops tbf_class_ops =
{
	.graft		=	tbf_graft,
	.leaf		=	tbf_leaf,
	.get		=	tbf_get,
	.put		=	tbf_put,
	.walk		=	tbf_walk,
	.dump		=	tbf_dump_class,
};

static struct Qdisc_ops tbf_qdisc_ops __read_mostly = {
	.next		=	NULL,
	.cl_ops		=	&tbf_class_ops,
	.id		=	"tbf",
	.priv_size	=	sizeof(struct tbf_sched_data),
	.enqueue	=	tbf_enqueue,
	.dequeue	=	tbf_dequeue,
	.peek		=	qdisc_peek_dequeued,
	.drop		=	tbf_drop,
	.init		=	tbf_init,
	.reset		=	tbf_reset,
	.destroy	=	tbf_destroy,
	.change		=	tbf_change,
	.dump		=	tbf_dump,
	.owner		=	THIS_MODULE,
};

static int __init tbf_module_init(void)
{
	return register_qdisc(&tbf_qdisc_ops);
}

static void __exit tbf_module_exit(void)
{
	unregister_qdisc(&tbf_qdisc_ops);
}
module_init(tbf_module_init)
module_exit(tbf_module_exit)
MODULE_LICENSE("GPL");
