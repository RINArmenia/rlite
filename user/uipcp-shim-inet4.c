#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>

#include "rlite-list.h"
#include "uipcp-container.h"


struct inet4_bindpoint {
    int fd;
    struct sockaddr_in addr;
    char *appl_name_s;

    struct list_head node;
};

struct inet4_endpoint {
    int fd;
    struct sockaddr_in addr;
    unsigned int port_id;

    struct list_head node;
};

struct shim_inet4 {
    struct list_head endpoints;
    struct list_head bindpoints;
};

#define SHIM(_u)    ((struct shim_inet4 *)((_u)->priv))

static int
parse_directory(int addr2sock, struct sockaddr_in *addr,
                struct rina_name *appl_name)
{
    char *appl_name_s = NULL;
    const char *dirfile = "/etc/rlite/shim-inet4-dir";
    FILE *fin;
    char *linebuf = NULL;
    size_t sz;
    size_t n;
    int found = 0;

    if (addr2sock) {
        appl_name_s = rina_name_to_string(appl_name);
        if (!appl_name_s) {
            PE("Out of memory\n");
            return -1;
        }
    }

    fin = fopen(dirfile, "r");
    if (!fin) {
        PE("Could not open directory file '%s'\n", dirfile);
        return -1;
    }

    while ((n = getline(&linebuf, &sz, fin)) > 0) {
        /* I know, strtok_r, strsep, etc. etc. I just wanted to have
         * some fun ;) */
        char *nm = linebuf;
        char *ip, *port, *eol;
        struct sockaddr_in cur_addr;
        int ret;

        while (*nm != '\0' && isspace(*nm)) nm++;
        if (*nm == '\0') continue;

        ip = nm;
        while (*ip != '\0' && !isspace(*ip)) ip++;
        if (*ip == '\0') continue;

        *ip = '\0';
        ip++;
        while (*ip != '\0' && isspace(*ip)) ip++;
        if (*ip == '\0') continue;

        port = ip;
        while (*port != '\0' && !isspace(*port)) port++;
        if (*port == '\0') continue;

        *port = '\0';
        port++;
        while (*port != '\0' && isspace(*port)) port++;
        if (*port == '\0') continue;

        eol = port;
        while (*eol != '\0' && !isspace(*eol)) eol++;
        if (*eol != '\0') *eol = '\0';

        memset(&cur_addr, 0, sizeof(cur_addr));
        cur_addr.sin_family = AF_INET;
        cur_addr.sin_port = htons(atoi(port));
        ret = inet_pton(AF_INET, ip, &cur_addr.sin_addr);
        if (ret != 1) {
            PE("Invalid IP address '%s'\n", ip);
            continue;
        }

        if (addr2sock) {
            if (strcmp(nm, appl_name_s) == 0) {
                memcpy(addr, &cur_addr, sizeof(cur_addr));
                found = 1;
            }

        } else { /* sock2addr */
            if (addr->sin_family == cur_addr.sin_family &&
                    addr->sin_port == cur_addr.sin_port &&
                    memcmp(&addr->sin_addr, &cur_addr.sin_addr,
                    sizeof(cur_addr.sin_addr)) == 0) {
                ret = rina_name_from_string(nm, appl_name);
                if (ret) {
                    PE("Invalid name '%s'\n", nm);
                }
                found = (ret == 0);
            }
        }

        printf("oho '%s' '%s'[%d] '%d'\n", nm, ip, ret, atoi(port));
    }

    if (linebuf) {
        free(linebuf);
    }

    fclose(fin);

    return found ? 0 : -1;
}

static int
appl_name_to_sock_addr(const struct rina_name *appl_name,
                       struct sockaddr_in *addr)
{
    return parse_directory(1, addr, (struct rina_name *)appl_name);
}

static int
sock_addr_to_appl_name(const struct sockaddr_in *addr,
                       struct rina_name *appl_name)
{
    return parse_directory(0, (struct sockaddr_in *)addr, appl_name);
}

/* ep->addr must be filled in before calling this function */
static int
open_bound_socket(int fd, struct sockaddr_in *addr)
{
    int enable = 1;

    fd = socket(PF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        PE("socket() failed [%d]\n", errno);
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable,
                   sizeof(enable))) {
        PE("setsockopt(SO_REUSEADDR) failed [%d]\n", errno);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)addr, sizeof(*addr))) {
        PE("bind() failed [%d]\n", errno);
        close(fd);
        return -1;
    }

    return 0;
}

static void accept_conn(struct rlite_evloop *loop, int lfd);

static int
shim_inet4_appl_unregister(struct uipcp *uipcp,
                           struct rina_kmsg_appl_register *req)
{
    char *appl_name_s = rina_name_to_string(&req->appl_name);
    struct shim_inet4 *shim = SHIM(uipcp);
    struct inet4_bindpoint *bp;
    int ret = -1;

