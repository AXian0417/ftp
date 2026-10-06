#ifndef _ftp_server_h_
#define _ftp_server_h_

#include "ftp_typedef.h"
#include "time.h"

typedef time_t(*ftp_unix_time_t)(void);

typedef enum ftp_role
{
    FTP_ROLE_GUEST = 0,     /** Allow LIST and RETR cmmands */
    FTP_ROLE_USER,          /** Allow LIST, RETR and STOR commands */
    FTP_ROLE_MAINTAINER,    /** Allow LIST, RETR, STOR, DELE and RNFR/RNTO commands */
    FTP_ROLE_ADMIN,         /** Allow all commands */
    FTP_ROLE_UNKNOWN,       /** Unknown role, used to check if the user is logged in */
} ftp_role_t;


/*
    Threads: ftp_server() may run in another thread than the API below.
    - Lifecycle (ftp_init, ftp_deinit, ftp_reset): only before ftp_server() starts
      or after it stops.
    - Other functions: from one thread at a time, they return at once.
      ftp_enable()/ftp_disable() record the request, ftp_server() starts or stops
      the server (signs out all clients) in its next call.
    - ftp_set_username()/ftp_set_password(): any time. New logins use the new
      credentials at once; ftp_server() signs out the clients logged in with that
      role in its next call.
    - ftp_set_pasv_address(): any time, the next PASV uses it.
*/
bool ftp_init(const char* mount_point);
bool ftp_deinit(void);
void ftp_reset(void);
void ftp_enable_time(const ftp_unix_time_t get_time);
void ftp_disable_time(void);
bool ftp_set_username(ftp_role_t role, const char* new_username);
bool ftp_set_password(ftp_role_t role, const char* new_password);
bool ftp_set_pasv_address(const char* ipv4);
bool ftp_enable(void);
bool ftp_enabled(void);
bool ftp_disable(void);
void ftp_server(uint32_t pulse_1ms);

#endif
