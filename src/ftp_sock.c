// SPDX-License-Identifier: MIT
/**
 * @file ftp_sock.c
 * @author A Xian (axian0417@gmail.com)
 *
 * @brief POSIX socket access of the FTP library.
 * @details The only file of the library that includes the socket headers; the rest reaches
 *          the network through this API and the ftp_sock_t wrapper, so socket calls
 *          elsewhere fail to compile. Only POSIX headers and names are used, no platform
 *          specific code. Sockets are non-blocking and no function waits, except
 *          ftp_sock_resolve(), which runs in the API caller's thread only. IPv4 addresses
 *          are in host byte order (192.168.4.1 = 0xC0A80401). Sockets and files share
 *          close()/fcntl(); the platform routes each descriptor to its owner.
 *
 * @copyright Copyright (c) 2026 A Xian
 *
 */

#include "ftp_sock.h"
#include "string.h"
#include "errno.h"
#include "unistd.h"
#include "fcntl.h"
#include "sys/select.h"
#include "sys/socket.h"
#include "arpa/inet.h"
#include "netdb.h"

#if defined(close) || defined(read) || defined(write)
#error "close/read/write are redefined as macros (e.g. LWIP_POSIX_SOCKETS_IO_NAMES = 1)"
#endif

/**
 * @brief Check socket is open
 */
bool ftp_sock_valid(ftp_sock_t s)
{
    return (s.fd >= 0);
}

/**
 * @brief Close socket and invalidate the handle, ignored if not open
 */
void ftp_sock_close(ftp_sock_t* s)
{
    if (s->fd >= 0)
    {
        close(s->fd);
    }

    s->fd = -1;
}

/**
 * @brief Set socket to non-blocking mode
 *
 * @return true on success
 */
bool ftp_sock_set_nonblocking(ftp_sock_t s)
{
    int flags = fcntl(s.fd, F_GETFL, 0);

    if (flags < 0)
    {
        return false;
    }

    return (fcntl(s.fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

/**
 * @brief Create TCP socket
 *
 * @return ftp_sock_t FTP_SOCK_INVALID on failure
 */
ftp_sock_t ftp_sock_tcp(void)
{
    ftp_sock_t s = { .fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) };

    return s;
}

/**
 * @brief Enable address reuse, bind and listen
 *
 * @param ip Local IPv4 address, FTP_SOCK_IP_ANY for any
 *
 * @return true on success
 */
bool ftp_sock_listen(ftp_sock_t s, uint32_t ip, uint16_t port, int backlog)
{
    struct sockaddr_in addr;
    int option = 1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(ip);

    return ((setsockopt(s.fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) == 0) &&
            (bind(s.fd, (const struct sockaddr*)&addr, sizeof(addr)) == 0) &&
            (listen(s.fd, backlog) == 0));
}

/**
 * @brief Accept a connection
 *
 * @param ip Peer IPv4 address, can be NULL
 *
 * @return ftp_sock_t FTP_SOCK_INVALID if no connection,
 *         check ftp_sock_would_block() for the reason
 */
ftp_sock_t ftp_sock_accept(ftp_sock_t ls, uint32_t* ip)
{
    struct sockaddr_in addr;
    socklen_t addr_size = sizeof(addr);
    ftp_sock_t s = { .fd = accept(ls.fd, (struct sockaddr*)&addr, &addr_size) };

    if ((s.fd >= 0) && ip)
    {
        *ip = ntohl(addr.sin_addr.s_addr);
    }

    return s;
}

/**
 * @brief Start a connect
 *
 * @return int 0 connected, 1 in progress (non-blocking), -1 failed
 */
int ftp_sock_connect(ftp_sock_t s, uint32_t ip, uint16_t port)
{
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(ip);

    if (connect(s.fd, (const struct sockaddr*)&addr, sizeof(addr)) == 0)
    {
        return 0;
    }

    return (errno == EINPROGRESS) ? 1 : -1;
}

/**
 * @brief Check if the socket is writable (e.g. a non-blocking connect completed)
 *
 * @note Never waits (zero timeout)
 *
 * @return true if writable
 */
bool ftp_sock_writable(ftp_sock_t s)
{
    fd_set wfds;
    struct timeval tv = { 0, 0 };

    FD_ZERO(&wfds);
    FD_SET(s.fd, &wfds);

    return ((select(s.fd + 1, NULL, &wfds, NULL, &tv) > 0) && FD_ISSET(s.fd, &wfds));
}

/**
 * @brief Get the pending socket error (SO_ERROR)
 *
 * @return int 0 if no error
 */
int ftp_sock_error(ftp_sock_t s)
{
    int error = 0;
    socklen_t len = sizeof(error);

    if (getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0)
    {
        return -1;
    }

    return error;
}

/**
 * @brief Send data
 *
 * @note Insertion point for TLS (FTPS)
 *
 * @return int Sent size, -1 on error
 */
int ftp_sock_send(ftp_sock_t s, const void* data, size_t size)
{
    return (int)send(s.fd, data, size, 0);
}

/**
 * @brief Receive data
 *
 * @note Insertion point for TLS (FTPS)
 * @note Does not block, the socket is in non-blocking mode
 *       (MSG_DONTWAIT is not POSIX)
 *
 * @return int Received size, 0 if peer closed, -1 on error
 */
int ftp_sock_recv(ftp_sock_t s, void* data, size_t size)
{
    return (int)recv(s.fd, data, size, 0);
}

/**
 * @brief Copy received data without removing it from the socket (MSG_PEEK)
 *
 * @return Bytes copied, 0 if the peer closed, -1 on error (check ftp_sock_would_block())
 */
int ftp_sock_peek(ftp_sock_t s, void* data, size_t size)
{
    return (int)recv(s.fd, data, size, MSG_PEEK);
}

/**
 * @brief Check if the last failed socket call means "try again"
 *
 * @note Kept here because the errno values come from the socket stack
 */
bool ftp_sock_would_block(void)
{
    return ((errno == EAGAIN) || (errno == EWOULDBLOCK));
}

/**
 * @brief Local IPv4 address of a connected socket (the interface the peer reached)
 *
 * @param ip IPv4 address in host byte order
 *
 * @return true on success
 */
bool ftp_sock_local_ip(ftp_sock_t s, uint32_t* ip)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    if ((getsockname(s.fd, (struct sockaddr*)&addr, &len) != 0) || (addr.sin_family != AF_INET))
    {
        return false;
    }

    *ip = ntohl(addr.sin_addr.s_addr);
    return true;
}

/**
 * @brief Convert IPv4 string to address
 *
 * @param ip IPv4 address in host byte order
 *
 * @return true on success
 */
bool ftp_sock_ip4_parse(const char* str, uint32_t* ip)
{
    struct in_addr addr;

    if ((str == NULL) || (inet_pton(AF_INET, str, &addr) != 1))
    {
        return false;
    }

    *ip = ntohl(addr.s_addr);
    return true;
}

/**
 * @brief Resolve a host name (IPv4)
 *
 * @param ip IPv4 address in host byte order
 *
 * @return true on success
 *
 * @note Blocks until the system resolver answers (uses the DNS servers configured
 *       by the application, DHCP or static). Never called by ftp_process().
 */
bool ftp_sock_resolve(const char* host, uint32_t* ip)
{
    struct addrinfo hints;
    struct addrinfo* res = NULL;
    bool ok = false;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if ((getaddrinfo(host, NULL, &hints, &res) == 0) && (res != NULL))
    {
        *ip = ntohl(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
        ok = true;
    }

    if (res != NULL)
    {
        freeaddrinfo(res);
    }

    return ok;
}
