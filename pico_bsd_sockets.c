/*********************************************************************
PicoTCP. Copyright (c) 2013 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.
Do not redistribute without a written permission by the Copyright
holders.

Author: Maxime Vincent, Daniele Lacamera
*********************************************************************/

#include "pico_defines.h"
#include "pico_config.h"    /* for zalloc and free */
#include "pico_bsd_sockets.h"
#include "pico_osal.h"
#include "pico_sntp_client.h"

#include <errno.h> /* should be there in C99 */

#define SOCK_OPEN                   0
#define SOCK_BOUND                  1
#define SOCK_LISTEN                 2
#define SOCK_CONNECTED              3
#define SOCK_ERROR                  4
#define SOCK_RESET_BY_PEER          5
#define SOCK_CLOSED                 100

#define bsd_dbg(...)                do {} while(0)
#define bsd_dbg_select(...)         do {} while(0)

/* Global signal sent on any event (for select) */
void * global_signal;

struct pico_bsd_endpoint {
  struct   pico_socket *s;
  int      socket_fd;
  int      posix_fd;        /* TODO: ifdef... */
  uint8_t  in_use;
  uint8_t  state;           /* for pico_state */
  uint8_t  nonblocking;     /* The non-blocking flag, for non-blocking socket operations */
  uint16_t events;          /* events that we filter for */
  uint16_t revents;         /* received events */
  uint16_t proto;
  void *   mutex_lock;      /* mutex for clearing revents */
  void *   signal;          /* signals new events */
  uint32_t timeout;         /* this is used for timeout sockets */
  int      error;           /* used for SO_ERROR sockopt after connect() */
};

/* MACRO's */
#define VALIDATE_NULL(param) \
    if(!param) \
    { \
        return -1; \
    }

#define VALIDATE_ONE(param,value) \
    if(param != value) { \
        pico_err = PICO_ERR_EINVAL; \
        errno = pico_err; \
        return -1; \
    }

#define VALIDATE_TWO(param,value1,value2) \
    if(param != value1 && param != value2) { \
        pico_err = PICO_ERR_EINVAL; \
        errno = pico_err; \
        return -1; \
    }


/* Private function prototypes */
static void pico_event_clear(struct pico_bsd_endpoint *ep, uint16_t events);
static uint16_t pico_bsd_wait(struct pico_bsd_endpoint * ep, int read, int write, int close);
static void pico_socket_event(uint16_t ev, struct pico_socket *s);

/* pico stack lock */
void * picoLock = NULL;

/************************/
/* Public API functions */
/************************/
void pico_bsd_init(void)
{
    global_signal = pico_signal_init();
    picoLock = pico_mutex_init();
}

void pico_bsd_deinit(void)
{
    pico_mutex_deinit(picoLock);
}

void pico_bsd_stack_tick(void)
{
    pico_mutex_lock(picoLock);
    pico_stack_tick();
    pico_stack_tick();
    pico_mutex_unlock(picoLock);
}

/** Declarations of helper functions **/
static struct pico_bsd_endpoint *pico_bsd_create_socket(void);
static int get_free_sd(struct pico_bsd_endpoint *ep);
static int new_sd(struct pico_bsd_endpoint *ep);
static void free_up_ep(struct pico_bsd_endpoint *ep);
static struct pico_bsd_endpoint *get_endpoint(int sd);
static int bsd_to_pico_addr(union pico_address *addr, struct sockaddr *_saddr, socklen_t socklen);
static uint16_t bsd_to_pico_port(struct sockaddr *_saddr, socklen_t socklen);
static int pico_addr_to_bsd(struct sockaddr *_saddr, socklen_t socklen, union pico_address *addr, uint16_t net);
static int pico_port_to_bsd(struct sockaddr *_saddr, socklen_t socklen, uint16_t port);

/** Global Sockets descriptors array **/
static struct pico_bsd_endpoint **PicoSockets       = NULL;
static int                        PicoSocket_max    = 0;

/*** Public socket functions ***/

/* Socket interface. */
int pico_newsocket(int domain, int type, int proto)
{
    struct pico_bsd_endpoint * ep = NULL;
    (void)proto;

#ifdef PICO_SUPPORT_IPV6
    VALIDATE_TWO(domain,AF_INET, AF_INET6);
#else
    VALIDATE_ONE(domain, AF_INET);
#endif
    VALIDATE_TWO(type,SOCK_STREAM,SOCK_DGRAM);

    if (AF_INET6 != PICO_PROTO_IPV6) {
        if (domain == AF_INET6)
            domain = PICO_PROTO_IPV6;
        else
            domain = PICO_PROTO_IPV4;
    }

    if (SOCK_STREAM != PICO_PROTO_TCP) {
        if (type == SOCK_STREAM)
            type = PICO_PROTO_TCP;
        else
            type = PICO_PROTO_UDP;
    }

    pico_mutex_lock(picoLock);
    ep = pico_bsd_create_socket();
    VALIDATE_NULL(ep);

    ep->proto = type;

    ep->s = pico_socket_open(domain, type,&pico_socket_event);
    if (!ep->s)
    {
        pico_free(ep);
        pico_mutex_unlock(picoLock);
        return -1;
    }

    ep->s->priv = ep; /* let priv point to the endpoint struct */

    /* open picotcp endpoint */
    ep->state = SOCK_OPEN;
    ep->mutex_lock = pico_mutex_init();
    ep->signal = pico_signal_init();
    ep->error = pico_err;
    pico_mutex_unlock(picoLock);
    return ep->socket_fd;
}


