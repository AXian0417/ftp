// SPDX-License-Identifier: MIT
/**
 * @file ftp_server.c
 * @author A Xian (axian0417@gmail.com)
 *
 * @brief FTP server: a single instance driven by ftp_server(), never waits.
 * @details Serves the files below a mount point through the POSIX file API and the clients
 *          through POSIX sockets (ftp_sock). Each poll accepts connections, reads one
 *          command line per client (MSG_PEEK, so lines split by TCP or pipelined commands
 *          are handled without a buffer per client) and advances the transfers in small
 *          steps. Paths are resolved into a per-client argument path and confined to the
 *          mount point; the working directory changes only with CWD/CDUP. Passive data
 *          connections (PASV, EPSV), ABOR during transfers, REST, UTF-8 names, optional
 *          MFMT. Four roles with fixed permissions; credentials can change at run time
 *          (seqlock, applied to new logins, sessions of a changed role signed out). The API
 *          may run in another thread: enable/disable record a desired state that
 *          ftp_server() applies. Client buffers come from the heap or, with
 *          FTP_STATIC_BUFFERS, from static storage. Leveled logs never contain passwords.
 *
 * @copyright Copyright (c) 2026 A Xian
 *
 */

#include "ftp_server.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include "assert.h"
#include "stdio.h"
#include "stdarg.h"
#include "ctype.h"
#include "inttypes.h"
#include "stdatomic.h"
#include "fcntl.h"
#include "unistd.h"
#include "dirent.h"
#include "sys/stat.h"
#if FTP_MFMT
#include "utime.h"
#endif
#include "ftp_sock.h"

/* close/read/write here are file operations, they must not reach the socket layer */
#if defined(close) || defined(read) || defined(write)
#error "close/read/write are redefined as macros (e.g. LWIP_POSIX_SOCKETS_IO_NAMES = 1)"
#endif

#define MAX(a, b)                               ((a) > (b) ? (a) : (b))

/* Logs: tag, and the client prefix "#<slot> <ip> " */
#define FTP_LOG_TAG                             "ftp"
#define IP4_FMT                                 "%u.%u.%u.%u"
#define IP4_ARG(ip)                             (unsigned)(((ip) >> 24) & 0xFFU), (unsigned)(((ip) >> 16) & 0xFFU), \
                                                (unsigned)(((ip) >> 8) & 0xFFU), (unsigned)((ip) & 0xFFU)
#define CLI_FMT                                 "#%u " IP4_FMT " "
#define CLI_ARG(c)                              (unsigned)((c) - clients), IP4_ARG((c)->ip_addr)
/* Path relative to the virtual root, for the logs */
#define VPATH(path)                             ((path) + strlen(ftp_root))
/* Result of a file operation: "ok" or "failed (errno N)" ("%.*d" with precision 0 prints nothing for 0) */
#define RESULT_FMT                              "%s%.*d%s"
#define RESULT_ARG(ok, err)                     (ok) ? "ok" : "failed (errno ", (ok) ? 0 : 1, (ok) ? 0 : (err), (ok) ? "" : ")"
#define ROLE_NAME(r)                            (((r) == FTP_ROLE_ADMIN) ? "admin" : \
                                                 ((r) == FTP_ROLE_MAINTAINER) ? "maintainer" : \
                                                 ((r) == FTP_ROLE_USER) ? "user" : "guest")
#define CMD_SIZE                                (6)
#define UNIX_2000_01_01_00_00_00                (946684800)
#define UNIX_SECONDS_180_DAYS                   (15552000)
#define LOG_TRX_SIZE                            (10240) 
#define LISTING_BUFFER_MARGIN                   (64)
#define ALLOW_LIST                              (1 << 0)
#define ALLOW_RETR                              (1 << 1)
#define ALLOW_STOR                              (1 << 2)
#define ALLOW_DELETE                            (1 << 3)
#define ALLOW_RENAME                            (1 << 4)
#define ALLOW_MKDIR                             (1 << 5)
#define ALLOW_RMDIR                             (1 << 6)
#define ALLOW_ALL                               (0xFFFFFFFFU)
#define GUEST_ALLOW                             (ALLOW_LIST | ALLOW_RETR)  
#define USER_ALLOW                              (ALLOW_LIST | ALLOW_RETR | ALLOW_STOR)
#define MAINT_ALLOW                             (ALLOW_LIST | ALLOW_RETR | ALLOW_STOR | ALLOW_DELETE | ALLOW_RENAME)

typedef enum state
{
    DISABLED = 0,
    START,
    RUNNING,
} state_t;

typedef enum open
{
    NOTHING_OPEN = 0,
    FILE_OPEN,
    DIR_OPEN
} open_t;

typedef enum result
{
    OK = 0,
    CONTINUE,
    FAILED,
    CLOSED,                             /** Peer closed the connection */
} result_t;

typedef enum client_state
{
    DISCONNECTED = 0,
    LISTEN_FOR_DATA,
    DATA_CONNECTED,
    END_TRANSFER,
    CONTINUE_LISTING,
    CONTINUE_FILE_TX,
    CONTINUE_FILE_RX,
} client_state_t;

typedef struct client
{
    int fd;                             /** Valid while open_type == FILE_OPEN */
    DIR* dir;                           /** Valid while open_type == DIR_OPEN */
    open_t open_type;

    client_state_t state;
    ftp_role_t role;

    bool active;
    bool name_list;
    bool rx_discard;                    /** Dropping the rest of a line longer than cmd_buffer */
    bool epsv_all;                      /** After "EPSV ALL" only EPSV may open data connections */
    uint32_t rest_offset;               /** Set by REST, used by the next command only (RETR/STOR) */

    ftp_sock_t cmd_sd;                  /** command socket */
    ftp_sock_t data_sd;                 /** data socket */
    ftp_sock_t ldata_sd;                /** listen data socket */

    uint32_t ip_addr;                   /** Client IP */

    uint32_t time;
    uint32_t cmd_timeout;
    uint32_t data_timeout;
    int32_t data_count;                 /** Bytes in the data buffer */
    uint32_t data_sent;                 /** Bytes of the data buffer already sent */
    bool data_eof;                      /** No more data after the buffer */
    uint32_t data_total;
    uint32_t data_last;

    char* path;                         /** Working directory, changed by CWD/CDUP only */
    char* arg_path;                     /** Path of the command argument (resolve_path()),
                                            kept during the transfer; shares the allocation of path */
    uint8_t* data;

    char* ctrl_tx;                      /** Control replies not sent yet */
    uint32_t ctrl_len;
    bool ctrl_broken;                   /** Control connection lost, sign out */
} client_t;

typedef struct user
{
    const char* default_name;           /** Used until ftp_set_username() for the role */
    const char* default_pass;           /** Used until ftp_set_password() for the role */
    ftp_role_t role;
    uint32_t allowed_cmds;
} user_t;

typedef enum auth
{
    AUTH_OK = 0,
    AUTH_NO_USER,                       /** No role has this user name */
    AUTH_BAD_PASS,                      /** User name known, password differs */
    AUTH_BUSY,                          /** Credentials being changed, try again */
} auth_t;

/*
    Default texts of the codes sent without a message (RFC 959 4.2: "<code> <SP> <text> <CRLF>").
    Codes always sent with their own message are not listed.
*/
typedef struct reply_text
{
    uint16_t status;
    const char* text;
} reply_text_t;

/*---------------------------------------------------------------------------------------*/

/* The default passwords must fit the password buffers (Kconfig) */
_Static_assert(sizeof(FTP_USER_DEFAULT_PASSWORD) <= FTP_MAX_PASS_LEN, "FTP_USER_DEFAULT_PASSWORD longer than FTP_MAX_PASS_LEN");
_Static_assert(sizeof(FTP_MAINT_DEFAULT_PASSWORD) <= FTP_MAX_PASS_LEN, "FTP_MAINT_DEFAULT_PASSWORD longer than FTP_MAX_PASS_LEN");
_Static_assert(sizeof(FTP_ADMIN_DEFAULT_PASSWORD) <= FTP_MAX_PASS_LEN, "FTP_ADMIN_DEFAULT_PASSWORD longer than FTP_MAX_PASS_LEN");

static const user_t users[FTP_ROLE_UNKNOWN] =
{
    [FTP_ROLE_GUEST] =
    { "guest", "", FTP_ROLE_GUEST, GUEST_ALLOW },

    [FTP_ROLE_USER] =
    { "user", FTP_USER_DEFAULT_PASSWORD, FTP_ROLE_USER, USER_ALLOW },

    [FTP_ROLE_MAINTAINER] =
    { "maint", FTP_MAINT_DEFAULT_PASSWORD, FTP_ROLE_MAINTAINER, MAINT_ALLOW },

    [FTP_ROLE_ADMIN] =
    { "admin", FTP_ADMIN_DEFAULT_PASSWORD, FTP_ROLE_ADMIN, ALLOW_ALL },
};

/*
    Credentials set by ftp_set_username()/ftp_set_password() (one copy, no defaults in RAM).
    Seqlock with one writer (the API thread): cred_seq is odd while the API writes.
    authenticate() compares under the seqlock and reports AUTH_BUSY if a write overlapped.
    The strings are atomic bytes (relaxed load/store): a compare racing with a write is
    not a data race, its result is discarded by the sequence check.
*/
typedef struct cred
{
    atomic_uint name_gen;               /** 0: default_name, else name[] (incremented per set) */
    atomic_uint pass_gen;               /** 0: default_pass, else pass[] */
    _Atomic char name[FTP_MAX_USER_LEN];
    _Atomic char pass[FTP_MAX_PASS_LEN];
} cred_t;

static cred_t creds[FTP_ROLE_UNKNOWN];
static atomic_uint cred_seq;

/* ftp_server() only: generations already seen, a change signs out the role's clients */
static unsigned int cred_seq_seen;
static unsigned int name_gen_seen[FTP_ROLE_UNKNOWN];
static unsigned int pass_gen_seen[FTP_ROLE_UNKNOWN];

/*
    One command line at a time, shared by all clients (ftp_server() is sequential).
    A line split by TCP or several lines in one segment stay in the socket until
    read_command_line() takes them one by one (MSG_PEEK, no buffer per client).
 */
static char cmd_buffer[FTP_CMD_BUFFER_SIZE + CMD_SIZE] = { 0 };


/* listening command socket descriptor */
static ftp_sock_t lc_sd = FTP_SOCK_INIT;

/*
    Threads: ftp_server() may run in another thread than the API.
    - ftp_init/deinit/reset only before ftp_server() starts or after it stops (lifecycle).
    - Other API: one thread at a time. ftp_enable/disable only set want_enable,
      ftp_server() changes state to match it. state is written by ftp_server() only
      (and by the lifecycle functions). Credentials: see cred_t.
      Only atomic load/store are used (no
      read-modify-write), so cores without exclusive instructions need no libatomic.
*/
static bool inited;                     /** Lifecycle only, not changed while ftp_server() runs */
static atomic_bool want_enable;
static _Atomic uint32_t ftp_log_ms;     /** Milliseconds from pulse_1ms, time of the default log output */
static bool start_error_logged;         /** Listen failure logged once until the server starts */
static _Atomic state_t state;
static _Atomic ftp_unix_time_t get_unix_time;

static char ftp_root[FTP_FILE_PATH_LEN];    /** Mount point, copied by ftp_init(), "" if not initialized */
static _Atomic uint32_t pasv_address;   /** Address announced by PASV to public clients, 0: local address */
static client_t clients[FTP_CLIENTS_MAX];

#if FTP_STATIC_BUFFERS
/* Buffers of client i: path (+ arg_path), data, ctrl_tx */
static char client_path_buf[FTP_CLIENTS_MAX][2 * FTP_FILE_PATH_LEN];
static uint8_t client_data_buf[FTP_CLIENTS_MAX][FTP_DATA_BUFFER_SIZE];
static char client_ctrl_buf[FTP_CLIENTS_MAX][FTP_CMD_BUFFER_SIZE];
#endif

