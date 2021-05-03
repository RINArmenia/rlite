/*
 * Control functionalities for the rlite stack.
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
#include "rlite/kernel-msg.h"
#include "rlite/utils.h"
#include "rlite/version.h"
#include "rlite-kernel.h"

#include <linux/module.h>
#include <linux/file.h>
#include <linux/aio.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/bitmap.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/nsproxy.h>
#include <net/net_namespace.h>
#include <asm/compat.h>

int verbosity = RL_VERB_DBG;
EXPORT_SYMBOL(verbosity);
module_param(verbosity, int, 0644);

struct rl_ctrl;
struct rl_dm;

/* The signature of a message handler. */
typedef int (*rl_msg_handler_t)(struct rl_ctrl *rc, struct rl_msg_base *bmsg);

/* Data structure associated to the an rlite control device. */
struct rl_ctrl {
    char msgbuf[1024];
    rl_msg_handler_t *handlers;

    struct file *file; /* backpointer */

    /* Pointer to the parent data model. */
    struct rl_dm *dm;

    /* Upqueue-related data structures. */
    struct list_head upqueue;
#define RL_UPQUEUE_SIZE_MAX (1 << 14)
    unsigned int upqueue_size;
    spinlock_t upqueue_lock;
    wait_queue_head_t upqueue_wqh;

    struct list_head flows_fetch_q;
    struct list_head regs_fetch_q;
    struct list_head node;

    unsigned flags;
};

/* A message to be delivered to an userspace application through an
 * rlite control device. */
struct upqueue_entry {
    void *sermsg;
    size_t serlen;
    struct list_head node;
};

struct registered_appl {
    /* Name of the registered application. */
    char *name;

    /* The event-loop where the registered applications registered
     * (and where it can be reached by flow allocation requests). */
    struct rl_ctrl *rc;

    /* Event id used by the registration request, needed if the
     * the IPCP is partially implemented in userspace. */
    uint32_t event_id;

    /* The IPCP where the application is registered. */
    struct ipcp_entry *ipcp;

#define APPL_REG_PENDING 0x1
#define APPL_REG_COMPLETE 0x2
    /* Is registration complete or are we waiting for uipcp response? */
    uint8_t state;

    unsigned int refcnt;
    struct list_head node;
};

#define IPCP_ID_BITMAP_SIZE 256
#define PORT_ID_BITMAP_SIZE 65536
#define CEP_ID_BITMAP_SIZE PORT_ID_BITMAP_SIZE
#define IPCP_HASHTABLE_BITS 6
#define PORT_ID_HASHTABLE_BITS 10
#define CEP_ID_HASHTABLE_BITS 10

/* Global data structures, shared by all the rl_dm instances. In other works
 * this is common to all the network namespaces. */
static struct rl_global {
    /* Lock for ipcp_factories. */
    struct mutex lock;
    struct list_head ipcp_factories;
#ifdef CONFIG_NET_NS
    /* This hash table maps network namespaces to rl_dm instances. */
    DECLARE_HASHTABLE(netns_table, 5);
#endif /* CONFIG_NET_NS */
} rl_global;

/* The rlite data model.
 * Main data structure containing all the rlite kernel objects for a given
 * network namespace. Objects include IPCPs, DIFs, registered applications,
 * flows, control devices, hash tables. etc. */
struct rl_dm {
    /* Bitmap to manage IPC process ids. */
    DECLARE_BITMAP(ipcp_id_bitmap, IPCP_ID_BITMAP_SIZE);

    /* Hash table to store information about each IPC process. */
    DECLARE_HASHTABLE(ipcp_table, IPCP_HASHTABLE_BITS);

    /* Bitmap to manage port ids. */
    DECLARE_BITMAP(port_id_bitmap, PORT_ID_BITMAP_SIZE);

    /* Hash tables to store information about each flow. */
    DECLARE_HASHTABLE(flow_table, PORT_ID_HASHTABLE_BITS);
    DECLARE_HASHTABLE(flow_table_by_cep, CEP_ID_HASHTABLE_BITS);
    uint32_t uid_cnt;

    /* Bitmap to manage connection endpoint ids. */
    DECLARE_BITMAP(cep_id_bitmap, CEP_ID_BITMAP_SIZE);

    struct list_head difs;

    /* Lock for flows table. */
    rwlock_t flows_lock;

    /* Lock for IPCPs table. */
    spinlock_t ipcps_lock;

    /* Lock for DIFs list. */
    spinlock_t difs_lock;

    /* List that contains all the rlite ctrl devices that
     * are currently opened. */
    struct list_head ctrl_devs;

    /* Lock for the ctrl_devs list */
    struct mutex general_lock;

    /* Data structures for deferred removal of registered_appl structs. */
    struct list_head appl_removeq;
    struct work_struct appl_removew;
    spinlock_t appl_removeq_lock;

    /* Data structures for deferred removal of flow_entry structs. */
    struct timer_list flows_putq_tmr;
    struct list_head flows_removeq;
    struct list_head flows_putq;
    struct work_struct flows_removew;

    /* Reference to the parent network namespace, and node for the
     * netns hash table. */
    struct net *net;
    unsigned int refcnt;
    struct hlist_node node;
};

static struct rl_dm *rl_dm_get(void);
static void rl_dm_put(struct rl_dm *dm);
static struct rl_dm *rl_dm_getref(struct rl_dm *dm);

#define FLOCK(dm) write_lock_bh(&dm->flows_lock)
#define FUNLOCK(dm) write_unlock_bh(&dm->flows_lock)
#define FRLOCK(dm) read_lock_bh(&dm->flows_lock)
#define FRUNLOCK(dm) read_unlock_bh(&dm->flows_lock)
#define PLOCK(dm) spin_lock_bh(&dm->ipcps_lock)
#define PUNLOCK(dm) spin_unlock_bh(&dm->ipcps_lock)
#define RALOCK(_p) spin_lock_bh(&(_p)->regapp_lock)
#define RAUNLOCK(_p) spin_unlock_bh(&(_p)->regapp_lock)

struct net *
rl_ipcp_net(struct ipcp_entry *ipcp)
{
    return ipcp->dm->net;
}
EXPORT_SYMBOL(rl_ipcp_net);

static struct ipcp_factory *
ipcp_factories_find(const char *dif_type)
{
    struct ipcp_factory *factory;

    if (!dif_type) {
        return NULL;
    }

    list_for_each_entry (factory, &rl_global.ipcp_factories, node) {
        if (strcmp(factory->dif_type, dif_type) == 0) {
            return factory;
        }
    }

    return NULL;
}

int
rl_ipcp_factory_register(struct ipcp_factory *factory)
{
    int ret = 0;

    if (!factory || !factory->create || !factory->owner || !factory->dif_type) {
        return -EINVAL;
    }

    mutex_lock(&rl_global.lock);

    if (ipcp_factories_find(factory->dif_type)) {
        ret = -EBUSY;
        goto out;
    }

    /* Check if IPCP ops are ok. */
    if (!factory->ops.destroy || !factory->ops.sdu_write) {
        ret = -EINVAL;
        goto out;
    }

    if (factory->ops.pduft_set &&
        (!factory->ops.pduft_del || !factory->ops.pduft_del_addr ||
         !factory->ops.pduft_flush || !factory->ops.pduft_flush_by_flow)) {
        ret = -EINVAL;
        goto out;
    }

    /* Insert the new factory into the IPC process factories
     * list. Ownership is not passed, it stills remains to
     * the invoking IPCP module. */
    list_add_tail(&factory->node, &rl_global.ipcp_factories);

    PI("IPC processes factory '%s' registered\n", factory->dif_type);
out:
    mutex_unlock(&rl_global.lock);

    return ret;
}
EXPORT_SYMBOL(rl_ipcp_factory_register);

int
rl_ipcp_factory_unregister(const char *dif_type)
{
    struct ipcp_factory *factory;

    mutex_lock(&rl_global.lock);

    factory = ipcp_factories_find(dif_type);
    if (!factory) {
        mutex_unlock(&rl_global.lock);
        return -EINVAL;
    }

    /* Just remove from the list, we don't have ownership of
     * the factory object. */
    list_del_init(&factory->node);

    mutex_unlock(&rl_global.lock);

    PI("IPC processes factory '%s' unregistered\n", dif_type);

    return 0;
}
EXPORT_SYMBOL(rl_ipcp_factory_unregister);

static inline unsigned int
upqentry_size(struct upqueue_entry *entry)
{
    return entry->serlen + sizeof(*entry);
}

int
rl_upqueue_append(struct rl_ctrl *rc, const struct rl_msg_base *rmsg,
                  bool maysleep)
{
    gfp_t gfp        = maysleep ? GFP_KERNEL : GFP_ATOMIC;
    unsigned long to = msecs_to_jiffies(5);
    DECLARE_WAITQUEUE(wait, current);
    struct upqueue_entry *entry;
    unsigned long exp;
    unsigned int serlen;
    void *serbuf;
    int ret = 0;

    if (rc == NULL) {
        return 0; /* Nothing to do. */
    }

    entry = rl_alloc(sizeof(*entry), gfp | __GFP_ZERO, RL_MT_UPQ);
    if (!entry) {
        RPV(1, "Out of memory\n");
        return -ENOMEM;
    }

    /* Serialize the response into serbuf and then put it into the upqueue. */
    serlen = rl_msg_serlen(rl_ker_numtables, RLITE_KER_MSG_MAX, rmsg);
    serbuf = rl_alloc(serlen, gfp | __GFP_ZERO, RL_MT_UPQ);
    if (!serbuf) {
        rl_free(entry, RL_MT_UPQ);
        RPV(1, "Out of memory\n");
        return -ENOMEM;
    }
    serlen =
        serialize_rlite_msg(rl_ker_numtables, RLITE_KER_MSG_MAX, serbuf, rmsg);

    entry->sermsg = serbuf;
    entry->serlen = serlen;

    if (maysleep) {
        add_wait_queue(&rc->upqueue_wqh, &wait);
    }

    exp = jiffies + to;

    for (;;) {
        spin_lock(&rc->upqueue_lock);
        if (rc->upqueue_size + upqentry_size(entry) > RL_UPQUEUE_SIZE_MAX) {
            /* No free space in the queue. */
            spin_unlock(&rc->upqueue_lock);
            if (!maysleep || !time_before(jiffies, exp)) {
                RPD(1, "upqueue overrun, dropping [cansleep=%d]\n", maysleep);
                rl_free(serbuf, RL_MT_UPQ);
                rl_free(entry, RL_MT_UPQ);
                ret = -ENOSPC;
                break;
            }

            /* Wait for more space, but not more than 2 seconds. */
            schedule_timeout(to);
            continue;
        }
        list_add_tail(&entry->node, &rc->upqueue);
        rc->upqueue_size += upqentry_size(entry);
        spin_unlock(&rc->upqueue_lock);
        break;
    }

    if (maysleep) {
        remove_wait_queue(&rc->upqueue_wqh, &wait);
    }

    if (ret == 0) {
        wake_up_interruptible_poll(&rc->upqueue_wqh,
                                   POLLIN | POLLRDNORM | POLLRDBAND);
    }

    return ret;
}
EXPORT_SYMBOL(rl_upqueue_append);

static struct dif *
dif_get(struct rl_dm *dm, const char *dif_name, const char *dif_type, int *err)
{
    struct dif *cur;

    *err = 0;

    spin_lock_bh(&dm->difs_lock);

    list_for_each_entry (cur, &dm->difs, node) {
        if (strcmp(cur->name, dif_name) == 0) {
            /* A DIF called 'dif_name' already exists. */
            if (strcmp(cur->ty, dif_type) == 0) {
                cur->refcnt++;
            } else {
                /* DIF type mismatch: report error. */
                cur  = NULL;
                *err = -EINVAL;
            }
            goto out;
        }
    }

    /* A DIF called 'dif_name' does not exist yet. */
    cur = rl_alloc(sizeof(*cur), GFP_ATOMIC | __GFP_ZERO, RL_MT_DIF);
    if (!cur) {
        *err = -ENOMEM;
        goto out;
    }

    cur->name = rl_strdup(dif_name, GFP_ATOMIC, RL_MT_DIF);
    if (!cur->name) {
        rl_free(cur, RL_MT_DIF);
        cur  = NULL;
        *err = -ENOMEM;
        goto out;
    }

    cur->ty = rl_strdup(dif_type, GFP_ATOMIC, RL_MT_DIF);
    if (!cur->ty) {
        rl_free(cur->name, RL_MT_DIF);
        rl_free(cur, RL_MT_DIF);
        cur  = NULL;
        *err = -ENOMEM;
        goto out;
    }

    cur->max_pdu_size = 8000; /* Currently unused. */
    cur->max_pdu_life = RL_MPL_MSECS_DFLT;
    cur->refcnt       = 1;
    list_add_tail(&cur->node, &dm->difs);

    PD("DIF %s [type '%s'] created\n", cur->name, cur->ty);

out:
    spin_unlock_bh(&dm->difs_lock);

    return cur;
}

static void
dif_put(struct rl_dm *dm, struct dif *dif)
{
    if (!dif) {
        return;
    }

    spin_lock_bh(&dm->difs_lock);
    dif->refcnt--;
    if (dif->refcnt) {
        goto out;
    }

    PD("DIF %s [type '%s'] destroyed\n", dif->name, dif->ty);

    list_del_init(&dif->node);
    rl_free(dif->ty, RL_MT_DIF);
    rl_free(dif->name, RL_MT_DIF);
    rl_free(dif, RL_MT_DIF);

out:
    spin_unlock_bh(&dm->difs_lock);
}

struct ipcp_entry *
__ipcp_get(struct rl_dm *dm, rl_ipcp_id_t ipcp_id)
{
    struct ipcp_entry *entry;
    struct hlist_head *head;

    PLOCK(dm);

    head = &dm->ipcp_table[hash_min(ipcp_id, HASH_BITS(dm->ipcp_table))];
    hlist_for_each_entry (entry, head, node) {
        if (entry->id == ipcp_id) {
            entry->refcnt++;
            PUNLOCK(dm);
            return entry;
        }
    }

    PUNLOCK(dm);

    return NULL;
}

struct ipcp_entry *
ipcp_nodm_get(rl_ipcp_id_t ipcp_id)
{
    struct rl_dm *dm        = rl_dm_get();
    struct ipcp_entry *ipcp = ipcp_get(dm, ipcp_id);

    rl_dm_put(dm);

    return ipcp;
}

struct ipcp_entry *
ipcp_select_by_dif(struct rl_dm *dm, const char *dif_name)
{
    struct ipcp_entry *selected = NULL;
    struct ipcp_entry *entry;
    int bucket;

    PLOCK(dm);

    /* Linear scan is not efficient, but let's stick to that for now. */
    hash_for_each(dm->ipcp_table, bucket, entry, node)
    {
        if (entry->flags & RL_K_IPCP_ZOMBIE) {
            /* Zombie ipcps cannot be selected. */
            continue;
        }

        if (!dif_name) {
            /* The request does not specify a DIF: select any DIF,
             * giving priority to higher ranked normal DIFs. */
            if (!selected || (strcmp(entry->dif->ty, "normal") == 0 &&
                              (strcmp(selected->dif->ty, "normal") != 0 ||
                               entry->txhdroom > selected->txhdroom))) {
                selected = entry;
            }
        } else if (strcmp(entry->dif->name, dif_name) == 0) {
            selected = entry;
            break;
        }
    }

    if (selected) {
        selected->refcnt++;
        PV("REFCNT++ %u: %u\n", selected->id, selected->refcnt);
    }

    PUNLOCK(dm);

    return selected;
}