int pico_bind(int sd, struct sockaddr * local_addr, socklen_t socklen)
{ 
    union pico_address addr = { .ip4 = { 0 } };
    uint16_t port;
    struct pico_bsd_endpoint *ep = get_endpoint(sd);

    VALIDATE_NULL(ep);
    VALIDATE_NULL(local_addr);
    VALIDATE_TWO(socklen, SOCKSIZE, SOCKSIZE6);

    if (bsd_to_pico_addr(&addr, local_addr, socklen) < 0)
    {
        ep->error = PICO_ERR_EINVAL;
        errno = pico_err;
        return -1;
    }
    port = bsd_to_pico_port(local_addr, socklen);

    pico_mutex_lock(picoLock);
    if(pico_socket_bind(ep->s, &addr, &port) < 0)
    {
        ep->error = pico_err;
        errno = pico_err;
        pico_mutex_unlock(picoLock);
        return -1;
    }

    ep->state = SOCK_BOUND;
    pico_mutex_unlock(picoLock);

    return 0;
}

int pico_getsockname(int sd, struct sockaddr * local_addr, socklen_t *socklen)
{ 
    union pico_address addr;
    uint16_t port, proto;
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    VALIDATE_NULL(ep);
    VALIDATE_NULL(local_addr);
    VALIDATE_NULL(socklen);
    pico_mutex_lock(picoLock);
    if(pico_socket_getname(ep->s, &addr, &port, &proto) < 0)
    {
        ep->error = pico_err;
        errno = pico_err;
        pico_mutex_unlock(picoLock);
        return -1;
    }

    if (proto == PICO_PROTO_IPV6)
        *socklen = SOCKSIZE6;
    else
        *socklen = SOCKSIZE;

    if (pico_addr_to_bsd(local_addr, *socklen, &addr, proto) < 0) {
        ep->error = pico_err;
        errno = pico_err;
        pico_mutex_unlock(picoLock);
        return -1;
    }
    pico_mutex_unlock(picoLock);
    pico_port_to_bsd(local_addr, *socklen, port);
    ep->error = pico_err;
    return 0;
}

int pico_getpeername(int sd, struct sockaddr * remote_addr, socklen_t *socklen)
{ 
    union pico_address addr;
    uint16_t port, proto;
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    VALIDATE_NULL(ep);
    VALIDATE_NULL(remote_addr);
    VALIDATE_NULL(socklen);
    pico_mutex_lock(picoLock);
    if(pico_socket_getpeername(ep->s, &addr, &port, &proto) < 0)
    {
        pico_mutex_unlock(picoLock);
        return -1;
    }

    if (proto == PICO_PROTO_IPV6)
        *socklen = SOCKSIZE6;
    else
        *socklen = SOCKSIZE;

    if (pico_addr_to_bsd(remote_addr, *socklen, &addr, proto) < 0) {
        pico_mutex_unlock(picoLock);
        return -1;
    }
    pico_mutex_unlock(picoLock);
    pico_port_to_bsd(remote_addr, *socklen, port);
    return 0;
}


int pico_listen(int sd, int backlog)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sd);

    VALIDATE_NULL(ep);
    VALIDATE_NULL(ep->s);
    VALIDATE_ONE(ep->state, SOCK_BOUND);

    pico_mutex_lock(picoLock);

    if(pico_socket_listen(ep->s, backlog) < 0)
    {
        ep->error = pico_err;
        errno = pico_err;
        pico_mutex_unlock(picoLock);
        return -1;
    }
    ep->state = SOCK_LISTEN;

    ep->error = pico_err;
    pico_mutex_unlock(picoLock);
    return 0;
}

int pico_connect(int sd, struct sockaddr *_saddr, socklen_t socklen)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    union pico_address addr;
    uint16_t port;
    uint16_t ev;

    VALIDATE_NULL(ep);
    VALIDATE_NULL(_saddr);
    if (bsd_to_pico_addr(&addr, _saddr, socklen) < 0)
    {
        ep->error = PICO_ERR_EINVAL;
        errno = pico_err;
        return -1;
    }
    port = bsd_to_pico_port(_saddr, socklen);
    pico_mutex_lock(picoLock);
    pico_socket_connect(ep->s, &addr, port);
    pico_mutex_unlock(picoLock);

    if (ep->nonblocking) {
        pico_err = PICO_ERR_EBUSY; /* should be EINPROGRESS */
        ep->error = pico_err;
    } else {
        /* wait for event */
        ev = pico_bsd_wait(ep, 0, 0, 0); /* wait for ERR, FIN and CONN */
    }

    if(ev & PICO_SOCK_EV_CONN)
    {
        /* clear the EV_CONN event */
        pico_event_clear(ep, PICO_SOCK_EV_CONN);
        ep->error = pico_err;
        return 0;
    } else {
        if (!(ep->nonblocking))
            pico_socket_close(ep->s);
    }
    ep->error = pico_err;
    errno = pico_err;
    return -1;
}

int pico_accept(int sd, struct sockaddr *_orig, socklen_t *socklen)
{
    struct pico_bsd_endpoint *ep, * client_ep = NULL;
    uint16_t events;
    union pico_address picoaddr;
    uint16_t port;

    ep = get_endpoint(sd);

    VALIDATE_NULL(ep);
    VALIDATE_ONE(ep->state, SOCK_LISTEN);

    if (ep->nonblocking)
        events = PICO_SOCK_EV_CONN;
    else 
        events = pico_bsd_wait(ep, 0, 0, 0); /* Wait for CONN, FIN and ERR */

    if(events & PICO_SOCK_EV_CONN)
    {
        struct pico_socket *s;
        pico_mutex_lock(picoLock);
        s = pico_socket_accept(ep->s,&picoaddr,&port);
        if (!s)
        {
            ep->error = pico_err;
            errno = pico_err;
            pico_mutex_unlock(picoLock);
            return -1;
        }

        /* Create a new client EP, only after the accept returned succesfully */
        client_ep = pico_bsd_create_socket();
        if (!client_ep)
        {
            ep->error = pico_err;
            errno = pico_err;
            pico_mutex_unlock(picoLock);
            return -1;
        }
        client_ep->s = s;
        client_ep->state = SOCK_OPEN;
        client_ep->mutex_lock = pico_mutex_init();
        client_ep->signal = pico_signal_init();

        client_ep->s->priv = client_ep;
        pico_event_clear(ep, PICO_SOCK_EV_CONN); /* clear the CONN event the listening socket */
        if (client_ep->s->net->proto_number == PICO_PROTO_IPV4)
            *socklen = SOCKSIZE;
        else
            *socklen = SOCKSIZE6;
        client_ep->state = SOCK_CONNECTED;
        if (pico_addr_to_bsd(_orig, *socklen, &picoaddr, client_ep->s->net->proto_number) < 0) {
            free_up_ep(client_ep);
            pico_mutex_unlock(picoLock);
            return -1;
        }
        pico_port_to_bsd(_orig, *socklen, port);

        client_ep->in_use = 1;
        pico_mutex_unlock(picoLock);
        ep->error = pico_err;
        return client_ep->socket_fd;
    }
    free_up_ep(client_ep);
    ep->error = pico_err;
    errno = pico_err;
    return -1;
}

