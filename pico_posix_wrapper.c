#include "pico_bsd_sockets.h"
#define _GNU_SOURCE
#define __GNU_SOURCE
#define __USE_GNU
#include <dlfcn.h>
#include <stdio.h>
#include "pico_ipv4.h"
#include "pico_ipv6.h"
#include "pico_stack.h"
#include "pico_socket.h"
#include "pico_dev_vde.h"
#include <fcntl.h>
#include <pthread.h>
#include <libvdeplug.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <pthread.h>
#include <sys/poll.h>


static __thread int in_the_stack = 0;
static int initialized = 0;
#define ptsock_dbg printf




#define swap_socketcall(call, name) \
{ \
  const char *msg; \
  if (host_##call == NULL) { \
    *(void **)(&host_##call) = dlsym(RTLD_NEXT, name); \
    if ((msg = dlerror()) != NULL) \
      fprintf (stderr, "%s: dlsym(%s): %s\n", "picotcp", name, msg); \
  } \
}


#define conditional_steal_call(call, i, ...) \
   if(in_the_stack) { \
    return host_##call(i, ## __VA_ARGS__); \
   } else { \
    if (get_pico_fd(i) > -1) { \
        int __pico_retval = pico_##call(get_pico_fd(i), ## __VA_ARGS__); \
        if (__pico_retval != 0) \
            errno = pico_err; \
        return __pico_retval; \
    }else { \
        return host_##call(i, ## __VA_ARGS__); \
    } \
   }

static int max_fd = 0;
static int *pico_fds = NULL;

static int remap_fd(int pico_fd)
{
    int new_fd = open("/dev/zero", O_RDONLY);
    int old_max = max_fd;
    int i;
    if (new_fd < 0) {
        abort();
    }
    if (max_fd < new_fd + 1)
        max_fd = new_fd + 1;
    if (pico_fds == NULL) {
        pico_fds = malloc(sizeof(int) * max_fd);
        for (i = 0; i < max_fd; i++)
            pico_fds[i] = -1;
        pico_fds[new_fd] = pico_fd;
        return new_fd;
    }
    pico_fds = realloc(pico_fds, sizeof(int) * max_fd);
    for (i = old_max; i < max_fd; i++)
        pico_fds[i] = -1;
    pico_fds[new_fd] = pico_fd;
    return new_fd;
}

static int get_pico_fd(int j)
{
    if (j >= max_fd)
        return -1;
    return pico_fds[j];
}


static int (*host_socket  ) (int domain, int type, int protocol) = NULL;
static int (*host_bind    ) (int sockfd, const struct sockaddr *addr, socklen_t addrlen);
static int (*host_connect ) (int sockfd, const struct sockaddr *addr, socklen_t addrlen);
static int (*host_accept  ) (int sockfd, struct sockaddr *addr, socklen_t *addrlen);
static int (*host_listen  ) (int sockfd, int backlog);
static ssize_t (*host_recvfrom) (int sockfd, void *buf, size_t len, int flags, struct sockaddr *addr, int *addrlen);
static ssize_t (*host_recv    ) (int sockfd, void *buf, size_t len, int flags);
static ssize_t (*host_read    ) (int sockfd, void *buf, size_t len);
static ssize_t (*host_sendto  ) (int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t addrlen);
static ssize_t (*host_send    ) (int sockfd, void *buf, size_t len, int flags);
static ssize_t (*host_write   ) (int sockfd, const void *buf, size_t len);
static int (*host_close   ) (int sockfd);
static int (*host_shutdown) (int sockfd, int how);
static int (*host_setsockopt) (int sockfd, int level, int optname, const void *optval, socklen_t optlen);
static int (*host_getsockopt) (int sockfd, int level, int optname, void *optval, socklen_t *optlen);

       int getaddrinfo(const char *node, const char *service,
                       const struct addrinfo *hints,
                       struct addrinfo **res);

       void freeaddrinfo(struct addrinfo *res);


static int (*host_getaddrinfo) (const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
static int (*host_freeaddrinfo) (struct addrinfo *res);
static int (*host_poll) (struct pollfd *pfd, nfds_t npfd, int timeout);
static int (*host_ppoll) (struct pollfd *pfd, nfds_t npfd, const struct timespec *timeout_ts, const sigset_t *sigmask);
static int (*host_select) (int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
static int (*host_pselect) (int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout,
                            const sigset_t *sigmask);

int pico_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    printf("Called select\n");
    return -1;

}
int pico_pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask)
{
    printf("Called pselect\n");
    return -1;
}


int pico_ppoll (struct pollfd *pfd, nfds_t npfd, const struct timespec *timeout_ts, const sigset_t *sigmask)
{
    int ret = 0, err = 0;
    int i, j = 0, k = 0;
    uint64_t timer = 0;
    //printf("ppoll called\n");
  
    if (in_the_stack)
      return host_ppoll(pfd, npfd, timeout_ts, sigmask);
    /* clear Revents */
    for(i = 0; i < npfd; i++) {
        pfd[i].revents = 0;
    }
    for(;;) {  
        for (i = 0; i < npfd; i++) {
            int pico_fd = get_pico_fd(pfd[i].fd);
            if (pico_fd < 0) {
                err = host_poll(&pfd[i], 1, 0);
                if (err > 0)
                    ret++;
                if (err < 0)
                    return -1;
            } else {
                uint16_t revents;
                if (pico_bsd_check_events(pico_fd, pfd[i].events, &revents) != 0) {
                    pfd[i].revents |= POLLHUP | POLLERR;
                    return -1;
                } else {
                    if (revents & PICO_SOCK_EV_CONN) {
                        pfd[i].revents |= POLLIN;
                    }
                    if (revents & PICO_SOCK_EV_RD) {
                        pfd[i].revents |= POLLIN;
                    }
                    if (revents & PICO_SOCK_EV_WR) {
                        pfd[i].revents |= POLLOUT;
                    }
                    if (revents & PICO_SOCK_EV_CLOSE) {
                        pfd[i].revents |= POLLHUP;
                    }
                    if (revents & PICO_SOCK_EV_FIN) {
                        pfd[i].revents |= POLLERR;
                    }
                }
                pfd[i].revents &= (pfd[i].events | POLLHUP | POLLERR);
                pfd[i].revents &= POLLIN | POLLOUT | POLLHUP | POLLERR;
                if (pfd[i].revents > 0) {
                    //printf("one socket unlocked\n");
                    ret++;
                }
            }
        }
        if (ret > 0) {
            //printf("-->returning %d\n", ret);
            return ret;
        }
        usleep(1000);
        timer++;
    
        if ((timeout_ts != NULL) && ((timer >  ((timeout_ts->tv_sec * 1000) + (timeout_ts->tv_nsec / 1000000)))))
            return 0;
    }
    return -1; /* Oh, really? */
}

int pico_poll (struct pollfd *pfd, nfds_t npfd, int timeout)
{
    struct timespec ts;    
    if (in_the_stack)
        return host_poll(pfd, npfd, timeout);

    if (timeout >= 0) {
        ts.tv_sec  = timeout/1000;
        ts.tv_nsec = (timeout % 1000) * 1000000;
        return pico_ppoll(pfd, npfd, &ts, NULL);
    }
    return pico_ppoll(pfd, npfd, NULL, NULL);
}

int socket(int domain, int type, int protocol)
{
  int new_sd, posix_fd = -1;
  ptsock_dbg ("Called Socket (pid=%d) in_the_stack=%d\n", getpid(), in_the_stack);
  if (in_the_stack)
    return host_socket(domain, type, protocol);
  if ((domain != AF_INET) && (domain != AF_INET6)) {
    return host_socket(domain, type, protocol);
  }
  new_sd = pico_newsocket(domain, type, protocol);
  if (new_sd < 0) {
    ptsock_dbg("socket() call failed.\n");
    abort();
  }
  posix_fd = remap_fd(new_sd);
  ptsock_dbg ("Socket stolen, sd=%d, fd = %d\n", new_sd, posix_fd);
  return posix_fd;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
  conditional_steal_call(bind, sockfd, addr, addrlen);
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
  conditional_steal_call(connect, sockfd, addr, addrlen);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    if(in_the_stack) { 
        return host_accept(sockfd, addr, addrlen); 
    } else {
        int posix_fd, new_sd, listen_sd = get_pico_fd(sockfd);
        if (listen_sd < 0) {
            return host_accept(sockfd, addr, addrlen);
        }

        new_sd = pico_accept(listen_sd, addr, addrlen);
        if (new_sd < 0)
            return -1;
        posix_fd = remap_fd(new_sd);
        ptsock_dbg ("Socket accepted, sd=%d, fd = %d\n", new_sd, posix_fd);
        return posix_fd;
    }
}

int listen(int sockfd, int backlog)
{
  conditional_steal_call(listen, sockfd, backlog);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
  conditional_steal_call(recvfrom, sockfd, buf, len, flags, 0, 0);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addrlen)
{
  conditional_steal_call(recvfrom, sockfd, buf, len, flags, addr, addrlen);
}

ssize_t read(int sockfd, void *buf, size_t len)
{
  conditional_steal_call(read, sockfd, buf, len);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
  conditional_steal_call(sendto, sockfd, buf, len, flags, 0, 0);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t addrlen)
{
  conditional_steal_call(sendto, sockfd, buf, len, flags, addr, addrlen);
}

ssize_t write(int sockfd, const void *buf, size_t len)
{
  conditional_steal_call(write, sockfd, buf, len);
}

int close(int sockfd)
{
    int pico_sd;
    if (in_the_stack)
        return host_close(sockfd);
    pico_sd = get_pico_fd(sockfd);
    if (pico_sd < 0)
        return host_close(sockfd);
    pico_close(pico_sd);
    pico_fds[sockfd] = -1;
    return 0;
}

int shutdown(int sockfd, int how)
{
  int pico_sd;
  if (in_the_stack)
      return host_shutdown(sockfd, how);
    pico_sd = get_pico_fd(sockfd);
    if (pico_sd < 0)
        return host_shutdown(sockfd, how);

    if (how != SHUT_WR)
        pico_fds[sockfd] = -1;
    pico_shutdown(pico_sd, how);
    return 0;
}

int setsockopt (int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
  conditional_steal_call(setsockopt, sockfd, level, optname, optval, optlen);
}

int getsockopt (int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
  conditional_steal_call(getsockopt, sockfd, level, optname, optval, optlen);
}

int poll(struct pollfd *pfd, nfds_t npfd, int timeout)
{
  conditional_steal_call(poll, pfd, npfd, timeout);
}

int ppoll(struct pollfd *pfd, nfds_t npfd, const struct timespec *timeout_ts, const sigset_t *sigmask)
{
  conditional_steal_call(poll, pfd, npfd, timeout_ts, sigmask);
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    conditional_steal_call(select, nfds, readfds, writefds, exceptfds, timeout);
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask)
{
    conditional_steal_call(select, nfds, readfds, writefds, exceptfds, timeout, sigmask);
}

void *pico_tick_thread(void *arg) {
    struct pico_ip4 addr, netmask, gateway, zero = {};
    struct pico_device *vde;
    struct pico_ip4 v4_ip_host, v4_ip_route;
    struct pico_ip4 v4_mask;
    struct pico_ip4 v4_zero={}, v4_gateway;
    struct pico_ip6 v6_public;
    struct pico_ip6 v6_netmask = {0xff,0xff,};
    struct pico_device *tun;
    in_the_stack = 1;

    pico_bsd_init();
    pico_stack_init();

    tun = (struct pico_device *) pico_tun_create("psx0");
    if (!tun)
        abort();

    pico_string_to_ipv4("192.168.2.150",&v4_ip_host.addr);
    pico_string_to_ipv4("192.168.2.1",&v4_ip_route.addr);
    pico_string_to_ipv4("255.255.0.0",&v4_mask.addr);
    pico_string_to_ipv4("192.168.2.1",&v4_gateway.addr);
    pico_string_to_ipv6("7a55::150",v6_public.addr);

    pico_ipv4_link_add(tun, v4_ip_host, v4_mask);
    pico_ipv4_route_add(v4_ip_route, v4_mask, v4_gateway, 1, NULL);
    pico_ipv6_link_add(tun, v6_public, v6_netmask);

    for (;;) {
        pico_bsd_stack_tick();
        usleep(1000);
    }
}


int __attribute__((constructor)) pico_wrapper_start(void)
{
    pthread_t ticker;
    if (initialized++)
        return 0;
    printf("Stealing all your system calls, please wait...\n");
    swap_socketcall(socket  , "socket");
    swap_socketcall(bind    , "bind");
    swap_socketcall(connect , "connect");
    swap_socketcall(accept  , "accept");
    swap_socketcall(listen  , "listen");
    swap_socketcall(recvfrom, "recvfrom");
    swap_socketcall(recv    , "recv");
    swap_socketcall(read    , "read");
    swap_socketcall(sendto  , "sendto");
    swap_socketcall(send    , "send");
    swap_socketcall(write   , "write");
    swap_socketcall(close   , "close");
    swap_socketcall(shutdown, "shutdown");
    swap_socketcall(setsockopt, "setsockopt");
    swap_socketcall(getaddrinfo, "getaddrinfo");
    swap_socketcall(freeaddrinfo, "freeaddrinfo");
    swap_socketcall(poll, "poll");
    swap_socketcall(ppoll, "ppoll");
    swap_socketcall(select, "select");
    swap_socketcall(pselect, "pselect");
    pthread_create(&ticker, NULL, pico_tick_thread, NULL);
    sleep(1);
    return 0;
}