static void send_reply(client_t* client, uint16_t status, const char* message);
static void send_replyf(client_t* client, uint16_t status, const char* fmt, ...);
static void flush_ctrl(client_t* client);
static void client_sign_out(client_t* client, const char* reason);
static void reset_transfer(client_t* client);

/*---------------------------------------------------------------------------------------*/

/**
 * @brief Converter string to all capital
 *
 * @param str string
 */
static void stoupper(char* str)
{
    while (str && *str != '\0')
    {
        *str = (char)toupper((int)(*str));
        str++;
    }
}

/**
 * @brief Pops the next argument from the command buffer, in place
 *
 * @param str Position in the command buffer, moved past the argument and one space
 * @param stop_on_space true: one word; false: the rest of the line
 *
 * @return The argument, terminated inside the command buffer ("" if none)
 *
 * @note Stops at the end of the line (CR/LF). "//" is reduced to "/".
 */
static char* pop_arg(char** str, bool stop_on_space)
{
    char* start = *str;
    char* rd = start;
    char* wr = start;

    while ((*rd != '\0') && (*rd != '\r') && (*rd != '\n') &&
        !(stop_on_space && (*rd == ' ')))
    {
        if (!((*rd == '/') && (wr > start) && (wr[-1] == '/')))
        {
            *wr++ = *rd;
        }

        rd++;
    }

    /* Next position before the terminator may overwrite the stop character */
    *str = (*rd == ' ') ? (rd + 1) : rd;
    *wr = '\0';

    return start;
}

/**
 * @brief Pops the command word from the string
 *
 * @param buf pop param to this buffer
 *
 * @return ftp_cmd_t
 */
static ftp_cmd_t pop_command(char** buf)
{
    ftp_cmd_t i;
    char* cmd;

    /* Telnet IP/Synch bytes (0xFF 0xF4 0xFF 0xF2) may precede ABOR, and spaces any command */
    while ((**buf != '\0') && (((unsigned char)**buf <= ' ') || ((unsigned char)**buf >= 0x7F)))
    {
        (*buf)++;
    }

    cmd = pop_arg(buf, true);

    stoupper(cmd);

    for (i = 0; (i < FTP_CMDS_MAX) && (strlen(cmd) < CMD_SIZE); i++)
    {
        if (!strcmp(cmd, ftp_cmd_name(i)))
        {
            return i;
        }
    }

    return FTP_CMD_NOT_SUPPORTED;
}

/**
 * @brief default time (unix type)
 *
 * @return time_t
 *
 * @note Standard C time(). ftp_enable_time() replaces it when the current
 *       time comes from elsewhere (RTC, network...). Used by LIST to choose
 *       between "Mon dd HH:MM" and "Mon dd YYYY".
 */
static time_t default_unix_time(void)
{
    return time(NULL);
}

/**
 * @brief Validate path is safe and within mount point
 *
 * @param base Base mount point path
 * @param path Full path to validate
 *
 * @return true if path is safe
 * @return false if path is potentially dangerous
 */
static bool is_path_safe(const char* base, const char* path)
{
    size_t base_len = strlen(base);

    /* Path must start with base mount point */
    if (strncmp(path, base, base_len) != 0)
    {
        return false;
    }

    /* Check for directory traversal patterns */
    if (strstr(path, "..") != NULL)
    {
        return false;
    }

    /* Reject the backslash as a path separator */
    if (strchr(path, '\\') != NULL)
    {
        return false;
    }

    /* Path should not end with multiple slashes */
    size_t len = strlen(path);
    if (len > 1 && path[len - 1] == '/' && path[len - 2] == '/')
    {
        return false;
    }

    return true;
}

/*---------------------------------------------------------------------------------------*/

/**
 * @brief listening socket
 *
 * @param sd socket data
 * @param port listening port, on all interfaces
 *
 * @return true if listening successful
 * @return false if listening failed
 */
static bool listening_socket(ftp_sock_t* sd, uint32_t port)
{
    /* open a socket for ftp data listen */
    *sd = ftp_sock_tcp();

    if (!ftp_sock_valid(*sd))
    {
        return false;
    }

    /* A blocking listening socket would stall the whole server loop in accept() */
    if (!ftp_sock_set_nonblocking(*sd))
    {
        FTP_LOGE("cannot set a socket non-blocking, check the platform fcntl() for sockets");
        ftp_sock_close(sd);
        return false;
    }

    /* bind the socket to a port number */
    if (!ftp_sock_listen(*sd, FTP_SOCK_IP_ANY, (uint16_t)port, FTP_CLIENTS_MAX))
    {
        ftp_sock_close(sd);
        return false;
    }

    return true;
}

/**
 * @brief Get the buffers of a client slot
 *
 * @param client Client object
 * @param index Slot index in clients[]
 *
 * @return true if the buffers are available
 * @return false if out of memory (heap only)
 */
static bool client_buffers_alloc(client_t* client, uint8_t index)
{
#if FTP_STATIC_BUFFERS
    client->path = client_path_buf[index];
    client->data = client_data_buf[index];
    client->ctrl_tx = client_ctrl_buf[index];
#else
    (void)index;

    client->path = (char*)malloc(2 * FTP_FILE_PATH_LEN);
    client->data = (uint8_t*)malloc(FTP_DATA_BUFFER_SIZE);
    client->ctrl_tx = (char*)malloc(FTP_CMD_BUFFER_SIZE);

    if ((client->path == NULL) || (client->data == NULL) || (client->ctrl_tx == NULL))
    {
        free(client->path);
        free(client->data);
        free(client->ctrl_tx);
        client->path = NULL;
        client->data = NULL;
        client->ctrl_tx = NULL;
        return false;
    }
#endif

    /* path and arg_path share one buffer */
    client->arg_path = client->path + FTP_FILE_PATH_LEN;

    return true;
}

/**
 * @brief Release the buffers of a client slot
 *
 * @param client Client object
 */
static void client_buffers_free(client_t* client)
{
#if !FTP_STATIC_BUFFERS
    free(client->path);
    free(client->data);
    free(client->ctrl_tx);
#endif

    client->path = NULL;
    client->arg_path = NULL;
    client->data = NULL;
    client->ctrl_tx = NULL;
}

/**
 * @brief Check any client connection
 *
 * @note If there is a client connected, it will be initialized
 *       at the same time.
 */
static void connection(void)
{
    uint8_t i;
    uint32_t ip = 0;

    ftp_sock_t new_sd = ftp_sock_accept(lc_sd, &ip);

    if (!ftp_sock_valid(new_sd))
    {
        return;
    }

    if (!ftp_sock_set_nonblocking(new_sd))
    {
        FTP_LOGW(IP4_FMT " refused: cannot set the connection non-blocking", IP4_ARG(ip));
        ftp_sock_close(&new_sd);
        return;
    }

    /*
        Several connections from the same IP are allowed (a client may open a second
        one for transfers). Closed connections free their slot at once (ctrl_broken),
        so a stale session does not need to be evicted here.
    */
    for (i = 0; i < FTP_CLIENTS_MAX; i++)
    {
        if (!clients[i].active)
        {
            if (!client_buffers_alloc(&clients[i], i))
            {
                break;
            }

            clients[i].active = true;
            clients[i].cmd_sd = new_sd;
            clients[i].ldata_sd = FTP_SOCK_INVALID;
            clients[i].data_sd = FTP_SOCK_INVALID;
            clients[i].ip_addr = ip;
            clients[i].cmd_timeout = 0;
            clients[i].role = FTP_ROLE_UNKNOWN;
            clients[i].open_type = NOTHING_OPEN;
            clients[i].fd = -1;
            clients[i].dir = NULL;
            clients[i].ctrl_len = 0;
            clients[i].ctrl_broken = false;
            clients[i].rx_discard = false;
            clients[i].epsv_all = false;
            clients[i].rest_offset = 0;

            strcpy(clients[i].path, ftp_root);

            FTP_LOGI(CLI_FMT "connected", CLI_ARG(&clients[i]));

            send_reply(&clients[i], FTP_220_SERVICE_READY, "FTP server");
            return;
        }
    }

    /* No free slot or out of memory, the client is not registered yet (best effort reply) */
    static const char unavailable[] = "421 Service not available\r\n";

    FTP_LOGW(IP4_FMT " refused: no free client slot or out of memory (421)", IP4_ARG(ip));

    ftp_sock_send(new_sd, unavailable, sizeof(unavailable) - 1);
    ftp_sock_close(&new_sd);
}

/**
 * @brief Check any client connection from data socket
 *
 * @param ld_sd Opened listening data socket
 * @param d_sd Opened data socket
 *
 * @return result_t, OK = client connection from data socket
 *
 * @note If client uses PASV command enter the passive mode, you need
 *       use listening_socket() create listening data socket and then
 *       use this function to wait for the client to connect.
 */
static result_t data_socket_connection(ftp_sock_t ld_sd, ftp_sock_t* d_sd)
{
    (*d_sd) = ftp_sock_accept(ld_sd, NULL);

    if (!ftp_sock_valid(*d_sd))
    {
        if (ftp_sock_would_block())
        {
            return CONTINUE;
        }

        return FAILED;
    }

    if (!ftp_sock_set_nonblocking(*d_sd))
    {
        ftp_sock_close(d_sd);
        return FAILED;
    }

    return OK;
}

/**
 * @brief Send what the socket accepts now, never waits
 *
 * @param sd Socket data
 * @param data Data to send
 * @param size Data size
 * @param sent Bytes sent (output)
 *
 * @return result_t OK all sent, CONTINUE socket buffer full, FAILED connection error
 */
static result_t send_some(ftp_sock_t sd, const uint8_t* data, uint32_t size, uint32_t* sent)
{
    *sent = 0;

    if (!ftp_sock_valid(sd))
    {
        return FAILED;
    }

    while (*sent < size)
    {
        int len = ftp_sock_send(sd, (data + *sent), (size - *sent));

        if (len > 0)
        {
            *sent += (uint32_t)len;
        }
        else if ((len < 0) && ftp_sock_would_block())
        {
            return CONTINUE;
        }
        else
        {
            return FAILED;
        }
    }

    return OK;
}

/**
 * @brief Queue a control reply and send as much as the socket takes now
 *
 * @param client Client object
 * @param data Reply
 * @param size Reply size
 *
 * @note The rest is sent by flush_ctrl() in the next rounds.
 *       If the queue overflows or the connection fails, the client is signed out.
 */
static void send_ctrl(client_t* client, const char* data, uint32_t size)
{
    if (client->ctrl_broken || (client->ctrl_tx == NULL))
    {
        return;
    }

    if (size > (FTP_CMD_BUFFER_SIZE - client->ctrl_len))
    {
        FTP_LOGW(CLI_FMT "control reply queue overflow, signing out", CLI_ARG(client));
        client->ctrl_broken = true;
        return;
    }

    memcpy(client->ctrl_tx + client->ctrl_len, data, size);
    client->ctrl_len += size;
    flush_ctrl(client);
}

/**
 * @brief Send the queued control replies, never waits
 *
 * @param client Client object
 */
static void flush_ctrl(client_t* client)
{
    uint32_t sent = 0;

    if (client->ctrl_len == 0)
    {
        return;
    }

    if (send_some(client->cmd_sd, (const uint8_t*)client->ctrl_tx, client->ctrl_len, &sent) == FAILED)
    {
        client->ctrl_broken = true;
        return;
    }

    memmove(client->ctrl_tx, client->ctrl_tx + sent, client->ctrl_len - sent);
    client->ctrl_len -= sent;
}

/**
 * @brief Send the rest of the data buffer on the data connection, never waits
 *
 * @param client Client object
 *
 * @return result_t OK buffer sent, CONTINUE try again next round, FAILED connection error
 */