int pico_sendto(int sd, void * buf, int len, int flags, struct sockaddr *_dst, socklen_t socklen)
{
    int retval = 0;
    int tot_len = 0;
    uint16_t port;
    union pico_address picoaddr;
    struct pico_bsd_endpoint *ep = get_endpoint(sd);

    VALIDATE_NULL(ep);

    if (!buf || (len <= 0)) {
        pico_err = PICO_ERR_EINVAL;
        errno = pico_err;
        ep->error = pico_err;
        return -1;
    }

    while (tot_len < len) {
        /* Write to the pico socket */
        pico_mutex_lock(picoLock);
        if (_dst == NULL) {
            retval = pico_socket_send(ep->s, ((uint8_t *)buf) + tot_len, len - tot_len);
        } else {
            if (bsd_to_pico_addr(&picoaddr, _dst, socklen) < 0) {
                ep->error = PICO_ERR_EINVAL;
                errno = pico_err;
                pico_mutex_unlock(picoLock);
                return -1;
            }
            port = bsd_to_pico_port(_dst, socklen);
            retval = pico_socket_sendto(ep->s, ((uint8_t *)buf) + tot_len, len - tot_len, &picoaddr, port);
        }
        pico_mutex_unlock(picoLock);

        /* If sending failed, return an error */
        if (retval < 0)
        {
            ep->error = pico_err;
            errno = pico_err;
            pico_event_clear(ep, PICO_SOCK_EV_WR);
            return -1;
        }

        /* No, error so clear the revent */
        //pico_event_clear(ep, PICO_SOCK_EV_WR);

        if (retval > 0)
        {
            tot_len += retval;
            break;
        }

        if (ep->nonblocking)
            break;

        /* If sent bytes (retval) < len-tot_len: socket full, we need to wait for a new WR event */
        if (retval < (len - tot_len))
        {
            uint16_t ev = 0;
            /* wait for a new WR or CLOSE event */
            ev = pico_bsd_wait(ep, 0, 1, 1);

            if (ev & (PICO_SOCK_EV_ERR | PICO_SOCK_EV_FIN | PICO_SOCK_EV_CLOSE))
            {
                ep->error = pico_err;
                errno = pico_err;
                pico_event_clear(ep, PICO_SOCK_EV_WR);
                /* closing and freeing the socket is done in the event handler */
                return -1;
            }
        }
        tot_len += retval;
    }
    ep->error = pico_err;
    return tot_len;
}

int pico_fcntl(int sd, int cmd, int arg)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    if (!ep) {
        pico_err = PICO_ERR_EINVAL;
        errno = pico_err;
        return -1;
    }

    if (cmd == F_SETFL) {
        if ((arg & O_NONBLOCK) != 0) {
            ep->nonblocking = 1;
        } else {
            ep->nonblocking = 0;
        }
        ep->error = PICO_ERR_NOERR;
        return 0;
    }

    if (cmd == F_GETFL) {
        (void)arg; /* F_GETFL: arg is ignored */
        ep->error = PICO_ERR_NOERR;
        if (ep->nonblocking)
            return O_NONBLOCK;
        else
            return 0;
    }
    pico_err = PICO_ERR_EINVAL;
    errno = pico_err;
    ep->error = pico_err;
    return -1;
}

/* 
 * RETURN VALUE
 *   Upon  successful completion, recv_from() shall return the length of the
 *   message in bytes. If no messages are available to be received and the 
 *   peer has performed an orderly shutdown, recv() shall return 0. Otherwise,
 *   −1 shall be returned and errno set to indicate the error.
 */
