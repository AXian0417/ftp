#ifndef _ftp_client_h_
#define _ftp_client_h_

#include "ftp_typedef.h"
#include "stddef.h"
#include "stdatomic.h"

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
    FTP_STATE_ABORT,                    /** ABOR sent, waiting for the replies */
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

typedef enum ftp_resolve_state
{
    FTP_RESOLVE_IDLE = 0,
    FTP_RESOLVE_PENDING,
    FTP_RESOLVE_DONE,
    FTP_RESOLVE_FAILED,
} ftp_resolve_state_t;

/**
 * @brief Host name resolution request
 *
 * @note Written by the resolver, possibly from another thread.
 */
typedef struct
{
    const char* host;                   /** Host name, valid until the request completes */
    uint32_t ip;                        /** IPv4 address, host byte order (written first) */
    _Atomic ftp_resolve_state_t state;  /** Written last: the atomic store publishes ip */
} ftp_resolve_t;

/**
 * @brief Non-blocking host name resolver
 *
 * Starts the resolution and returns at once, the result is written to req later
 * (req->ip first, then req->state). Must never wait.
 *
 * Only needed when ftp_connect() is called from the thread that runs ftp_process()
 * (single thread, no RTOS, or FTP callbacks). Pass NULL otherwise.
 * Reference implementation for lwIP: port/ftp_resolver_lwip.c (ftp_resolver_lwip).
 */
typedef void (*ftp_resolver_t)(const char* host, ftp_resolve_t* req);

struct _ftp_client
{
    char name[32];
    void* user_data;

    ftp_state_t state;                  /** Owned by ftp_process() */
    ftp_state_t last_state;
    _Atomic ftp_state_t state_pub;      /** state published by ftp_process() for the API threads */
    uint32_t ticks;

    ftp_sock_t cmd_sd;
    ftp_sock_t data_sd;
    uint32_t data_ip;                   /** PASV address, host byte order */
    uint16_t data_port;                 /** PASV port */

    /* host name resolution */
    ftp_resolver_t resolver;            /** NULL: resolved by ftp_connect() in the caller's thread */
    ftp_resolve_t dns;
    uint32_t server_ip;                 /** host byte order */
    bool server_ip_valid;

    /*
        Request slot: the API (any thread) fills the request fields and bumps req_seq,
        ftp_process() takes it and bumps ack_seq when done. Only atomic load/store
        are used (no read-modify-write), one API thread per client at a time.
    */
    atomic_uint req_seq;
    atomic_uint ack_seq;
    atomic_bool abort_req;              /** ftp_abort(): set by the API, taken by ftp_process() */
    unsigned int req_taken;             /** Owned by ftp_process() */
    uint8_t req_type;
    ftp_cmd_t req_cmd;

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
    bool inited;
    bool cmd_broken;                    /** Command connection failed, disconnect */
    uint16_t multi_line_code;
    _Atomic int last_resp_code;         /** Written by ftp_process(), read by any thread */

    char status[128];
    ftp_cmd_t job_cmd;
    ftp_mode_t mode;
    ftp_client_cb_t callback;

    int file_fd;                        /** Local file, valid while file_open */
    bool file_open;

    struct _ftp_client* next;
};

bool ftp_client_register(ftp_client_t* client, const char* name, void* user, ftp_client_cb_t cb, ftp_resolver_t resolver);
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
bool ftp_abort(ftp_client_t* client);

/* lwIP reference resolver, defined in port/ftp_resolver_lwip.c (add it to the build when used) */
void ftp_resolver_lwip(const char* host, ftp_resolve_t* req);
const char* ftp_client_status_message(ftp_client_t* client);
const char* ftp_client_state_string(ftp_client_t* client);
int ftp_last_resp_code(ftp_client_t* client);

#endif