void tx_completion_func(unsigned long arg);

static int
ipcp_add_entry(struct rl_dm *dm, struct rl_kmsg_ipcp_create *req,
               struct ipcp_entry **pentry)
{
    struct ipcp_entry *entry;
    struct ipcp_entry *cur;
    struct dif *dif;
    int bucket;
    int ret = 0;
    int cpu;

    *pentry = NULL;

    entry = rl_alloc(sizeof(*entry), GFP_KERNEL | __GFP_ZERO, RL_MT_IPCP);
    if (!entry) {
        return -ENOMEM;
    }

    entry->stats = alloc_percpu(*(entry->stats));
    if (!entry->stats) {
        rl_free(entry, RL_MT_SHIM);
        return -ENOMEM;
    }
    for_each_possible_cpu(cpu)
    {
        memset(per_cpu_ptr(entry->stats, cpu), 0, sizeof(entry->stats[0]));
    }

    PLOCK(dm);

    /* Check if an IPC process with that name already exists.
     * This check is also performed by userspace. */
    hash_for_each(dm->ipcp_table, bucket, cur, node)
    {
        if (strcmp(cur->name, req->name) == 0) {
            PUNLOCK(dm);
            free_percpu(entry->stats);
            rl_free(entry, RL_MT_IPCP);
            return -EINVAL;
        }
    }

    /* Create or take a reference to the specified DIF. */
    dif = dif_get(dm, req->dif_name, req->dif_type, &ret);
    if (!dif) {
        PUNLOCK(dm);
        free_percpu(entry->stats);
        rl_free(entry, RL_MT_IPCP);
        return ret;
    }

    /* Try to alloc an IPC process id from the bitmap. */
    entry->id = bitmap_find_next_zero_area(dm->ipcp_id_bitmap,
                                           IPCP_ID_BITMAP_SIZE, 0, 1, 0);
    if (entry->id < IPCP_ID_BITMAP_SIZE) {
        bitmap_set(dm->ipcp_id_bitmap, entry->id, 1);
        /* Build and insert an IPC process entry in the hash table. */
        entry->name             = req->name;
        req->name               = NULL; /* move */
        entry->dif              = dif;
        entry->addr             = RL_ADDR_NULL;
        entry->flow_del_wait_ms = 4000;
        entry->refcnt           = 1;
        entry->txhdroom         = 0;
        entry->rxhdroom         = 0;
        entry->tailroom         = 0;
        entry->max_sdu_size     = (1 << 16) - 1;
        INIT_LIST_HEAD(&entry->registered_appls);
        spin_lock_init(&entry->regapp_lock);
        init_waitqueue_head(&entry->uipcp_wqh);
        mutex_init(&entry->lock);
        hash_add(dm->ipcp_table, &entry->node, entry->id);
        init_waitqueue_head(&entry->tx_wqh);
        entry->dm = rl_dm_getref(dm);
        *pentry   = entry;
    } else {
        ret = -ENOSPC;
        dif_put(dm, dif);
        free_percpu(entry->stats);
        rl_free(entry, RL_MT_IPCP);
    }

    PUNLOCK(dm);

    return ret;
}

static int
ipcp_add(struct rl_dm *dm, struct rl_kmsg_ipcp_create *req,
         rl_ipcp_id_t *ipcp_id)
{
    struct ipcp_factory *factory;
    struct ipcp_entry *entry = NULL;
    int ret                  = ipcp_add_entry(dm, req, &entry);

    if (ret) {
        return ret;
    }

    BUG_ON(entry == NULL);

    mutex_lock(&rl_global.lock);

    factory = ipcp_factories_find(req->dif_type);
    if (!factory) {
        ret = -ENXIO;
        goto out;
    }

    /* Take a reference on the module that will own the new IPC
     * process, in order to prevent the owner to be unloaded
     * while the IPC process is in use.
     * Note that this operation **must** happen before the
     * constructor invocation (factory->create()), in order to
     * avoid race conditions. */
    if (!try_module_get(factory->owner)) {
        PE("IPC process module [%s] unexpectedly "
           "disappeared\n",
           factory->dif_type);
        ret = -ENXIO;
        goto out;
    }
    entry->owner = factory->owner;

    entry->priv = factory->create(entry);
    if (!entry->priv) {
        ret = -EINVAL;
        goto out;
    }

    entry->ops = factory->ops;
    entry->flags |= factory->use_cep_ids ? RL_K_IPCP_USE_CEP_IDS : 0;
    *ipcp_id = entry->id;

out:
    mutex_unlock(&rl_global.lock);
    if (ret) {
        ipcp_put(entry);
    }

    return ret;
}

static struct registered_appl *
__ipcp_application_get(struct ipcp_entry *ipcp, const char *appl_name)
{
    struct registered_appl *app;

    list_for_each_entry (app, &ipcp->registered_appls, node) {
        if (strcmp(app->name, appl_name) == 0) {
            app->refcnt++;
            return app;
        }
    }

    return NULL;
}

static struct registered_appl *
ipcp_application_get(struct ipcp_entry *ipcp, const char *appl_name)
{
    struct registered_appl *app;

    RALOCK(ipcp);
    app = __ipcp_application_get(ipcp, appl_name);
    RAUNLOCK(ipcp);

    return app;
}

static void
appl_del(struct registered_appl *app)
{
    struct ipcp_entry *ipcp = app->ipcp;

    if (ipcp->ops.appl_register) {
        mutex_lock(&ipcp->lock);
        ipcp->ops.appl_register(ipcp, app->name, 0);
        mutex_unlock(&ipcp->lock);
    }

    ipcp_put(ipcp);

    /* From here on registered application cannot be referenced anymore, and so
     * that we don't need locks. */
    if (app->name)
        rl_free(app->name, RL_MT_REGAPP);
    rl_free(app, RL_MT_REGAPP);
}

static void
appl_removew_func(struct work_struct *w)
{
    struct rl_dm *dm = container_of(w, struct rl_dm, appl_removew);
    struct registered_appl *app, *tmp;
    struct list_head removeq;

    INIT_LIST_HEAD(&removeq);

    spin_lock_bh(&dm->appl_removeq_lock);
    list_for_each_entry_safe (app, tmp, &dm->appl_removeq, node) {
        list_del_init(&app->node);
        list_add_tail_safe(&app->node, &removeq);
    }
    spin_unlock_bh(&dm->appl_removeq_lock);

    list_for_each_entry_safe (app, tmp, &removeq, node) {
        appl_del(app);
    }
}

static void
ipcp_application_put(struct registered_appl *app)
{
    struct ipcp_entry *ipcp;

    if (!app) {
        return;
    }

    ipcp = app->ipcp;

    RALOCK(ipcp);

    app->refcnt--;
    if (app->refcnt) {
        RAUNLOCK(ipcp);
        return;
    }

    list_del_init(&app->node);

    RAUNLOCK(ipcp);

    if (ipcp->ops.appl_register) {
        /* Perform cleanup operation in process context, because we need
         * to take the per-ipcp mutex. */
        spin_lock_bh(&app->rc->dm->appl_removeq_lock);
        list_add_tail_safe(&app->node, &app->rc->dm->appl_removeq);
        spin_unlock_bh(&app->rc->dm->appl_removeq_lock);
        schedule_work(&app->rc->dm->appl_removew);
    } else {
        /* No mutex required, perform the removal in current context. */
        appl_del(app);
    }
}

static int
ipcp_application_add(struct ipcp_entry *ipcp, char *appl_name,
                     struct rl_ctrl *rc, uint32_t event_id, bool uipcp)
{
    struct registered_appl *app, *newapp;
    int ret = 0;

    RALOCK(ipcp);
    app = __ipcp_application_get(ipcp, appl_name);
    if (app) {
        struct rl_ctrl *old_rc = app->rc;

        RAUNLOCK(ipcp);
        ipcp_application_put(app);
        if (old_rc == rc) {
            /* This registration was already asked on this
             * control device. There is nothing to do,
             * inform the caller. */
            return 1;
        }

        /* Application was already registered on a different
         * control device. */
        return -EBUSY;
    }

    /* Create a new registered application. */
    newapp = rl_alloc(sizeof(*newapp), GFP_ATOMIC | __GFP_ZERO, RL_MT_REGAPP);
    if (!newapp) {
        return -ENOMEM;
    }

    newapp->name = rl_strdup(appl_name, GFP_ATOMIC, RL_MT_REGAPP);
    if (!newapp->name) {
        rl_free(newapp, RL_MT_REGAPP);
        return -ENOMEM;
    }
    newapp->rc       = rc;
    newapp->event_id = event_id;
    newapp->refcnt   = 1;
    newapp->ipcp     = ipcp;
    newapp->state    = uipcp ? APPL_REG_PENDING : APPL_REG_COMPLETE;
    list_add_tail(&newapp->node, &ipcp->registered_appls);

    RAUNLOCK(ipcp);

    PLOCK(ipcp->dm);
    ipcp->refcnt++;
    PV("REFCNT++ %u: %u\n", ipcp->id, ipcp->refcnt);
    PUNLOCK(ipcp->dm);

    if (ipcp->ops.appl_register) {
        mutex_lock(&ipcp->lock);
        ret = ipcp->ops.appl_register(ipcp, appl_name, 1);
        mutex_unlock(&ipcp->lock);
        if (ret) {
            ipcp_application_put(newapp);
        }
    }

    return ret;
}

static int
ipcp_application_del(struct ipcp_entry *ipcp, char *appl_name)
{
    struct registered_appl *app;

    app = ipcp_application_get(ipcp, appl_name);
    if (!app) {
        return -EINVAL;
    }

    ipcp_application_put(app); /* To match ipcp_application_get(). */
    ipcp_application_put(app); /* To remove the application. */

    return 0;
}

/* To be called under RALOCK, l is initialized by the caller. */
static void
application_steal(struct registered_appl *appl, struct list_head *l)
{
    if (appl->refcnt == 1) {
        /* Just move the reference. */
        list_del_init(&appl->node);
        list_add_tail_safe(&appl->node, l);
    } else {
        /* Do what ipcp_application_put() would do, but
         * without taking the RALOCK. */
        appl->refcnt--;
    }
}

static void
application_del_by_rc(struct rl_ctrl *rc)
{
    struct ipcp_entry *ipcp;
    int bucket;
    struct registered_appl *app;
    struct registered_appl *tmp;
    struct list_head remove_apps;

    INIT_LIST_HEAD(&remove_apps);

    PLOCK(rc->dm);

    /* For each IPC processes. */
    hash_for_each(rc->dm->ipcp_table, bucket, ipcp, node)
    {
        RALOCK(ipcp);
        /* For each application registered to this IPC process. */
        list_for_each_entry_safe (app, tmp, &ipcp->registered_appls, node) {
            if (app->rc == rc) {
                application_steal(app, &remove_apps);
            }
        }
        RAUNLOCK(ipcp);

        /* If the control device to be deleted is an uipcp attached to
         * this IPCP, detach it. */
        if (ipcp->uipcp == rc) {
            ipcp->uipcp = NULL;
            PI("IPC process %s detached from uipcp %p\n", ipcp->name, rc);
        }
    }

    PUNLOCK(rc->dm);

    /* Remove the selected applications without holding locks (we are in
     * process context here). */
    list_for_each_entry_safe (app, tmp, &remove_apps, node) {
        PD("Application %s will be automatically unregistered\n", app->name);

        /* Notify userspace IPCP if needed. */
        if (app->state == APPL_REG_COMPLETE && !app->ipcp->ops.appl_register &&
            app->ipcp->uipcp) {
            struct rl_kmsg_appl_register ntfy;

            ntfy.hdr.msg_type = RLITE_KER_APPL_REGISTER;
            ntfy.hdr.event_id = 0;
            ntfy.dif_name     = app->ipcp->dif->name; /* borrow the string */
            ntfy.reg          = false;
            ntfy.appl_name    = app->name;
            app->name         = NULL; /* move */
            rl_upqueue_append(app->ipcp->uipcp,
                              (const struct rl_msg_base *)&ntfy, true);
            app->name      = ntfy.appl_name;
            ntfy.appl_name = NULL; /* back */
            ntfy.dif_name  = NULL; /* return the borrowed string. */
            rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&ntfy));
        }

        /* Remove. */
        ipcp_application_put(app);
    }
}

/* To be called under FLOCK or FRLOCK. */
struct flow_entry *
flow_lookup(struct rl_dm *dm, rl_port_t port_id)
{
    struct flow_entry *entry;
    struct hlist_head *head;
    head = &dm->flow_table[hash_min(port_id, HASH_BITS(dm->flow_table))];
    hlist_for_each_entry (entry, head, node) {
        if (entry->local_port == port_id) {
            return entry;
        }
    }

    return NULL;
}
EXPORT_SYMBOL(flow_lookup);

struct flow_entry *
flow_get(struct rl_dm *dm, rl_port_t port_id)
{
    struct flow_entry *flow;

    FRLOCK(dm);
    flow = flow_lookup(dm, port_id);
    if (flow) {
        atomic_inc(&flow->refcnt);
        PV("FLOWREFCNT %u ++: %u\n", flow->local_port,
           atomic_read(&flow->refcnt));
    }
    FRUNLOCK(dm);

    return flow;
}
EXPORT_SYMBOL(flow_get);

struct flow_entry *
flow_nodm_get(rl_port_t port_id)
{
    struct rl_dm *dm = rl_dm_get();
    struct flow_entry *flow;

    flow = flow_get(dm, port_id);
    rl_dm_put(dm);

    return flow;
}

struct flow_entry *
flow_get_by_cep(struct rl_dm *dm, rlm_cepid_t cep_id)
{
    struct flow_entry *entry;
    struct hlist_head *head;

    FRLOCK(dm);

    head = &dm->flow_table_by_cep[hash_min(cep_id,
                                           HASH_BITS(dm->flow_table_by_cep))];
    hlist_for_each_entry (entry, head, node_cep) {
        if (entry->local_cep == cep_id) {
            atomic_inc(&entry->refcnt);
            PV("FLOWREFCNT %u ++: %u\n", entry->local_port,
               atomic_read(&entry->refcnt));
            FRUNLOCK(dm);
            return entry;
        }
    }

    FRUNLOCK(dm);

    return NULL;
}
EXPORT_SYMBOL(flow_get_by_cep);

void
flow_get_ref(struct flow_entry *flow)
{
    if (unlikely(!flow)) {
        return;
    }

    atomic_inc(&flow->refcnt);
    PV("FLOWREFCNT %u ++: %u\n", flow->local_port, atomic_read(&flow->refcnt));
}
EXPORT_SYMBOL(flow_get_ref);