int pico_recvfrom(int sd, void * _buf, int len, int flags, struct sockaddr *_addr, socklen_t *socklen)
{
    int retval = 0;
    int tot_len = 0;
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    union pico_address picoaddr;
    uint16_t port;
    unsigned char *buf = (unsigned char *)_buf;
    bsd_dbg("Recvfrom called \n");

    VALIDATE_NULL(ep);

    if (!buf || (len <= 0)) {
        pico_err = PICO_ERR_EINVAL;
        errno = pico_err;
        ep->error = pico_err;
        return -1;
    }

    while (tot_len < len) {
        pico_mutex_lock(picoLock);
        retval = pico_socket_recvfrom(ep->s, buf + tot_len ,  len - tot_len, &picoaddr, &port);
        pico_mutex_unlock(picoLock);
        bsd_dbg("pico_socket_recvfrom returns %d, first bytes are %c-%c-%c-%c\n", retval, buf[0], buf[1], buf[2], buf[3]);

        /* pico_socket_recvfrom failed */
        if (retval < 0) {
            ep->error = pico_err;
            return 0; /* socket error or closed */
        }

        /* If received 0 bytes, return -1 or amount of bytes received */
        if (retval == 0) {
            pico_event_clear(ep, PICO_SOCK_EV_RD);
            if (tot_len > 0) {
                bsd_dbg("Recvfrom returning %d\n", tot_len);
                ep->error = pico_err;
                return tot_len;
            }
        }

        if (retval > 0) {
            if (ep->proto == PICO_PROTO_UDP) {
                if (_addr && socklen > 0)
                {
                    if (pico_addr_to_bsd(_addr, *socklen, &picoaddr, ep->s->net->proto_number) < 0) {
                        pico_err = PICO_ERR_EINVAL;
                        errno = pico_err;
                        ep->error = pico_err;
                        return -1;
                    }
                    pico_port_to_bsd(_addr, *socklen, port);
                }
                /* If in a recvfrom call, for UDP we should return immediately after the first dgram */
                ep->error = pico_err;
                return retval + tot_len; 
            } else {
                /* TCP: continue until recvfrom = 0, socket buffer empty */
                tot_len += retval;
                continue;
            }
        }

        if (ep->nonblocking)
        {
            if (retval == 0)
            {
                pico_err = PICO_ERR_EAGAIN; /* or EWOULDBLOCK */
                errno = pico_err;
                retval = -1; /* BSD-speak: -1 == 0 bytes received */
            }
            break;
        }

        /* If recv bytes (retval) < len-tot_len: socket empty, we need to wait for a new RD event */
        if (retval < (len - tot_len))
        {
            uint16_t ev = 0;
            /* wait for a new RD event -- also wait for CLOSE event */
            ev = pico_bsd_wait(ep, 1, 0, 1); 
            if (ev & (PICO_SOCK_EV_ERR | PICO_SOCK_EV_FIN | PICO_SOCK_EV_CLOSE))
            {
                /* closing and freeing the socket is done in the event handler */
                pico_event_clear(ep, PICO_SOCK_EV_RD);
                ep->error = pico_err;
                return 0; /* return 0 on a properly closed socket */
            }
        }
        tot_len += retval;
    }
    pico_event_clear(ep, PICO_SOCK_EV_RD); // What if still space available and we clear it here??
    bsd_dbg("Recvfrom returning %d (full block)\n", tot_len);
    ep->error = pico_err;

    if (tot_len == 0)
    {
        errno = pico_err;
        tot_len = -1; /* BSD-speak: -1 == 0 bytes received */
    }
    return tot_len;
}

int pico_write(int sd, void * buf, int len)
{
    return pico_sendto(sd, buf, len, 0, NULL, 0);
}

int pico_send(int sd, void * buf, int len, int flags)
{
    return pico_sendto(sd, buf, len, flags, NULL, 0);
}

int pico_read(int sd, void * buf, int len)
{
    return pico_recvfrom(sd, buf, len, 0, NULL, 0);
}

int pico_recv(int sd, void * buf, int len, int flags)
{
    return pico_recvfrom(sd, buf, len, flags, NULL, 0);
}



int pico_close(int sd)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    VALIDATE_NULL(ep);

    if(ep->s) /* valid socket, try to close it */
    {
        pico_mutex_lock(picoLock);
        pico_socket_close(ep->s);
        ep->s->priv = NULL;
        pico_mutex_unlock(picoLock);
    }
    ep->in_use = 0;
    ep->error = pico_err;
    return 0;
}

int pico_shutdown(int sd, int how)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sd);
    VALIDATE_NULL(ep);

    if(ep->s) /* valid socket, try to close it */
    {
        pico_mutex_lock(picoLock);
        pico_socket_shutdown(ep->s, how);
        ep->error = pico_err;
        pico_mutex_unlock(picoLock);
    } else {
        ep->error = PICO_ERR_EINVAL;
        errno = pico_err;
    }
    return 0;
}

/*** Helper functions ***/
static int bsd_to_pico_addr(union pico_address *addr, struct sockaddr *_saddr, socklen_t socklen)
{
    VALIDATE_TWO(socklen, SOCKSIZE, SOCKSIZE6);
    if (socklen == SOCKSIZE6) {
        struct sockaddr_in6 *saddr = (struct sockaddr_in6 *)_saddr;
        memcpy(&addr->ip6.addr, &saddr->sin6_addr.s6_addr, 16);
        saddr->sin6_family = AF_INET6;
    } else {
        struct sockaddr_in *saddr = (struct sockaddr_in *)_saddr;
        addr->ip4.addr = saddr->sin_addr.s_addr;
        saddr->sin_family = AF_INET;
    }
    return 0;
}

static uint16_t bsd_to_pico_port(struct sockaddr *_saddr, socklen_t socklen)
{
    VALIDATE_TWO(socklen, SOCKSIZE, SOCKSIZE6);
    if (socklen == SOCKSIZE6) {
        struct sockaddr_in6 *saddr = (struct sockaddr_in6 *)_saddr;
        return saddr->sin6_port;
    } else {
        struct sockaddr_in *saddr = (struct sockaddr_in *)_saddr;
        return saddr->sin_port;
    }
}

static int pico_port_to_bsd(struct sockaddr *_saddr, socklen_t socklen, uint16_t port)
{
    VALIDATE_TWO(socklen, SOCKSIZE, SOCKSIZE6);
    if (socklen == SOCKSIZE6) {
        struct sockaddr_in6 *saddr = (struct sockaddr_in6 *)_saddr;
        saddr->sin6_port = port;
        return 0;
    } else {
        struct sockaddr_in *saddr = (struct sockaddr_in *)_saddr;
        saddr->sin_port = port;
        return 0;
    }
    pico_err = PICO_ERR_EINVAL;
    errno = pico_err;
    return -1;
}

static int pico_addr_to_bsd(struct sockaddr *_saddr, socklen_t socklen, union pico_address *addr, uint16_t net)
{
    VALIDATE_TWO(socklen, SOCKSIZE, SOCKSIZE6);
    if ((socklen == SOCKSIZE6) && (net == PICO_PROTO_IPV6)) {
        struct sockaddr_in6 *saddr = (struct sockaddr_in6 *)_saddr;
        memcpy(&saddr->sin6_addr.s6_addr, &addr->ip6.addr, 16);
        saddr->sin6_family = AF_INET6;
    } else if ((socklen == SOCKSIZE) && (net == PICO_PROTO_IPV4)) {
        struct sockaddr_in *saddr = (struct sockaddr_in *)_saddr;
        saddr->sin_addr.s_addr = addr->ip4.addr;
        saddr->sin_family = AF_INET;
    }
    return 0;
}