static result_t send_data_buffer(client_t* client)
{
    uint32_t sent = 0;
    result_t result = send_some(client->data_sd,
        client->data + client->data_sent,
        (uint32_t)client->data_count - client->data_sent, &sent);

    if (sent > 0)
    {
        client->data_sent += sent;
        client->data_total += sent;
        client->data_timeout = 0;

        if ((client->data_total - client->data_last) > LOG_TRX_SIZE)
        {
            client->data_last = client->data_total;
            FTP_LOGV(CLI_FMT "sent %" PRIu32 " bytes", CLI_ARG(client), client->data_total);
        }
    }

    return result;
}

/**
 * @brief Receive data from the socket
 *
 * @param sd Socket data
 * @param data Data buffer
 * @param size Size you want to receive
 * @param actual_size actual size from receive
 *
 * @return result_t
 */
static result_t receive_data(ftp_sock_t sd, uint8_t* data, uint32_t size, int32_t* actual_size)
{
    if (!ftp_sock_valid(sd))
    {
        return FAILED;
    }

    int len = ftp_sock_recv(sd, data, size);

    if (len > 0)
    {
        (*actual_size) = (int32_t)len;
        return OK;
    }

    (*actual_size) = 0;

    if (len == 0)
    {
        return CLOSED;
    }
    else if (ftp_sock_would_block())
    {
        return CONTINUE;
    }

    return FAILED;
}

/*---------------------------------------------------------------------------------------*/

/**
 * @brief Close file or directory
 *
 * @param client Client object
 */
static void close_files_dir(client_t* client)
{
    if (client->open_type == FILE_OPEN)
    {
        close(client->fd);
        client->fd = -1;
    }
    else if (client->open_type == DIR_OPEN)
    {
        closedir(client->dir);
        client->dir = NULL;
    }

    client->open_type = NOTHING_OPEN;
}

/**
 * @brief Open file
 *
 * @param client Client object
 * @param flags open() flags
 *
 * @return true if file is opened
 * @return false if file can't open
 */
static bool open_file(client_t* client, int flags)
{
    close_files_dir(client);

    client->fd = open(client->arg_path, flags, 0666);

    if (client->fd >= 0)
    {
        client->open_type = FILE_OPEN;
    }

    FTP_LOGD(CLI_FMT "open \"%s\" " RESULT_FMT, CLI_ARG(client), VPATH(client->arg_path),
        RESULT_ARG(client->fd >= 0, errno));

    return (client->fd >= 0);
}

/**
 * @brief Open directory
 *
 * @param client Client object
 *
 * @return true if directory is opened
 * @return false if directory can't open
 */
static bool open_directory(client_t* client)
{
    close_files_dir(client);

    client->dir = opendir(client->arg_path);

    if (client->dir != NULL)
    {
        client->open_type = DIR_OPEN;
    }

    FTP_LOGD(CLI_FMT "open directory \"%s\" " RESULT_FMT, CLI_ARG(client), VPATH(client->arg_path),
        RESULT_ARG(client->dir != NULL, errno));

    return (client->dir != NULL);
}

/**
 * @brief Read file
 *
 * @param client Client object
 * @param data Data buffer
 * @param size Size you want to read
 *
 * @return int32_t -1 = read error, other = actual size of read
 *
 * @note POSIX allows a short read before the end of the file, so read until
 *       the buffer is full or read() returns 0. A count below size therefore
 *       means end of file (the caller relies on this).
 */
static int32_t read_file(client_t* client, uint8_t* data, uint32_t size)
{
    uint32_t total = 0;

    while (total < size)
    {
        ssize_t n = read(client->fd, data + total, size - total);

        if (n < 0)
        {
            close_files_dir(client);
            return -1;
        }

        if (n == 0)
        {
            close_files_dir(client);
            break;
        }

        total += (uint32_t)n;
    }

    return (int32_t)total;
}

/**
 * @brief write file
 *
 * @param client Client object
 * @param data Data you want to write
 * @param size Size of the data
 *
 * @return int32_t -1 = write error, other = actual size of write
 *
 * @note A short write on a regular file means the medium is full, it is an error.
 */
static int32_t write_file(client_t* client, uint8_t* data, uint32_t size)
{
    ssize_t n = write(client->fd, data, size);

    if (n != (ssize_t)size)
    {
        close_files_dir(client);
        return -1;
    }

    return (int32_t)n;
}

/**
 * @brief format list item for the LIST
 *
 * @param client Client object
 * @param buf Buffer of the format
 * @param size Buffer size
 * @param name Entry name
 * @param st Entry status (not used for NLST)
 *
 * @return uint32_t Size of a item, 0 if it does not fit
 */
static uint32_t format_list_item(client_t* client, char* buf, uint32_t size,
    const char* name, const struct stat* st)
{
    int add_size;

    if (client->name_list)
    {
        /* Just want the file name */
        add_size = snprintf(buf, size, "%s\r\n", name);
    }
    else
    {
        time_t time_val = st->st_mtime;
        time_t time_now = atomic_load(&get_unix_time)();
        char str_time[32];
        const char* type = S_ISDIR(st->st_mode) ? "d" : "-";

        /* No valid time source */
        if (time_now < UNIX_2000_01_01_00_00_00)
        {
            time_now = UNIX_2000_01_01_00_00_00;
        }

        /* If over 180 days, only show year and date */
        if ((time_val + UNIX_SECONDS_180_DAYS) < time_now)
        {
            strftime(str_time, sizeof(str_time), "%b %d %Y", localtime(&time_val));
        }
        else
        {
            strftime(str_time, sizeof(str_time), "%b %d %H:%M", localtime(&time_val));
        }

        add_size = snprintf(buf, size,
            "%srw-rw-rw- 1 root root %9lu %s %s\r\n",
            type, (unsigned long)st->st_size, str_time, name);
    }

    if ((add_size < 0) || ((uint32_t)add_size >= size))
    {
        FTP_LOGD(CLI_FMT "list entry \"%s\" too long for the buffer, skipped", CLI_ARG(client), name);

        return 0;
    }

    return (uint32_t)add_size;
}

/**
 * @brief List files and directories under a directory
 *
 * @param client Client object
 * @param size Size of the data string for the LIST
 *
 * @return result_t
 *
 * @note The entry name is appended to client->arg_path for stat() and removed
 *       again. NLST needs only the name, no stat().
 */
static result_t list_directory(client_t* client, uint32_t* size)
{
    uint32_t next = 0;
    uint8_t list_count = 0;
    result_t result = CONTINUE;
    size_t dir_len = strlen(client->arg_path);

    /* read up to FTP_LIST_NUM directory items */
    while (((FTP_DATA_BUFFER_SIZE - next) > LISTING_BUFFER_MARGIN) &&
        (list_count < FTP_LIST_NUM))
    {
        struct stat st;
        struct dirent* entry;

        /* NULL with errno unchanged is the end of the directory */
        errno = 0;
        entry = readdir(client->dir);

        if (entry == NULL)
        {
            if (errno != 0)
            {
                result = FAILED;
                FTP_LOGW(CLI_FMT "reading directory \"%s\" failed (%d)", CLI_ARG(client),
                    VPATH(client->arg_path), errno);
            }
            else
            {
                result = OK;
                FTP_LOGV(CLI_FMT "end of directory", CLI_ARG(client));
            }
            break;
        }

        /* Ignore . or .. entry */
        if ((strcmp(entry->d_name, ".") == 0) ||
            (strcmp(entry->d_name, "..") == 0))
        {
            continue;
        }

        if (!client->name_list)
        {
            int n = snprintf(client->arg_path + dir_len, FTP_FILE_PATH_LEN - dir_len,
                "/%s", entry->d_name);
            bool found = (n > 0) && ((size_t)n < (FTP_FILE_PATH_LEN - dir_len)) &&
                (stat(client->arg_path, &st) == 0);

            client->arg_path[dir_len] = '\0';

            /* Name too long for the path buffer, or removed meanwhile */
            if (!found)
            {
                FTP_LOGD(CLI_FMT "list: skip \"%s\" (name too long or removed)", CLI_ARG(client), entry->d_name);
                continue;
            }
        }

        next += format_list_item(client, (char*)(client->data + next),
            (FTP_DATA_BUFFER_SIZE - next), entry->d_name, &st);

        /* add the entry to the list */
        FTP_LOGV(CLI_FMT "list \"%s\"", CLI_ARG(client), entry->d_name);

        list_count++;
    }

    if (result == OK)
    {
        close_files_dir(client);
    }

    (*size) = next;

    return result;
}

/**
 * @brief Close file or directory
 *
 * @param client Client object
 */
static void close_fs(client_t* client)
{
    close_files_dir(client);
}

/*---------------------------------------------------------------------------------------*/

/**
 * @brief Send a reply with a formatted message
 *
 * @param client Client object
 * @param status reply code
 * @param fmt printf format of the message
 *
 * @note Formatted straight into the reply queue (no intermediate buffer).
 *       If "status" is 221, 550..., it will be close socket and fs too
 */
static void send_replyf(client_t* client, uint16_t status, const char* fmt, ...)
{
    int size = 0;

    if (!client->ctrl_broken && (client->ctrl_tx != NULL))
    {
        uint32_t room = FTP_CMD_BUFFER_SIZE - client->ctrl_len;
        char* tail = client->ctrl_tx + client->ctrl_len;
        int head = snprintf(tail, room, "%u ", status);
        int text = -1;
        va_list ap;

        if ((head > 0) && ((uint32_t)head < room))
        {
            va_start(ap, fmt);
            text = vsnprintf(tail + head, room - (uint32_t)head, fmt, ap);
            va_end(ap);
        }

        /* "<code> <text>\r\n" and the terminator of snprintf */
        if ((text >= 0) && ((uint32_t)(head + text + 2) < room))
        {
            size = head + text + 2;
            tail[size - 2] = '\r';
            tail[size - 1] = '\n';
        }
        else if ((client->ctrl_len > 0) || (room < 6))
        {
            /* Does not fit behind the queued replies */
            FTP_LOGW(CLI_FMT "control reply queue overflow, signing out", CLI_ARG(client));
            client->ctrl_broken = true;
        }
        else
        {
            /* Longer than the whole buffer: cut the message, keep the line ending */
            size = (int)room - 1;
            tail[size - 2] = '\r';
            tail[size - 1] = '\n';
        }

        FTP_LOGD(CLI_FMT "-> %.*s", CLI_ARG(client), (size > 2) ? (size - 2) : 0, tail);

        client->ctrl_len += (uint32_t)size;
        flush_ctrl(client);
    }

    switch (status)
    {
        case 221:	/* Server closed connection */
        {
            /* Last chance for the queued replies (best effort, no waiting) */
            flush_ctrl(client);
            ftp_sock_close(&client->cmd_sd);
            ftp_sock_close(&client->data_sd);
            ftp_sock_close(&client->ldata_sd);
            close_fs(client);
            break;
        }
        case 426:	/* The connection has been closed, the transfer failed */
        case 451:	/* Requested action aborted, local error in processing */
        case 550:	/* The requested action failed, the file is invalid
                       (e.g. no such file found, unacceptable) */
        case 553:	/* The requested action failed, invalid file name */
        {
            ftp_sock_close(&client->data_sd);
            close_fs(client);
            break;
        }
        default: { break; }
    }
}

static const reply_text_t reply_texts[] =
{
    { FTP_150_FILE_STATUS_OK,               "Opening data connection" },
    { FTP_200_COMMAND_OK,                   "OK" },
    { FTP_221_GOODBYE,                      "Goodbye" },
    { FTP_226_TRANSFER_COMPLETE,            "Transfer complete" },
    { FTP_250_ACTION_COMPLETE,              "OK" },
    { FTP_331_USER_NAME_OK,                 "Password required" },
    { FTP_332_NEED_ACCOUNT,                 "Account required" },
    { FTP_350_PENDING_FURTHER_INFO,         "Pending" },
    { FTP_425_CANT_OPEN_DATA_CONNECTION,    "Can't open data connection" },
    { FTP_426_CONNECTION_CLOSED,            "Transfer aborted" },
    { FTP_451_LOCAL_ERROR,                  "Local error" },
    { FTP_501_SYNTAX_ERROR_IN_PARAMETERS,   "Bad argument" },
    { FTP_502_COMMAND_NOT_IMPLEMENTED,      "Not implemented" },
    { FTP_504_COMMAND_NOT_SUPPORTED,        "Parameter not supported" },
    { FTP_550_ACTION_NOT_TAKEN,             "Action not taken" },
    { FTP_553_FILE_NAME_NOT_ALLOWED,        "File name not allowed" },
};