    if (!appl_name_s) {
        PE("Out of memory\n");
        return -1;
    }

    list_for_each_entry(bp, &shim->bindpoints, node) {
        if (strcmp(appl_name_s, bp->appl_name_s) == 0) {
            rlite_evloop_fdcb_del(&uipcp->appl.loop, bp->fd);
            list_del(&bp->node);
            close(bp->fd);
            free(bp->appl_name_s);
            free(bp);
            ret = 0;

            break;
        }
    }

    if (ret) {
        PE("Could not find endpoint for appl_name %s\n", appl_name_s);
    }

    if (appl_name_s) {
        free(appl_name_s);
    }

    return ret;
}

static int
shim_inet4_appl_register(struct rlite_evloop *loop,
                         const struct rina_msg_base_resp *b_resp,
                         const struct rina_msg_base *b_req)
{
    struct rlite_appl *application = container_of(loop, struct rlite_appl,
                                                   loop);
    struct uipcp *uipcp = container_of(application, struct uipcp, appl);
    struct rina_kmsg_appl_register *req =
                (struct rina_kmsg_appl_register *)b_resp;
    struct shim_inet4 *shim = SHIM(uipcp);
    struct inet4_bindpoint *bp;
    int ret;

    if (!req->reg) {
        /* Process the unregistration. */
        return shim_inet4_appl_unregister(uipcp, req);
    }

    /* Process the registration. */

    bp = malloc(sizeof(*bp));
    if (!bp) {
        PE("Out of memory\n");
        return -1;
    }

    bp->appl_name_s = rina_name_to_string(&req->appl_name);
    if (!bp->appl_name_s) {
        PE("Out of memory\n");
        goto err1;
    }

    ret = appl_name_to_sock_addr(&req->appl_name, &bp->addr);
    if (ret) {
        PE("Failed to get inet4 address from appl_name '%s'\n",
           bp->appl_name_s);
        goto err2;
    }

    /* Open a listening socket, bind() and listen(). */
    ret = open_bound_socket(bp->fd, &bp->addr);
    if (ret) {
        goto err2;
    }

    if (listen(bp->fd, 5)) {
        PE("listen() failed [%d]\n", errno);
        goto err3;
    }

    /* The accept_conn() callback will be invoked on new incoming
     * connections. */
    rlite_evloop_fdcb_add(&uipcp->appl.loop, bp->fd, accept_conn);

    list_add_tail(&bp->node, &shim->bindpoints);

    return 0;

err3:
    close(bp->fd);
err2:
    free(bp->appl_name_s);
err1:
    free(bp);

    return -1;
}

static int
shim_inet4_fa_req(struct rlite_evloop *loop,
                  const struct rina_msg_base_resp *b_resp,
                  const struct rina_msg_base *b_req)
{
    struct rlite_appl *application = container_of(loop, struct rlite_appl,
                                                  loop);
    struct uipcp *uipcp = container_of(application, struct uipcp, appl);
    struct rina_kmsg_fa_req *req = (struct rina_kmsg_fa_req *)b_resp;
    struct shim_inet4 *shim = SHIM(uipcp);
    struct sockaddr_in remote_addr;
    struct inet4_endpoint *ep;
    int ret;

    PD("[uipcp %u] Got reflected message\n", uipcp->ipcp_id);

    assert(b_req == NULL);

    ep = malloc(sizeof(*ep));
    if (!ep) {
        PE("Out of memory\n");
        return -1;
    }

    ep->port_id = req->local_port;

    ret = appl_name_to_sock_addr(&req->local_application, &ep->addr);
    if (ret) {
        PE("Failed to get inet4 address for local application\n");
        goto err1;
    }

    ret = appl_name_to_sock_addr(&req->remote_application, &remote_addr);
    if (ret) {
        PE("Failed to get inet4 address for remote application\n");
        goto err1;
    }

    /* Open a client-side socket, bind() and connect(). */
    ret = open_bound_socket(ep->fd, &ep->addr);
    if (ret) {
        goto err1;
    }

    /* Don't select() on ep->fd for incoming packets, that will be received in
     * kernel space. */

    if (connect(ep->fd, (const struct sockaddr *)&remote_addr,
                sizeof(remote_addr))) {
        PE("Failed to connect to remote addr\n");
        goto err2;
    }

    list_add_tail(&ep->node, &shim->endpoints);

    return 0;

err2:
    close(ep->fd);
err1:
    free(ep);

    return -1;
}

static int
lfd_to_appl_name(struct shim_inet4 *shim, int lfd, struct rina_name *name)
{
    struct inet4_bindpoint *ep;

    list_for_each_entry(ep, &shim->bindpoints, node) {
        if (lfd == ep->fd) {
            return rina_name_from_string(ep->appl_name_s, name);
        }
    }

    return -1;
}

