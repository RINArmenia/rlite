/*
 * RINA normal IPC process
 *
 *    Vincenzo Maffione <v.maffione@gmail.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/types.h>
#include <rina/rina-utils.h>
#include <rina/rina-ipcp-types.h>
#include "rina-kernel.h"

#include <linux/module.h>
#include <linux/aio.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/hashtable.h>
#include <linux/ktime.h>


#define PDUFT_HASHTABLE_BITS    3

struct rina_normal {
    struct ipcp_entry *ipcp;

    /* Implementation of the PDU Forwarding Table (PDUFT). */
    DECLARE_HASHTABLE(pdu_ft, PDUFT_HASHTABLE_BITS);
};

static void *
rina_normal_create(struct ipcp_entry *ipcp)
{
    struct rina_normal *priv;

    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv) {
        return NULL;
    }

    priv->ipcp = ipcp;
    hash_init(priv->pdu_ft);

    printk("%s: New IPC created [%p]\n", __func__, priv);

    return priv;
}

static void
rina_normal_destroy(struct ipcp_entry *ipcp)
{
    struct rina_normal *priv = ipcp->priv;

    kfree(priv);

    printk("%s: IPC [%p] destroyed\n", __func__, priv);
}

enum hrtimer_restart
snd_inact_tmr_cb(struct hrtimer *timer)
{
    struct dtp *dtp = container_of(timer, struct dtp, snd_inact_tmr);

    PD("%s\n", __func__);
    dtp->set_drf = true;

    /* InitialSeqNumPolicy */
    dtp->next_seq_num_to_send = 0;

    /* Discard the retransmission queue. */

    /* Discard the closed window queue */

    /* Send control ack PDU */

    /* Send transfer PDU with zero length. */

    /* Notify user flow that there has been no activity for a while */

    return HRTIMER_NORESTART;
}

enum hrtimer_restart
rcv_inact_tmr_cb(struct hrtimer *timer)
{
    PD("%s\n", __func__);
    return HRTIMER_NORESTART;
}

static int
rina_normal_flow_init(struct ipcp_entry *ipcp, struct flow_entry *flow)
{
    struct dtp *dtp = &flow->dtp;
    struct fc_config *fc = &flow->cfg.dtcp.fc;

    dtp->set_drf = true;
    dtp->next_seq_num_to_send = 0;
    dtp->snd_lwe = dtp->snd_rwe = dtp->next_seq_num_to_send;
    dtp->last_seq_num_sent = -1;
    dtp->rcv_lwe = dtp->rcv_rwe = 0;
    dtp->max_seq_num_rcvd = -1;
    dtp->next_snd_ctl_seq = dtp->last_ctrl_seq_num_rcvd = 0;

    dtp->snd_inact_tmr.function = snd_inact_tmr_cb;
    dtp->rcv_inact_tmr.function = rcv_inact_tmr_cb;

    if (fc->fc_type == RINA_FC_T_WIN) {
        dtp->max_cwq_len = fc->cfg.w.max_cwq_len;
        dtp->snd_rwe += fc->cfg.w.initial_credit;
        dtp->rcv_rwe += fc->cfg.w.initial_credit;
    }

    return 0;
}

static struct flow_entry *
pduft_lookup(struct rina_normal *priv, uint64_t dest_addr)
{
    struct flow_entry *entry;
    struct hlist_head *head;

    head = &priv->pdu_ft[hash_min(dest_addr, HASH_BITS(priv->pdu_ft))];
    hlist_for_each_entry(entry, head, ftnode) {
        if (entry->pduft_dest_addr == dest_addr) {
            return entry;
        }
    }

    return NULL;
}

static int
rina_normal_sdu_write(struct ipcp_entry *ipcp,
                      struct flow_entry *flow,
                      struct rina_buf *rb)
{
    struct rina_normal *priv = (struct rina_normal *)ipcp->priv;
    struct rina_pci *pci;
    struct flow_entry *lower_flow;
    struct ipcp_entry *lower_ipcp;
    struct dtp *dtp = &flow->dtp;
    struct fc_config *fc = &flow->cfg.dtcp.fc;
    int len = rb->len;
    int ret;