/**
 * @brief Default text of a reply code
 *
 * @param status reply code
 *
 * @return Text, never empty
 */
static const char* reply_text(uint16_t status)
{
    size_t i;

    for (i = 0; i < sizeof(reply_texts) / sizeof(reply_texts[0]); i++)
    {
        if (reply_texts[i].status == status)
        {
            return reply_texts[i].text;
        }
    }

    return (status < 400U) ? "OK" : "Failed";
}

/**
 * @brief send reply
 *
 * @param client Client object
 * @param status reply code
 * @param message reply message, NULL or "" for the default text of the code
 */
static void send_reply(client_t* client, uint16_t status, const char* message)
{
    send_replyf(client, status, "%s", ((message != NULL) && (message[0] != '\0')) ? message : reply_text(status));
}

/**
 * @brief Resolve a command argument against the working directory
 *
 * @param client Client object
 * @param arg Argument: absolute ("/a/b", from the virtual root), relative ("a/b")
 *            or empty (the working directory)
 * @param out Result, FTP_FILE_PATH_LEN bytes (usually client->arg_path)
 *
 * @return true if the path fits and stays within the mount point
 * @return false otherwise (out is not usable)
 *
 * @note The working directory (client->path) is not changed.
 */
static bool resolve_path(client_t* client, const char* arg, char* out)
{
    const char* base = client->path;
    const char* sep = "/";
    size_t base_len;
    int len;

    if (arg[0] == '/')
    {
        base = ftp_root;

        while (*arg == '/')
        {
            arg++;
        }
    }

    base_len = strlen(base);

    if ((arg[0] == '\0') || ((base_len > 0) && (base[base_len - 1] == '/')))
    {
        sep = "";
    }

    len = snprintf(out, FTP_FILE_PATH_LEN, "%s%s%s", base, sep, arg);

    if ((len < 0) || (len >= FTP_FILE_PATH_LEN))
    {
        FTP_LOGD(CLI_FMT "path too long: \"%s\"", CLI_ARG(client), arg);
        return false;
    }

    if (!is_path_safe(ftp_root, out))
    {
        FTP_LOGW(CLI_FMT "path outside the root refused: \"%s\"", CLI_ARG(client), arg);
        return false;
    }

    return true;
}

/**
 * @brief Change the working directory to its parent (CDUP, CWD ..)
 *
 * @param client Client object
 *
 * @note Never goes above the mount point.
 */
static void cwd_parent(client_t* client)
{
    size_t len = strlen(client->path);

    /* Never leave the mount point */
    if (len <= strlen(ftp_root))
    {
        strcpy(client->path, ftp_root);
        return;
    }

    if (client->path[len - 1] == '/')
    {
        client->path[len - 1] = '\0';
        len--;

        if (len < strlen(ftp_root))
        {
            strcpy(client->path, ftp_root);
        }
    }
    else
    {
        while (len)
        {
            if (client->path[len - 1] == '/')
            {
                client->path[len - 1] = '\0';
                len--;
                break;
            }
            len--;
        }

        if (len < strlen(ftp_root))
        {
            strcpy(client->path, ftp_root);
        }
    }

    FTP_LOGD(CLI_FMT "cwd \"%s\"", CLI_ARG(client), VPATH(client->path));
}

/**
 * @brief Pop the path argument and resolve it into client->arg_path
 *
 * @param client Client object
 * @param buffer Buffer pointer of the param
 *
 * @return The argument as given (an empty argument is the working directory),
 *         NULL if the path is too long or leaves the mount point
 */
static const char* get_param_path(client_t* client, char** buffer)
{
    const char* arg = pop_arg(buffer, false);

    return resolve_path(client, arg, client->arg_path) ? arg : NULL;
}

/**
 * @brief Check a file path is not empty and does not end with '/'
 *
 * @param path Path
 *
 * @return true if path can name a file
 */
static bool check_file_path(const char* path)
{
    size_t len = strlen(path);

    return ((len > 0) && (path[len - 1] != '/'));
}

/**
 * @brief Compare an input with a credential string
 *
 * @param input User input
 * @param set Credential string set by the API (atomic bytes)
 * @param size Size of set
 * @param def Default credential
 * @param gen Generation of set, 0 = not set (use def)
 */