static void
accept_conn(struct rlite_evloop *loop, int lfd)
{
    struct rlite_appl *application = container_of(loop, struct rlite_appl,
                                                   loop);
    struct uipcp *uipcp = container_of(application, struct uipcp, appl);
    struct shim_inet4 *shim = SHIM(uipcp);
    struct sockaddr_in remote_addr;
    socklen_t addrlen = sizeof(remote_addr);
    struct rina_name remote_appl, local_appl;
    struct inet4_endpoint *ep;
    int sfd;

    /* First of all let's call accept, so that we consume the event
     * on lfd, independently of what happen next. This is important
     * in order to avoid spinning on this fd. */
    sfd = accept(lfd, (struct sockaddr *)&remote_addr, &addrlen);
    if (sfd < 0) {
        PE("Accept failed\n");
        return;
    }

    /* Lookup the local registered application that is listening on lfd. */
    if (lfd_to_appl_name(shim, lfd, &local_appl)) {
        PE("Cannot find the local application corresponding "
           "to fd %d\n", lfd);
        return;
    }

    ep = malloc(sizeof(*ep));
    if (!ep) {
        PE("Out of memory\n");
        return;
    }

    ep->fd = sfd;
    memcpy(&ep->addr, &remote_addr, sizeof(remote_addr));

    /* Lookup the remote IP address and port. */
    if (sock_addr_to_appl_name(&ep->addr, &remote_appl)) {
        PE("Failed to get appl_name from remote address\n");
        goto err1;
    }

    list_add_tail(&ep->node, &shim->endpoints);

    uipcp_issue_fa_req_arrived(uipcp, 0, 0,
                               &local_appl, &remote_appl, NULL);
    return;

err1:
    free(ep);
}

static int
shim_inet4_fa_req_arrived(struct rlite_evloop *loop,
                      const struct rina_msg_base_resp *b_resp,
                      const struct rina_msg_base *b_req)
{
    struct rlite_appl *application = container_of(loop, struct rlite_appl,
                                                   loop);
    struct uipcp *uipcp = container_of(application, struct uipcp, appl);
    struct rina_kmsg_fa_req_arrived *req =
                    (struct rina_kmsg_fa_req_arrived *)b_resp;
    assert(b_req == NULL);

    PD("flow request arrived: [ipcp_id = %u, data_port_id = %u]\n",
            req->ipcp_id, req->port_id);

    (void)uipcp;

    return 0;
}

static int
shim_inet4_fa_resp(struct rlite_evloop *loop,
              const struct rina_msg_base_resp *b_resp,
              const struct rina_msg_base *b_req)
{
    struct rlite_appl *application = container_of(loop, struct rlite_appl,
                                                   loop);
    struct uipcp *uipcp = container_of(application, struct uipcp, appl);
    struct rina_kmsg_fa_resp *resp =
                (struct rina_kmsg_fa_resp *)b_resp;

    PD("[uipcp %u] Got reflected message\n", uipcp->ipcp_id);

    assert(b_req == NULL);

    (void)resp;

    return 0;
}

static int
shim_inet4_flow_deallocated(struct rlite_evloop *loop,
                       const struct rina_msg_base_resp *b_resp,
                       const struct rina_msg_base *b_req)
{
    struct rlite_appl *application = container_of(loop, struct rlite_appl,
                                                   loop);
    struct uipcp *uipcp = container_of(application, struct uipcp, appl);
    struct rina_kmsg_flow_deallocated *req =
                (struct rina_kmsg_flow_deallocated *)b_resp;

    (void)req;
    (void)uipcp;

    return 0;
}

static int
shim_inet4_init(struct uipcp *uipcp)
{
    struct shim_inet4 *shim = malloc(sizeof(*shim));

    if (!shim) {
        PE("Out of memory\n");
        return -1;
    }

    uipcp->priv = shim;

    list_init(&shim->endpoints);
    list_init(&shim->bindpoints);

    return 0;
}

static int
shim_inet4_fini(struct uipcp *uipcp)
{
    struct shim_inet4 *shim = SHIM(uipcp);
    struct list_head *elem;

    {
        struct inet4_bindpoint *bp;

        while ((elem = list_pop_front(&shim->bindpoints))) {
            bp = container_of(elem, struct inet4_bindpoint, node);
            close(bp->fd);
            free(bp->appl_name_s);
            free(bp);
        }
    }

    {
        struct inet4_endpoint *ep;

        while ((elem = list_pop_front(&shim->endpoints))) {
            ep = container_of(elem, struct inet4_endpoint, node);
            close(ep->fd);
            free(ep);
        }
    }

    return 0;
}

struct uipcp_ops shim_inet4_ops = {
    .init = shim_inet4_init,
    .fini = shim_inet4_fini,
    .appl_register = shim_inet4_appl_register,
    .fa_req = shim_inet4_fa_req,
    .fa_req_arrived = shim_inet4_fa_req_arrived,
    .fa_resp = shim_inet4_fa_resp,
    .flow_deallocated = shim_inet4_flow_deallocated,
};