    lower_flow = pduft_lookup(priv, flow->remote_addr);
    if (unlikely(!lower_flow && flow->remote_addr != ipcp->addr)) {
        PD("%s: No route to IPCP %lu, dropping packet\n", __func__,
            (long unsigned)flow->remote_addr);
        rina_buf_free(rb);
        return 0;
    }
    if (lower_flow) {
        /* This SDU will be sent to a remote IPCP, using an N-1 flow. */
        lower_ipcp = lower_flow->txrx.ipcp;
        BUG_ON(!lower_ipcp);
    }

    /* Stop the sender inactivity timer if it was activated or the callback
     * running , but without waiting for the callback to finish. */
    hrtimer_try_to_cancel(&dtp->snd_inact_tmr);

    rina_buf_pci_push(rb);

    pci = RINA_BUF_PCI(rb);
    pci->dst_addr = flow->remote_addr;
    pci->src_addr = ipcp->addr;
    pci->conn_id.qos_id = 0;
    pci->conn_id.dst_cep = flow->remote_port;
    pci->conn_id.src_cep = flow->local_port;
    pci->pdu_type = PDU_TYPE_DT;
    pci->pdu_flags = dtp->set_drf ? 1 : 0;
    pci->seqnum = dtp->next_seq_num_to_send++;

    dtp->set_drf = false;
    if (fc->fc_type == RINA_FC_T_WIN) {
        if (pci->seqnum > dtp->snd_rwe) {
            /* PDU not in the sender window, let's try to
             * insert it into the Closed Window Queue. */
            if (dtp->cwq_len < dtp->max_cwq_len) {
                /* There's room in the queue. */
                list_add_tail(&rb->node, &dtp->cwq);
                dtp->cwq_len++;
            } else {
                /* POL: FlowControlOverrun */

                /* TODO Set blocking write (backpressure) ? */
                PD("%s: Dropping overrun PDU [%lu]", __func__,
                        (long unsigned)pci->seqnum);
                rina_buf_free(rb);

                return len;
            }
        } else {
            /* PDU in the sender window. */
            /* POL: TxControl. */
            dtp->snd_lwe = flow->dtp.next_seq_num_to_send;
            dtp->last_seq_num_sent = pci->seqnum;
        }
    } else {
        /* DTCP not present */
        dtp->snd_lwe = flow->dtp.next_seq_num_to_send; /* NIS */
        dtp->last_seq_num_sent = pci->seqnum;
    }

    if (lower_flow) {
        /* Directly call the underlying IPCP for now. RMT component
         * is not implemented explicitely for now. */
        ret = lower_ipcp->ops.sdu_write(lower_ipcp, lower_flow, rb);
        if (likely(ret >= sizeof(struct rina_pci))) {
            ret -= sizeof(struct rina_pci);
        }
    } else {
        /* This SDU gets loopbacked to this IPCP, since this is a
         * self flow (flow->remote_addr == ipcp->addr). */

        ret = ipcp->ops.sdu_rx(ipcp, rb);
        ret = (ret == 0) ? len : ret;
    }

    /* 3 * (MPL + R + A) */
    hrtimer_start(&dtp->snd_inact_tmr, ktime_set(0, 1 << 30),
                  HRTIMER_MODE_REL);

    return ret;
}

static int
rina_normal_mgmt_sdu_write(struct ipcp_entry *ipcp,
                           const struct rina_mgmt_hdr *mhdr,
                           struct rina_buf *rb)
{
    struct rina_normal *priv = (struct rina_normal *)ipcp->priv;
    struct rina_pci *pci;
    struct flow_entry *lower_flow;
    struct ipcp_entry *lower_ipcp;
    uint64_t dst_addr = 0; /* Not valid. */
    int ret = rb->len;

    if (mhdr->type == RINA_MGMT_HDR_T_OUT_DST_ADDR) {
        lower_flow = pduft_lookup(priv, mhdr->remote_addr);
        if (unlikely(!lower_flow)) {
            PI("%s: No route to IPCP %lu, dropping packet\n", __func__,
                    (long unsigned)mhdr->remote_addr);
            rina_buf_free(rb);

            return ret;
        }
        dst_addr = mhdr->remote_addr;
    } else if (mhdr->type == RINA_MGMT_HDR_T_OUT_LOCAL_PORT) {
        lower_flow = flow_lookup(mhdr->local_port);
        if (!lower_flow || lower_flow->upper.ipcp != ipcp) {
            PI("%s: Invalid mgmt header local port %u, "
                    "dropping packet\n", __func__,
                    mhdr->local_port);
            rina_buf_free(rb);

            return ret;
        }
    } else {
        rina_buf_free(rb);

        return ret;
    }
    lower_ipcp = lower_flow->txrx.ipcp;
    BUG_ON(!lower_ipcp);