static void free_up_ep(struct pico_bsd_endpoint *ep)
{
    if (ep->signal)
        pico_signal_deinit(ep->signal);
    if (ep->mutex_lock)
        pico_mutex_deinit(ep->mutex_lock);
    pico_free(ep);
}

static int get_free_sd(struct pico_bsd_endpoint *ep)
{
    int i;
    for (i = 0; i < PicoSocket_max; i++) {
        if (!PicoSockets[i]->in_use) {
            free_up_ep(PicoSockets[i]);
            PicoSockets[i] = ep;
            return i;
        }
    }
    return -1;
}

/* DLA TODO: make a GC for freeing up the last socket descriptor periodically if not in use */

static int new_sd(struct pico_bsd_endpoint *ep)
{
    int sd = PicoSocket_max;
    struct pico_bsd_endpoint **new;
    new = pico_zalloc(sizeof(void *) * ++PicoSocket_max);
    if (!new) {
        PicoSocket_max--;
        pico_err = PICO_ERR_ENOMEM;
        errno = pico_err;
        return -1;
    }
    if (sd > 0) {
        memcpy(new, PicoSockets, sd * sizeof(void *));
        pico_free(PicoSockets);
    }
    PicoSockets = new;
    new[sd] = ep;
    return sd;
}

/* picoLock must be taken already ! */
static struct pico_bsd_endpoint *pico_bsd_create_socket(void)
{
    struct pico_bsd_endpoint *ep = pico_zalloc(sizeof(struct pico_bsd_endpoint));
    if (!ep) {
        pico_err = PICO_ERR_ENOMEM;
        errno = pico_err;
    }
    ep->in_use = 1;
    ep->socket_fd = get_free_sd(ep);
    if (ep->socket_fd < 0) {
        ep->socket_fd = new_sd(ep);
    }
    return ep;
}

static struct pico_bsd_endpoint *get_endpoint(int sd)
{
    if ((sd > PicoSocket_max) || (sd < 0) || 
         (PicoSockets[sd]->in_use == 0)) {
        pico_err = PICO_ERR_EINVAL;
        errno = pico_err;
        return NULL;
    }
    return PicoSockets[sd];
}

/* wait for one of the selected events, return any of those that occurred */
uint16_t pico_bsd_select(struct pico_bsd_endpoint *ep)
{
    uint16_t events = ep->events & ep->revents; /* maybe an event we are waiting for, was already queued ? */
    /* wait for one of the selected events... */
    while (!events)
    {
        pico_signal_wait(ep->signal);
        events = (ep->revents & ep->events); /* filter for the events we were waiting for */
    }
    /* the event we were waiting for happened, now report it */
    return events; /* return any event(s) that occurred, that we were waiting for */
}


/****************************/
/* Private helper functions */
/****************************/
static uint16_t pico_bsd_wait(struct pico_bsd_endpoint * ep, int read, int write, int close)
{
  pico_mutex_lock(ep->mutex_lock);

  ep->events = PICO_SOCK_EV_ERR;
  ep->events |= PICO_SOCK_EV_FIN;
  ep->events |= PICO_SOCK_EV_CONN;
  if (close)
      ep->events |= PICO_SOCK_EV_CLOSE;
  if (read) 
      ep->events |= PICO_SOCK_EV_RD;
  if (write)
      ep->events |= PICO_SOCK_EV_WR;

  pico_mutex_unlock(ep->mutex_lock);

  return pico_bsd_select(ep);
}


static void pico_event_clear(struct pico_bsd_endpoint *ep, uint16_t events)
{
    pico_mutex_lock(ep->mutex_lock);
    ep->revents &= ~events; /* clear those events */
    pico_mutex_unlock(ep->mutex_lock);
}

/* NOTE: __NO__ picoLock'ing here !! */
/* this is called from pico_stack_tick, so picoLock is already locked */
static void pico_socket_event(uint16_t ev, struct pico_socket *s)
{
    struct pico_bsd_endpoint * ep = (struct pico_bsd_endpoint *)(s->priv);
    if (!s)
        return;
    if(!ep || !ep->s || !ep->mutex_lock || !ep->signal )
    {
        if(ev & (PICO_SOCK_EV_CLOSE | PICO_SOCK_EV_FIN) )
        {
            pico_signal_send(global_signal); /* Signal this event globally (e.g. for select()) */
            pico_socket_close(s);
        }
        /* endpoint not initialized yet! */
        return;
    }

    if(ep->in_use != 1)
        return;

    pico_mutex_lock(ep->mutex_lock); /* lock over the complete body is needed,
                                        as the event might get cleared in another process.. */
    ep->revents |= ev; /* set those events */

    if(ev & PICO_SOCK_EV_CONN)
    {
        if(ep->state != SOCK_LISTEN)
        {
            ep->state  = SOCK_CONNECTED;
        }
    }

    if(ev & PICO_SOCK_EV_ERR)
    {
      if(pico_err == PICO_ERR_ECONNRESET)
      {
        bsd_dbg("Connection reset by peer...\n");
        ep->state = SOCK_RESET_BY_PEER;
      }
    }

    if (ev & PICO_SOCK_EV_CLOSE) {
        ep->state = SOCK_CLOSED;
    }

    if (ev & PICO_SOCK_EV_FIN) {
        /* DO NOT set ep->s = NULL, we might still be transmitting stuff! */
        ep->state = SOCK_CLOSED;
    }
 
    pico_signal_send(global_signal); /* Signal this event globally (e.g. for select()) */
    pico_signal_send(ep->signal);    /* Signal the endpoint that was blocking on this event */

    pico_mutex_unlock(ep->mutex_lock);
}


#define DNSQUERY_OK 1
#define DNSQUERY_FAIL 0xFF
struct dnsquery_cookie
{
    struct addrinfo **res;
    void            *signal;
    uint8_t         block;
    uint8_t        revents;
    int             cleanup;
};