/* To be called under FLOCK(). */
static void
flows_putq_add(struct flow_entry *flow, unsigned jdelta)
{
    struct rl_dm *dm = flow->txrx.ipcp->dm;

    atomic_inc(&flow->refcnt);
    PV("FLOWREFCNT %u ++: %u\n", flow->local_port, atomic_read(&flow->refcnt));

    if (flow->expires == ~0U) { /* don't insert twice */
        struct flow_entry *cur;

        flow->expires = jiffies + jdelta;
        /* Insert flow in the putq, keeping the putq sorted
         * by expiration time, in ascending order. */
        list_for_each_entry (cur, &dm->flows_putq, node_rm) {
            if (time_after(cur->expires, flow->expires)) {
                break;
            }
        }
        /* Insert 'flow' right before 'cur'. */
        list_add_tail_safe(&flow->node_rm, &cur->node_rm);
        /* Adjust timer expiration according to the new first entry. */
        cur = list_first_entry(&dm->flows_putq, struct flow_entry, node_rm);
        mod_timer(&dm->flows_putq_tmr, cur->expires);
    }
}

static void
flows_putq_del(struct flow_entry *flow)
{
    struct rl_dm *dm = flow->txrx.ipcp->dm;

    FLOCK(dm);
    flow->expires = ~0U;
    list_del_init(&flow->node_rm);
    FUNLOCK(dm);

    flow_put(flow);
}

static void
flows_putq_drain(
#ifdef RL_HAVE_TIMER_SETUP
    struct timer_list *tmr
#else
    unsigned long arg
#endif
)
{
#ifdef RL_HAVE_TIMER_SETUP
    struct rl_dm *dm = from_timer(dm, tmr, flows_putq_tmr);
#else  /* !RL_HAVE_TIMER_SETUP */
    struct rl_dm *dm = (struct rl_dm *)arg;
#endif /* !RL_HAVE_TIMER_SETUP */
    struct flow_entry *flow, *tmp;

    /* Call flow_put on all the expired flows, which are sorted in
     * ascending expriration ordedr. */
    FLOCK(dm);
    list_for_each_entry_safe (flow, tmp, &dm->flows_putq, node_rm) {
        if (!time_before(jiffies, flow->expires)) {
            list_del_init(&flow->node_rm);
            flow->expires = ~0U;
            __flow_put(flow, false); /* match flows_putq_add() */
            if (flow->flags & RL_FLOW_NEVER_BOUND) {
                PW("Removing flow %u since it was never bound\n",
                   flow->local_port);
            }
            __flow_put(flow, false);
        } else {
            /* We can stop here. */
            break;
        }
    }

    /* Reschedule if needed. */
    if (!list_empty(&dm->flows_putq)) {
        flow = list_first_entry(&dm->flows_putq, struct flow_entry, node_rm);
        mod_timer(&dm->flows_putq_tmr, flow->expires);
    }
    FUNLOCK(dm);
}

void
__flow_put(struct flow_entry *entry, bool lock)
{
    struct ipcp_entry *ipcp;
    struct rl_dm *dm;
    struct dtp *dtp;

    if (unlikely(!entry)) {
        return;
    }

    dtp  = &entry->dtp;
    ipcp = entry->txrx.ipcp;
    dm   = ipcp->dm;

    if (lock) {
        FLOCK(dm);
    }

    /* Although the flow reference counter does not generally need to be
     * accessed under FLOCK(), here it is necessary to avoid a race
     * condition. If we decremented the reference counter before taking
     * the FLOCK, a CPU could call flow_get (or similar) after the
     * reference counter drops to 0 and before the FLOCK is taken,
     * which would basically result into a double free. */
    if (!atomic_dec_and_test(&entry->refcnt)) {
        /* Flow is still being used by someone. */
        FUNLOCK(dm);
        return;
    }

    entry->flags |= RL_FLOW_DEALLOCATED;

    /* We postpone flow removal, at least for MPL, and also allow
     * cwq and rtxq to be drained. We check the flag
     * to make sure that this flow_entry() invocation is not due to a
     * postponed removal, so that we avoid postponing forever. */
    if (!(entry->flags & RL_FLOW_DEL_POSTPONED) &&
        (entry->flags & RL_FLOW_ALLOCATED) &&
        !(entry->flags & RL_FLOW_NEVER_BOUND)) {
        entry->flags |= RL_FLOW_DEL_POSTPONED;
        spin_lock_bh(&dtp->lock);
        if (dtp->cwq_len > 0 || !rb_list_empty(&dtp->rtxq)) {
            PD("Flow removal postponed, cwq contains "
               "%u PDUs and rtxq contains %u PDUs\n",
               dtp->cwq_len, dtp->rtxq_len);

            /* No one can write or read from this flow anymore, so there
             * is no reason to have the inactivity timer running. */
            del_timer(&dtp->snd_inact_tmr);
            del_timer(&dtp->rcv_inact_tmr);
        }
        spin_unlock_bh(&dtp->lock);

        /* Reference counter is zero here, we need to reset it
         * to 1 and let the delayed remove function do its job. */
        atomic_inc(&entry->refcnt);
        PV("FLOWREFCNT %u ++: %u\n", entry->local_port,
           atomic_read(&entry->refcnt));
        flows_putq_add(entry, msecs_to_jiffies(
                                  ipcp->flow_del_wait_ms) /* should be MPL */);
        if (lock) {
            FUNLOCK(dm);
        }
        return;
    }

    /* Detach from tables. */
    hash_del(&entry->node);
    bitmap_clear(dm->port_id_bitmap, entry->local_port, 1);
    if (ipcp->flags & RL_K_IPCP_USE_CEP_IDS) {
        hash_del(&entry->node_cep);
        bitmap_clear(dm->cep_id_bitmap, entry->local_cep, 1);
    }

    /* Enqueue into the remove list and schedule the work. */
    list_add_tail_safe(&entry->node_rm, &dm->flows_removeq);
    schedule_work(&dm->flows_removew);

    if (lock) {
        FUNLOCK(dm);
    }
}
EXPORT_SYMBOL(__flow_put);

/* Called in process context (workqueue worker). */
static void
flow_del(struct flow_entry *entry)
{
    struct rl_kmsg_flow_deallocated ntfy;
    struct ipcp_entry *upper_ipcp;
    struct ipcp_entry *ipcp;
    struct rl_buf *tmp;
    struct rl_buf *rb;
    struct dtp *dtp;

#if 0
    flow_config_dump(&entry->cfg);
#endif
    dtp        = &entry->dtp;
    ipcp       = entry->txrx.ipcp;
    upper_ipcp = entry->upper.ipcp;

    if (ipcp->ops.flow_deallocated) {
        /* Kernel-space IPCP, handle the flow deallocation here. */
        ipcp->ops.flow_deallocated(ipcp, entry);
    }

    if (verbosity >= RL_VERB_VERY) {
        dtp_dump(dtp);
    }
    dtp_fini(dtp);

    /* dtp_fini() may print txrx.rx_qsize, so we purge the queue after
     * calling that function. */
    rb_list_foreach_safe (rb, tmp, &entry->txrx.rx_q) {
        rb_list_del(rb);
        rl_buf_free(rb);
    }
    entry->txrx.rx_qsize = 0;

    if (upper_ipcp) {
        upper_ipcp->ops.pduft_flush_by_flow(upper_ipcp, entry);
    }
    if (entry->local_appl)
        rl_free(entry->local_appl, RL_MT_FLOW);
    if (entry->remote_appl)
        rl_free(entry->remote_appl, RL_MT_FLOW);

    /* Probe references before freeing. */
    if (!list_empty(&entry->node_rm)) {
        PE("Some list has a dangling reference to flow %u\n",
           entry->local_port);
    }
    {
        struct flow_entry *rflow;

        FLOCK(ipcp->dm);
        list_for_each_entry (rflow, &ipcp->dm->flows_removeq, node_rm) {
            if (rflow == entry) {
                PE("removeq has a dangling reference to flow %u\n",
                   entry->local_port);
            }
        }
        FUNLOCK(ipcp->dm);
    }

    if (!ipcp->ops.flow_deallocated) {
        /* Prepare a flow deallocation message for the uipcp. */
        memset(&ntfy, 0, sizeof(ntfy));
        ntfy.hdr.msg_type   = RLITE_KER_FLOW_DEALLOCATED;
        ntfy.hdr.event_id   = 0;
        ntfy.ipcp_id        = ipcp->id;
        ntfy.local_port_id  = entry->local_port;
        ntfy.remote_port_id = entry->remote_port;
        ntfy.remote_addr    = entry->remote_addr;
    }

    if (entry->upper.rc) {
        struct rl_ctrl *rc = entry->upper.rc;

        entry->upper.rc = NULL; /* just to stay safe */
        fput(rc->file);
    }

    rl_iodevs_probe_flow_references(entry);

    PD("flow entry %u removed\n", entry->local_port);
    rl_free(entry, RL_MT_FLOW);

    if (!ipcp->ops.flow_deallocated) {
        if (!ipcp->uipcp) {
            PW("No uipcp to notify\n");
        } else {
            /* Notify the uipcp about flow deallocation, if it makes sense. */
            if (ntfy.local_port_id != RL_PORT_ID_NONE &&
                ntfy.remote_port_id != RL_PORT_ID_NONE &&
                ntfy.remote_addr != RL_ADDR_NULL) {
                rl_upqueue_append(ipcp->uipcp,
                                  (const struct rl_msg_base *)&ntfy, true);
            }
            rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&ntfy));
        }
    }

    /* We are in process context here, so we can safely do the
     * removal. This is done for either the IPCP which supports
     * the flow (ipcp) and the IPCP which uses the flow (upper_ipcp). */

    if (upper_ipcp) {
        mutex_lock(&ipcp->lock);
        ipcp->shortcut_flows--;
        if (ipcp->shortcut_flows == 0) {
            ipcp->shortcut = NULL;
        }
        mutex_unlock(&ipcp->lock);

        ipcp_put(upper_ipcp);
    }
    ipcp_put(ipcp);
}

static void
flows_removew_func(struct work_struct *w)
{
    struct rl_dm *dm = container_of(w, struct rl_dm, flows_removew);
    struct flow_entry *flow, *tmp;
    struct list_head removeq;

    INIT_LIST_HEAD(&removeq);

    /* Move the entries to a temporary queue while holding the lock. */
    FLOCK(dm);
    list_for_each_entry_safe (flow, tmp, &dm->flows_removeq, node_rm) {
        list_del_init(&flow->node_rm);
        list_add_tail_safe(&flow->node_rm, &removeq);
    }
    FUNLOCK(dm);

    /* Destroy the entries without holding the lock (but still grab
     * the lock to modify flow->node_rm). */
    list_for_each_entry_safe (flow, tmp, &removeq, node_rm) {
        FLOCK(dm);
        list_del_init(&flow->node_rm);
        FUNLOCK(dm);
        flow_del(flow);
    }
}

static int
flow_add(struct ipcp_entry *ipcp, struct upper_ref upper, uint32_t event_id,
         const char *local_appl, const char *remote_appl,
         const struct rl_flow_config *flowcfg,
         const struct rina_flow_spec *flowspec, struct flow_entry **pentry,
         gfp_t gfp)
{
    struct rl_dm *dm = ipcp->dm;
    struct flow_entry *entry;
    int ret = 0;

    if (ipcp->flags & RL_K_IPCP_ZOMBIE) {
        /* Zombie ipcps don't accept new flows. */
        return -ENXIO;
    }

    *pentry = entry = rl_alloc(sizeof(*entry), gfp | __GFP_ZERO, RL_MT_FLOW);
    if (!entry) {
        return -ENOMEM;
    }

    FLOCK(dm);

    /* Try to alloc a port id and a cep id from the bitmaps, cep
     * ids being allocated only if needed. */
    entry->local_port = bitmap_find_next_zero_area(
        dm->port_id_bitmap, PORT_ID_BITMAP_SIZE, 0, 1, 0);
    if (ipcp->flags & RL_K_IPCP_USE_CEP_IDS) {
        entry->local_cep = bitmap_find_next_zero_area(
            dm->cep_id_bitmap, CEP_ID_BITMAP_SIZE, 0, 1, 0);
    } else {
        entry->local_cep = 0;
    }

    if (entry->local_port < PORT_ID_BITMAP_SIZE &&
        entry->local_cep < CEP_ID_BITMAP_SIZE) {
        bitmap_set(dm->port_id_bitmap, entry->local_port, 1);

        if (ipcp->flags & RL_K_IPCP_USE_CEP_IDS) {
            bitmap_set(dm->cep_id_bitmap, entry->local_cep, 1);
        }

        /* Build and insert a flow entry in the hash table. */
        entry->local_appl  = rl_strdup(local_appl, GFP_ATOMIC, RL_MT_FLOW);
        entry->remote_appl = rl_strdup(remote_appl, GFP_ATOMIC, RL_MT_FLOW);
        entry->remote_port = RL_PORT_ID_NONE; /* Not valid. */
        entry->remote_cep  = RL_PORT_ID_NONE; /* Not valid. */
        entry->remote_addr = RL_ADDR_NULL;    /* Not valid. */
        entry->qos_id      = 0;               /* default */
        entry->upper       = upper;
        if (upper.rc) {
            get_file(upper.rc->file);
        }
        entry->event_id = event_id;
        atomic_set(&entry->refcnt, 1); /* Cogito, ergo sum. */
        entry->flags = RL_FLOW_PENDING | RL_FLOW_NEVER_BOUND;
        memcpy(&entry->spec, flowspec, sizeof(*flowspec));
        txrx_init(&entry->txrx, ipcp);
        hash_add(dm->flow_table, &entry->node, entry->local_port);
        if (ipcp->flags & RL_K_IPCP_USE_CEP_IDS) {
            hash_add(dm->flow_table_by_cep, &entry->node_cep, entry->local_cep);
        }
        entry->uid = dm->uid_cnt++; /* generate an unique id */
        INIT_LIST_HEAD(&entry->node_rm);
        entry->expires = ~0U;
        dtp_init(&entry->dtp);

        atomic_inc(&entry->refcnt); /* on behalf of the caller */
        PV("FLOWREFCNT %u = %u\n", entry->local_port,
           atomic_read(&entry->refcnt));

        /* Start the unbound timer */
        flows_putq_add(entry, RL_UNBOUND_FLOW_TO);
        FUNLOCK(dm);

        PLOCK(dm);
        ipcp->refcnt++;
        PV("REFCNT++ %u: %u\n", ipcp->id, ipcp->refcnt);
        PUNLOCK(dm);

        if (flowcfg) {
            memcpy(&entry->cfg, flowcfg, sizeof(entry->cfg));
            if (ipcp->ops.flow_init) {
                /* Let the IPCP do some
                 * specific initialization. */
                ipcp->ops.flow_init(ipcp, entry);
            }
        }
    } else {
        FUNLOCK(dm);

        rl_free(entry, RL_MT_FLOW);
        *pentry = NULL;
        ret     = -ENOSPC;
    }

    return ret;
}