static bool cred_match(const char* input, const _Atomic char* set, size_t size,
    const char* def, unsigned int gen)
{
    size_t i;

    if (gen == 0)
    {
        return (strcmp(input, def) == 0);
    }

    for (i = 0; i < size; i++)
    {
        char c = atomic_load_explicit(&set[i], memory_order_relaxed);

        if (c != input[i])
        {
            return false;
        }

        if (c == '\0')
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Length of a credential string
 *
 * @param set Credential string set by the API (atomic bytes)
 * @param size Size of set
 * @param def Default credential
 * @param gen Generation of set, 0 = not set (use def)
 */
static size_t cred_len(const _Atomic char* set, size_t size, const char* def, unsigned int gen)
{
    size_t i;

    if (gen == 0)
    {
        return strlen(def);
    }

    for (i = 0; (i < size) && (atomic_load_explicit(&set[i], memory_order_relaxed) != '\0'); i++)
    {
    }

    return i;
}

/**
 * @brief Authenticate client role
 *
 * @param client Client object
 * @param user User id from client input
 * @param pass User password from client input
 * @param expected Length of the expected password for AUTH_BAD_PASS (for the log)
 *
 * @return AUTH_OK, AUTH_NO_USER, AUTH_BAD_PASS, or AUTH_BUSY if the credentials
 *         changed meanwhile
 */
static auth_t authenticate(client_t* client, const char* user, const char* pass, size_t* expected)
{
    uint8_t i;
    uint8_t match = FTP_ROLE_UNKNOWN;
    uint8_t name_match = FTP_ROLE_UNKNOWN;
    unsigned int seq = atomic_load_explicit(&cred_seq, memory_order_acquire);

    if (seq & 1U)
    {
        return AUTH_BUSY;
    }

    for (i = 0; i < FTP_ROLE_UNKNOWN; i++)
    {
        unsigned int name_gen = atomic_load_explicit(&creds[i].name_gen, memory_order_relaxed);
        unsigned int pass_gen = atomic_load_explicit(&creds[i].pass_gen, memory_order_relaxed);

        if (!cred_match(user, creds[i].name, FTP_MAX_USER_LEN, users[i].default_name, name_gen))
        {
            continue;
        }

        if (name_match == FTP_ROLE_UNKNOWN)
        {
            name_match = i;
            *expected = cred_len(creds[i].pass, FTP_MAX_PASS_LEN, users[i].default_pass, pass_gen);
        }

        if (cred_match(pass, creds[i].pass, FTP_MAX_PASS_LEN, users[i].default_pass, pass_gen))
        {
            match = i;
            break;
        }
    }

    /* The result is valid only if no write started meanwhile */
    atomic_thread_fence(memory_order_acquire);

    if (atomic_load_explicit(&cred_seq, memory_order_relaxed) != seq)
    {
        return AUTH_BUSY;
    }

    if (match == FTP_ROLE_UNKNOWN)
    {
        return (name_match == FTP_ROLE_UNKNOWN) ? AUTH_NO_USER : AUTH_BAD_PASS;
    }

    client->role = users[match].role;

    return AUTH_OK;
}

/**
 * @brief Check client role has permission to some operation
 *
 * @param client Client object
 * @param cmd_flag Command flag
 *
 * @return true if client has "cmd_flag" operation permission
 * @return false if client has not "cmd_flag" operation permission
 *
 * @note If result is false, it will be send 550 reply to client too.
 */
static bool has_permission(client_t* client, uint32_t cmd_flag)
{
    /* Permissions follow the role (FTP_ROLE_UNKNOWN: not logged in, none) */
    bool has = (client->role < FTP_ROLE_UNKNOWN) &&
        ((users[client->role].allowed_cmds & cmd_flag) != 0);

    if (!has)
    {
        send_reply(client, FTP_550_ACTION_NOT_TAKEN, "Permission denied");
    }

    return has;
}

/**
 * @brief Client sign out
 *
 * @param client Client object
 * @param reason Logged as "disconnected: <reason>" (NULL: no log)
 */
static void client_sign_out(client_t* client, const char* reason)
{
    if (!client)
    {
        return;
    }

    if (reason != NULL)
    {
        FTP_LOGI(CLI_FMT "disconnected: %s", CLI_ARG(client), reason);
    }

    close_fs(client);

    /* Close all sockets */
    ftp_sock_close(&client->cmd_sd);
    ftp_sock_close(&client->data_sd);
    ftp_sock_close(&client->ldata_sd);

    client_buffers_free(client);

    /* Reset the client structure */
    memset(client, 0, sizeof(client_t));
    client->cmd_sd = FTP_SOCK_INVALID;
    client->data_sd = FTP_SOCK_INVALID;
    client->ldata_sd = FTP_SOCK_INVALID;
    client->active = false;
}

#if FTP_MFMT
/**
 * @brief Parse "YYYYMMDDHHMMSS" (UTC) into a time_t
 *
 * @param str Time string
 * @param out Seconds since 1970-01-01 00:00:00 UTC
 *
 * @return true if the string is a valid time from 1970 on
 *
 * @note Standard C has no timegm(): days from the civil date (proleptic Gregorian).
 */
static bool parse_utc_time(const char* str, time_t* out)
{
    int v[6];
    const int len[6] = { 4, 2, 2, 2, 2, 2 };
    int i, j;
    long days;
    int y, m, era, yoe, doy, doe;

    if (strlen(str) != 14)
    {
        return false;
    }

    for (i = 0; i < 6; i++)
    {
        v[i] = 0;

        for (j = 0; j < len[i]; j++, str++)
        {
            if ((*str < '0') || (*str > '9'))
            {
                return false;
            }

            v[i] = (v[i] * 10) + (*str - '0');
        }
    }

    static const uint8_t month_days[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = ((v[0] % 4) == 0) && (((v[0] % 100) != 0) || ((v[0] % 400) == 0));

    /* year, month, day (no 31 February), hour, minute, second (60: leap second) */
    if ((v[0] < 1970) || (v[1] < 1) || (v[1] > 12) || (v[2] < 1) ||
        (v[2] > month_days[v[1] - 1]) || ((v[1] == 2) && (v[2] == 29) && !leap) ||
        (v[3] > 23) || (v[4] > 59) || (v[5] > 60))
    {
        return false;
    }

    y = v[0] - ((v[1] <= 2) ? 1 : 0);
    m = v[1];
    era = y / 400;
    yoe = y - (era * 400);
    doy = ((153 * (m + ((m > 2) ? -3 : 9))) + 2) / 5 + v[2] - 1;
    doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;
    days = ((long)era * 146097L) + doe - 719468L;

    *out = (time_t)((days * 86400L) + (v[3] * 3600L) + (v[4] * 60L) + v[5]);

    return true;
}
#endif

/**
 * @brief Private, loopback or link-local IPv4 address (not reachable from the internet)
 *
 * @param ip IPv4 address in host byte order
 */
static bool is_local_ip4(uint32_t ip)
{
    return (((ip & 0xFF000000U) == 0x0A000000U) ||     /* 10.0.0.0/8 */
        ((ip & 0xFFF00000U) == 0xAC100000U) ||     /* 172.16.0.0/12 */
        ((ip & 0xFFFF0000U) == 0xC0A80000U) ||     /* 192.168.0.0/16 */
        ((ip & 0xFF000000U) == 0x7F000000U) ||     /* 127.0.0.0/8 */
        ((ip & 0xFFFF0000U) == 0xA9FE0000U));      /* 169.254.0.0/16 */
}

/**
 * @brief Address announced by PASV
 *
 * @param client Client object
 * @param ip IPv4 address in host byte order
 *
 * @return true on success
 *
 * @note The interface the client reached, so every interface works without settings.
 *       Behind NAT a public client gets the address set by ftp_set_pasv_address()
 *       instead; a client of a local network keeps the interface address (no hairpin NAT).
 */
static bool pasv_ip(const client_t* client, uint32_t* ip)
{
    uint32_t public_ip = atomic_load(&pasv_address);

    if ((public_ip != 0U) && !is_local_ip4(client->ip_addr))
    {
        *ip = public_ip;
        return true;
    }

    return ftp_sock_local_ip(client->cmd_sd, ip);
}

/**
 * @brief Open the passive data port of the client (PASV, EPSV)
 *
 * @param client Client object
 * @param port Port opened, one per client slot
 *
 * @return true if the port listens
 */
static bool open_passive(client_t* client, uint16_t* port)
{
    *port = (uint16_t)(FTP_PASSIVE_DATA_PORT + (client - clients));

    /* Some clients send PASV several times very quickly */
    ftp_sock_close(&client->ldata_sd);
    ftp_sock_close(&client->data_sd);
    client->state = DISCONNECTED;

    if (!listening_socket(&client->ldata_sd, *port))
    {
        FTP_LOGW(CLI_FMT "passive port %u unavailable (in use?)", CLI_ARG(client), *port);
        return false;
    }

    client->data_timeout = 0;
    client->state = LISTEN_FOR_DATA;
    FTP_LOGV(CLI_FMT "passive port %u", CLI_ARG(client), *port);

    return true;
}

/**
 * @brief Is the line in cmd_buffer an ABOR command (Telnet bytes before it allowed)
 */
static bool line_is_abort(const char* line)
{
    while ((*line != '\0') && (((unsigned char)*line <= ' ') || ((unsigned char)*line >= 0x7F)))
    {
        line++;
    }

    return (toupper((unsigned char)line[0]) == 'A') && (toupper((unsigned char)line[1]) == 'B') &&
        (toupper((unsigned char)line[2]) == 'O') && (toupper((unsigned char)line[3]) == 'R') &&
        ((unsigned char)line[4] <= ' ');
}

/**
 * @brief Take one complete command line from the control socket into cmd_buffer
 *
 * @param client Client object
 * @param abort_only During a transfer: take the line only if it is ABOR, leave others queued
 *
 * @return true if cmd_buffer holds a line (terminated, without CR/LF)
 *
 * @note Never waits. The line is peeked first (MSG_PEEK) and removed only when it is
 *       complete, so an incomplete line stays in the socket until the rest arrives.
 *       CR, LF or CRLF end a line (an urgent LF sent with MSG_OOB may be missing).
 */
static bool read_command_line(client_t* client, bool abort_only)
{
    const int size = FTP_CMD_BUFFER_SIZE + CMD_SIZE - 1;
    int n = ftp_sock_peek(client->cmd_sd, cmd_buffer, (size_t)size);
    int end;
    int take;
    char term;

    if (n <= 0)
    {
        /* Peer closed or broke the control connection: free the slot */
        if ((n == 0) || !ftp_sock_would_block())
        {
            client->ctrl_broken = true;
        }

        return false;
    }

    for (end = 0; (end < n) && (cmd_buffer[end] != '\r') && (cmd_buffer[end] != '\n'); end++)
    {
    }

    if (end == n)
    {
        if (n < size)
        {
            /* Incomplete line: wait for the rest */
            return false;
        }

        /* No line end in a full buffer: drop it, one 500 per line */
        if (!abort_only)
        {
            ftp_sock_recv(client->cmd_sd, cmd_buffer, (size_t)n);

            if (!client->rx_discard)
            {
                send_reply(client, FTP_500_SYNTAX_ERROR, "Line too long");
                client->rx_discard = true;
            }
        }

        return false;
    }

    term = cmd_buffer[end];
    cmd_buffer[end] = '\0';

    if (abort_only && !line_is_abort(cmd_buffer))
    {
        return false;
    }

    take = end + 1;

    if ((term == '\r') && (take < n) && (cmd_buffer[take] == '\n'))
    {
        take++;
    }

    /* The bytes are known to be there: recv() returns them at once */
    if (ftp_sock_recv(client->cmd_sd, cmd_buffer, (size_t)take) != take)
    {
        client->ctrl_broken = true;
        return false;
    }

    cmd_buffer[end] = '\0';
    client->cmd_timeout = 0;

    /* The end of a line that was too long: nothing to execute */
    if (client->rx_discard)
    {
        client->rx_discard = false;
        return false;
    }

    /* An empty line (e.g. the LF of a CRLF split by TCP) is no command */
    return (end > 0);
}

/**
 * @brief Client command process
 *
 * @param client Client object
 */
static void client_command(client_t* client)
{
    int res;
    struct stat st;
    char* buf = (char*)cmd_buffer;

    /* During a transfer only ABOR is taken, other commands wait in the socket */
    bool in_transfer = (client->state > END_TRANSFER);

    if (in_transfer)
    {
        /* The transfer is the activity (data_timeout covers a stalled transfer) */
        client->cmd_timeout = 0;
    }
    else if (client->cmd_timeout > FTP_CMD_TIMEOUT_MS)
    {
        send_reply(client, FTP_421_SERVICE_NOT_AVAILABLE,
            "Timeout - closing control connection");
        client_sign_out(client, "command timeout");
        return;
    }

    /* One line per call; arguments are cut in place by pop_arg() */
    if (!read_command_line(client, in_transfer))
    {
        return;
    }

    /* buffer is moved as commands are being popped */
    ftp_cmd_t cmd = pop_command(&buf);

    /* RFC 3659: REST applies to the command that immediately follows it only */
    uint32_t rest = client->rest_offset;

    client->rest_offset = 0;

    if (((cmd != FTP_CMD_USER) && (cmd != FTP_CMD_PASS) &&
        (cmd != FTP_CMD_QUIT) && (cmd != FTP_CMD_FEAT) &&
        (cmd != FTP_CMD_OPTS) && (cmd != FTP_CMD_AUTH)) && (client->role == FTP_ROLE_UNKNOWN))
    {
        send_reply(client, FTP_332_NEED_ACCOUNT, NULL);
        return;
    }

    if ((cmd >= 0) && (cmd < FTP_CMDS_MAX))
    {
        if (cmd == FTP_CMD_PASS)
        {
            FTP_LOGD(CLI_FMT "<- PASS ****", CLI_ARG(client));
        }
        else
        {
            FTP_LOGD(CLI_FMT "<- %s%s%.*s", CLI_ARG(client), (cmd >= 0) ? ftp_cmd_name(cmd) : (char*)cmd_buffer,
                (strcspn(buf, "\r\n") > 0) ? " " : "", (int)strcspn(buf, "\r\n"), buf);
        }
    }

    switch (cmd)
    {
        case FTP_CMD_FEAT:
        {
            /* RFC 2389, one feature per line with a leading space */
            static const char feat[] =
                "211-Features:\r\n"
                " EPSV\r\n"
                " MDTM\r\n"
#if FTP_MFMT
                " MFMT\r\n"
#endif
                " REST STREAM\r\n"
                " SIZE\r\n"
                " UTF8\r\n"
                "211 End\r\n";

            send_ctrl(client, feat, sizeof(feat) - 1);
            break;
        }
        case FTP_CMD_OPTS:
        {
            /* RFC 2640: file names are passed to the file system unchanged,
               UTF-8 is always on (the file system must store UTF-8 names) */
            char* opt = pop_arg(&buf, false);

            stoupper(opt);

            if ((strcmp(opt, "UTF8 ON") == 0) || (strcmp(opt, "UTF8") == 0))
            {
                send_reply(client, FTP_200_COMMAND_OK, "UTF8 mode enabled");
            }
            else
            {
                send_reply(client, FTP_501_SYNTAX_ERROR_IN_PARAMETERS, NULL);
            }
            break;
        }
        case FTP_CMD_AUTH:
        {
            send_reply(client, FTP_504_COMMAND_NOT_SUPPORTED, "not-supported");
            break;
        }
        case FTP_CMD_SYST:
        {
            send_reply(client, FTP_215_SYSTEM_TYPE, "UNIX Type: L8");
            break;
        }
        case FTP_CMD_CDUP:
        {
            cwd_parent(client);
            send_reply(client, FTP_250_ACTION_COMPLETE, NULL);
            break;
        }
        case FTP_CMD_CWD:
        {
            char* arg = pop_arg(&buf, false);

            if (strlen(arg) > 0)
            {
                /* Reject the backslash as a path separator */
                if (strchr(arg, '\\') != NULL)
                {
                    FTP_LOGD(CLI_FMT "CWD refused (backslash): \"%s\"", CLI_ARG(client), arg);
                    send_reply(client, FTP_550_ACTION_NOT_TAKEN, "Invalid path");
                    break;
                }

                // Ignore . entry
                if (strcmp(arg, ".") == 0)
                {
                    send_reply(client, FTP_250_ACTION_COMPLETE, NULL);
                    break;
                }

                // Ignore .. entry
                if (strcmp(arg, "..") == 0)
                {
                    cwd_parent(client);
                    send_reply(client, FTP_250_ACTION_COMPLETE, NULL);
                    break;
                }
            }

            /* The working directory changes only if the target is a directory */
            if (!resolve_path(client, arg, client->arg_path))
            {
                send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
                break;
            }

            /* Mount point checked by ftp_init() (some file systems cannot stat a volume root) */
            if ((strcmp(client->arg_path, ftp_root) == 0) ||
                ((stat(client->arg_path, &st) == 0) && S_ISDIR(st.st_mode)))
            {
                size_t mp_len = strlen(ftp_root);
                size_t len = strlen(client->arg_path);

                /* Keep the working directory without a trailing '/' */
                while ((len > mp_len) && (client->arg_path[len - 1] == '/'))
                {
                    client->arg_path[--len] = '\0';
                }

                strcpy(client->path, client->arg_path);

                FTP_LOGD(CLI_FMT "cwd \"%s\"", CLI_ARG(client), VPATH(client->path));
                send_reply(client, FTP_250_ACTION_COMPLETE, NULL);
            }
            else
            {
                send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
            }
            break;
        }
        case FTP_CMD_PWD:
        case FTP_CMD_XPWD:
        {
            char* path = client->path;
            const char* mp = ftp_root;
            size_t mp_len = strlen(mp);

            if (strncmp(path, mp, mp_len) == 0)
            {
                path += mp_len;
            }

            if (*path == '\0')
            {
                path = "/";
            }

            /* RFC 959: the pathname is quoted */
            send_replyf(client, FTP_257_PATHNAME_CREATED, "\"%s\"", path);
            break;
        }
        case FTP_CMD_SIZE:
        {
            {
                if (!get_param_path(client, &buf))
                {
                    send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
                    break;
                }

                if ((stat(client->arg_path, &st) == 0) && !S_ISDIR(st.st_mode))
                {

                    send_replyf(client, FTP_213_FILE_STATUS, "%"PRIu32, (uint32_t)st.st_size);
                }
                else
                {
                    send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
                }
            }
            break;
        }
        case FTP_CMD_MDTM:
        {
            {
                if (!get_param_path(client, &buf))
                {
                    send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
                    break;
                }

                if ((stat(client->arg_path, &st) == 0) && !S_ISDIR(st.st_mode))
                {
                    /* RFC 3659: MDTM time is UTC */
                    struct tm* tm = gmtime(&st.st_mtime);

                    strftime((char*)client->data, FTP_DATA_BUFFER_SIZE,
                        "%Y%m%d%H%M%S", tm);

                    send_reply(client, FTP_213_FILE_STATUS, (char*)client->data);
                }
                else
                {
                    send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
                }
            }
            break;
        }
        case FTP_CMD_MFMT:
        {
#if FTP_MFMT
            /* draft-somers-ftp-mfxx: MFMT YYYYMMDDHHMMSS <path>, time in UTC like MDTM */
            const char* date_time = pop_arg(&buf, true);
            const char* arg;
            time_t mtime;
            struct utimbuf times;
            char stamp[16];

            if (!parse_utc_time(date_time, &mtime))
            {
                send_reply(client, FTP_501_SYNTAX_ERROR_IN_PARAMETERS, "Time format YYYYMMDDHHMMSS");
                break;
            }

            /* Changing a file counts as writing it */
            if (!has_permission(client, ALLOW_STOR))
            {
                break;
            }

            arg = get_param_path(client, &buf);

            if ((arg == NULL) || (arg[0] == '\0'))
            {
                send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
                break;
            }

            if (stat(client->arg_path, &st) != 0)
            {
                send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
                break;
            }

            /* Access time kept */
            times.actime = st.st_atime;
            times.modtime = mtime;
            res = utime(client->arg_path, &times);

            FTP_LOGD(CLI_FMT "MFMT \"%s\" " RESULT_FMT, CLI_ARG(client),
                VPATH(client->arg_path), RESULT_ARG(res == 0, errno));

            /* Reply the time actually stored (file systems may round it, FAT: 2 s) */
            if ((res != 0) || (stat(client->arg_path, &st) != 0))
            {
                send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
                break;
            }

            strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", gmtime(&st.st_mtime));
            send_replyf(client, FTP_213_FILE_STATUS, "Modify=%s; %s", stamp, arg);
#else
            send_reply(client, FTP_502_COMMAND_NOT_IMPLEMENTED, NULL);
#endif
            break;
        }
        case FTP_CMD_TYPE:
        {
            send_reply(client, FTP_200_COMMAND_OK, NULL);
            break;
        }
        case FTP_CMD_USER:
        {
            /* Kept in the data buffer until PASS */
            snprintf((char*)client->data, FTP_DATA_BUFFER_SIZE, "%s", pop_arg(&buf, true));

            send_reply(client, FTP_331_USER_NAME_OK, NULL);
            break;
        }
        case FTP_CMD_PASS:
        {
            const char* pass = pop_arg(&buf, true);

            size_t expected = 0;
            auth_t auth = authenticate(client, (char*)client->data, pass, &expected);

            /* The client only sees "Login incorrect", the log tells why (never the password) */
            if (auth == AUTH_OK)
            {
                FTP_LOGI(CLI_FMT "login \"%s\" ok (%s)", CLI_ARG(client), (char*)client->data,
                    ROLE_NAME(client->role));
                send_reply(client, FTP_230_USER_LOGGED_IN, "Login successful");
            }
            else if (auth == AUTH_NO_USER)
            {
                FTP_LOGI(CLI_FMT "login \"%s\" failed: unknown user", CLI_ARG(client), (char*)client->data);
                send_reply(client, FTP_530_NOT_LOGGED_IN, "Login incorrect");
            }
            else if (auth == AUTH_BAD_PASS)
            {
                FTP_LOGI(CLI_FMT "login \"%s\" failed: password mismatch (got %u chars, expected %u)",
                    CLI_ARG(client), (char*)client->data, (unsigned)strlen(pass), (unsigned)expected);
                send_reply(client, FTP_530_NOT_LOGGED_IN, "Login incorrect");
            }
            else
            {
                /* Credentials changed during the check (rare): RFC 959 421 closes the connection */
                send_reply(client, FTP_421_SERVICE_NOT_AVAILABLE, "Account being changed, try again");
                flush_ctrl(client);
                client_sign_out(client, "login during an account change");
            }
            break;
        }
        case FTP_CMD_PASV:
        {
            uint32_t ip_addr = 0;
            uint16_t port;

            /* RFC 2428: after EPSV ALL other data connection commands are refused */
            if (client->epsv_all)
            {
                send_reply(client, FTP_503_BAD_SEQUENCE, "Only EPSV after EPSV ALL");
                break;
            }

            if (pasv_ip(client, &ip_addr) && open_passive(client, &port))
            {
                send_replyf(client, FTP_227_PASSIVE_MODE, "Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
                    (unsigned)((ip_addr >> 24) & 0xFFU), (unsigned)((ip_addr >> 16) & 0xFFU),
                    (unsigned)((ip_addr >> 8) & 0xFFU), (unsigned)(ip_addr & 0xFFU),
                    (unsigned)(port >> 8), (unsigned)(port & 0xFFU));
            }
            else
            {
                send_reply(client, FTP_425_CANT_OPEN_DATA_CONNECTION, NULL);
            }
            break;
        }
        case FTP_CMD_EPSV:
        {
            /* RFC 2428: only the port is announced, the client uses the control connection address */
            char* arg = pop_arg(&buf, true);
            uint16_t port;

            stoupper(arg);

            if (strcmp(arg, "ALL") == 0)
            {
                client->epsv_all = true;
                send_reply(client, FTP_200_COMMAND_OK, "EPSV ALL ok");
            }
            else if ((arg[0] != '\0') && (strcmp(arg, "1") != 0))
            {
                /* 2 = IPv6, others unknown */
                send_reply(client, FTP_522_PROTOCOL_NOT_SUPPORTED, "Network protocol not supported, use (1)");
            }
            else if (open_passive(client, &port))
            {
                send_replyf(client, FTP_229_EXTENDED_PASSIVE_MODE,
                    "Entering Extended Passive Mode (|||%u|)", (unsigned)port);
            }
            else
            {
                send_reply(client, FTP_425_CANT_OPEN_DATA_CONNECTION, NULL);
            }
            break;
        }
        case FTP_CMD_LIST:
        case FTP_CMD_NLST:
        {
            if (get_param_path(client, &buf))
            {
                client->name_list = (cmd == FTP_CMD_LIST) ? false : true;
                reset_transfer(client);

                if (open_directory(client))
                {
                    client->state = CONTINUE_LISTING;
                    send_reply(client, FTP_150_FILE_STATUS_OK, NULL);
                }
                else
                {
                    send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
                }
            }
            else
            {
                send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
            }
            break;
        }
        case FTP_CMD_RETR:
        case FTP_CMD_STOR:
        case FTP_CMD_APPE:
        {
            int flags;
            uint32_t allow;
            client_state_t state;

            if (get_param_path(client, &buf))
            {
                reset_transfer(client);

                if (check_file_path(client->arg_path))
                {
                    if (cmd == FTP_CMD_RETR)
                    {
                        flags = O_RDONLY;
                        allow = ALLOW_RETR;
                        state = CONTINUE_FILE_TX;
                    }
                    else if (cmd == FTP_CMD_STOR)
                    {
                        /* Restarted upload: keep the part already stored */
                        flags = (rest > 0) ? (O_WRONLY | O_CREAT) : (O_WRONLY | O_CREAT | O_TRUNC);
                        allow = ALLOW_STOR;
                        state = CONTINUE_FILE_RX;
                    }
                    else if (cmd == FTP_CMD_APPE)
                    {
                        flags = O_WRONLY | O_CREAT | O_APPEND;
                        allow = ALLOW_STOR;
                        state = CONTINUE_FILE_RX;
                    }
                    else
                    {
                        client->state = END_TRANSFER;
                        send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
                        break;
                    }

                    if (!has_permission(client, allow))
                    {
                        break;
                    }

                    if (open_file(client, flags))
                    {
                        /* REST: continue at the offset, which must lie within the file (APPE ignores it) */
                        if ((rest > 0) && (cmd != FTP_CMD_APPE))
                        {
                            struct stat fst;

                            if ((fstat(client->fd, &fst) != 0) || ((off_t)rest > fst.st_size) ||
                                (lseek(client->fd, (off_t)rest, SEEK_SET) != (off_t)rest))
                            {
                                close_files_dir(client);
                                client->state = END_TRANSFER;
                                send_reply(client, FTP_554_INVALID_REST, "Invalid restart position");
                                break;
                            }

                            FTP_LOGD(CLI_FMT "restart at %" PRIu32, CLI_ARG(client), rest);
                        }

                        client->state = state;
                        send_reply(client, FTP_150_FILE_STATUS_OK, NULL);
                        break;
                    }
                }

                client->state = END_TRANSFER;
                send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
            }
            else
            {
                client->state = END_TRANSFER;
                send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
            }
            break;
        }
        case FTP_CMD_DELE:
        case FTP_CMD_RMD:
        {
            uint32_t allow = (cmd == FTP_CMD_DELE) ? ALLOW_DELETE : ALLOW_RMDIR;

            if (!has_permission(client, allow))
            {
                break;
            }

            if (get_param_path(client, &buf))
            {
                if (check_file_path(client->arg_path))
                {
                    /* DELE removes a file only, RMD an (empty) directory only (RFC 959) */
                    res = (cmd == FTP_CMD_DELE) ? unlink(client->arg_path) : rmdir(client->arg_path);

                    FTP_LOGD(CLI_FMT "%s \"%s\" " RESULT_FMT, CLI_ARG(client), ftp_cmd_name(cmd),
                        VPATH(client->arg_path), RESULT_ARG(res == 0, errno));

                    send_reply(client, (res == 0) ? 250 : 550, NULL);
                    break;
                }
            }
            send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
            break;
        }
        case FTP_CMD_MKD:
        {
            if (!has_permission(client, ALLOW_MKDIR))
            {
                break;
            }

            const char* arg = get_param_path(client, &buf);

            if (arg)
            {
                if (check_file_path(client->arg_path))
                {
                    res = mkdir(client->arg_path, 0777);

                    FTP_LOGD(CLI_FMT "MKD \"%s\" " RESULT_FMT, CLI_ARG(client),
                        VPATH(client->arg_path), RESULT_ARG(res == 0, errno));

                    if (res == 0)
                    {
                        /* RFC 959: 257 "<pathname>" created */
                        send_replyf(client, FTP_257_PATHNAME_CREATED, "\"%s\" created", arg);
                    }
                    else
                    {
                        send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
                    }
                    break;
                }
            }
            send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
            break;
        }
        case FTP_CMD_RNFR:
        {
            if (!has_permission(client, ALLOW_RENAME))
            {
                break;
            }

            if (get_param_path(client, &buf))
            {
                res = stat(client->arg_path, &st);

                if (res == 0)
                {
                    // save the path of the file to rename
                    strcpy((char*)client->data, client->arg_path);
                    send_reply(client, FTP_350_PENDING_FURTHER_INFO, NULL);
                }
                else
                {
                    send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
                }

                FTP_LOGD(CLI_FMT "RNFR \"%s\" " RESULT_FMT, CLI_ARG(client),
                    VPATH(client->arg_path), RESULT_ARG(res == 0, errno));

                break;
            }
            send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
            break;
        }
        case FTP_CMD_RNTO:
        {
            if (!has_permission(client, ALLOW_RENAME))
            {
                break;
            }

            if (get_param_path(client, &buf))
            {
                res = rename((const char*)client->data,
                    (const char*)client->arg_path);

                FTP_LOGD(CLI_FMT "RNTO \"%s\" " RESULT_FMT, CLI_ARG(client),
                    VPATH(client->arg_path), RESULT_ARG(res == 0, errno));

                send_reply(client, (res == 0) ? 250 : 550, NULL);
                break;
            }
            send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
            break;
        }
        case FTP_CMD_REST:
        {
            /* Stream mode: the marker is a byte offset */
            const char* arg = pop_arg(&buf, true);
            char* end = NULL;
            unsigned long offset = strtoul(arg, &end, 10);

            if ((arg[0] < '0') || (arg[0] > '9') || (*end != '\0') || (offset > UINT32_MAX))
            {
                send_reply(client, FTP_501_SYNTAX_ERROR_IN_PARAMETERS, NULL);
                break;
            }

            client->rest_offset = (uint32_t)offset;
            send_replyf(client, FTP_350_PENDING_FURTHER_INFO, "Restarting at %lu", offset);
            break;
        }
        case FTP_CMD_ABOR:
        {
            /* RFC 959: 426 for the aborted transfer, then 226 */
            if (in_transfer)
            {
                FTP_LOGI(CLI_FMT "transfer of \"%s\" aborted by the client", CLI_ARG(client),
                    VPATH(client->arg_path));
                send_reply(client, FTP_426_CONNECTION_CLOSED, "Transfer aborted");
                send_reply(client, FTP_226_TRANSFER_COMPLETE, "Abort successful");
            }
            else
            {
                send_reply(client, FTP_226_TRANSFER_COMPLETE, "No transfer to abort");
            }

            close_fs(client);
            ftp_sock_close(&client->data_sd);
            ftp_sock_close(&client->ldata_sd);
            client->state = DISCONNECTED;
            break;
        }
        case FTP_CMD_MODE:
        case FTP_CMD_STRU:
        {
            /* Only stream mode and file structure (RFC 959 minimum implementation) */
            char* arg = pop_arg(&buf, true);

            stoupper(arg);

            if (strcmp(arg, (cmd == FTP_CMD_MODE) ? "S" : "F") == 0)
            {
                send_reply(client, FTP_200_COMMAND_OK, NULL);
            }
            else
            {
                send_reply(client, FTP_504_COMMAND_NOT_SUPPORTED, NULL);
            }
            break;
        }
        case FTP_CMD_ALLO:
        {
            send_reply(client, FTP_202_SUPERFLUOUS, "No storage allocation necessary");
            break;
        }
        case FTP_CMD_NOOP:
        {
            client->cmd_timeout = 0;
            send_reply(client, FTP_200_COMMAND_OK, NULL);
            break;
        }
        case FTP_CMD_STAT:
        {
            char* msg = (char*)client->data; // Reuse data buffer for large response
            int len = 0;
            int connected_count = 0;
            uint32_t ip_addr = 0;

            /* The address this client reached, not the listen address (may be "any") */
            (void)ftp_sock_local_ip(client->cmd_sd, &ip_addr);

            for (int i = 0; i < FTP_CLIENTS_MAX; i++)
            {
                if (clients[i].active)
                {
                    connected_count++;
                }
            }

            len += snprintf(msg + len, FTP_DATA_BUFFER_SIZE - len,
                "211- FTP Server Status:\r\n"
                "      IP: %u.%u.%u.%u:%d\r\n"
                "   Mount: %s\r\n"
                " Clients: %d/%d\r\n",
                IP4_ARG(ip_addr),
                FTP_CMD_PORT,
                "/",
                connected_count,
                FTP_CLIENTS_MAX);

            len += snprintf(msg + len, FTP_DATA_BUFFER_SIZE - len, "211 End of status.\r\n");

            send_ctrl(client, msg, (uint32_t)len);
            break;
        }
        case FTP_CMD_QUIT:
        {
            send_reply(client, FTP_221_GOODBYE, NULL);
            client_sign_out(client, "quit");
            break;
        }
        default:
        {
            send_reply(client, FTP_502_COMMAND_NOT_IMPLEMENTED, NULL);
            break;
        }
    }

}

/**
 * @brief Reset the transfer bookkeeping before LIST/NLST/RETR/STOR/APPE
 *
 * @param client Client object
 */
static void reset_transfer(client_t* client)
{
    client->time = 0;
    client->data_timeout = 0;
    client->data_count = 0;
    client->data_sent = 0;
    client->data_eof = false;
    client->data_total = 0;
    client->data_last = 0;
}

/**
 * @brief Make sure the data connection is established before a transfer
 *
 * @param client Client object
 *
 * @return true if the data socket is connected
 *
 * @note The client may send the transfer command before connecting
 *       to the passive data port.
 */
static bool data_connection_ready(client_t* client)
{
    result_t result;

    if (ftp_sock_valid(client->data_sd))
    {
        return true;
    }

    result = data_socket_connection(client->ldata_sd, &client->data_sd);

    if (result == OK)
    {
        client->data_timeout = 0;
        return true;
    }

    if ((result == FAILED) || (client->data_timeout > FTP_DATA_TIMEOUT_MS))
    {
        FTP_LOGD(CLI_FMT "no data connection (PASV missing)", CLI_ARG(client));
        close_fs(client);
        send_reply(client, FTP_425_CANT_OPEN_DATA_CONNECTION, NULL);
        client->state = END_TRANSFER;
    }

    return false;
}

/**
 * @brief Client state process
 *
 * @param client Client object
 */
static void client_state(client_t* client)
{
    int32_t len;
    uint32_t size = 0;
    result_t result = OK;

    switch (client->state)
    {
        case LISTEN_FOR_DATA:
        {
            if (data_socket_connection(client->ldata_sd, &client->data_sd) == OK)
            {
                client->cmd_timeout = 0;
                client->data_timeout = 0;
                client->state = DATA_CONNECTED;

                FTP_LOGV(CLI_FMT "data connection opened", CLI_ARG(client));
            }
            else if (client->data_timeout > FTP_DATA_TIMEOUT_MS)
            {

                /* close the listening socket */
                ftp_sock_close(&client->ldata_sd);
                send_reply(client, FTP_421_SERVICE_NOT_AVAILABLE, "Data connection timeout");
                client_sign_out(client, "data connection not opened in time");
            }
            break;
        }
        case DATA_CONNECTED:
        {
            if (client->data_timeout > FTP_DATA_TIMEOUT_MS)
            {
                /* close the listening and the data socket */
                ftp_sock_close(&client->data_sd);
                ftp_sock_close(&client->ldata_sd);
                close_fs(client);
                client->state = DISCONNECTED;

                FTP_LOGW(CLI_FMT "transfer failed: data connection timeout", CLI_ARG(client));
            }
            break;
        }
        case END_TRANSFER:
        {
            if (ftp_sock_valid(client->ldata_sd))
            {
                ftp_sock_close(&client->ldata_sd);
            }

            if (ftp_sock_valid(client->data_sd))
            {
                ftp_sock_close(&client->data_sd);
            }

            client->state = DISCONNECTED;
            break;
        }
        case CONTINUE_LISTING:
        {
            if (!data_connection_ready(client))
            {
                break;
            }

            /* Previous chunk fully sent: build the next one */
            if ((uint32_t)client->data_count == client->data_sent)
            {
                if (client->data_eof)
                {
                    send_reply(client, FTP_226_TRANSFER_COMPLETE, NULL);
                    client->state = END_TRANSFER;
                    break;
                }

                result = list_directory(client, &size);

                if (result == FAILED)
                {
                    send_reply(client, FTP_451_LOCAL_ERROR, NULL);
                    client->state = END_TRANSFER;
                    break;
                }

                client->data_count = (int32_t)size;
                client->data_sent = 0;
                client->data_eof = (result == OK);
            }

            /* Send what the socket accepts now, the rest in the next rounds */
            if (send_data_buffer(client) == FAILED)
            {
                FTP_LOGW(CLI_FMT "listing failed: data connection error", CLI_ARG(client));
                send_reply(client, FTP_426_CONNECTION_CLOSED, NULL);
                client->state = END_TRANSFER;
            }
            else if (client->data_timeout > FTP_DATA_TIMEOUT_MS)
            {
                FTP_LOGW(CLI_FMT "listing failed: data connection timeout", CLI_ARG(client));
                send_reply(client, FTP_426_CONNECTION_CLOSED, NULL);
                client->state = END_TRANSFER;
            }

            client->cmd_timeout = 0;
            break;
        }
        case CONTINUE_FILE_TX:
        {
            if (!data_connection_ready(client))
            {
                break;
            }

            /* Previous block fully sent: read the next one */
            if ((uint32_t)client->data_count == client->data_sent)
            {
                if (client->data_eof)
                {
                    FTP_LOGI(CLI_FMT "sent \"%s\" %" PRIu32 " bytes in %" PRIu32 " ms", CLI_ARG(client),
                        VPATH(client->arg_path), client->data_total, client->time);

                    send_reply(client, FTP_226_TRANSFER_COMPLETE, NULL);
                    client->state = END_TRANSFER;
                    break;
                }

                client->data_count = read_file(client, client->data, FTP_DATA_BUFFER_SIZE);

                if (client->data_count < 0)
                {
                    send_reply(client, FTP_451_LOCAL_ERROR, NULL);
                    client->state = END_TRANSFER;
                    break;
                }

                client->data_sent = 0;
                client->data_eof = (client->data_count < FTP_DATA_BUFFER_SIZE);
            }

            /* Send what the socket accepts now, the rest in the next rounds */
            if (send_data_buffer(client) == FAILED)
            {
                FTP_LOGW(CLI_FMT "sending \"%s\" failed: %s", CLI_ARG(client), VPATH(client->arg_path),
                    "file read or connection error");
                send_reply(client, FTP_426_CONNECTION_CLOSED, NULL);
                client->state = END_TRANSFER;
            }
            else if (client->data_timeout > FTP_DATA_TIMEOUT_MS)
            {
                FTP_LOGW(CLI_FMT "sending \"%s\" failed: data connection timeout", CLI_ARG(client),
                    VPATH(client->arg_path));
                send_reply(client, FTP_426_CONNECTION_CLOSED, NULL);
                client->state = END_TRANSFER;
            }
            break;
        }
        case CONTINUE_FILE_RX:
        {
            if (!data_connection_ready(client))
            {
                break;
            }

            result = receive_data(client->data_sd, client->data,
                FTP_DATA_BUFFER_SIZE, &len);

            if (result == OK)
            {
                client->data_timeout = 0;

                client->data_count = write_file(client, client->data, (uint32_t)len);

                /* save received data to file */
                if (client->data_count == -1)
                {
                    send_reply(client, FTP_451_LOCAL_ERROR, NULL);
                    client->state = END_TRANSFER;
                    FTP_LOGW(CLI_FMT "writing \"%s\" failed (%d), medium full?", CLI_ARG(client),
                        VPATH(client->arg_path), errno);
                }
                else
                {
                    client->data_total += client->data_count;

                    if ((client->data_total - client->data_last) > LOG_TRX_SIZE)
                    {
                        client->data_last = client->data_total;
                        FTP_LOGV(CLI_FMT "received %" PRIu32 " bytes", CLI_ARG(client), client->data_total);
                    }
                }
            }
            else if (result == CONTINUE)
            {
                /* nothing received */
                if (client->data_timeout > FTP_DATA_TIMEOUT_MS)
                {
                    close_files_dir(client);
                    send_reply(client, FTP_426_CONNECTION_CLOSED, NULL);
                    client->state = END_TRANSFER;
                    FTP_LOGW(CLI_FMT "receiving \"%s\" failed: data connection timeout", CLI_ARG(client),
                        VPATH(client->arg_path));
                }
            }
            else if (result == CLOSED)
            {
                FTP_LOGI(CLI_FMT "received \"%s\" %" PRIu32 " bytes in %" PRIu32 " ms", CLI_ARG(client),
                    VPATH(client->arg_path), client->data_total, client->time);

                /* Peer closed the data connection: transfer completed */
                close_files_dir(client);
                send_reply(client, FTP_226_TRANSFER_COMPLETE, NULL);
                client->state = END_TRANSFER;
            }
            else
            {
                FTP_LOGW(CLI_FMT "receiving \"%s\" failed: data connection error (%d)", CLI_ARG(client),
                    VPATH(client->arg_path), errno);

                close_files_dir(client);
                send_reply(client, FTP_426_CONNECTION_CLOSED, NULL);
                client->state = END_TRANSFER;
            }
            break;
        }
        case DISCONNECTED:
        default: { break; }
    }
}

/*---------------------------------------------------------------------------------------*/

/**
 * @brief Sign out the clients of roles whose credentials changed
 *
 * @note Called by ftp_server() in every state, never waits. The new credentials
 *       themselves are used by authenticate() at once.
 */
static void cred_watch(void)
{
    ftp_role_t role;
    unsigned int seq = atomic_load_explicit(&cred_seq, memory_order_relaxed);

    /* Nothing set since the last call: one load per ftp_server() call */
    if (seq == cred_seq_seen)
    {
        return;
    }

    /* While a write is in progress (odd), scan again in the next call */
    if ((seq & 1U) == 0)
    {
        cred_seq_seen = seq;
    }

    for (role = 0; role < FTP_ROLE_UNKNOWN; role++)
    {
        unsigned int name_gen = atomic_load_explicit(&creds[role].name_gen, memory_order_relaxed);
        unsigned int pass_gen = atomic_load_explicit(&creds[role].pass_gen, memory_order_relaxed);
        uint8_t i;

        if ((name_gen == name_gen_seen[role]) && (pass_gen == pass_gen_seen[role]))
        {
            continue;
        }

        name_gen_seen[role] = name_gen;
        pass_gen_seen[role] = pass_gen;

        for (i = 0; i < FTP_CLIENTS_MAX; i++)
        {
            if (clients[i].active && (clients[i].role == role))
            {
                send_reply(&clients[i], FTP_421_SERVICE_NOT_AVAILABLE, "Account changed");
                flush_ctrl(&clients[i]);
                client_sign_out(&clients[i], "account changed");
            }
        }
    }
}

/**
 * @brief Request a credential change (API thread, the only writer)
 *
 * @param dst Requested string of the role
 * @param size Size of dst
 * @param src New value
 * @param gen Generation counter of dst
 */
static void cred_request(_Atomic char* dst, size_t size, const char* src, atomic_uint* gen)
{
    unsigned int seq = atomic_load_explicit(&cred_seq, memory_order_relaxed);
    size_t i;

    /* Odd: ftp_server() ignores the strings until the write is complete */
    atomic_store_explicit(&cred_seq, seq + 1U, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);

    for (i = 0; src[i] != '\0'; i++)
    {
        atomic_store_explicit(&dst[i], src[i], memory_order_relaxed);
    }

    for (; i < size; i++)
    {
        atomic_store_explicit(&dst[i], '\0', memory_order_relaxed);
    }

    atomic_store_explicit(gen, atomic_load_explicit(gen, memory_order_relaxed) + 1U,
        memory_order_relaxed);
    atomic_store_explicit(&cred_seq, seq + 2U, memory_order_release);
}

/**
 * @brief Stop the server: sign out all clients and release the command port
 *
 * @note Called by ftp_server() only, when ftp_disable() was requested.
 */
static void server_stop(void)
{
    uint8_t i;

    for (i = 0; i < FTP_CLIENTS_MAX; i++)
    {
        if (clients[i].active)
        {
            send_reply(&clients[i], FTP_221_GOODBYE, NULL);
            client_sign_out(&clients[i], "server disabled");
        }
    }

    /* Release the command port so that the server can be enabled again */
    ftp_sock_close(&lc_sd);

    atomic_store(&state, DISABLED);
    FTP_LOGI("stopped");
}

/*---------------------------------------------------------------------------------------*/

/**
 * @brief FTP initialization
 *
 * @param mount_point POSIX path of the directory served as the root, copied
 *
 * @return true if initialization is successful
 * @return false if initialization is failed
 *
 * @note Lifecycle: call before ftp_server() starts or after it stops.
 */
bool ftp_init(const char* mount_point)
{
    assert(mount_point);

    struct stat st;
    size_t mp_len = strlen(mount_point);

    ftp_deinit();

    /* The last character of mount_point can't be "/" */
    if ((mp_len == 0) || (mount_point[mp_len - 1] == '/'))
    {
        FTP_LOGE("mount point must not be empty or end with '/'");
        return false;
    }

    /* Every client path starts with it */
    if (mp_len >= FTP_FILE_PATH_LEN)
    {
        FTP_LOGE("mount point longer than FTP_FILE_PATH_LEN - 1 (%d)", FTP_FILE_PATH_LEN - 1);
        return false;
    }

    /* Fail here, not with 550 on every command: file system not mounted,
       a file system specific path instead of a POSIX one (e.g. a drive prefix "0:/..."),
       or the platform has no file syscalls */
    if ((stat(mount_point, &st) != 0) || !S_ISDIR(st.st_mode))
    {
        FTP_LOGE("mount point \"%s\" is not an accessible directory (%d)", mount_point, errno);
        return false;
    }

    memcpy(ftp_root, mount_point, mp_len + 1);
    lc_sd = FTP_SOCK_INVALID;
    inited = true;

    return true;
}

/**
 * @brief FTP deinitialization
 *
 * @return true if deinitialization is successful
 * @return false if deinitialization is failed
 *
 * @note Lifecycle: call before ftp_server() starts or after it stops.
 *       It will be sign out all client too.
 */
bool ftp_deinit(void)
{
    uint8_t i;

    inited = false;
    atomic_store(&want_enable, false);
    atomic_store(&state, DISABLED);

    /*
        Only active clients own sockets. The static initial value 0 is a valid
        descriptor (stdin) and must not be closed.
    */
    for (i = 0; i < FTP_CLIENTS_MAX; i++)
    {
        if (clients[i].active)
        {
            client_sign_out(&clients[i], NULL);
        }

        clients[i].cmd_sd = FTP_SOCK_INVALID;
        clients[i].data_sd = FTP_SOCK_INVALID;
        clients[i].ldata_sd = FTP_SOCK_INVALID;
    }

    ftp_sock_close(&lc_sd);
    ftp_root[0] = '\0';

    ftp_disable_time();

    return true;
}

/**
 * @brief FTP reset
 *
 * @note Lifecycle: call before ftp_server() starts or after it stops.
 *       It will be reinitialization, the server is disabled afterwards.
 */
void ftp_reset(void)
{
    char mount_point[FTP_FILE_PATH_LEN];
    const ftp_unix_time_t get_time = atomic_load(&get_unix_time);

    if (ftp_root[0] == '\0')
    {
        return;
    }

    /* ftp_init() clears ftp_root first */
    strcpy(mount_point, ftp_root);
    ftp_init(mount_point);
    ftp_enable_time(get_time);
}

/**
 * @brief FTP enable time function
 *
 * @param get_time function of the system time get
 */
void ftp_enable_time(const ftp_unix_time_t get_time)
{
    assert(get_time);

    atomic_store(&get_unix_time, get_time);
}

/**
 * @brief FTP disable time function
 *
 * @note LIST uses the standard C time()
 */
void ftp_disable_time(void)
{
    atomic_store(&get_unix_time, default_unix_time);
}

/**
 * @brief FTP set role username
 *
 * @param role Role
 * @param new_username new username
 *
 * @return true if the request is recorded
 * @return false if role is invalid or the name is empty or too long
 *
 * @note Any time, returns at once. New logins use it at once; ftp_server() signs
 *       out the clients logged in with this role in its next call.
 */
bool ftp_set_username(ftp_role_t role, const char* new_username)
{
    if ((role >= FTP_ROLE_UNKNOWN) || (new_username == NULL) ||
        (new_username[0] == '\0') || (strlen(new_username) >= FTP_MAX_USER_LEN))
    {
        return false;
    }

    cred_request(creds[role].name, FTP_MAX_USER_LEN, new_username, &creds[role].name_gen);

    return true;
}

/**
 * @brief FTP set role password
 *
 * @param role Role
 * @param new_password new password
 *
 * @return true if the request is recorded
 * @return false if role is invalid or the password is too long
 *
 * @note Any time, returns at once. New logins use it at once; ftp_server() signs
 *       out the clients logged in with this role in its next call. An empty password is allowed
 *       (the guest role has none).
 */
bool ftp_set_password(ftp_role_t role, const char* new_password)
{
    if ((role >= FTP_ROLE_UNKNOWN) || (new_password == NULL) ||
        (strlen(new_password) >= FTP_MAX_PASS_LEN))
    {
        return false;
    }

    cred_request(creds[role].pass, FTP_MAX_PASS_LEN, new_password, &creds[role].pass_gen);

    return true;
}

/**
 * @brief Set the address announced by PASV to public clients (NAT public address)
 *
 * @param ipv4 IPv4 address, NULL or "" for none
 *
 * @return true if set
 * @return false if ipv4 is not an IPv4 address (the previous address is kept)
 *
 * @note Any time, the next PASV uses it. Without it PASV announces the interface the
 *       client reached. Clients of a local network (private, loopback, link-local
 *       addresses) always get the interface address. EPSV announces no address.
 */
bool ftp_set_pasv_address(const char* ipv4)
{
    uint32_t ip = 0U;

    if ((ipv4 != NULL) && (ipv4[0] != '\0') && !ftp_sock_ip4_parse(ipv4, &ip))
    {
        FTP_LOGE("invalid passive address \"%s\"", ipv4);
        return false;
    }

    atomic_store(&pasv_address, ip);

    return true;
}

/**
 * @brief FTP enable
 *
 * @return true if the request is recorded
 * @return false if the server is not initialized
 *
 * @note Returns at once, ftp_server() starts the server.
 */
bool ftp_enable(void)
{
    if (!inited)
    {
        FTP_LOGE("ftp_enable(): not initialized (ftp_init() missing or failed)");
        return false;
    }

    atomic_store(&want_enable, true);

    return true;
}

/**
 * @brief FTP is enable or not
 *
 * @return true is enable (requested)
 * @return false is disable (requested)
 */
bool ftp_enabled(void)
{
    return atomic_load(&want_enable);
}

/**
 * @brief FTP disable
 *
 * @return true if the server was enabled (the request is recorded)
 * @return false if the server was already disabled
 *
 * @note Returns at once, ftp_server() signs out all clients and releases the port.
 */
bool ftp_disable(void)
{
    /* Load and store, no exchange: only the API thread writes want_enable */
    bool was_enabled = atomic_load(&want_enable);

    atomic_store(&want_enable, false);

    return was_enabled;
}

/**
 * @brief FTP server
 *
 * @param pulse_1ms 1ms Clock for FTP running
 */
void ftp_server(uint32_t pulse_1ms)
{
    bool want = atomic_load(&want_enable);

    /* Only ftp_server() writes it; the API thread reads it for its logs */
    atomic_store_explicit(&ftp_log_ms,
        atomic_load_explicit(&ftp_log_ms, memory_order_relaxed) + pulse_1ms, memory_order_relaxed);

    cred_watch();

    switch (atomic_load(&state))
    {
        case DISABLED:
        {
            if (want)
            {
                atomic_store(&state, START);
            }
            break;
        }
        case START:
        {
            if (!want)
            {
                atomic_store(&state, DISABLED);
                break;
            }

            /* listening command socket descriptor */
            if (listening_socket(&lc_sd, FTP_CMD_PORT))
            {
                atomic_store(&state, RUNNING);

                start_error_logged = false;
                FTP_LOGI("started on port %d, %d clients, root \"%s\"",
                    FTP_CMD_PORT, FTP_CLIENTS_MAX, ftp_root);
            }
            else
            {
                /* Retried every call: log once */
                if (!start_error_logged)
                {
                    FTP_LOGE("cannot listen on port %d, retrying", FTP_CMD_PORT);
                    start_error_logged = true;
                }
            }
            break;
        }
        case RUNNING:
        {
            uint8_t i;

            if (!want)
            {
                server_stop();
                break;
            }

            connection();

            for (i = 0; i < FTP_CLIENTS_MAX; i++)
            {
                if (clients[i].active)
                {
                    clients[i].time += pulse_1ms;
                    clients[i].cmd_timeout += pulse_1ms;
                    clients[i].data_timeout += pulse_1ms;

                    flush_ctrl(&clients[i]);
                    client_state(&clients[i]);
                    client_command(&clients[i]);

                    if (clients[i].active && clients[i].ctrl_broken)
                    {
                        client_sign_out(&clients[i], "connection closed");
                    }
                }
            }
            break;
        }
    }
}