static struct dnsquery_cookie *dnsquery_cookie_create(struct addrinfo **res, uint8_t block)
{
    struct dnsquery_cookie *ck = pico_zalloc(sizeof(struct dnsquery_cookie));
    if (!ck) {
        pico_err = PICO_ERR_ENOMEM;
        errno = pico_err;
        return NULL;
    }
    ck->signal = pico_signal_init();
    ck->res = res;
    ck->block = block;
    ck->cleanup = 0;
    return ck;
}

#ifdef PICO_SUPPORT_IPV6
static void dns_ip6_cb(char *ip, void *arg)
{
    struct dnsquery_cookie *ck = (struct dnsquery_cookie *)arg;
    struct addrinfo *new;
    if (ck->cleanup) { /* call is no more valid. Caller got tired of waiting. */
        pico_signal_deinit(ck->signal);
        pico_free(ck);
        return;
    }
    if (ip) {
        new = pico_zalloc(sizeof(struct addrinfo));
        if (!new) {
            ck->revents = DNSQUERY_FAIL;
            if (ck->block)
                pico_signal_send(ck->signal);
            return;
        }
        new->ai_family = AF_INET6;
        new->ai_addr = pico_zalloc(sizeof(struct sockaddr_in6));
        if (!new->ai_addr) {
            pico_free(new);
            ck->revents = DNSQUERY_FAIL;
            if (ck->block)
                pico_signal_send(ck->signal);
            return;
        }
        new->ai_addrlen = sizeof(struct sockaddr_in6);
        pico_string_to_ipv6(ip, (((struct sockaddr_in6*)(new->ai_addr))->sin6_addr.s6_addr)); 
        ((struct sockaddr_in6*)(new->ai_addr))->sin6_family = AF_INET6;
        new->ai_next = *ck->res;
        *ck->res = new;
        ck->revents = DNSQUERY_OK;
    }
    if (ck->block)
        pico_signal_send(ck->signal);
}
#endif

static void dns_ip4_cb(char *ip, void *arg)
{
    struct dnsquery_cookie *ck = (struct dnsquery_cookie *)arg;
    struct addrinfo *new;
    if (ck->cleanup) { /* call is no more valid. Caller got tired of waiting. */
        pico_signal_deinit(ck->signal);
        pico_free(ck);
        return;
    }
    if (ip) {
        new = pico_zalloc(sizeof(struct addrinfo));
        if (!new) {
            ck->revents = DNSQUERY_FAIL;
            if (ck->block)
                pico_signal_send(ck->signal);
            return;
        }
        new->ai_family = AF_INET;
        new->ai_addr = pico_zalloc(sizeof(struct sockaddr_in));
        if (!new->ai_addr) {
            pico_free(new);
            ck->revents = DNSQUERY_FAIL;
            if (ck->block)
                pico_signal_send(ck->signal);
            return;
        }
        new->ai_addrlen = sizeof(struct sockaddr_in);
        pico_string_to_ipv4(ip, &(((struct sockaddr_in*)new->ai_addr)->sin_addr.s_addr));    
        ((struct sockaddr_in*)(new->ai_addr))->sin_family = AF_INET;
        new->ai_next = *ck->res;
        *ck->res = new;
        ck->revents = DNSQUERY_OK;
    }
    if (ck->block)
        pico_signal_send(ck->signal);
}

#ifdef PICO_SUPPORT_DNS_CLIENT
int pico_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    struct dnsquery_cookie *ck4 = NULL;
    struct dnsquery_cookie *ck6 = NULL;
    struct sockaddr_in sa4;
    *res = NULL;
    (void)service;
    bsd_dbg("Called pico_getaddrinfo, looking for %s\n", node);

#ifdef PICO_SUPPORT_IPV6
    struct sockaddr_in6 sa6;
    if (pico_string_to_ipv6(node, sa6.sin6_addr.s6_addr) == 0) {
        ck6 = dnsquery_cookie_create(res, 0);
        dns_ip6_cb((char *)node, ck6);
        pico_signal_deinit(ck6->signal);
        pico_free(ck6);
        return 0;
    }
#endif

    if (pico_string_to_ipv4(node, &sa4.sin_addr.s_addr) == 0) {
        ck4 = dnsquery_cookie_create(res, 0);
        dns_ip4_cb((char*)node, ck4);
        pico_signal_deinit(ck4->signal);
        pico_free(ck4);
        return 0;
    }

#ifdef PICO_SUPPORT_IPV6
    {
        if (!hints || (hints->ai_family == AF_INET6)) {
            ck6 = dnsquery_cookie_create(res, 1);
            if (!ck6)
                return -1;
            pico_mutex_lock(picoLock);
            pico_dns_client_getaddr6(node, dns_ip6_cb, ck6);
            bsd_dbg("Resolving AAAA record %s\n", node);
            pico_mutex_unlock(picoLock);
        }
    }
#endif /* PICO_SUPPORT_IPV6 */

    if (!hints || (hints->ai_family == AF_INET)) {
        ck4 = dnsquery_cookie_create(res, 1);
        pico_mutex_lock(picoLock);
        pico_dns_client_getaddr(node, dns_ip4_cb, ck4);
        bsd_dbg("Resolving A record %s\n", node);
        pico_mutex_unlock(picoLock);
    }

    if (ck6) {
        if (pico_signal_wait_timeout(ck6->signal, 2000) == 0) {
            pico_signal_deinit(ck6->signal);
            pico_free(ck6);
        } else {
            ck6->cleanup = 1;
        }
    }

    if (ck4) {
        if (pico_signal_wait_timeout(ck4->signal, 2000) == 0) {
            pico_signal_deinit(ck4->signal);
            pico_free(ck4);
        } else {
            ck4->cleanup = 1;
        }
    }
    if (*res)
        return 0;
    return -1;
}

void pico_freeaddrinfo(struct addrinfo *res)
{
    struct addrinfo *cur = res;
    struct addrinfo *nxt;
    while(cur) {
        if (cur->ai_addr)
            pico_free(cur->ai_addr);
        nxt = cur->ai_next;
        pico_free(cur);
        cur = nxt;
    }
}