static void
flow_rc_probe_references(struct rl_ctrl *rc)
{
    struct flow_entry *flow;
    struct hlist_node *tmp;
    int bucket;

    FLOCK(rc->dm);
    hash_for_each_safe(rc->dm->flow_table, bucket, tmp, flow, node)
    {
        if (flow->upper.rc == rc) {
            PE("Flow %u has a dangling reference to rc %p\n", flow->local_port,
               rc);
        }
    }
    FUNLOCK(rc->dm);
}

void
flow_make_mortal(struct flow_entry *flow)
{
    bool never_bound = false;
    struct rl_dm *dm;

    if (!flow) {
        return;
    }

    dm = flow->txrx.ipcp->dm;
    FLOCK(dm);

    if (flow->flags & RL_FLOW_NEVER_BOUND) {
        never_bound = true;
        /* Here reference counter is (likely) 3. Reset it to 2, so that
         * proper flow destruction happens in rl_io_release(). If we
         * didn't do it, the flow would live forever with its refcount
         * set to 1. */
        flow->flags &= ~RL_FLOW_NEVER_BOUND;
        atomic_dec(&flow->refcnt);
        PV("FLOWREFCNT %u --: %u\n", flow->local_port,
           atomic_read(&flow->refcnt));
    }

    FUNLOCK(dm);

    if (never_bound) {
        flows_putq_del(flow);
    }
}

bool
rl_ipcp_has_flows(struct ipcp_entry *ipcp, bool report_all)
{
    struct flow_entry *flow;
    int bucket;
    bool has_flows = false;

    FRLOCK(ipcp->dm);
    hash_for_each(ipcp->dm->flow_table, bucket, flow, node)
    {
        if (flow->txrx.ipcp == ipcp) {
            has_flows = true;
            if (report_all) {
                PE("Flow %u has a horizontal dangling reference to ipcp %u\n",
                   flow->local_port, ipcp->id);
            }
        }
        if (flow->upper.ipcp == ipcp) {
            has_flows = true;
            if (report_all) {
                PE("Flow %u has a vertical dangling reference to ipcp %u\n",
                   flow->local_port, ipcp->id);
            }
        }
        if (!report_all && has_flows) {
            break;
        }
    }
    FRUNLOCK(ipcp->dm);

    return has_flows;
}
EXPORT_SYMBOL(rl_ipcp_has_flows);

static void
ipcp_probe_references(struct ipcp_entry *ipcp)
{
    rl_ipcp_has_flows(ipcp, /*report_all=*/true);

    {
        struct registered_appl *appl;

        RALOCK(ipcp);
        list_for_each_entry (appl, &ipcp->registered_appls, node) {
            PE("Registered application %s has a dangling reference to "
               "ipcp %d\n",
               appl->name, ipcp->id);
        }
        RAUNLOCK(ipcp);
    }

    rl_iodevs_probe_ipcp_references(ipcp);
}

int
__ipcp_put(struct ipcp_entry *entry)
{
    struct rl_dm *dm;

    if (!entry) {
        return 0;
    }

    dm = entry->dm;

    PLOCK(dm);

    entry->refcnt--;
    if (entry->refcnt) {
        PUNLOCK(dm);
        return 0;
    }

    hash_del(&entry->node);
    bitmap_clear(dm->ipcp_id_bitmap, entry->id, 1);

    PUNLOCK(dm);

    /* Inoke the destructor method, if the constructor
     * was called. */
    if (entry->priv) {
        BUG_ON(entry->ops.destroy == NULL);
        /* No locking (entry->lock) is necessary here, because the current
         * thread has already removed the last reference to this IPCP,
         * and so it cannot be referenced anymore. This also means no
         * concurrent access is possible. */
        entry->ops.destroy(entry);
    }

    /* If the module was refcounted for this IPC process instance,
     * remove the reference. Note that this operation **must** happen
     * after the destructor invokation, in order to avoid a race
     * conditions that may lead to kernel page faults. */
    if (entry->owner) {
        module_put(entry->owner);
    }

    if (entry->name)
        rl_free(entry->name, RL_MT_UTILS /* moved */);
    dif_put(entry->dm, entry->dif);

    ipcp_probe_references(entry);

    {
        /* Upqueue an RLITE_KER_IPCP_UPDATE message to each
         * opened ctrl device. */
        struct rl_kmsg_ipcp_update upd;
        struct rl_ctrl *rcur;

        memset(&upd, 0, sizeof(upd));
        upd.hdr.msg_type = RLITE_KER_IPCP_UPDATE;
        upd.update_type  = RL_IPCP_UPDATE_DEL;
        upd.ipcp_id      = entry->id;
        /* All the other fields are zeroed, since they are
         * not useful to userspace. */

        mutex_lock(&dm->general_lock);
        list_for_each_entry (rcur, &dm->ctrl_devs, node) {
            if (rcur->flags & RL_F_IPCPS) {
                rl_upqueue_append(rcur, RLITE_MB(&upd), false);
            }
        }
        mutex_unlock(&dm->general_lock);

        rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&upd));
    }

    free_percpu(entry->stats);
    rl_dm_put(entry->dm);
    rl_free(entry, RL_MT_IPCP);

    return 0;
}

static int
ipcp_pduft_flush(struct ipcp_entry *ipcp)
{
    int ret = 0;

    if (!ipcp) {
        return -EINVAL;
    }

    if (ipcp->ops.pduft_flush) {
        mutex_lock(&ipcp->lock);
        ret = ipcp->ops.pduft_flush(ipcp);
        mutex_unlock(&ipcp->lock);
    }

    return ret;
}

static int
ipcp_del(struct rl_ctrl *rc, rl_ipcp_id_t ipcp_id)
{
    struct ipcp_entry *entry;
    int ret = 0;

    if (ipcp_id >= IPCP_ID_BITMAP_SIZE) {
        /* No IPC process found. */
        return -ENXIO;
    }

    /* Lookup and remove the IPC process entry in the hash table corresponding
     * to the given ipcp_id. */
    entry = ipcp_get(rc->dm, ipcp_id);
    if (!entry) {
        return -ENXIO;
    }

    ipcp_put(entry); /* To match the ipcp_get(). */

    if (entry->flags & RL_K_IPCP_ZOMBIE) {
        /* If this happens it means that someone already asked for this IPCP to
         * be destroy IPCP, so this cannot be allowed. The IPCP is still
         * referenced in the system, and will be destroyed as soon as the last
         * reference drops. */
        return -ENXIO;
    }
    entry->flags |= RL_K_IPCP_ZOMBIE;
    barrier();

    /* Flush the PDUFT. Nobody will do it again afterwards, so we need to make
     * sure that nobody will add entries to the pduft from now on (see
     * rl_ipcp_pduft_mod()). */
    ipcp_pduft_flush(entry);

    /* Unregister all the applications associated to this IPCP. */
    {
        struct list_head remove_apps;
        struct registered_appl *app;
        struct registered_appl *tmp;

        INIT_LIST_HEAD(&remove_apps);
        RALOCK(entry);
        list_for_each_entry_safe (app, tmp, &entry->registered_appls, node) {
            application_steal(app, &remove_apps);
        }
        RAUNLOCK(entry);
        list_for_each_entry_safe (app, tmp, &remove_apps, node) {
            PD("Application %s will be automatically unregistered\n",
               app->name);
            ipcp_application_put(app);
        }
    }

    /* Shutdown all the allocated flows bound by user-space applications. */
    rl_iodevs_shutdown_by_ipcp(entry);

    ret = ipcp_put(entry); /* Let the recount drop to 0. */

    return ret;
}

static int
ipcp_update_fill(struct ipcp_entry *ipcp, struct rl_kmsg_ipcp_update *upd,
                 int update_type)
{
    const char *dif_name = NULL;
    int ret              = 0;

    memset(upd, 0, sizeof(*upd));

    upd->hdr.msg_type = RLITE_KER_IPCP_UPDATE;
    upd->update_type  = update_type;
    upd->ipcp_id      = ipcp->id;
    upd->ipcp_addr    = ipcp->addr;
    upd->txhdroom     = ipcp->txhdroom;
    upd->rxhdroom     = ipcp->rxhdroom;
    upd->tailroom     = ipcp->tailroom;
    upd->max_sdu_size = ipcp->max_sdu_size;
    memcpy(&upd->pcisizes, &ipcp->pcisizes, sizeof(upd->pcisizes));
    if (ipcp->name) {
        upd->ipcp_name = rl_strdup(ipcp->name, GFP_ATOMIC, RL_MT_UTILS);
        if (!upd->ipcp_name) {
            ret = -ENOMEM;
        }
    }

    if (ipcp->dif) {
        dif_name      = ipcp->dif->name;
        upd->dif_type = rl_strdup(ipcp->dif->ty, GFP_ATOMIC, RL_MT_UTILS);
        if (!upd->dif_type) {
            ret = -ENOMEM;
        }
    }
    if (dif_name) {
        upd->dif_name = rl_strdup(dif_name, GFP_ATOMIC, RL_MT_UTILS);
        if (!upd->dif_name) {
            ret = -ENOMEM;
        }
    }

    return ret;
}

static int
ipcp_update_all(struct rl_ctrl *rc, rl_ipcp_id_t ipcp_id, int update_type)
{
    struct ipcp_entry *ipcp = ipcp_get(rc->dm, ipcp_id);
    struct rl_kmsg_ipcp_update upd;
    struct rl_ctrl *rcur;
    int ret = 0;

    if (!ipcp) {
        PE("IPCP %u unexpectedly disappeared\n", ipcp_id);
        return -ENXIO;
    }

    if (ipcp_update_fill(ipcp, &upd, update_type)) {
        RPV(1, "Out of memory\n");
        ret = -ENOMEM;
        goto out;
    }

    mutex_lock(&rc->dm->general_lock);
    list_for_each_entry (rcur, &rc->dm->ctrl_devs, node) {
        if (rcur->flags & RL_F_IPCPS) {
            rl_upqueue_append(rcur, RLITE_MB(&upd), false);
        }
    }
    mutex_unlock(&rc->dm->general_lock);

out:
    rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&upd));
    ipcp_put(ipcp);

    return ret;
}

static int
rl_ipcp_create(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_create *req = (struct rl_kmsg_ipcp_create *)bmsg;
    struct rl_kmsg_ipcp_create_resp resp;
    rl_ipcp_id_t ipcp_id;
    int ret;

    ret = ipcp_add(rc->dm, req, &ipcp_id);
    if (ret) {
        return ret;
    }

    memset(&resp, 0, sizeof(resp));
    resp.hdr.msg_type = RLITE_KER_IPCP_CREATE_RESP;
    resp.hdr.event_id = req->hdr.event_id;
    resp.ipcp_id      = ipcp_id;

    /* Enqueue the response into the upqueue. */
    ret = rl_upqueue_append(rc, RLITE_MB(&resp), true);
    rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&resp));
    if (ret) {
        goto err;
    }

    PI("IPC process %u created\n", ipcp_id);

    /* Upqueue an RLITE_KER_IPCP_UPDATE message to each
     * opened ctrl device. */
    ipcp_update_all(rc, ipcp_id, RL_IPCP_UPDATE_ADD);

    return 0;

err:
    ipcp_del(rc, ipcp_id);

    return ret;
}

static int
rl_ipcp_destroy(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_destroy *req = (struct rl_kmsg_ipcp_destroy *)bmsg;
    struct rl_kmsg_ipcp_update upd;
    struct rl_ctrl *rcur;
    int ret;

    /* Release the IPC process with the given ID. */
    ret = ipcp_del(rc, req->ipcp_id);
    if (ret) {
        PE("Failed to remove IPC process %u\n", req->ipcp_id);
        return ret;
    }

    PI("IPC process %u is going to be removed\n", req->ipcp_id);

    /* Upqueue an RLITE_KER_IPCP_UPDATE message to each
     * opened ctrl device. */
    memset(&upd, 0, sizeof(upd));
    upd.hdr.msg_type = RLITE_KER_IPCP_UPDATE;
    upd.update_type  = RL_IPCP_UPDATE_UIPCP_DEL;
    upd.ipcp_id      = req->ipcp_id;
    /* All the other fields are zeroed, since they are
     * not useful to userspace. */

    mutex_lock(&rc->dm->general_lock);
    list_for_each_entry (rcur, &rc->dm->ctrl_devs, node) {
        if (rcur->flags & RL_F_IPCPS) {
            rl_upqueue_append(rcur, RLITE_MB(&upd), false);
        }
    }
    mutex_unlock(&rc->dm->general_lock);

    rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&upd));

    return ret;
}

struct flows_fetch_q_entry {
    struct rl_kmsg_flow_fetch_resp resp;
    struct list_head node;
};

static int
rl_flow_fetch(struct rl_ctrl *rc, struct rl_msg_base *b_req)
{
    struct rl_kmsg_flow_fetch *req = (struct rl_kmsg_flow_fetch *)b_req;
    struct flows_fetch_q_entry *fqe;
    struct flow_entry *entry;
    int bucket;
    int ret = -ENOMEM;

    if (req->ipcp_id != 0xffff) {
        /* Validate req->ipcp_id. */
        struct ipcp_entry *ipcp = ipcp_get(rc->dm, req->ipcp_id);

        if (!ipcp) {
            return -EINVAL;
        }
        ipcp_put(ipcp);
    }

    FLOCK(rc->dm);

    if (list_empty(&rc->flows_fetch_q)) {
        hash_for_each(rc->dm->flow_table, bucket, entry, node)
        {
            if (req->ipcp_id != 0xffff &&
                entry->txrx.ipcp->id != req->ipcp_id) {
                /* Filter out this flow as user asked only for flows
                 * belonging to a specific IPCP. */
                continue;
            }

            fqe = rl_alloc(sizeof(*fqe), GFP_ATOMIC, RL_MT_FFETCH);
            if (!fqe) {
                RPV(1, "Out of memory\n");
                break;
            }

            memset(fqe, 0, sizeof(*fqe));
            list_add_tail(&fqe->node, &rc->flows_fetch_q);

            fqe->resp.hdr.msg_type = RLITE_KER_FLOW_FETCH_RESP;
            fqe->resp.end          = 0;
            fqe->resp.ipcp_id      = entry->txrx.ipcp->id;
            fqe->resp.local_port   = entry->local_port;
            fqe->resp.remote_port  = entry->remote_port;
            fqe->resp.local_addr   = entry->txrx.ipcp->addr;
            fqe->resp.remote_addr  = entry->remote_addr;
            fqe->resp.spec         = entry->spec;
            fqe->resp.flow_control =
                !!(entry->cfg.dtcp.flags & DTCP_CFG_FLOW_CTRL);
        }

        fqe = rl_alloc(sizeof(*fqe), GFP_ATOMIC, RL_MT_FFETCH);
        if (!fqe) {
            RPV(1, "Out of memory\n");
        } else {
            memset(fqe, 0, sizeof(*fqe));
            list_add_tail(&fqe->node, &rc->flows_fetch_q);
            fqe->resp.hdr.msg_type = RLITE_KER_FLOW_FETCH_RESP;
            fqe->resp.end          = 1;
        }
    }

    if (!list_empty(&rc->flows_fetch_q)) {
        fqe = list_first_entry(&rc->flows_fetch_q, struct flows_fetch_q_entry,
                               node);
        list_del_init(&fqe->node);
        fqe->resp.hdr.event_id = req->hdr.event_id;
        ret = rl_upqueue_append(rc, RLITE_MB(&fqe->resp), false);
        rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&fqe->resp));
        rl_free(fqe, RL_MT_FFETCH);
    }

    FUNLOCK(rc->dm);

    return ret;
}