    rina_buf_pci_push(rb);

    pci = RINA_BUF_PCI(rb);
    pci->dst_addr = dst_addr;
    pci->src_addr = ipcp->addr;
    pci->conn_id.qos_id = 0;  /* Not valid. */
    pci->conn_id.dst_cep = 0; /* Not valid. */
    pci->conn_id.src_cep = 0; /* Not valid. */
    pci->pdu_type = PDU_TYPE_MGMT;
    pci->pdu_flags = 0; /* Not valid. */
    pci->seqnum = 0; /* Not valid. */

    ret = lower_ipcp->ops.sdu_write(lower_ipcp, lower_flow, rb);
    if (ret >= sizeof(*pci)) {
        ret -= sizeof(*pci);
    }

    return ret;
}

static int
rina_normal_config(struct ipcp_entry *ipcp, const char *param_name,
                   const char *param_value)
{
    struct rina_normal *priv = (struct rina_normal *)ipcp->priv;
    int ret = -EINVAL;

    if (strcmp(param_name, "address") == 0) {
        uint64_t address;

        ret = kstrtou64(param_value, 10, &address);
        if (ret == 0) {
            PI("IPCP %u address set to %llu\n", ipcp->id, address);
            ipcp->addr = address;
        }
    }

    (void)priv;

    return ret;
}

static int
rina_normal_pduft_set(struct ipcp_entry *ipcp, uint64_t dest_addr,
                      struct flow_entry *flow)
{
    struct rina_normal *priv = (struct rina_normal *)ipcp->priv;
    struct flow_entry *prev;

    prev = pduft_lookup(priv, dest_addr);
    if (prev) {
        hash_del(&flow->ftnode);
    }

    flow->pduft_dest_addr = dest_addr;
    hash_add(priv->pdu_ft, &flow->ftnode, dest_addr);

    return 0;
}

static void
sdu_rx_sv_update(struct ipcp_entry *ipcp, struct flow_entry *flow,
                 uint64_t seqnum)
{
    const struct dtcp_config *cfg = &flow->cfg.dtcp;

    if (cfg->flow_control) {
        /* POL: RcvrFlowControl */
        /* We should not unconditionally increment the receiver RWE,
         * but instead use some logic related to buffer management
         * (e.g. see the amount of receiver buffer available). */
        flow->dtp.rcv_rwe++;

        if (!cfg->rtx_control) {
            /* POL: ReceivingFlowControl */
            if (cfg->fc.fc_type == RINA_FC_T_WIN) {
                /* Send a flow control only control PDU. */
                struct rina_buf *rb = rina_buf_alloc_ctrl(2, GFP_ATOMIC);
                struct rina_pci_ctrl *pcic;

                if (rb) {
                    pcic = (struct rina_pci_ctrl *)RINA_BUF_DATA(rb);
                    pcic->base.dst_addr = flow->remote_addr;
                    pcic->base.src_addr = ipcp->addr;
                    pcic->base.conn_id.qos_id = 0;
                    pcic->base.conn_id.dst_cep = flow->remote_port;
                    pcic->base.conn_id.src_cep = flow->local_port;
                    pcic->base.pdu_type = PDU_TYPE_FC;
                    pcic->base.pdu_flags = 0;
                    pcic->base.seqnum = flow->dtp.next_snd_ctl_seq++;
                    pcic->last_ctrl_seq_num_rcvd =
                            flow->dtp.last_ctrl_seq_num_rcvd;
                    pcic->new_rwe = flow->dtp.rcv_rwe;
                    pcic->new_lwe = flow->dtp.rcv_lwe;
                    pcic->my_rwe = flow->dtp.snd_rwe;
                    pcic->my_lwe = flow->dtp.snd_lwe;
                    rina_buf_free(rb); //TODO
                }
            }
        }
    }
}