/* Legacy gethostbyname call implementation */
static struct hostent PRIV_HOSTENT = { };
struct hostent *pico_gethostbyname(const char *name)
{
    struct addrinfo *res;
    struct addrinfo hint = {.ai_family = AF_INET};
    int ret;
    if (!PRIV_HOSTENT.h_addr_list) {
        /* Done only once: reserve space for 2 entries */
        PRIV_HOSTENT.h_addr_list = pico_zalloc(2 * sizeof(void*));
        PRIV_HOSTENT.h_addr_list[1] = NULL;
    }
    ret = pico_getaddrinfo(name, NULL, &hint, &res);
    if (ret == 0) {
        if (PRIV_HOSTENT.h_name != NULL) {
            pico_free(PRIV_HOSTENT.h_name);
            PRIV_HOSTENT.h_name = NULL;
        }
        if (PRIV_HOSTENT.h_addr_list[0] != NULL) {
            pico_free(PRIV_HOSTENT.h_addr_list[0]);
            PRIV_HOSTENT.h_addr_list[0] = NULL;
        }
        PRIV_HOSTENT.h_name = pico_zalloc(strlen(name));
        if (!PRIV_HOSTENT.h_name) {
            pico_freeaddrinfo(res);
            return NULL;
        }
        strcpy(PRIV_HOSTENT.h_name, name);
        PRIV_HOSTENT.h_addrtype = res->ai_addr->sa_family;
        if (PRIV_HOSTENT.h_addrtype == AF_INET) {
            PRIV_HOSTENT.h_length = 4;
            PRIV_HOSTENT.h_addr_list[0] = pico_zalloc(4); 
            if (!PRIV_HOSTENT.h_addr_list[0]) {
                pico_freeaddrinfo(res);
                return NULL;
            }
            memcpy (PRIV_HOSTENT.h_addr_list[0], &(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr), 4);
        } else {
            /* Only IPv4 supported by this ancient call. */
            pico_freeaddrinfo(res);
            return NULL;
        }
        pico_freeaddrinfo(res);
        return &PRIV_HOSTENT;
    }
    return NULL;
}
#endif

int pico_getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
    struct pico_bsd_endpoint *ep = get_endpoint(sockfd);
    int ret;
    bsd_dbg("called getsockopt\n");
    if (level != SOL_SOCKET) {
        pico_err = PICO_ERR_EPROTONOSUPPORT;
        errno = pico_err;
        return -1;
    }
    if (!ep) {
        pico_err = PICO_ERR_EINVAL;
        errno = pico_err;
        return -1;
    }
    if (!optval) {
        pico_err = PICO_ERR_EFAULT;
        errno = pico_err;
        return -1;
    }

    if (optname == SO_ERROR)
    {
        *((int*)optval) = ep->error;
        return 0;
    }

    pico_mutex_lock(ep->mutex_lock);
    ret = pico_socket_getoption(ep->s, sockopt_get_name(optname), optval);
    pico_mutex_unlock(ep->mutex_lock);
    return ret;

}

int pico_setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{

    struct pico_bsd_endpoint *ep = get_endpoint(sockfd);
    int ret;
    bsd_dbg("called setsockopt\n");
    if (level != SOL_SOCKET) {
        pico_err = PICO_ERR_EPROTONOSUPPORT;
        errno = pico_err;
        return -1;
    }
    if (!ep) {
        pico_err = PICO_ERR_EINVAL;
        errno = pico_err;
        return -1;
    }
    if (!optval) {
        pico_err = PICO_ERR_EFAULT;
        errno = pico_err;
        return -1;
    }
    if ((optname == SO_REUSEADDR) || (optname == SO_REUSEPORT))
        return 0; /* Pretend it was OK. */
    if (optname == SO_ERROR)
        return PICO_ERR_ENOPROTOOPT;

    pico_mutex_lock(ep->mutex_lock);
    ret = pico_socket_setoption(ep->s, sockopt_get_name(optname), (void *)optval);
    pico_mutex_unlock(ep->mutex_lock);
    return ret;
}
#ifdef PICO_SUPPORT_SNTP_CLIENT
int pico_gettimeofday(struct timeval *tv, struct timezone *tz)
{
    int ret;
    (void)tz;

    struct pico_timeval ptv;

    ret= pico_sntp_gettimeofday(&ptv);

    tv->tv_sec = ptv.tv_sec;
    tv->tv_usec= ptv.tv_msec * 1000; /* pico_timeval uses milliseconds instead of microseconds */
    return ret;
}

long XTIME(void) {
    struct timeval t;
    pico_gettimeofday(&t, NULL); 
    return (long)t.tv_sec;
}
#endif

const char *pico_inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    if ((!dst) || (!src))
        return NULL;

    switch (af)
    {
        case AF_INET:
            if (size < INET_ADDRSTRLEN)
                return NULL;
            pico_ipv4_to_string(dst, *((const uint32_t *)src));
            break;
#ifdef PICO_SUPPORT_IPV6
        case AF_INET6:
            if (size < INET6_ADDRSTRLEN)
                return NULL;
            pico_ipv6_to_string(dst, ((struct in6_addr *)src)->s6_addr);
            break;
#endif
        default:
            return NULL;
            break;
    }
    return dst;
}

char *pico_inet_ntoa(struct in_addr in)
{
    static char ipbuf[INET_ADDRSTRLEN];
    pico_ipv4_to_string(ipbuf, (const uint32_t)in.s_addr);
    return ipbuf;
}