struct regs_fetch_q_entry {
    struct rl_kmsg_reg_fetch_resp resp;
    struct list_head node;
};

static int
rl_reg_fetch(struct rl_ctrl *rc, struct rl_msg_base *b_req)
{
    struct rl_kmsg_reg_fetch *req = (struct rl_kmsg_reg_fetch *)b_req;
    struct regs_fetch_q_entry *fqe;
    struct registered_appl *appl;
    struct ipcp_entry *ipcp;
    int bucket;
    int ret = -ENOMEM;

    if (req->ipcp_id != 0xffff) {
        /* Validate req->ipcp_id. */
        ipcp = ipcp_get(rc->dm, req->ipcp_id);
        if (!ipcp) {
            return -EINVAL;
        }
        ipcp_put(ipcp);
    }

    PLOCK(rc->dm);

    if (list_empty(&rc->regs_fetch_q)) {
        hash_for_each(rc->dm->ipcp_table, bucket, ipcp, node)
        {
            if (req->ipcp_id != 0xffff && ipcp->id != req->ipcp_id) {
                /* Filter out this ipcp as user asked only for application
                 * names registered within a specific IPCP. */
                continue;
            }

            RALOCK(ipcp);
            list_for_each_entry (appl, &ipcp->registered_appls, node) {
                fqe = rl_alloc(sizeof(*fqe), GFP_ATOMIC, RL_MT_FFETCH);
                if (!fqe) {
                    RPV(1, "Out of memory\n");
                    break;
                }

                memset(fqe, 0, sizeof(*fqe));
                list_add_tail(&fqe->node, &rc->regs_fetch_q);

                fqe->resp.hdr.msg_type = RLITE_KER_REG_FETCH_RESP;
                fqe->resp.end          = 0;
                fqe->resp.ipcp_id      = ipcp->id;
                fqe->resp.pending      = appl->state != APPL_REG_COMPLETE;
                fqe->resp.appl_name =
                    rl_strdup(appl->name, GFP_ATOMIC, RL_MT_UTILS);
            }
            RAUNLOCK(ipcp);
        }

        fqe = rl_alloc(sizeof(*fqe), GFP_ATOMIC, RL_MT_FFETCH);
        if (!fqe) {
            RPV(1, "Out of memory\n");
        } else {
            memset(fqe, 0, sizeof(*fqe));
            list_add_tail(&fqe->node, &rc->regs_fetch_q);
            fqe->resp.hdr.msg_type = RLITE_KER_REG_FETCH_RESP;
            fqe->resp.end          = 1;
        }
    }

    if (!list_empty(&rc->regs_fetch_q)) {
        fqe = list_first_entry(&rc->regs_fetch_q, struct regs_fetch_q_entry,
                               node);
        list_del_init(&fqe->node);
        fqe->resp.hdr.event_id = req->hdr.event_id;
        ret = rl_upqueue_append(rc, RLITE_MB(&fqe->resp), false);
        rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&fqe->resp));
        rl_free(fqe, RL_MT_FFETCH);
    }

    PUNLOCK(rc->dm);

    return ret;
}

int
rl_configstr_to_u16(const char *src, uint16_t *dst, int *changed)
{
    uint16_t v;
    int ret = kstrtou16(src, 10, &v);

    if (ret == 0) {
        if (changed) {
            *changed = *dst != v;
        }
        *dst = v;
    }
    return ret;
}
EXPORT_SYMBOL(rl_configstr_to_u16);

int
rl_configstr_to_u32(const char *src, uint32_t *dst, int *changed)
{
    uint32_t v;
    int ret = kstrtou32(src, 10, &v);

    if (ret == 0) {
        if (changed) {
            *changed = *dst != v;
        }
        *dst = v;
    }
    return ret;
}
EXPORT_SYMBOL(rl_configstr_to_u32);

int
rl_configstr_to_u64(const char *src, uint64_t *dst, int *changed)
{
    uint64_t v;
    int ret = kstrtou64(src, 10, &v);

    if (ret == 0) {
        if (changed) {
            *changed = *dst != v;
        }
        *dst = v;
    }
    return ret;
}
EXPORT_SYMBOL(rl_configstr_to_u64);

static int
rl_ipcp_config(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_config *req = (struct rl_kmsg_ipcp_config *)bmsg;
    struct ipcp_entry *entry;
    int notify = 0;
    int ret;

    if (!req->name || !req->value) {
        return -EINVAL;
    }

    /* Find the IPC process entry corresponding to req->ipcp_id and
     * fill the DIF name field. */
    entry = ipcp_get(rc->dm, req->ipcp_id);
    if (!entry) {
        return -EINVAL;
    }

    ret = -ENOSYS; /* parameter not implemented */
    /* Check if the IPCP knows how to change this parameter. */
    mutex_lock(&entry->lock);
    if (entry->ops.config) {
        ret = entry->ops.config(entry, req->name, req->value, &notify);
    }
    mutex_unlock(&entry->lock);

    if (ret == -ENOSYS) {
        /* This operation was not managed by ops.config, let's see if
         *  we can manage it here. */
        if (strcmp(req->name, "txhdroom") == 0) {
            ret = rl_configstr_to_u16(req->value, &entry->txhdroom, NULL);
        } else if (strcmp(req->name, "rxhdroom") == 0) {
            ret = rl_configstr_to_u16(req->value, &entry->rxhdroom, NULL);
        } else if (strcmp(req->name, "mss") == 0) {
            ret =
                rl_configstr_to_u32(req->value, &entry->max_sdu_size, &notify);
        } else if (strcmp(req->name, "flow-del-wait-ms") == 0) {
            ret =
                rl_configstr_to_u32(req->value, &entry->flow_del_wait_ms, NULL);
        } else {
            ret = -EINVAL; /* unknown request */
        }
    }

    if (ret == 0) {
        PD("Configured IPC process %s: %s <= %s\n", entry->name, req->name,
           req->value);

        if (notify) {
            /* Upqueue an RLITE_KER_IPCP_UPDATE message to each
             * opened ctrl device. */
            ipcp_update_all(rc, req->ipcp_id, RL_IPCP_UPDATE_UPD);
        }
    }

    ipcp_put(entry);

    return ret;
}

static int
rl_ipcp_config_get(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_config_get_req *req =
        (struct rl_kmsg_ipcp_config_get_req *)bmsg;
    struct ipcp_entry *entry;
    char valbuf[64];
    int ret;

    if (!req->param_name) {
        return -EINVAL;
    }

    /* Find the IPC process entry corresponding to req->ipcp_id and
     * fill the DIF name field. */
    entry = ipcp_get(rc->dm, req->ipcp_id);
    if (!entry) {
        return -EINVAL;
    }

    ret = -ENOSYS; /* parameter not implemented */
    /* Check if the IPCP knows how to change this parameter. */
    mutex_lock(&entry->lock);
    if (entry->ops.config_get) {
        ret = entry->ops.config_get(entry, req->param_name, valbuf,
                                    sizeof(valbuf));
    }
    mutex_unlock(&entry->lock);

    if (ret == -ENOSYS) {
        /* This operation was not managed by ops.config, let's see if
         *  we can manage it here. */
        ret = 0;
        if (strcmp(req->param_name, "txhdroom") == 0) {
            snprintf(valbuf, sizeof(valbuf), "%u", entry->txhdroom);
        } else if (strcmp(req->param_name, "rxhdroom") == 0) {
            snprintf(valbuf, sizeof(valbuf), "%u", entry->rxhdroom);
        } else if (strcmp(req->param_name, "mss") == 0) {
            snprintf(valbuf, sizeof(valbuf), "%u", entry->max_sdu_size);
        } else if (strcmp(req->param_name, "flow-del-wait-ms") == 0) {
            snprintf(valbuf, sizeof(valbuf), "%u", entry->flow_del_wait_ms);
        } else {
            ret = -EINVAL; /* unknown request */
        }
    }

    if (ret == 0) {
        struct rl_kmsg_ipcp_config_get_resp resp;

        memset(&resp, 0, sizeof(resp));
        resp.hdr.msg_type = RLITE_KER_IPCP_CONFIG_GET_RESP;
        resp.hdr.event_id = req->hdr.event_id;
        resp.param_value  = rl_strdup(valbuf, GFP_KERNEL, RL_MT_UTILS);
        ret = rl_upqueue_append(rc, (const struct rl_msg_base *)&resp, true);
        rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&resp));
    }

    ipcp_put(entry);

    return ret;
}

static int
rl_ipcp_sched_config(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_msg_ipcp *imsg = (struct rl_msg_ipcp *)bmsg;
    struct ipcp_entry *ipcp;
    int ret;

    /* Find the IPC process entry corresponding to req->ipcp_id and
     * fill the DIF name field. */
    ipcp = ipcp_get(rc->dm, imsg->ipcp_id);
    if (!ipcp) {
        return -EINVAL;
    }

    ret = -ENOSYS; /* not implemented */
    /* Check if the IPCP knows how to change this parameter. */
    mutex_lock(&ipcp->lock);
    if (ipcp->ops.sched_config) {
        ret = ipcp->ops.sched_config(ipcp, bmsg);
    }
    mutex_unlock(&ipcp->lock);

    ipcp_put(ipcp);

    return ret;
}

static int
rl_ipcp_pduft_mod(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_pduft_mod *req = (struct rl_kmsg_ipcp_pduft_mod *)bmsg;
    struct ipcp_entry *ipcp;
    struct flow_entry *flow;
    int ret = -EINVAL; /* Report failure by default. */

    flow = flow_get(rc->dm, req->local_port);
    ipcp = ipcp_get(rc->dm, req->ipcp_id);

    if (ipcp && flow && flow->upper.ipcp == ipcp && ipcp->ops.pduft_set &&
        !(ipcp->flags & RL_K_IPCP_ZOMBIE)) {
        mutex_lock(&ipcp->lock);
        /* We allow this operation only if the requesting IPCP (req->ipcp_id)
         * is really using the requested flow, i.e. 'flow->upper.ipcp == ipcp'.
         * In this situation we are sure that 'ipcp' will not be deleted before
         * 'flow' is deleted, so we can rely on the internal pduft lock.
         * Moreover, we don't allow any operation on zombies to avoid a
         * resources leak, because the pduft of a zombie IPCP is not flushed
         * anymore (so references to flows in the pduft will stay there forever,
         * and so the IPCPs bound to them). */
        if (req->hdr.msg_type == RLITE_KER_IPCP_PDUFT_SET) {
            ret = ipcp->ops.pduft_set(ipcp, &req->match, flow);
        } else { /* RLITE_KER_IPCP_PDUFT_DEL */
            ret = ipcp->ops.pduft_del_addr(ipcp, &req->match);
        }
        mutex_unlock(&ipcp->lock);
    }

    if (ret == 0) {
        PV("Set IPC process %s PDUFT entry: %llu --> %u\n", ipcp->name,
           (unsigned long long)req->match.dst_addr, req->local_port);
    }

    flow_put(flow);
    ipcp_put(ipcp);

    return ret;
}

static int
rl_ipcp_pduft_flush(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_pduft_flush *req =
        (struct rl_kmsg_ipcp_pduft_flush *)bmsg;
    struct ipcp_entry *ipcp;
    int ret;

    ipcp = ipcp_get(rc->dm, req->ipcp_id);
    ret  = ipcp_pduft_flush(ipcp);
    if (ret == 0) {
        PV("Flushed PDUFT for IPC process %s\n", ipcp->name);
    }
    ipcp_put(ipcp);

    return ret;
}

static int
rl_ipcp_qos_supported(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_qos_supported *req =
        (struct rl_kmsg_ipcp_qos_supported *)bmsg;
    struct ipcp_entry *ipcp;
    int ret = -EINVAL;

    ipcp = ipcp_get(rc->dm, req->ipcp_id);
    if (ipcp) {
        if (ipcp->ops.qos_supported) {
            /* IPCP is able to validate QoS. */
            ret = ipcp->ops.qos_supported(ipcp, &req->flowspec);
        } else {
            /* IPCP only supports best effort. */
            ret = rina_flow_spec_best_effort(&req->flowspec) ? 0 : -ENOSYS;
        }
    }
    ipcp_put(ipcp);

    return ret;
}

static int
rl_ipcp_uipcp_set(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_uipcp_set *req = (struct rl_kmsg_ipcp_uipcp_set *)bmsg;
    struct ipcp_entry *entry;
    int ret = -EINVAL; /* Report failure by default. */

    /* Find the IPC process entry corresponding to req->ipcp_id and
     * fill the entry->uipcp field. */
    entry = ipcp_get(rc->dm, req->ipcp_id);
    if (entry) {
        mutex_lock(&entry->lock);
        if (entry->uipcp) {
            ret = -EBUSY;
        } else {
            entry->uipcp = rc;
            ret          = 0;
            wake_up_interruptible(&entry->uipcp_wqh);
        }
        mutex_unlock(&entry->lock);
    }

    if (ret == 0) {
        PI("IPC process %s attached to uipcp %p\n", entry->name, rc);
    }
    ipcp_put(entry);

    return ret;
}

static int
rl_ipcp_uipcp_wait(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_uipcp_wait *req =
        (struct rl_kmsg_ipcp_uipcp_wait *)bmsg;
    DECLARE_WAITQUEUE(wait, current);
    struct ipcp_entry *entry;
    int ret = 0;

    /* Find the IPC process entry corresponding to req->ipcp_id and wait
     * for the entry->uipcp field to be filled. */
    entry = ipcp_get(rc->dm, req->ipcp_id);
    if (!entry) {
        return -EINVAL;
    }

    add_wait_queue(&entry->uipcp_wqh, &wait);

    for (;;) {
        struct rl_ctrl *uipcp;

        set_current_state(TASK_INTERRUPTIBLE);

        mutex_lock(&entry->lock);
        uipcp = entry->uipcp;
        mutex_unlock(&entry->lock);

        if (uipcp) {
            break;
        }

        if (signal_pending(current)) {
            ret = -ERESTARTSYS;
            break;
        }

        schedule();
    }

    __set_current_state(TASK_RUNNING);
    remove_wait_queue(&entry->uipcp_wqh, &wait);

    ipcp_put(entry);

    return ret;
}

