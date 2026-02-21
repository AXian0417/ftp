#ifndef _ftp_client_h_
#define _ftp_client_h_

#include "ftp_typedef.h"
#include "lwip/dns.h"
#include "lwip/sockets.h"
#include "ff.h"

typedef struct _ftp_client ftp_client_t;

typedef enum ftp_state
{
    FTP_STATE_IDLE = 0,
    FTP_STATE_RESOLVING,
    FTP_STATE_CONNECTING,
    FTP_STATE_WAIT_GREETING,
    FTP_STATE_WAIT_LOGIN,
    FTP_STATE_READY,
    FTP_STATE_CONNECTED,
    FTP_STATE_PASV_REQUEST,
    FTP_STATE_PASV_RESPONSE,
    FTP_STATE_DATA_CONNECT,
    FTP_STATE_SEND_COMMAND,
    FTP_STATE_WAIT_RESPONSE,
    FTP_STATE_LIST,
    FTP_STATE_RETR,
    FTP_STATE_STOR,
    FTP_STATE_QUIT,
    FTP_STATE_ERROR,
    FTP_STATE_MAX,
} ftp_state_t;

typedef enum ftp_events
{
    FTP_ON_STATUS = 0,
    FTP_ON_CONNECTED,
    FTP_ON_DISCONNECTED,
    FTP_ON_ERROR,
    FTP_ON_TRANSFER_START,
    FTP_ON_TRANSFER_DATA,
    FTP_ON_TRANSFER_END,
} ftp_events_t;

typedef enum ftp_mode
{
    FTP_MODE_NONE = 0,
    FTP_MODE_UPLOAD,
    FTP_MODE_DOWNLOAD,
    FTP_MODE_LISTING
} ftp_mode_t;

typedef void (*ftp_client_cb_t)(ftp_client_t* client, ftp_events_t event, void* data, size_t len);

struct _ftp_client
{
    char name[32];
    void* user_data;

    ftp_state_t state;
    ftp_state_t last_state;
    uint32_t ticks;

    int cmd_sd;
    int data_sd;
    struct sockaddr_in data_addr;
    ip_addr_t resolved_ip;

    size_t response_len;
    size_t data_len;
    char cmd_buf[FTP_CMD_BUFFER_SIZE];
    char response_buf[FTP_CMD_BUFFER_SIZE];
    char data_buf[FTP_DATA_BUFFER_SIZE];

    /* server info */
    char host[64];
    char user[FTP_MAX_USER_LEN];
    char pass[FTP_MAX_PASS_LEN];
    char path[FTP_FILE_PATH_LEN];
    uint16_t port;

    /* operation requested */
    bool dns_pending;
    bool disconnecting; /* Flag to prevent DNS callback after disconnect */  
    bool inited; 
    uint16_t multi_line_code;
    int last_resp_code;

    char status[128];
    ftp_cmd_t job_cmd;
    ftp_mode_t mode;
    ftp_client_cb_t callback;

    FIL file;
    bool file_open;

    struct _ftp_client* next;
};

bool ftp_client_register(ftp_client_t* client, const char* name, void* user, ftp_client_cb_t cb);
bool ftp_client_unregister(ftp_client_t* client);
void ftp_process(uint32_t pulse_1ms);
bool ftp_connect(ftp_client_t* client, const char* host, uint16_t port, const char* user, const char* pass);
bool ftp_open_url(ftp_client_t* client, const char* url);
bool ftp_open(ftp_client_t* client, const char* host, uint16_t port);
bool ftp_user(ftp_client_t* client, const char* user, const char* pass);
bool ftp_password(ftp_client_t* client, const char* pass);
bool ftp_quit(ftp_client_t* client);
bool ftp_ls(ftp_client_t* client, const char* path);
bool ftp_cd(ftp_client_t* client, const char* path);
bool ftp_pwd(ftp_client_t* client);
bool ftp_mkdir(ftp_client_t* client, const char* path);
bool ftp_rmdir(ftp_client_t* client, const char* path);
bool ftp_rename(ftp_client_t* client, const char* from_name, const char* to_name);
bool ftp_get(ftp_client_t* client, const char* remote_file, const char* local_file);
bool ftp_put(ftp_client_t* client, const char* local_file, const char* remote_file);
bool ftp_put_url(ftp_client_t* client, const char* url);
bool ftp_delete(ftp_client_t* client, const char* filename);
bool ftp_status(ftp_client_t* client);
const char* ftp_client_status_message(ftp_client_t* client);
const char* ftp_client_state_string(ftp_client_t* client);
int ftp_last_resp_code(ftp_client_t* client);

#endif
