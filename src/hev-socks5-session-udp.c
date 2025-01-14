/*
 ============================================================================
 Name        : hev-socks5-session-udp.c
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2017 - 2021 hev
 Description : Socks5 Session UDP
 ============================================================================
 */

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <hev-task.h>
#include <hev-task-io.h>
#include <hev-task-io-socket.h>
#include <hev-memory-allocator.h>
#include <hev-socks5-udp.h>
#include <hev-socks5-misc.h>
#include <hev-socks5-client-udp.h>

#include "hev-logger.h"
#include "hev-compiler.h"
#include "hev-config-const.h"
#include "hev-tsocks-cache.h"

#include "hev-socks5-session-udp.h"

#define task_io_yielder hev_socks5_task_io_yielder

typedef struct _HevSocks5UDPFrame HevSocks5UDPFrame;

struct _HevSocks5UDPFrame
{
    HevListNode node;
    struct sockaddr_in6 addr;
    void *data;
    size_t len;
};

static int
hev_socks5_session_udp_fwd_f (HevSocks5SessionUDP *self)
{
    HevSocks5UDPFrame *frame;
    struct sockaddr *addr;
    HevListNode *node;
    HevSocks5UDP *udp;
    int res;

    LOG_D ("%p socks5 session udp fwd f", self);

    node = hev_list_first (&self->frame_list);
    if (!node)
        return 0;

    frame = container_of (node, HevSocks5UDPFrame, node);
    udp = HEV_SOCKS5_UDP (self->base.client);
    addr = (struct sockaddr *)&frame->addr;

    res = hev_socks5_udp_sendto (udp, frame->data, frame->len, addr);
    if (res <= 0) {
        LOG_E ("%p socks5 session udp fwd f send", self);
        res = -1;
    }

    hev_list_del (&self->frame_list, node);
    hev_free (frame->data);
    hev_free (frame);
    self->frames--;

    return res;
}

static int
hev_socks5_session_udp_fwd_b (HevSocks5SessionUDP *self)
{
    struct sockaddr_in6 addr = { 0 };
    struct sockaddr *saddr;
    struct sockaddr *daddr;
    HevSocks5UDP *udp;
    uint8_t buf[UDP_BUF_SIZE];
    int res;
    int fd;

    LOG_D ("%p socks5 session udp fwd b", self);

    udp = HEV_SOCKS5_UDP (self->base.client);
    fd = HEV_SOCKS5 (udp)->fd;
    if (fd < 0) {
        LOG_E ("%p socks5 session udp fd", self);
        return -1;
    }

    res = recv (fd, buf, 1, MSG_PEEK);
    if (res <= 0) {
        if ((res < 0) && (errno == EAGAIN))
            return 0;
        LOG_E ("%p socks5 session udp fwd b peek", self);
        return -1;
    }

    addr.sin6_family = AF_INET6;
    saddr = (struct sockaddr *)&addr;
    daddr = (struct sockaddr *)&self->addr;

    res = hev_socks5_udp_recvfrom (udp, buf, sizeof (buf), saddr);
    if (res <= 0) {
        LOG_E ("%p socks5 session udp fwd b recv", self);
        return -1;
    }

    fd = hev_tsocks_cache_get (saddr);
    if (fd < 0) {
        LOG_E ("%p socks5 session udp tsocks get", self);
        return -1;
    }

    res = sendto (fd, buf, res, 0, daddr, sizeof (self->addr));
    if (res <= 0) {
        if ((res < 0) && (errno == EAGAIN))
            return 0;
        LOG_E ("%p socks5 session udp fwd b send", self);
        return -1;
    }

    return 1;
}

static void
hev_socks5_session_udp_splice (HevSocks5Session *base)
{
    HevSocks5SessionUDP *self = HEV_SOCKS5_SESSION_UDP (base);
    int res_f = 1;
    int res_b = 1;

    LOG_D ("%p socks5 session udp splice", self);

    for (;;) {
        HevTaskYieldType type;

        if (res_f >= 0)
            res_f = hev_socks5_session_udp_fwd_f (self);
        if (res_b >= 0)
            res_b = hev_socks5_session_udp_fwd_b (self);

        if (res_f < 0 || res_b < 0)
            break;
        else if (res_f > 0 || res_b > 0)
            type = HEV_TASK_YIELD;
        else
            type = HEV_TASK_WAITIO;

        if (task_io_yielder (type, base->client) < 0)
            break;
    }
}

static HevSocks5SessionUDPClass _klass = {
    {
        .name = "HevSoscks5SessionUDP",
        .splicer = hev_socks5_session_udp_splice,
        .finalizer = hev_socks5_session_udp_destruct,
    },
};

int
hev_socks5_session_udp_construct (HevSocks5SessionUDP *self)
{
    int res;

    res = hev_socks5_session_construct (&self->base);
    if (res < 0)
        return -1;

    LOG_D ("%p socks5 session udp construct", self);

    HEV_SOCKS5_SESSION (self)->klass = HEV_SOCKS5_SESSION_CLASS (&_klass);

    return 0;
}

void
hev_socks5_session_udp_destruct (HevSocks5Session *base)
{
    HevSocks5SessionUDP *self = HEV_SOCKS5_SESSION_UDP (base);
    HevListNode *node;

    LOG_D ("%p socks5 session udp destruct", self);

    node = hev_list_first (&self->frame_list);
    while (node) {
        HevSocks5UDPFrame *frame;

        frame = container_of (node, HevSocks5UDPFrame, node);
        node = hev_list_node_next (node);
        hev_free (frame->data);
        hev_free (frame);
    }

    hev_socks5_session_destruct (base);
}

HevSocks5SessionUDP *
hev_socks5_session_udp_new (struct sockaddr *addr)
{
    HevSocks5SessionUDP *self = NULL;
    HevSocks5ClientUDP *udp;
    int res;

    self = hev_malloc0 (sizeof (HevSocks5SessionUDP));
    if (!self)
        return NULL;

    LOG_D ("%p socks5 session udp new", self);

    res = hev_socks5_session_udp_construct (self);
    if (res < 0) {
        hev_free (self);
        return NULL;
    }

    udp = hev_socks5_client_udp_new ();
    if (!udp) {
        hev_free (self);
        return NULL;
    }

    self->base.client = HEV_SOCKS5_CLIENT (udp);
    memcpy (&self->addr, addr, sizeof (struct sockaddr_in6));

    return self;
}

int
hev_socks5_session_udp_send (HevSocks5SessionUDP *self, void *data, size_t len,
                             struct sockaddr *addr)
{
    HevSocks5UDPFrame *frame;

    LOG_D ("%p socks5 session udp send", self);

    if (self->frames > UDP_POOL_SIZE)
        return -1;

    frame = hev_malloc (sizeof (HevSocks5UDPFrame));
    if (!frame)
        return -1;

    frame->len = len;
    frame->data = data;
    memset (&frame->node, 0, sizeof (frame->node));
    memcpy (&frame->addr, addr, sizeof (struct sockaddr_in6));

    self->frames++;
    hev_list_add_tail (&self->frame_list, &frame->node);
    hev_task_wakeup (HEV_SOCKS5_SESSION (self)->task);

    return 0;
}