static int
rl_ipcp_get_stats(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_ipcp_stats_req *req = (struct rl_kmsg_ipcp_stats_req *)bmsg;
    struct rl_kmsg_ipcp_stats_resp resp;
    struct ipcp_entry *ipcp;
    int ret = -EINVAL; /* Report failure by default. */

    ipcp = ipcp_get(rc->dm, req->ipcp_id);
    if (ipcp) {
        int cpu;

        memset(&resp, 0, sizeof(resp));
        resp.hdr.msg_type = RLITE_KER_IPCP_STATS_RESP;
        resp.hdr.event_id = req->hdr.event_id;
        /* Collect stats from all the CPUs. */
        for_each_possible_cpu(cpu)
        {
            struct rl_ipcp_stats *cpustats = per_cpu_ptr(ipcp->stats, cpu);
            unsigned num   = sizeof(resp.stats) / sizeof(resp.stats.tx_pkt);
            uint64_t *ssrc = (uint64_t *)cpustats;
            uint64_t *sdst = (uint64_t *)&resp.stats;
            unsigned i;

            for (i = 0; i < num; i++, sdst++, ssrc++) {
                *sdst += *ssrc;
            }
        }
        ret = rl_upqueue_append(rc, (const struct rl_msg_base *)&resp, false);
        rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&resp));
    }
    ipcp_put(ipcp);

    return ret;
}

static int
rl_uipcp_fa_req_arrived(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_uipcp_fa_req_arrived *req =
        (struct rl_kmsg_uipcp_fa_req_arrived *)bmsg;
    struct ipcp_entry *ipcp;
    int ret = -EINVAL; /* Report failure by default. */

    ipcp = ipcp_get(rc->dm, req->ipcp_id);
    if (ipcp) {
        ret = rl_fa_req_arrived(ipcp, req->kevent_id, req->remote_port,
                                req->remote_cep, req->qos_id, req->remote_addr,
                                req->local_appl, req->remote_appl,
                                &req->flowcfg, &req->flowspec, true);
    }

    ipcp_put(ipcp);

    return ret;
}

static int
rl_uipcp_fa_resp_arrived(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_uipcp_fa_resp_arrived *req =
        (struct rl_kmsg_uipcp_fa_resp_arrived *)bmsg;
    struct ipcp_entry *ipcp;
    int ret = -EINVAL; /* Report failure by default. */

    ipcp = ipcp_get(rc->dm, req->ipcp_id);
    if (ipcp) {
        ret = rl_fa_resp_arrived(ipcp, req->local_port, req->remote_port,
                                 req->remote_cep, req->qos_id, req->remote_addr,
                                 req->response, &req->flowcfg, true);
    }
    ipcp_put(ipcp);

    return ret;
}

/* May be called under FLOCK. */
void
rl_flow_shutdown(struct flow_entry *flow)
{
    int deallocated = 0;

    spin_lock_bh(&flow->txrx.rx_lock);
    if (flow->flags & RL_FLOW_ALLOCATED) {
        /* Set the EOF condition on the flow. */
        flow->txrx.flags |= RL_TXRX_EOF;
        flow->flags |= RL_FLOW_DEALLOCATED;
        deallocated = 1;
    }
    spin_unlock_bh(&flow->txrx.rx_lock);

    if (deallocated) {
        /* Wake up readers and pollers, so that they can read the EOF. */
        wake_up_interruptible_poll(&flow->txrx.rx_wqh,
                                   POLLIN | POLLRDNORM | POLLRDBAND);
    }
}
EXPORT_SYMBOL(rl_flow_shutdown);

static int
rl_flow_dealloc(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_flow_dealloc *req = (struct rl_kmsg_flow_dealloc *)bmsg;
    struct flow_entry *flow;
    int ret = -ENXIO;

    /* We look up the flow by port id (as usual), but we also check that
     * the uid matches, to avoid shutting down a flow that reused the
     * same port-id. The problem is that a flow can be shut down both by
     * the application and the uipcp, and they typically both try to do
     * that, and one of the two fails (but it is not an error).
     * As a consequence, a race condition is possible, so that before
     * the uipcp manages to shutdown the flow, the flow has already been
     * deallocated and another flow has been allocated with the same
     * port-id. If we didn't check, the new flow would be incorrectly
     * shut down. */
    flow = flow_get(rc->dm, req->port_id);
    if (flow && flow->uid == req->uid) {
        rl_flow_shutdown(flow);
        ret = 0;
    }
    flow_put(flow);

    return ret;
}

static int
rl_flow_get_stats(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_flow_stats_req *req = (struct rl_kmsg_flow_stats_req *)bmsg;
    struct rl_kmsg_flow_stats_resp resp;
    struct flow_entry *flow;
    struct dtp *dtp;
    int ret = 0;

    flow = flow_get(rc->dm, req->port_id);
    if (!flow) {
        return -EINVAL;
    }
    dtp = &flow->dtp;

    memset(&resp, 0, sizeof(resp));
    resp.hdr.msg_type = RLITE_KER_FLOW_STATS_RESP;
    resp.hdr.event_id = req->hdr.event_id;

    spin_lock_bh(&flow->txrx.rx_lock);
    spin_lock_bh(&dtp->lock);

    /* Copy in rl_io device stats. */
    memcpy(&resp.stats, &flow->stats, sizeof(resp.stats));

    /* Copy in DTP state. */
    resp.dtp.snd_lwe                = dtp->snd_lwe;
    resp.dtp.snd_rwe                = dtp->snd_rwe;
    resp.dtp.next_seq_num_to_use    = dtp->next_seq_num_to_use;
    resp.dtp.last_seq_num_sent      = dtp->last_seq_num_sent;
    resp.dtp.last_ctrl_seq_num_rcvd = dtp->last_ctrl_seq_num_rcvd;
    resp.dtp.cwq_len                = dtp->cwq_len;
    resp.dtp.max_cwq_len            = dtp->max_cwq_len;
    resp.dtp.rtxq_len               = dtp->rtxq_len;
    resp.dtp.max_rtxq_len           = dtp->max_rtxq_len;
    resp.dtp.rtt                    = jiffies_to_msecs(dtp->rtt) * 1000;
    resp.dtp.rtt_stddev             = jiffies_to_msecs(dtp->rtt_stddev) * 1000;
    resp.dtp.cgwin                  = dtp->cgwin;
    resp.dtp.rcv_lwe                = dtp->rcv_lwe;
    resp.dtp.rcv_next_seq_num       = dtp->rcv_next_seq_num;
    resp.dtp.rcv_rwe                = dtp->rcv_rwe;
    resp.dtp.max_seq_num_rcvd       = dtp->max_seq_num_rcvd;
    resp.dtp.last_lwe_sent          = dtp->last_lwe_sent;
    resp.dtp.last_seq_num_acked     = dtp->last_seq_num_acked;
    resp.dtp.next_snd_ctl_seq       = dtp->next_snd_ctl_seq;
    resp.dtp.seqq_len               = dtp->seqq_len;

    spin_unlock_bh(&dtp->lock);
    spin_unlock_bh(&flow->txrx.rx_lock);

    flow_put(flow);

    ret = rl_upqueue_append(rc, (const struct rl_msg_base *)&resp, false);
    rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&resp));

    return ret;
}

static int
rl_flow_cfg_update(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_flow_cfg_update *req =
        (struct rl_kmsg_flow_cfg_update *)bmsg;
    struct flow_entry *flow;
    int ret = 0;

    flow = flow_get(rc->dm, req->port_id);
    if (!flow) {
        return -EINVAL;
    }

    if (flow->txrx.ipcp->ops.flow_cfg_update) {
        ret = flow->txrx.ipcp->ops.flow_cfg_update(flow, &req->flowcfg);
    }
    flow_put(flow);

    return ret;
}

/* Connect the upper IPCP which is using this flow
 * so that rl_sdu_rx() can deliver SDU to the IPCP. */
static int
upper_ipcp_flow_bind(struct rl_ctrl *rc, rl_ipcp_id_t upper_ipcp_id,
                     struct flow_entry *flow)
{
    struct ipcp_entry *ipcp = flow->txrx.ipcp;
    struct ipcp_entry *upper_ipcp;

    /* Lookup the IPCP user of 'flow'. */
    upper_ipcp = ipcp_get(rc->dm, upper_ipcp_id);
    if (!upper_ipcp) {
        PE("No such upper ipcp %u\n", upper_ipcp_id);

        return -ENXIO;
    }

#if 0
    if (upper_ipcp->uipcp != rc) {
        PE("Control device %p cannot bind flow to kernel datapath "
           "without first declaring itself an IPCP\n", rc);
        ipcp_put(upper_ipcp);

        return -EINVAL;
    }
#endif

    flow->upper.ipcp = upper_ipcp;

    mutex_lock(&ipcp->lock);
    /* The ipcp->shortcut field must be set only while there is one and
     * only one upper IPCP. */
    if (ipcp->shortcut_flows == 0) {
        /* Reuse the reference, without increasing the reference counter. */
        ipcp->shortcut = upper_ipcp;
    } else if (upper_ipcp != ipcp->shortcut) {
        ipcp->shortcut = NULL;
    }
    ipcp->shortcut_flows++;
    mutex_unlock(&ipcp->lock);

    return 0;
}

static int
rl_appl_register(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_appl_register *req = (struct rl_kmsg_appl_register *)bmsg;
    uint32_t event_id                 = req->hdr.event_id;
    struct rl_ctrl *uipcp             = NULL;
    struct ipcp_entry *ipcp;
    int ret = 0;

    /* Find an IPC Process entry corresponding to req->dif_name. */
    ipcp = ipcp_select_by_dif(rc->dm, req->dif_name);
    if (!ipcp) {
        return -ENXIO;
    }

    if (!ipcp->ops.appl_register) {
        /* If appl_register and uipcp are both non-NULL we give priority
         * to the kernel-space path (useful for shim-wifi). */
        uipcp = ipcp->uipcp;
        if (!uipcp) {
            /* This IPCP handles the registration in user-space, but no uipcp
             * is associated to it. */
            ret = -ENXIO;
            goto out;
        }
    }

    if (req->reg) {
        ret = ipcp_application_add(ipcp, req->appl_name, rc, event_id,
                                   uipcp != NULL);
    } else {
        ret = ipcp_application_del(ipcp, req->appl_name);
    }

    if (!ret && uipcp) {
        /* Reflect to userspace this (un)registration, so that
         * userspace IPCP can take appropriate actions. */
        req->hdr.event_id = 0; /* clear it, not needed */
        rl_upqueue_append(uipcp, (const struct rl_msg_base *)req, true);
    }

    if (ret || !uipcp || !req->reg) {
        /* Complete the (un)registration immediately notifying the
         * requesting application. */
        struct rl_kmsg_appl_register_resp resp;

        if (ret > 0) {
            /* ipcp_application_add() returned a positive result.
             * This is not an error. */
            ret = 0;
        }

        resp.hdr.msg_type = RLITE_KER_APPL_REGISTER_RESP;
        resp.hdr.event_id = event_id;
        resp.ipcp_id      = ipcp->id;
        resp.reg          = req->reg;
        resp.response     = ret ? RLITE_ERR : RLITE_SUCC;
        resp.appl_name    = rl_strdup(req->appl_name, GFP_ATOMIC, RL_MT_UTILS);

        rl_upqueue_append(rc, (const struct rl_msg_base *)&resp, false);
        rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&resp));

        if (!ret) {
            PI("Application process %s %sregistered to IPC process %s\n",
               req->appl_name, (req->reg ? "" : "un"), ipcp->name);
        }

        /* If ret != 0, we just appended a negative response, so the error
         * code for the system call can be reset. */
        ret = 0;
    }
out:
    ipcp_put(ipcp);

    return ret;
}

static int
rl_appl_register_resp(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_appl_register_resp *resp =
        (struct rl_kmsg_appl_register_resp *)bmsg;
    struct ipcp_entry *ipcp;
    int ret = -EINVAL; /* Report failure by default. */

    ipcp = ipcp_get(rc->dm, resp->ipcp_id);

    if (!ipcp || (!ipcp->ops.appl_register && !ipcp->uipcp) || !resp->reg) {
        PE("Spurious/malicious application register response to "
           "IPCP %u\n",
           resp->ipcp_id);
    } else {
        struct registered_appl *app;

        app = ipcp_application_get(ipcp, resp->appl_name);
        if (!app) {
            PE("Application register response does not match registration for "
               "'%s'\n",
               resp->appl_name);
        } else {
            ret                = 0;
            resp->hdr.event_id = app->event_id;

            if (resp->response != 0) {
                /* Userspace IPCP denied the registration. */
                ipcp_application_put(app);

            } else {
                app->state = APPL_REG_COMPLETE;
                PI("Application process %s %sregistered to IPC process %s\n",
                   resp->appl_name, (resp->reg ? "" : "un"), ipcp->name);
            }
            rl_upqueue_append(app->rc, (const struct rl_msg_base *)resp, true);
        }
        ipcp_application_put(app);
    }

    ipcp_put(ipcp);

    return ret;
}

static const struct file_operations rl_ctrl_fops;

static int
rl_appl_move(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_appl_move *req = (struct rl_kmsg_appl_move *)bmsg;
    struct registered_appl *app;
    struct ipcp_entry *ipcp;
    struct file *dst_file;
    struct rl_ctrl *dst_rc;
    int ret = 0;

    dst_file = fget(req->fd);
    if (dst_file == NULL || dst_file->f_op != &rl_ctrl_fops) {
        return -EBADF;
    }
    dst_rc = dst_file->private_data;

    ipcp = ipcp_get(rc->dm, req->ipcp_id);
    if (!ipcp) {
        ret = -ENXIO;
        goto out;
    }

    RALOCK(ipcp);
    /* Search all the applications registered to this control device. */
    list_for_each_entry (app, &ipcp->registered_appls, node) {
        if (app->rc == rc) {
            /* Move the reference. */
            app->rc = dst_rc;
        }
    }
    RAUNLOCK(ipcp);

    ipcp_put(ipcp);
out:
    fput(dst_file);

    return ret;
}

static int
rl_append_allocate_flow_resp_arrived(struct rl_ctrl *rc, uint32_t event_id,
                                     rl_port_t port_id, uint8_t response,
                                     bool maysleep)
{
    struct rl_kmsg_fa_resp_arrived resp;

    memset(&resp, 0, sizeof(resp));
    resp.hdr.msg_type = RLITE_KER_FA_RESP_ARRIVED;
    resp.hdr.event_id = event_id;
    resp.port_id      = port_id;
    resp.response     = response;

    /* Enqueue the response into the upqueue. */
    return rl_upqueue_append(rc, RLITE_MB(&resp), maysleep);
}