int pico_pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask)
{
    /* 
     * EV_READ:     sets the readfds
     * EV_WRITE:    sets the writefds
     * EV_CONN:     sets the readfds (a.k.a. someone connects to your listening socket)
     * EV_CLOSE:    sets the readfds, then next recv() returns 0;
     * EV_FIN:      sets the readfds, then next recv() returns 0;
     * EV_ERR:      sets the exceptfds
     */

    int i = 0;          /* socket fds */
    int nfds_out = 0;   /* amount of changed sockets */
    (void) sigmask;

    bsd_dbg_select("===  IN: PICO SELECT === readfds[0]: 0x%x -- writefds[0]: 0x%x\n", readfds?(*(uint8_t *)readfds):0, writefds?(*(uint8_t *)writefds):0);

    pico_fd_set readfds_out = {};
    pico_fd_set writefds_out = {};
    pico_fd_set exceptfds_out = {};
    /* First, loop over all possible file descriptors, check if one has an event pending that we're waiting for */
    while (nfds_out == 0)
    {
        for (i = 0; i < nfds; i++)
        {
            struct pico_bsd_endpoint *ep = get_endpoint(i);
            bsd_dbg_select("\t~~~ SELECT: fds %d - ep:%p ", i, ep);
            if (ep)
            {
                /* Is this endpoint still valid? */
                if (!ep->in_use)
                {
                    bsd_dbg_select(" ep->in_use = 0\n");
                    break;
                }

                /* READ event needed and available? */
                if (readfds && PICO_FD_ISSET(i,readfds) && (ep->revents & (PICO_SOCK_EV_CONN | PICO_SOCK_EV_CLOSE | PICO_SOCK_EV_RD)))
                {
                    bsd_dbg_select("- READ_EV - ");
                    nfds_out++;
                    PICO_FD_SET(i, &readfds_out);
                }

                /* WRITE event needed and available? */
                if (writefds && PICO_FD_ISSET(i,writefds) && (ep->revents & (PICO_SOCK_EV_WR)))
                {
                    bsd_dbg_select("- WRITE_EV - ");
                    nfds_out++;
                    PICO_FD_SET(i, &writefds_out);
                }

                /* EXCEPTION event needed and available? */
                if (exceptfds && PICO_FD_ISSET(i,exceptfds) && (ep->revents & (PICO_SOCK_EV_ERR)))
                {
                    bsd_dbg_select("- EXCEPT_EV - ");
                    nfds_out++;
                    PICO_FD_SET(i, &exceptfds_out);
                }
            }

            if (ep)
                bsd_dbg_select("- s:%p - ev:%x", ep->s, ep->revents);
            bsd_dbg_select("\n");
        }

        /*  If there was a hit, break out of the loop */
        if (nfds_out)
            break;

        /* If not, wait for a semaphore signaling an event from the stack */
        if (pico_signal_wait_timeout(global_signal, (timeout->tv_sec * 1000) + ((timeout->tv_nsec) / 1000000)) == -1)
        {
            /* On timeout, break out of the loop */
            bsd_dbg_select("\t~~~ SELECT: TIMEOUT\n");
            break;
        } else {
            /* Process the received event -> re-iterate */
            bsd_dbg_select("\t~~~ SELECT: Socket event, re-iterating fds\n");
        }
    }

    /* Copy back result only if descriptor was valid */
    if (readfds)
        memcpy(readfds, &readfds_out, sizeof(pico_fd_set));
    if (writefds)
        memcpy(writefds, &writefds_out, sizeof(pico_fd_set));
    if (exceptfds)
        memcpy(exceptfds, &exceptfds_out, sizeof(pico_fd_set));

    bsd_dbg_select("=== OUT: PICO SELECT === fds changed: %d\n", nfds_out);

    return nfds_out;
}

int pico_select(int nfds, pico_fd_set *readfds, pico_fd_set *writefds, pico_fd_set *exceptfds, struct timeval *timeout) 
{
    struct timespec ts;
    if (timeout) {
        ts.tv_sec = timeout->tv_sec;
        ts.tv_nsec = timeout->tv_usec * 1000;
        return pico_pselect(nfds, readfds, writefds, exceptfds, &ts, NULL);
    } else
        return pico_pselect(nfds, readfds, writefds, exceptfds, NULL, NULL); 
}

int pico_ppoll(struct pollfd *pfd, nfds_t npfd, const struct timespec *timeout, const sigset_t *sigmask) {
    int i;
    int ret = 0;
    (void) sigmask;

    while (ret == 0) {
        for (i = 0; i < npfd; i++) {
            struct pico_bsd_endpoint *ep = get_endpoint(pfd[i].fd);
            pfd[i].revents = 0u;

            /* Always polled events */
            if (!ep) {
                pfd[i].revents |= POLLNVAL; 
            }
            if (!ep->in_use) {
                pfd[i].revents |= POLLNVAL;
            }
            if (ep->events & (PICO_SOCK_EV_FIN | PICO_SOCK_EV_ERR)) {
                pfd[i].revents |= POLLERR;
                ret++;
            }
            if (ep->events & PICO_SOCK_EV_CLOSE)
                pfd[i].revents |= POLLHUP;

            /* Checking POLLIN */
            if ((pfd[i].events & POLLIN)  && (ep->events & (PICO_SOCK_EV_RD | PICO_SOCK_EV_CONN))) {
                pfd[i].revents |= POLLIN;
                if (pfd[i].events & POLLRDNORM)
                    pfd[i].revents |= POLLRDNORM;
            }
            /* Checking POLLOUT */
            if ((pfd[i].events & POLLOUT) && (ep->events & (PICO_SOCK_EV_WR))) {
                pfd[i].revents |= POLLOUT;
                if (pfd[i].events & POLLWRNORM)
                    pfd[i].revents |= POLLWRNORM;
            }

            if (pfd[i].revents != 0)
                ret++;
        } /* End for loop */
        if ((ret == 0) && timeout && (pico_signal_wait_timeout(global_signal, (timeout->tv_sec * 1000) + ((timeout->tv_nsec) / 1000000)) == -1))
                return 0; /* Timeout */
    } /* End while loop */
    return ret;
}

int pico_poll(struct pollfd *pfd, nfds_t npfd, int timeout)
{
    struct timespec ts = {0U, 0U};
    if (timeout >= 0) {
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (timeout % 1000) * 1000000;
        return pico_ppoll(pfd, npfd, &ts, NULL);
    } else {
        return pico_ppoll(pfd, npfd, NULL, NULL);
    }
}