static int
rina_normal_sdu_rx(struct ipcp_entry *ipcp, struct rina_buf *rb)
{
    struct rina_pci *pci = RINA_BUF_PCI(rb);
    struct flow_entry *flow = flow_lookup(pci->conn_id.dst_cep);
    struct dtp *dtp;

    if (!flow) {
        PI("%s: No flow for port-id %u: dropping PDU",
                __func__, pci->conn_id.dst_cep);
        rina_buf_free(rb);
        return 0;
    }

    dtp = &flow->dtp;

    hrtimer_try_to_cancel(&dtp->rcv_inact_tmr);

    rina_buf_pci_pop(rb);

    if (pci->pdu_type == PDU_TYPE_DT) {
        /* Data transfer PDU. */
        int ret;

        if (pci->pdu_flags & 1) {
            /* DRF is set: either first PDU or new run. */

            /* Flush reassembly queue */

            dtp->rcv_lwe = pci->seqnum + 1;
            dtp->max_seq_num_rcvd = pci->seqnum;

            sdu_rx_sv_update(ipcp, flow, pci->seqnum);

            ret = rina_sdu_rx(ipcp, rb, pci->conn_id.dst_cep);
        } else {
            if (unlikely(pci->seqnum < dtp->rcv_lwe)) {
                /* This is a duplicate. Probably we sould not drop it
                 * if the flow configuration does not require it. */
                PD("%s: Dropping duplicate PDU [seq=%lu]\n", __func__,
                    (long unsigned)pci->seqnum);
                rina_buf_free(rb);

                /* Send ACK flow PDU */

                return 0;
            }

            if (unlikely(dtp->rcv_lwe < pci->seqnum &&
                                pci->seqnum <= dtp->max_seq_num_rcvd)) {
                /* This may go in a gap or be a duplicate
                 * amongst the gaps. */

                PD("%s: Possible gap fill, RLWE jumps %lu --> %lu\n",
                    __func__, (long unsigned)dtp->rcv_lwe,
                    (unsigned long)pci->seqnum + 1);
                // TODO, for now just pass it up, and ignore gaps

                dtp->rcv_lwe = pci->seqnum + 1;
                ret = rina_sdu_rx(ipcp, rb, pci->conn_id.dst_cep);
            } else if (pci->seqnum == dtp->max_seq_num_rcvd + 1) {
                /* In order PDU. */
                dtp->rcv_lwe++;
                dtp->max_seq_num_rcvd++;
                ret = rina_sdu_rx(ipcp, rb, pci->conn_id.dst_cep);
            } else {
                /* Out of order. */
                PD("%s: Out of order packet, RLWE jumps %lu --> %lu\n",
                    __func__, (long unsigned)dtp->rcv_lwe,
                    (unsigned long)pci->seqnum + 1);

                // TODO, for now just pass it up, and ignore out of order
                dtp->rcv_lwe = pci->seqnum + 1;
                dtp->max_seq_num_rcvd = pci->seqnum;
                ret = rina_sdu_rx(ipcp, rb, pci->conn_id.dst_cep);
            }
        }

        /* 2 * (MPL + R + A) */
        hrtimer_start(&dtp->rcv_inact_tmr, ktime_set(0, (1 << 30)/3*2),
                      HRTIMER_MODE_REL);

        return ret;
    }

    /* Control PDU. TODO */
    rina_buf_free(rb);

    return 0;
}

static int __init
rina_normal_init(void)
{
    struct ipcp_factory factory;
    int ret;

    memset(&factory, 0, sizeof(factory));
    factory.owner = THIS_MODULE;
    factory.dif_type = DIF_TYPE_NORMAL;
    factory.create = rina_normal_create;
    factory.ops.destroy = rina_normal_destroy;
    factory.ops.flow_allocate_req = NULL; /* Reflect to userspace. */
    factory.ops.flow_allocate_resp = NULL; /* Reflect to userspace. */
    factory.ops.flow_init = rina_normal_flow_init;
    factory.ops.sdu_write = rina_normal_sdu_write;
    factory.ops.config = rina_normal_config;
    factory.ops.pduft_set = rina_normal_pduft_set;
    factory.ops.mgmt_sdu_write = rina_normal_mgmt_sdu_write;
    factory.ops.sdu_rx = rina_normal_sdu_rx;

    ret = rina_ipcp_factory_register(&factory);

    return ret;
}

static void __exit
rina_normal_fini(void)
{
    rina_ipcp_factory_unregister(DIF_TYPE_NORMAL);
}

module_init(rina_normal_init);
module_exit(rina_normal_fini);
MODULE_LICENSE("GPL");
