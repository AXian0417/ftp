#ifndef _ftp_sock_h_
#define _ftp_sock_h_

#include "stdint.h"
#include "stdbool.h"
#include "stddef.h"
#include "ftp_typedef.h"

#define FTP_SOCK_IP_ANY                         (0x00000000U)

bool ftp_sock_valid(ftp_sock_t s);
void ftp_sock_close(ftp_sock_t* s);
bool ftp_sock_set_nonblocking(ftp_sock_t s);
ftp_sock_t ftp_sock_tcp(void);
bool ftp_sock_listen(ftp_sock_t s, uint32_t ip, uint16_t port, int backlog);
ftp_sock_t ftp_sock_accept(ftp_sock_t ls, uint32_t* ip);
int ftp_sock_connect(ftp_sock_t s, uint32_t ip, uint16_t port);
bool ftp_sock_writable(ftp_sock_t s);
int ftp_sock_error(ftp_sock_t s);
int ftp_sock_send(ftp_sock_t s, const void* data, size_t size);
int ftp_sock_recv(ftp_sock_t s, void* data, size_t size);
int ftp_sock_peek(ftp_sock_t s, void* data, size_t size);
bool ftp_sock_would_block(void);
bool ftp_sock_local_ip(ftp_sock_t s, uint32_t* ip);
bool ftp_sock_ip4_parse(const char* str, uint32_t* ip);
bool ftp_sock_resolve(const char* host, uint32_t* ip);

#endif
