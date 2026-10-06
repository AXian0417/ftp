// SPDX-License-Identifier: MIT
/**
 * @file ftp_resolver_lwip.c
 * @author A Xian (axian0417@gmail.com)
 *
 * @brief Optional non-blocking host name resolver for lwIP.
 * @details Needed only when ftp_connect() is called from the thread of ftp_process()
 *          (single thread, no RTOS, or FTP callbacks): ftp_client_register(&client, "ftp",
 *          NULL, on_event, ftp_resolver_lwip). Uses lwIP's own DNS table, so the DNS
 *          servers configured by the application (DHCP or dns_setserver()) apply. With an
 *          RTOS the lookup is passed to the tcpip thread with tcpip_callback(); with NO_SYS
 *          it is called directly. The request inside ftp_client_t must stay valid until it
 *          completes: do not unregister a client while it is resolving.
 *
 * @copyright Copyright (c) 2026 A Xian
 *
 */

#include "ftp_client.h"
#include "lwip/opt.h"
#include "lwip/dns.h"
#include "lwip/err.h"

#if !NO_SYS
#include "lwip/tcpip.h"
#endif

#if !LWIP_DNS
#error "ftp_resolver_lwip.c requires LWIP_DNS = 1"
#endif

/**
 * @brief Write the result (ip first, then state)
 */
static void dns_found(const char* name, const ip_addr_t* addr, void* arg)
{
    ftp_resolve_t* req = (ftp_resolve_t*)arg;

    (void)name;

    if ((addr != NULL) && IP_IS_V4(addr))
    {
        req->ip = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(addr)));
        req->state = FTP_RESOLVE_DONE;
    }
    else
    {
        req->state = FTP_RESOLVE_FAILED;
    }
}

/**
 * @brief Start the lookup, runs in the lwIP context
 *        (tcpip thread with RTOS, the caller's loop with NO_SYS)
 */
static void dns_start(void* arg)
{
    ftp_resolve_t* req = (ftp_resolve_t*)arg;
    ip_addr_t addr;
    err_t err = dns_gethostbyname(req->host, &addr, dns_found, req);

    if (err == ERR_OK)
    {
        /* Found in the cache or an address string */
        dns_found(req->host, &addr, req);
    }
    else if (err != ERR_INPROGRESS)
    {
        req->state = FTP_RESOLVE_FAILED;
    }

    /* ERR_INPROGRESS: lwIP calls dns_found() later */
}

/**
 * @brief Non-blocking resolver (ftp_resolver_t)
 *
 * @param host Host name
 * @param req Request, req->state is FTP_RESOLVE_PENDING until the result is written
 */
void ftp_resolver_lwip(const char* host, ftp_resolve_t* req)
{
    req->host = host;
    req->state = FTP_RESOLVE_PENDING;

#if NO_SYS
    /* Single thread: the lwIP raw API is called in the same loop, no locking needed */
    dns_start(req);
#else
    /* Hand over to the tcpip thread, the lwIP raw API must not be called from other threads */
    if (tcpip_callback(dns_start, req) != ERR_OK)
    {
        req->state = FTP_RESOLVE_FAILED;
    }
#endif
}
