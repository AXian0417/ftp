#ifndef _ftp_server_h_
#define _ftp_server_h_

#include "ftp_typedef.h"
#include "time.h"

#define FTP_CFG_DEFAULT(MOUNT_POINT) {          \
    .server_ip = "192.168.4.1",                 \
    .mount_point = MOUNT_POINT,                 \
}

typedef time_t(*ftp_unix_time_t)(void);

typedef enum ftp_role
{
    FTP_ROLE_GUEST = 0,     /** Allow LIST and RETR cmmands */
    FTP_ROLE_USER,          /** Allow LIST, RETR and STOR commands */
    FTP_ROLE_MAINTAINER,    /** Allow LIST, RETR, STOR, DELE and RNFR/RNTO commands */
    FTP_ROLE_ADMIN,         /** Allow all commands */
    FTP_ROLE_UNKNOWN,       /** Unknown role, used to check if the user is logged in */
} ftp_role_t;

typedef const struct ftp_config
{
    char* server_ip;
    char* mount_point;
} ftp_config_t;

bool ftp_init(const ftp_config_t* config);
bool ftp_deinit(void);
void ftp_reset(void);
void ftp_enable_time(const ftp_unix_time_t get_time);
void ftp_disable_time(void);
bool ftp_set_username(ftp_role_t role, const char* new_username);
bool ftp_set_password(ftp_role_t role, const char* new_password);
bool ftp_enable(void);
bool ftp_enabled(void);
bool ftp_disable(void);
void ftp_server(uint32_t pulse_1ms);

#endif