/* (1): client application --> kernel IPCP */
static int
rl_fa_req(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_fa_req *req    = (struct rl_kmsg_fa_req *)bmsg;
    struct ipcp_entry *ipcp_entry = NULL;
    struct flow_entry *flow_entry = NULL;
    uint32_t event_id             = req->hdr.event_id;
    struct upper_ref upper        = {
        .rc = rc,
    };
    rl_ipcp_id_t ipcp_id = -1;
    rl_port_t local_port = 0;
    int ret              = -ENXIO;

    /* Look up an IPC process entry for the specified DIF. */
    ipcp_entry = ipcp_select_by_dif(rc->dm, req->dif_name);
    if (!ipcp_entry) {
        goto out;
    }
    ipcp_id = ipcp_entry->id;

    /* Allocate a port id and the associated flow entry. */
    ret =
        flow_add(ipcp_entry, upper, event_id, req->local_appl, req->remote_appl,
                 NULL, &req->flowspec, &flow_entry, GFP_KERNEL);
    if (ret) {
        goto out;
    }

    /* We are the initiator for this flow. */
    flow_entry->flags |= RL_FLOW_INITIATOR;

    local_port = flow_entry->local_port;

    if (req->upper_ipcp_id != 0xffff) {
        ret = upper_ipcp_flow_bind(rc, req->upper_ipcp_id, flow_entry);
        if (ret) {
            goto out;
        }
    }

    if (ipcp_entry->ops.flow_allocate_req) {
        /* This IPCP handles the flow allocation in kernel-space. This is
         * currently true for shim IPCPs. */
        ret = ipcp_entry->ops.flow_allocate_req(ipcp_entry, flow_entry,
                                                &req->flowspec);
    } else {
        if (!ipcp_entry->uipcp) {
            /* No userspace IPCP to use, this happens when no uipcp is assigned
             * to this IPCP. */
            ret = -ENXIO;
        } else {
            /* This IPCP handles the flow allocation in user-space. This is
             * currently true for normal IPCPs.
             * Reflect the flow allocation request message to userspace. */
            req->hdr.event_id = 0; /* clear it, not needed */
            req->local_port   = flow_entry->local_port;
            req->local_cep    = flow_entry->local_cep;
            req->uid          = flow_entry->uid; /* tell the uid to the uipcp */
            ret               = rl_upqueue_append(ipcp_entry->uipcp,
                                    (const struct rl_msg_base *)req, true);
        }
    }

out:
    if (flow_entry) {
        flow_put(flow_entry); /* match flow_add() */
        /* The flow_entry variable cannot be used in this function after this
         * point, because a concurrent rl_fa_resp_arrived() with a negative
         * response may kill the flow. */
        flow_entry = NULL;
    }

    if (ret == 0) {
        PD("Flow allocation requested to IPC process %s, "
           "port-id %u\n",
           ipcp_entry->name, local_port);
    }
    ipcp_put(ipcp_entry);

    if (ret == 0) {
        return 0;
    }

    /* Create a negative response message. This must be done before
     * calling flow_put(), which drops the reference on rc. */
    ret = rl_append_allocate_flow_resp_arrived(rc, event_id, 0, 1, true);

    if (flow_entry) {
        flows_putq_del(flow_entry); /* match flow_add() */
        flow_put(flow_entry);       /* delete */
    }

    return ret;
}

/* (3): server application --> kernel IPCP */
static int
rl_fa_resp(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    struct rl_kmsg_fa_resp *resp = (struct rl_kmsg_fa_resp *)bmsg;
    struct flow_entry *flow_entry;
    struct ipcp_entry *ipcp;
    int ret = -EINVAL;

    /* Lookup the flow corresponding to the port-id specified
     * by the request. */
    flow_entry = flow_get(rc->dm, resp->port_id);
    if (!flow_entry) {
        PE("no pending flow corresponding to port-id %u\n", resp->port_id);
        return ret;
    }

    if (resp->kevent_id != flow_entry->event_id) {
        PE("kevent_id mismatch: %u != %u\n", resp->kevent_id,
           flow_entry->event_id);
        goto out;
    }

    BUG_ON(rc != flow_entry->upper.rc);

    /* Check that the flow is in pending state and make the
     * transition to the allocated state. */
    spin_lock_bh(&flow_entry->txrx.rx_lock);
    if (!(flow_entry->flags & RL_FLOW_PENDING)) {
        PE("flow %u is in invalid state %x\n", flow_entry->local_port,
           flow_entry->flags);
        spin_unlock_bh(&flow_entry->txrx.rx_lock);
        goto out;
    }
    flow_entry->flags &= ~RL_FLOW_PENDING;
    if (resp->response == 0) {
        flow_entry->flags |= RL_FLOW_ALLOCATED;
        flow_entry->upper.rc = NULL;
    }
    spin_unlock_bh(&flow_entry->txrx.rx_lock);
    if (resp->response == 0) {
        fput(rc->file);
    }

    if (!resp->response && resp->upper_ipcp_id != 0xffff) {
        ret = upper_ipcp_flow_bind(rc, resp->upper_ipcp_id, flow_entry);
        if (ret) {
            goto out;
        }
    }

    PD("Flow allocation response [%u] issued to IPC process %s, "
       "port-id %u\n",
       resp->response, flow_entry->txrx.ipcp->name, flow_entry->local_port);

    /* Notify the involved IPC process about the response. */
    ipcp = flow_entry->txrx.ipcp;
    if (ipcp->ops.flow_allocate_resp) {
        /* This IPCP handles the flow allocation in kernel-space. This is
         * currently true for shim IPCPs. */
        ret = ipcp->ops.flow_allocate_resp(ipcp, flow_entry, resp->response);
    } else {
        if (!ipcp->uipcp) {
            /* No userspace IPCP to use, this happens when no uipcp is assigned
             * to this IPCP. */
            ret = -ENXIO;
        } else {
            /* This IPCP handles the flow allocation in user-space. This is
             * currently true for normal IPCPs.
             * Reflect the flow allocation response message to userspace. */
            resp->hdr.event_id = 0;
            resp->cep_id       = flow_entry->local_cep;
            ret                = rl_upqueue_append(ipcp->uipcp,
                                    (const struct rl_msg_base *)resp, true);
        }
    }
out:
    if (ret || resp->response) {
        flows_putq_del(flow_entry);
        flow_put(flow_entry);
    }

    flow_put(flow_entry);

    return ret;
}

/* This may be called from softirq context.
 * (2): server application <-- kernel IPCP */
int
rl_fa_req_arrived(struct ipcp_entry *ipcp, uint32_t kevent_id,
                  rl_port_t remote_port, rlm_cepid_t remote_cep,
                  rlm_qosid_t qos_id, rlm_addr_t remote_addr,
                  const char *local_appl, const char *remote_appl,
                  const struct rl_flow_config *flowcfg,
                  const struct rina_flow_spec *flowspec, bool maysleep)
{
    struct flow_entry *flow_entry = NULL;
    struct registered_appl *app;
    struct rl_kmsg_fa_req_arrived req;
    struct upper_ref upper;
    int ret = -EINVAL;

    /* See whether the local application is registered to this
     * IPC process. */
    app = ipcp_application_get(ipcp, local_appl);
    if (!app) {
        goto out;
    }

    memset(&req, 0, sizeof(req));
    if (flowspec) {
        memcpy(&req.flowspec, flowspec, sizeof(*flowspec));
    } else {
        rl_flow_spec_default(&req.flowspec);
    }

    /* Allocate a port id and the associated flow entry. */
    upper.rc   = app->rc;
    upper.ipcp = NULL;
    ret = flow_add(ipcp, upper, kevent_id, local_appl, remote_appl, flowcfg,
                   &req.flowspec, &flow_entry, GFP_ATOMIC);
    if (ret) {
        goto out;
    }
    flow_entry->remote_port = remote_port;
    flow_entry->remote_cep  = remote_cep;
    flow_entry->qos_id      = qos_id;
    flow_entry->remote_addr = remote_addr;
    flow_entry->uid = kevent_id; /* overwrite uid with the one generated by the
                                  * uipcp */

    PD("Flow allocation request arrived to IPC process %s, "
       "port-id %u\n",
       ipcp->name, flow_entry->local_port);

    req.hdr.msg_type = RLITE_KER_FA_REQ_ARRIVED;
    req.hdr.event_id = 0;
    req.kevent_id    = kevent_id;
    req.ipcp_id      = ipcp->id;
    req.port_id      = flow_entry->local_port;
    req.local_appl =
        local_appl ? rl_strdup(local_appl, GFP_ATOMIC, RL_MT_UTILS) : NULL;
    req.remote_appl =
        remote_appl ? rl_strdup(remote_appl, GFP_ATOMIC, RL_MT_UTILS) : NULL;
    if (ipcp->dif->name) {
        req.dif_name = rl_strdup(ipcp->dif->name, GFP_ATOMIC, RL_MT_UTILS);
    }

    flow_put(flow_entry); /* match flow_add() */

    /* Enqueue the request into the upqueue. */
    ret = rl_upqueue_append(app->rc, RLITE_MB(&req), maysleep);
    if (ret) {
        flows_putq_del(flow_entry); /* match flow_add() */
        flow_put(flow_entry);       /* delete */
    } else {
        /* The flow_entry variable is invalid from here, rl_fa_resp() may be
         * called concurrently and call flow_put(). */
        flow_entry = NULL;
    }
    rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&req));
out:
    ipcp_application_put(app);

    return ret;
}
EXPORT_SYMBOL(rl_fa_req_arrived);

/* (4): client application <-- kernel IPCP */
int
rl_fa_resp_arrived(struct ipcp_entry *ipcp, rl_port_t local_port,
                   rl_port_t remote_port, rlm_cepid_t remote_cep,
                   rlm_qosid_t qos_id, rlm_addr_t remote_addr, uint8_t response,
                   struct rl_flow_config *flowcfg, bool maysleep)
{
    struct flow_entry *flow_entry = NULL;
    int ret                       = -EINVAL;
    struct rl_ctrl *rc;

    flow_entry = flow_get(ipcp->dm, local_port);
    if (!flow_entry) {
        return ret;
    }

    spin_lock_bh(&flow_entry->txrx.rx_lock);
    if (!(flow_entry->flags & RL_FLOW_PENDING)) {
        spin_unlock_bh(&flow_entry->txrx.rx_lock);
        goto out;
    }
    rc = flow_entry->upper.rc;
    flow_entry->flags &= ~RL_FLOW_PENDING;
    if (response == 0) {
        flow_entry->flags |= RL_FLOW_ALLOCATED;
        flow_entry->upper.rc = NULL;
    }
    flow_entry->remote_port = remote_port;
    flow_entry->remote_cep  = remote_cep;
    flow_entry->qos_id      = qos_id;
    flow_entry->remote_addr = remote_addr;
    spin_unlock_bh(&flow_entry->txrx.rx_lock);

    if (flowcfg) {
        memcpy(&flow_entry->cfg, flowcfg, sizeof(*flowcfg));
        if (ipcp->ops.flow_init) {
            /* Let the IPCP do some
             * specific initialization. */
            ipcp->ops.flow_init(ipcp, flow_entry);
        }
    }

    PD("Flow allocation response arrived to IPC process %s, "
       "port-id %u, remote addr %llu\n",
       ipcp->name, local_port, (long long unsigned)remote_addr);

    ret = rl_append_allocate_flow_resp_arrived(rc, flow_entry->event_id,
                                               local_port, response, maysleep);
    if (response == 0) {
        fput(rc->file);
    }

    if (response || ret) {
        /* Negative response --> delete the flow. */
        flows_putq_del(flow_entry);
        flow_put(flow_entry);
    }

out:
    flow_put(flow_entry);

    return ret;
}
EXPORT_SYMBOL(rl_fa_resp_arrived);

/* Share the same tx_wqh with other flows supported by the same IPCP. */
void
rl_flow_share_tx_wqh(struct flow_entry *flow)
{
    flow->txrx.tx_wqh = &flow->txrx.ipcp->tx_wqh;
}
EXPORT_SYMBOL(rl_flow_share_tx_wqh);

#ifdef RL_MEMTRACK
static int
rl_memtrack_dump(struct rl_ctrl *rc, struct rl_msg_base *bmsg)
{
    rl_memtrack_dump_stats();
    return 0;
}
#endif /* RL_MEMTRACK */

/* The table containing all the message handlers. */
static rl_msg_handler_t rl_ctrl_handlers[] = {
    [RLITE_KER_IPCP_CREATE]           = rl_ipcp_create,
    [RLITE_KER_IPCP_DESTROY]          = rl_ipcp_destroy,
    [RLITE_KER_FLOW_FETCH]            = rl_flow_fetch,
    [RLITE_KER_IPCP_CONFIG]           = rl_ipcp_config,
    [RLITE_KER_IPCP_PDUFT_SET]        = rl_ipcp_pduft_mod,
    [RLITE_KER_IPCP_PDUFT_DEL]        = rl_ipcp_pduft_mod,
    [RLITE_KER_IPCP_PDUFT_FLUSH]      = rl_ipcp_pduft_flush,
    [RLITE_KER_APPL_REGISTER]         = rl_appl_register,
    [RLITE_KER_APPL_REGISTER_RESP]    = rl_appl_register_resp,
    [RLITE_KER_FA_REQ]                = rl_fa_req,
    [RLITE_KER_FA_RESP]               = rl_fa_resp,
    [RLITE_KER_IPCP_UIPCP_SET]        = rl_ipcp_uipcp_set,
    [RLITE_KER_IPCP_UIPCP_WAIT]       = rl_ipcp_uipcp_wait,
    [RLITE_KER_UIPCP_FA_REQ_ARRIVED]  = rl_uipcp_fa_req_arrived,
    [RLITE_KER_UIPCP_FA_RESP_ARRIVED] = rl_uipcp_fa_resp_arrived,
    [RLITE_KER_FLOW_DEALLOC]          = rl_flow_dealloc,
    [RLITE_KER_FLOW_STATS_REQ]        = rl_flow_get_stats,
    [RLITE_KER_FLOW_CFG_UPDATE]       = rl_flow_cfg_update,
    [RLITE_KER_IPCP_QOS_SUPPORTED]    = rl_ipcp_qos_supported,
    [RLITE_KER_APPL_MOVE]             = rl_appl_move,
    [RLITE_KER_REG_FETCH]             = rl_reg_fetch,
    [RLITE_KER_IPCP_STATS_REQ]        = rl_ipcp_get_stats,
    [RLITE_KER_IPCP_CONFIG_GET_REQ]   = rl_ipcp_config_get,
    [RLITE_KER_IPCP_SCHED_WRR]        = rl_ipcp_sched_config,
    [RLITE_KER_IPCP_SCHED_PFIFO]      = rl_ipcp_sched_config,
#ifdef RL_MEMTRACK
    [RLITE_KER_MEMTRACK_DUMP] = rl_memtrack_dump,
#endif /* RL_MEMTRACK */
    [RLITE_KER_MSG_MAX] = NULL,
};

static ssize_t
rl_ctrl_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
    struct rl_ctrl *rc = (struct rl_ctrl *)f->private_data;
    struct rl_msg_base *bmsg;
    char *kbuf;
    ssize_t ret;

    if (len < sizeof(struct rl_msg_base)) {
        /* This message doesn't even contain version and message type. */
        return -EINVAL;
    }

    kbuf = rl_alloc(len, GFP_KERNEL, RL_MT_MISC);
    if (!kbuf) {
        return -ENOMEM;
    }

    /* Copy the userspace serialized message into a temporary kernelspace
     * buffer. */
    if (unlikely(copy_from_user(kbuf, ubuf, len))) {
        rl_free(kbuf, RL_MT_MISC);
        return -EFAULT;
    }

    ret = deserialize_rlite_msg(rl_ker_numtables, RLITE_KER_MSG_MAX, kbuf, len,
                                rc->msgbuf, sizeof(rc->msgbuf));
    if (ret) {
        rl_free(kbuf, RL_MT_MISC);
        return -EINVAL;
    }

    bmsg = RLITE_MB(rc->msgbuf);

    /* Demultiplex the message to the right message handler. */
    if (bmsg->hdr.msg_type > RLITE_KER_MSG_MAX ||
        !rc->handlers[bmsg->hdr.msg_type]) {
        rl_free(kbuf, RL_MT_MISC);
        return -EINVAL;
    }

    /* Check permissions. */
    switch (bmsg->hdr.msg_type) {
    case RLITE_KER_IPCP_CREATE:
    case RLITE_KER_IPCP_DESTROY:
    case RLITE_KER_IPCP_CONFIG:
    case RLITE_KER_IPCP_PDUFT_SET:
    case RLITE_KER_IPCP_PDUFT_FLUSH:
    case RLITE_KER_APPL_REGISTER_RESP:
    case RLITE_KER_IPCP_UIPCP_SET:
    case RLITE_KER_UIPCP_FA_REQ_ARRIVED:
    case RLITE_KER_UIPCP_FA_RESP_ARRIVED:
    case RLITE_KER_FLOW_DEALLOC:
#if 1
        if (!capable(CAP_SYS_ADMIN)) {
            rl_free(kbuf, RL_MT_MISC);
            return -EPERM;
        }
#endif
        break;
    case RLITE_KER_FLOW_FETCH:
        break;
    }

    /* Carry out the requested operation. */
    ret = rc->handlers[bmsg->hdr.msg_type](rc, bmsg);
    rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, bmsg);
    rl_free(kbuf, RL_MT_MISC);
    if (ret) {
        return ret;
    }

    *ppos += len;

    return len;
}

static ssize_t
rl_ctrl_read(struct file *f, char __user *buf, size_t len, loff_t *ppos)
{
    DECLARE_WAITQUEUE(wait, current);
    struct upqueue_entry *entry;
    struct rl_ctrl *rc = (struct rl_ctrl *)f->private_data;
    bool blocking      = !(f->f_flags & O_NONBLOCK);
    int ret            = 0;

    if (blocking) {
        add_wait_queue(&rc->upqueue_wqh, &wait);
    }
    while (len) {
        set_current_state(TASK_INTERRUPTIBLE);

        spin_lock(&rc->upqueue_lock);
        if (list_empty(&rc->upqueue)) {
            /* No pending messages? Let's sleep. */
            spin_unlock(&rc->upqueue_lock);

            if (signal_pending(current)) {
                ret = -ERESTARTSYS;
                break;
            }

            if (!blocking) {
                ret = -EAGAIN;
                break;
            }

            schedule();
            continue;
        }

        entry = list_first_entry(&rc->upqueue, struct upqueue_entry, node);
        if (len < entry->serlen) {
            /* Not enough space? Don't pop the entry from the upqueue. */
            ret = -ENOBUFS;
        } else if (unlikely(copy_to_user(buf, entry->sermsg, entry->serlen))) {
            ret = -EFAULT;
        } else {
            ret = entry->serlen;
            *ppos += ret;

            /* Unlink and free the upqueue entry and the associated message. */
            list_del_init(&entry->node);
            rc->upqueue_size -= upqentry_size(entry);
            rl_free(entry->sermsg, RL_MT_UPQ);
            rl_free(entry, RL_MT_UPQ);
        }

        spin_unlock(&rc->upqueue_lock);
        break;
    }

    __set_current_state(TASK_RUNNING);
    if (blocking) {
        remove_wait_queue(&rc->upqueue_wqh, &wait);
    }

    if (ret > 0) {
        /* Some space was freed up in the upqueue: wake up processes
         * blocked on rl_upqueue_append(). */
        wake_up_interruptible_poll(&rc->upqueue_wqh,
                                   POLLOUT | POLLWRNORM | POLLWRBAND);
    }

    return ret;
}

static unsigned int
rl_ctrl_poll(struct file *f, poll_table *wait)
{
    struct rl_ctrl *rc = (struct rl_ctrl *)f->private_data;
    unsigned int mask  = 0;

    poll_wait(f, &rc->upqueue_wqh, wait);

    spin_lock(&rc->upqueue_lock);
    if (!list_empty(&rc->upqueue)) {
        mask |= POLLIN | POLLRDNORM;
    }
    spin_unlock(&rc->upqueue_lock);

    mask |= POLLOUT | POLLWRNORM;

    return mask;
}

static int
initial_ipcp_update(struct rl_ctrl *rc)
{
    struct ipcp_entry *entry;
    int bucket;
    int ret = 0;

    PLOCK(rc->dm);

    hash_for_each(rc->dm->ipcp_table, bucket, entry, node)
    {
        struct rl_kmsg_ipcp_update upd;

        ret = ipcp_update_fill(entry, &upd, RL_IPCP_UPDATE_ADD);

        rl_upqueue_append(rc, (const struct rl_msg_base *)&upd, false);

        rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX, RLITE_MB(&upd));
    }

    PUNLOCK(rc->dm);

    if (ret) {
        RPV(1, "Out of memory\n");
    }

    return ret;
}

static int
rl_ctrl_open(struct inode *inode, struct file *f)
{
    struct rl_ctrl *rc;

    rc = rl_alloc(sizeof(*rc), GFP_KERNEL | __GFP_ZERO, RL_MT_CTLDEV);
    if (!rc) {
        return -ENOMEM;
    }

    rc->dm = rl_dm_get();
    if (!rc->dm) {
        rl_free(rc, RL_MT_CTLDEV);
        return -ENOMEM;
    }

    f->private_data = rc;
    rc->file        = f;
    INIT_LIST_HEAD(&rc->upqueue);
    rc->upqueue_size = 0;
    spin_lock_init(&rc->upqueue_lock);
    init_waitqueue_head(&rc->upqueue_wqh);

    INIT_LIST_HEAD(&rc->flows_fetch_q);
    INIT_LIST_HEAD(&rc->regs_fetch_q);

    rc->handlers = rl_ctrl_handlers;

    mutex_lock(&rc->dm->general_lock);
    list_add_tail(&rc->node, &rc->dm->ctrl_devs);
    mutex_unlock(&rc->dm->general_lock);

    return 0;
}

static int
rl_ctrl_release(struct inode *inode, struct file *f)
{
    struct rl_ctrl *rc = (struct rl_ctrl *)f->private_data;
    struct rl_dm *dm   = rc->dm;

    mutex_lock(&dm->general_lock);
    list_del_init(&rc->node);
    mutex_unlock(&dm->general_lock);

    /* We must invalidate (e.g. unregister) all the
     * application names registered with this ctrl device. */
    application_del_by_rc(rc);
    flow_rc_probe_references(rc);

    /* Drain upqueue. */
    {
        struct upqueue_entry *ue, *uet;

        list_for_each_entry_safe (ue, uet, &rc->upqueue, node) {
            list_del_init(&ue->node);
            rl_free(ue->sermsg, RL_MT_UPQ);
            rl_free(ue, RL_MT_UPQ);
        }
    }

    /* Drain flows-fetch queue. */
    {
        struct flows_fetch_q_entry *fqe, *fqet;

        list_for_each_entry_safe (fqe, fqet, &rc->flows_fetch_q, node) {
            list_del_init(&fqe->node);
            rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX,
                        RLITE_MB(&fqe->resp));
            rl_free(fqe, RL_MT_FFETCH);
        }
    }

    /* Drain regs-fetch queue. */
    {
        struct regs_fetch_q_entry *fqe, *fqet;

        list_for_each_entry_safe (fqe, fqet, &rc->regs_fetch_q, node) {
            list_del_init(&fqe->node);
            rl_msg_free(rl_ker_numtables, RLITE_KER_MSG_MAX,
                        RLITE_MB(&fqe->resp));
            rl_free(fqe, RL_MT_FFETCH);
        }
    }

    rl_free(rc, RL_MT_CTLDEV);
    f->private_data = NULL;

    rl_dm_put(dm);

    return 0;
}

static long
rl_ctrl_ioctl(struct file *f, unsigned int cmd, unsigned long flags)
{
    struct rl_ctrl *rc   = (struct rl_ctrl *)f->private_data;
    unsigned int changed = flags ^ rc->flags;

    /* We have only one command, to change the flags. */
    if (cmd != RLITE_IOCTL_CHFLAGS) {
        return -EINVAL;
    }

    if (flags & ~RL_F_ALL) {
        return -EINVAL;
    }

    if (changed & flags & RL_F_IPCPS) {
        /* User turned on IPCP updates. Enqueue RLITE_KER_IPCP_UPDATE
         * messages for all the IPCPs in the system. */
        initial_ipcp_update(rc);
    }
    rc->flags = flags;

    return 0;
}

#ifdef CONFIG_COMPAT
static long
rl_ctrl_compat_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    return rl_ctrl_ioctl(f, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations rl_ctrl_fops = {
    .owner          = THIS_MODULE,
    .release        = rl_ctrl_release,
    .open           = rl_ctrl_open,
    .write          = rl_ctrl_write,
    .read           = rl_ctrl_read,
    .poll           = rl_ctrl_poll,
    .unlocked_ioctl = rl_ctrl_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = rl_ctrl_compat_ioctl,
#endif
    .llseek = noop_llseek,
};

static struct miscdevice rl_ctrl_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "rlite",
    .fops  = &rl_ctrl_fops,
};

extern struct miscdevice rl_io_misc;

/* We want to know if a rl_dm instance is empty so that we can remove
 * it and release its network namespace (if it is not the default one). */
static bool
rl_dm_empty(struct rl_dm *dm)
{
    return hash_empty(dm->ipcp_table) && hash_empty(dm->flow_table) &&
           hash_empty(dm->flow_table_by_cep) && list_empty(&dm->difs) &&
           list_empty(&dm->ctrl_devs) && list_empty(&dm->appl_removeq) &&
           !work_pending(&dm->appl_removew) &&
           !timer_pending(&dm->flows_putq_tmr) &&
           list_empty(&dm->flows_removeq) && list_empty(&dm->flows_putq) &&
           !work_pending(&dm->flows_removew);
}

/* To be called under rl_global.lock. */
static struct rl_dm *
rl_dm_lookup(struct net *net)
{
    struct rl_dm *dm;
    struct hlist_head *head;

    head = &rl_global.netns_table[hash_min((uintptr_t)net,
                                           HASH_BITS(rl_global.netns_table))];
    hlist_for_each_entry (dm, head, node) {
        if (dm->net == net) {
            return dm;
        }
    };

    return NULL;
}

static struct rl_dm *
rl_dm_get(void)
{
    struct net *net = current->nsproxy ? current->nsproxy->net_ns : &init_net;
    struct rl_dm *dm;

    mutex_lock(&rl_global.lock);
    dm = rl_dm_lookup(net);
    if (dm != NULL) {
        /* A data model for the current namespace was already created. Increment
         * its reference counter and return it. */
        dm->refcnt++;
        mutex_unlock(&rl_global.lock);

        return dm;
    }

    /* Data model not found for the current namespace. Let's create one and
     * return it. */
    dm = rl_alloc(sizeof(*dm), GFP_KERNEL, RL_MT_DM);
    if (dm == NULL) {
        return NULL;
    }

    bitmap_zero(dm->ipcp_id_bitmap, IPCP_ID_BITMAP_SIZE);
    hash_init(dm->ipcp_table);
    bitmap_zero(dm->port_id_bitmap, PORT_ID_BITMAP_SIZE);
    hash_init(dm->flow_table);
    bitmap_zero(dm->cep_id_bitmap, CEP_ID_BITMAP_SIZE);
    hash_init(dm->flow_table_by_cep);
    mutex_init(&dm->general_lock);
    rwlock_init(&dm->flows_lock);
    spin_lock_init(&dm->ipcps_lock);
    spin_lock_init(&dm->difs_lock);
    spin_lock_init(&dm->appl_removeq_lock);
    INIT_LIST_HEAD(&dm->difs);
    INIT_LIST_HEAD(&dm->ctrl_devs);
    INIT_LIST_HEAD(&dm->appl_removeq);
    INIT_LIST_HEAD(&dm->flows_removeq);
    INIT_LIST_HEAD(&dm->flows_putq);
    INIT_WORK(&dm->appl_removew, appl_removew_func);
    INIT_WORK(&dm->flows_removew, flows_removew_func);
#ifdef RL_HAVE_TIMER_SETUP
    timer_setup(&dm->flows_putq_tmr, flows_putq_drain, 0);
#else  /* !RL_HAVE_TIMER_SETUP */
    setup_timer(&dm->flows_putq_tmr, flows_putq_drain, (unsigned long)dm);
#endif /* !RL_HAVE_TIMER_SETUP */
    dm->net    = net;
    dm->refcnt = 1; /* Cogito, ergo sum. */
    hash_add(rl_global.netns_table, &dm->node, (uintptr_t)net);
    /* Grab a reference to the parent network namespace. */
    get_net(net);
    mutex_unlock(&rl_global.lock);

    PD("Data model created for namespace %p\n", net);

    return dm;
}

static struct rl_dm *
rl_dm_getref(struct rl_dm *dm)
{
    mutex_lock(&rl_global.lock);
    dm->refcnt++;
    mutex_unlock(&rl_global.lock);

    return dm;
}

static void
rl_dm_put(struct rl_dm *dm)
{
    if (dm == NULL) {
        return;
    }

    mutex_lock(&rl_global.lock);
    BUG_ON(dm->refcnt == 0);
    dm->refcnt--;
    if (dm->refcnt > 0) {
        mutex_unlock(&rl_global.lock);
        return; /* still in use */
    }

    /* This data model is not used anymore. We can get rid of it. */
    hash_del(&dm->node);
    mutex_unlock(&rl_global.lock);

    del_timer(&dm->flows_putq_tmr);
    cancel_work_sync(&dm->flows_removew);
    cancel_work_sync(&dm->appl_removew);
    BUG_ON(!rl_dm_empty(dm));
    put_net(dm->net);
    PD("Data model for namespace %p destroyed\n", dm->net);
    dm->net = NULL;
    rl_free(dm, RL_MT_DM);
}

static int __init
rlite_init(void)
{
    int ret;

    mutex_init(&rl_global.lock);
    INIT_LIST_HEAD(&rl_global.ipcp_factories);
    hash_init(rl_global.netns_table);

    ret = misc_register(&rl_ctrl_misc);
    if (ret) {
        PE("Failed to register rlite misc device\n");
        return ret;
    }

    ret = misc_register(&rl_io_misc);
    if (ret) {
        misc_deregister(&rl_ctrl_misc);
        PE("Failed to register rlite-io misc device\n");
        return ret;
    }

    PD("using "
#ifndef RL_SKB
       "custom"
#else
       "native"
#endif
       " packet buffers\n");
    PD("revision id  : %s\n", RL_REVISION_ID);
    PD("revision date: %s\n", RL_REVISION_DATE);

    return 0;
}

static void __exit
rlite_fini(void)
{
    misc_deregister(&rl_io_misc);
    misc_deregister(&rl_ctrl_misc);
}

module_init(rlite_init);
module_exit(rlite_fini);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vincenzo Maffione <v.maffione@gmail.com>");
