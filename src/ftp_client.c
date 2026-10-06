// SPDX-License-Identifier: MIT
/**
 * @file ftp_client.c
 * @author A Xian (axian0417@gmail.com)
 *
 * @brief FTP client: a non-blocking state machine driven by ftp_process().
 * @details Several clients may be registered; ftp_process() advances each of them and never
 *          waits. API calls only post a request (request slot with atomic sequence numbers)
 *          and may come from another thread; ftp_process() publishes the state for those
 *          threads. Host names are resolved with getaddrinfo() in the caller's thread or by
 *          a registered non-blocking resolver. Replies are collected per client and handed
 *          to the states one reply at a time, multi-line replies included. Transfers use
 *          passive mode; downloads and uploads read and write local files through the POSIX
 *          file API. ftp_abort() aborts a running transfer with ABOR. Events are reported
 *          through the registered callback.
 *
 * @copyright Copyright (c) 2026 A Xian
 *
 */

#include "ftp_client.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include "assert.h"
#include "stdio.h"
#include "stdarg.h"
#include "ctype.h"
#include "fcntl.h"
#include "unistd.h"
#include "ftp_sock.h"

/* close/read/write here are file operations, they must not reach the socket layer */
#if defined(close) || defined(read) || defined(write)
#error "close/read/write are redefined as macros (e.g. LWIP_POSIX_SOCKETS_IO_NAMES = 1)"
#endif

/* Port range */
#define PORT_MIN                                (1)
#define PORT_MAX                                (65535)

/* Logs: tag, and the client prefix "[<name>] " */
#define FTP_LOG_TAG                             "ftp-cli"
#define CNAME(client)                           ((client)->name[0] ? (client)->name : "ftp")

/* Milliseconds from pulse_1ms, time of the default log output (written by ftp_process()) */
static _Atomic uint32_t ftp_log_ms;

#define SAFE_STRCPY(dest, src) do { \
    strncpy((dest), (src), sizeof(dest) - 1); \
    (dest)[sizeof(dest) - 1] = '\0'; \
} while(0)

static ftp_client_t* client_list = NULL;

static const char* state_str[FTP_STATE_MAX] = 
{
    [FTP_STATE_IDLE]            = "Idle",
    [FTP_STATE_RESOLVING]       = "Resolving",
    [FTP_STATE_CONNECTING]      = "Connecting",
    [FTP_STATE_WAIT_GREETING]   = "Wait greeting",
    [FTP_STATE_WAIT_LOGIN]      = "Wait login",
    [FTP_STATE_READY]           = "Ready",
    [FTP_STATE_CONNECTED]       = "Connected",
    [FTP_STATE_PASV_REQUEST]    = "PASV request",
    [FTP_STATE_PASV_RESPONSE]   = "PASV response",
    [FTP_STATE_DATA_CONNECT]    = "Data connect",
    [FTP_STATE_SEND_COMMAND]    = "Sending command",
    [FTP_STATE_WAIT_RESPONSE]   = "Waiting response",
    [FTP_STATE_LIST]            = "Listing",
    [FTP_STATE_RETR]            = "Downloading",
    [FTP_STATE_STOR]            = "Uploading",
    [FTP_STATE_ABORT]           = "Aborting",
    [FTP_STATE_QUIT]            = "Quit",
    [FTP_STATE_ERROR]           = "Error",
};

/*------------------------------------------ Private API ------------------------------------------*/

/**
 * @brief Create non-blocking TCP socket
 *
 * @return ftp_sock_t FTP_SOCK_INVALID on failure
 */
static ftp_sock_t socket_create(void)
{
    ftp_sock_t s = ftp_sock_tcp();

    if (ftp_sock_valid(s) && !ftp_sock_set_nonblocking(s))
    {
        ftp_sock_close(&s);
    }

    return s;
}

/**
 * @brief Wait for a non-blocking connect without blocking
 *
 * @param s Socket
 *
 * @return int 1 connected, 0 in progress, -1 failed
 */
static int connect_poll(ftp_sock_t s)
{
    if (!ftp_sock_writable(s))
    {
        return 0;
    }

    return (ftp_sock_error(s) == 0) ? 1 : -1;
}

/**
 * @brief Close file
 * 
 * @param client FTP client object
 */
static void file_close(ftp_client_t* client)
{
    if (client->file_open)
    {
        close(client->file_fd);
        client->file_fd = -1;
        client->file_open = false;
    }
}

/**
 * @brief Abort the current data transfer and go back to ready state
 *
 * @param client FTP client object
 */
static void transfer_abort(ftp_client_t* client)
{
    ftp_sock_close(&client->data_sd);
    file_close(client);
    client->data_len = 0;
    client->mode = FTP_MODE_NONE;
    client->job_cmd = FTP_CMD_NOT_SUPPORTED;
    client->state = FTP_STATE_READY;
}

/**
 * @brief Parse single-line FTP response code (first 3 chars) if available
 * 
 * @param buf Input buffer
 * @param len Buffer length

 * @return int Parsed code on success, -1 on failure
 */
static int parse_code(const char* buf, size_t len)
{
    if (len < 3)
    {
        return -1;
    }

    if (!isdigit((unsigned char)buf[0]) ||
        !isdigit((unsigned char)buf[1]) ||
        !isdigit((unsigned char)buf[2]))
    {
        return -1;
    }

    char code_s[4] = { buf[0], buf[1], buf[2], '\0' };

    return atoi(code_s);
}

/**
 * @brief Parse PASV response: "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)."
 * 
 * @param resp Input buffer
 * @param ip Output IPv4 address (host byte order)
 * @param port Output port

 * @return int 0 on success, -1 on failure
 */
static int parse_pasv(const char* resp, uint32_t* ip, uint16_t* port)
{
    const char* p = strchr(resp, '(');
    int v[6];

    if (!p)
    {
        return -1;
    }

    if (sscanf(p + 1, "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    {
        return -1;
    }

    for (int i = 0; i < 6; i++)
    {
        if ((v[i] < 0) || (v[i] > 255))
        {
            return -1;
        }
    }

    *ip = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | (uint32_t)v[3];
    *port = (uint16_t)((v[4] << 8) | v[5]);

    return 0;
}

/**
 * @brief Parse URL directly into client struct
 * 
 * @param client FTP client object
 * @param url URL string
 * @param user Output user pointer
 * @param pass Output password pointer
 * @param host Output host pointer
 * @param port Output port pointer
 * @param path Output path pointer

 * @return bool true on success, false on failure
 */
static bool parse_url(ftp_client_t* client, const char* url, char** user, char** pass, char** host, uint16_t* port, char** path)
{
    if (strlen(url) >= FTP_DATA_BUFFER_SIZE)
    {
        return false;
    }

    strcpy(client->data_buf, url);
    char* p = client->data_buf;
    
    *user = NULL;
    *pass = NULL;
    *host = NULL;
    *path = NULL;
    *port = FTP_CMD_PORT;

    /* 1. Check scheme */
    if (strncmp(p, "ftp://", 6) == 0)
    {
        p += 6;
    }
    else
    {
        /* Only support ftp:// */
        return false; 
    }

    /* 2. Check for user:pass@ */
    char* at = strchr(p, '@');
    if (at)
    {
        *at = '\0';
        char* colon = strchr(p, ':');
        if (colon)
        {
            *colon = '\0';
            *user = p;
            *pass = colon + 1;
        }
        else
        {
            *user = p;
        }
        p = at + 1;
    }

    /* 3. Check for host:port and path */
    /* Find first slash for path */
    char* slash = strchr(p, '/');
    if (slash)
    {
        *slash = '\0';
        *path = slash + 1;
    }

    /* Check for port in host part */
    char* colon = strchr(p, ':');
    
    if (colon)
    {
        *colon = '\0';
        *host = p;

        unsigned long parsed_port = strtoul(colon + 1, NULL, 10);

        if ((parsed_port >= PORT_MIN) && 
            (parsed_port <= PORT_MAX))
        {
            *port = (uint16_t)parsed_port;
        }
        else
        {
            /* Invalid port, use default */
            *port = FTP_CMD_PORT;
        }
    }
    else
    {
        *host = p;
    }

    return true;
}

/**
 * @brief User callback
 * 
 * @param client FTP client object
 * @param event Event type
 * @param data Event data
 * @param len Event data length
 */
static void callback(ftp_client_t* client, ftp_events_t event, const void* data, size_t len)
{
    if (client->callback)
    {
        client->callback(client, event, (void*)data, len);
    }
}

/**
 * @brief Event callback
 * 
 * @param client FTP client object
 * @param event Event type
 * @param fmt Format string
 * @param ... Variable arguments
 */
static void event_callback(ftp_client_t* client, ftp_events_t event, const char* fmt, ...)
{
    va_list ap;

    if (fmt) 
    {
        memset(client->status, 0, sizeof(client->status));
        va_start(ap, fmt);
        vsnprintf(client->status, sizeof(client->status), fmt, ap);
        va_end(ap);
    } 
    else 
    {
        client->status[0] = '\0';
    }

    callback(client, event, client->status, strlen(client->status));

    if (event == FTP_ON_ERROR)
    {
        FTP_LOGW("[%s] %s", CNAME(client), client->status);
    }
    else
    {
        FTP_LOGI("[%s] %s", CNAME(client), client->status);
    }
}

/**
 * @brief Pop a complete line from buffer
 * 
 * @param buf Input buffer
 * @param len Buffer length
 * @param out_line Output line buffer
 * @param max_out Maximum output length

 * @return bool true on success, false on failure
 */
static bool pop_line(char* buf, size_t* len, char* out_line, size_t max_out) 
{
    /* Search for newline in the valid data range */
    char* newline = memchr(buf, '\n', *len);

    if (!newline)
    {
        return false;
    }

    size_t line_len = newline - buf;

    /* Safety: if we have \r before \n, exclude it from length for output */
    size_t out_len = line_len;

    if (out_len > 0 && buf[out_len - 1] == '\r')
    {
        out_len--;
    }

    /* Copy to output buffer (truncated if max_out is too small) */
    size_t copy_len = (out_len < max_out - 1) ? out_len : (max_out - 1);
    memcpy(out_line, buf, copy_len);
    out_line[copy_len] = '\0';

    /* Move remaining data to the front */
    /* consume \n as well */
    size_t consumed = line_len + 1; 
    size_t remaining = *len - consumed;

    if (remaining > 0)
    {
        memmove(buf, buf + consumed, remaining);
    }
    
    *len = remaining;
    
    buf[*len] = '\0';

    return true;
}

/**
 * @brief Pop the next complete reply, parse the code
 *
 * @param client FTP client object
 * @param line Output line buffer (the last line of the reply)
 * @param max_len Maximum output length
 *
 * @return int Parsed code, -1 for a line without a code, 0 if no complete reply yet
 *
 * @note RFC 959 multi-line replies ("ddd-text" ... "ddd text") are returned once, as
 *       their last line, so every state sees one reply per command.
 */
static int get_message(ftp_client_t* client, char* line, size_t max_len)
{
    while (pop_line(client->response_buf, &client->response_len, line, max_len))
    {
        size_t len = strlen(line);
        int code;

        if (len == 0)
        {
            continue;
        }

        code = parse_code(line, len);

        if (client->multi_line_code > 0)
        {
            /* Inside a multi-line reply: only "ddd " with the same code ends it */
            if ((code == client->multi_line_code) && ((line[3] == ' ') || (line[3] == '\0')))
            {
                client->multi_line_code = 0;
                return code;
            }

            FTP_LOGV("[%s] <- %s", CNAME(client), line);
            continue;
        }

        if ((code > 0) && (line[3] == '-'))
        {
            client->multi_line_code = code;
            FTP_LOGV("[%s] <- %s", CNAME(client), line);
            continue;
        }

        return code;
    }

    return 0;
}

/**
 * @brief Send a command to the FTP server
 * 
 * @param client FTP client object
 * @param cmd Command to send
 * @param arg Command arguments

 * @return int Number of bytes sent, or -1 on error
 */
static int send_command(ftp_client_t* client, ftp_cmd_t cmd, const char* arg)
{
    if (!ftp_sock_valid(client->cmd_sd))
    {
        return -1;
    }

    int len;
    memset(client->cmd_buf, 0, FTP_CMD_BUFFER_SIZE);
    client->last_resp_code = 0;

    if (arg && *arg)
    {
        len = snprintf(client->cmd_buf, FTP_CMD_BUFFER_SIZE, 
            "%s %s\r\n", ftp_cmd_name(cmd), arg); 
    }
    else
    {
        len = snprintf(client->cmd_buf, FTP_CMD_BUFFER_SIZE,  
            "%s\r\n", ftp_cmd_name(cmd));
    }

    /* Check for buffer overflow */
    if (len < 0 || len >= FTP_CMD_BUFFER_SIZE)
    {
        FTP_LOGE("[%s] command longer than the buffer, not sent", CNAME(client));
        return -1;
    }

    client->ticks = 0;
    client->multi_line_code = 0;

    /*
        A command is far smaller than the socket buffer: a partial send means the
        connection is broken. Handled at the end of this ftp_process() round, the
        caller may still change the state after this call.
    */
    if (ftp_sock_send(client->cmd_sd, client->cmd_buf, len) != len)
    {
        FTP_LOGW("[%s] %s not sent, disconnecting", CNAME(client), ftp_cmd_name(cmd));
        client->cmd_broken = true;
        return -1;
    }

    return 0;
}

/**
 * @brief Receive a command from the FTP server
 * 
 * @param client FTP client object
 */
static void recv_response(ftp_client_t* client)
{
    size_t space = sizeof(client->response_buf) - client->response_len - 1;

    /* Nothing to receive before the connection is established */
    if ((!ftp_sock_valid(client->cmd_sd)) || (client->state == FTP_STATE_CONNECTING))
    {
        return;
    }

    if (space == 0)
    {
        char* nl = memchr(client->response_buf, '\n', client->response_len);

        /* Unread complete lines fill the buffer (no state reads them now): drop the oldest */
        if (nl != NULL)
        {
            size_t drop = (size_t)(nl - client->response_buf) + 1;

            FTP_LOGV("[%s] unread reply line dropped", CNAME(client));
            client->response_len -= drop;
            memmove(client->response_buf, nl + 1, client->response_len);
            client->response_buf[client->response_len] = '\0';
            return;
        }

        /* Line longer than the buffer, drop it (recv of 0 bytes would look like a close) */
        FTP_LOGW("[%s] server reply longer than the buffer, discarded", CNAME(client));
        client->response_len = 0;
        client->response_buf[0] = '\0';
        return;
    }

    int n = ftp_sock_recv(client->cmd_sd,
                client->response_buf + client->response_len,
                space);

    if (n > 0)
    {
        client->response_len += n;
        client->response_buf[client->response_len] = '\0';
    }
    else if ((n == 0) || !ftp_sock_would_block())
    {
        /* Connection closed or broken */
        /* Only report if we aren't intentionally disconnecting */
        if (client->state != FTP_STATE_IDLE)
        {
            ftp_sock_close(&client->data_sd);
            ftp_sock_close(&client->cmd_sd);
            file_close(client);
            event_callback(client, FTP_ON_DISCONNECTED, "Server closed connection");
            client->state = FTP_STATE_IDLE;
        }
    }
}

/**
 * @brief Process a response from the FTP server
 * 
 * @param client FTP client object
 * @param code Parsed code
 * @param line Response line
 */
static void process_response(ftp_client_t* client, int code, char* line)
{
    client->last_resp_code = code;

    /* Check for 421 Service not available universally */
    if (code == FTP_421_SERVICE_NOT_AVAILABLE)
    {
        event_callback(client, FTP_ON_ERROR, "Service not available: %s", line);
        file_close(client);
        ftp_sock_close(&client->data_sd);
        ftp_sock_close(&client->cmd_sd);
        client->mode = FTP_MODE_NONE;
        client->job_cmd = FTP_CMD_NOT_SUPPORTED;
        client->state = FTP_STATE_IDLE;
        return;
    }

    switch (client->state)
    {

        default:
        case FTP_STATE_WAIT_RESPONSE:
        {
            const char* filename;
            int code_class = code / 100;

            switch (code_class)
            {
                case 1: /* 1xx - Start (Transfer initiated) */
                {
                    filename = strrchr(client->path, '/');
                    filename = filename ? filename + 1 : client->path;

                    switch (client->job_cmd)
                    {
                        case FTP_CMD_LIST:
                        case FTP_CMD_NLST:
                        {
                            client->state = FTP_STATE_LIST;
                            client->mode = FTP_MODE_LISTING;
                            event_callback(client, FTP_ON_TRANSFER_START, "List: %s", line);
                            break;
                        }
                        case FTP_CMD_RETR:
                        {
                            client->state = FTP_STATE_RETR;
                            client->mode = FTP_MODE_DOWNLOAD;
                            event_callback(client, FTP_ON_TRANSFER_START, "Get: %s", filename);
                            break;
                        }
                        case FTP_CMD_STOR:
                        {
                            client->state = FTP_STATE_STOR;
                            client->mode = FTP_MODE_UPLOAD;
                            client->data_len = 0;
                            event_callback(client, FTP_ON_TRANSFER_START, "Put: %s", filename);
                            break;
                        }
                        
                        default: { break; }
                    }
                    break;
                }
                case 2: /* 2xx - Done (Command completed) */
                {
                    if ((client->job_cmd == FTP_CMD_PASV) && 
                        (code == FTP_227_PASSIVE_MODE))
                    {
                        /* 
                            PASV response parsing usually happens in ftp_process but 
                            we acknowledge the command success here. 
                            Actual socket connection happens in process. 
                        */
                    }
                    else
                    {
                        FTP_LOGV("[%s] <- %s", CNAME(client), line);
                    }

                    client->mode = FTP_MODE_NONE;
                    client->state = FTP_STATE_READY;
                    client->job_cmd = FTP_CMD_NOT_SUPPORTED;
                    break;
                }
                case 3: /* 3xx - Intermediate (Need more info) */
                {
                    if ((client->job_cmd == FTP_CMD_RNFR) && 
                        (code == FTP_350_PENDING_FURTHER_INFO))
                    {
                        client->job_cmd = FTP_CMD_RNTO;
                        send_command(client, FTP_CMD_RNTO, client->data_buf);
                        /* Stay in current state (implicitly WAIT_RESPONSE) for RNTO */
                    }
                    break;
                }
                default: /* 4xx/5xx - Error */
                {
                    const char* cmd_name = "unknown";

                    if ((client->job_cmd >= 0) && (client->job_cmd < FTP_CMDS_MAX))
                    {
                        cmd_name = ftp_cmd_name(client->job_cmd);
                    }

                    event_callback(client, FTP_ON_ERROR, "Command %s failed: %s", cmd_name, line);
                    file_close(client);

                    client->mode = FTP_MODE_NONE;
                    client->state = FTP_STATE_READY;
                    client->job_cmd = FTP_CMD_NOT_SUPPORTED;
                    ftp_sock_close(&client->data_sd);
                    break;
                }
            }
            
            break;
        }
        case FTP_STATE_RETR:
        case FTP_STATE_STOR:
        case FTP_STATE_LIST:
        {
            if (code == FTP_226_TRANSFER_COMPLETE)
            {
                file_close(client);
                event_callback(client, FTP_ON_TRANSFER_END, "%s", line);
                client->state = FTP_STATE_READY;
                client->mode = FTP_MODE_NONE;
                client->job_cmd = FTP_CMD_NOT_SUPPORTED;
            }
            else if (code >= 400)
            {
                event_callback(client, FTP_ON_ERROR, "Transfer failed: %s", line);
                file_close(client);
                ftp_sock_close(&client->data_sd);
                client->state = FTP_STATE_READY;
                client->mode = FTP_MODE_NONE;
                client->job_cmd = FTP_CMD_NOT_SUPPORTED;
            }   
            break;
        }
        case FTP_STATE_READY:
        {
            FTP_LOGV("[%s] <- %s (ignored, ready)", CNAME(client), line);
            break;
        }
    }
}

/*------------------------------------------ Request slot ------------------------------------------*/

/*
    API functions may be called from any thread, ftp_process() runs in another one.
    The API only fills the request fields while the slot is free and then posts it;
    every state change is made by ftp_process(). Only atomic load/store are used
    (no read-modify-write), so it also works on cores without exclusive access.
    Fields written by the API while the slot is free: host, user, pass, port, path,
    data_buf (rename target / local file name), server_ip, req_type, req_cmd.
*/

typedef enum req_type
{
    REQ_CONNECT = 1,
    REQ_USER,
    REQ_PASS,
    REQ_JOB,                            /** RETR/STOR open the local file named in data_buf */
    REQ_URL_JOB,                        /** Job from a URL on the current connection, no local file */
} req_type_t;

/**
 * @brief Check the request slot is free (API side)
 * 
 * @param client FTP client object

 * @return bool true if a new request can be filled in
 */
static bool request_free(ftp_client_t* client)
{
    assert(client);
    assert(client->inited);

    if (atomic_load_explicit(&client->req_seq, memory_order_acquire) !=
        atomic_load_explicit(&client->ack_seq, memory_order_acquire))
    {
        FTP_LOGW("[%s] busy, previous request not finished", CNAME(client));
        return false;
    }

    return true;
}

/**
 * @brief A string (NULL: empty) fits a buffer of the given size with its terminator
 */
static bool fits(const char* str, size_t size)
{
    return (str == NULL) || (strlen(str) < size);
}

/**
 * @brief State seen by the API threads (published by ftp_process())
 */
static ftp_state_t api_state(ftp_client_t* client)
{
    return atomic_load_explicit(&client->state_pub, memory_order_relaxed);
}

/**
 * @brief Post the filled request to ftp_process() (API side)
 * 
 * @param client FTP client object
 * @param type Request type
 * @param cmd Command for REQ_JOB / job after login for REQ_CONNECT
 */
static void request_post(ftp_client_t* client, req_type_t type, ftp_cmd_t cmd)
{
    unsigned int seq = atomic_load_explicit(&client->req_seq, memory_order_relaxed);

    client->req_type = (uint8_t)type;
    client->req_cmd = cmd;

    /* Release: the request fields become visible before the new sequence */
    atomic_store_explicit(&client->req_seq, seq + 1, memory_order_release);
}

/**
 * @brief Post a simple command (API side)
 * 
 * @param client FTP client object
 * @param cmd Command to send
 * @param arg Command arguments

 * @return bool true if the request is accepted
 */
static bool generic_request(ftp_client_t* client, ftp_cmd_t cmd, const char* arg)
{
    if (!request_free(client) || (api_state(client) != FTP_STATE_READY))
    {
        return false;
    }

    SAFE_STRCPY(client->path, arg ? arg : "");
    request_post(client, REQ_JOB, cmd);

    return true;
}

/**
 * @brief Take a posted request and apply it (ftp_process side)
 * 
 * @param client FTP client object
 */
static void request_take(ftp_client_t* client)
{
    unsigned int seq = atomic_load_explicit(&client->req_seq, memory_order_acquire);

    if (seq == client->req_taken)
    {
        return;
    }

    client->req_taken = seq;

    switch ((req_type_t)client->req_type)
    {
        case REQ_CONNECT:
        {
            if ((client->state != FTP_STATE_IDLE) &&
                (client->state != FTP_STATE_QUIT) &&
                (client->state != FTP_STATE_ERROR))
            {
                event_callback(client, FTP_ON_ERROR, "Connect rejected, state: %d", client->state);
                break;
            }

            client->response_len = 0;
            client->data_len = 0;
            client->mode = FTP_MODE_NONE;
            client->job_cmd = client->req_cmd;      /* job after login (URL), or none */

            if (client->server_ip_valid)
            {
                client->state = FTP_STATE_CONNECTING;
            }
            else if (client->resolver == NULL)
            {
                event_callback(client, FTP_ON_ERROR, "No resolver for host name '%s'", client->host);
                client->job_cmd = FTP_CMD_NOT_SUPPORTED;
            }
            else if (client->dns.state == FTP_RESOLVE_PENDING)
            {
                /* A previous lookup may still write its result into client->dns */
                event_callback(client, FTP_ON_ERROR, "DNS busy, try again later");
                client->job_cmd = FTP_CMD_NOT_SUPPORTED;
            }
            else
            {
                event_callback(client, FTP_ON_STATUS, "Resolving:'%s'", client->host);
                client->dns.host = client->host;
                client->dns.state = FTP_RESOLVE_PENDING;
                client->state = FTP_STATE_RESOLVING;
                client->resolver(client->host, &client->dns);
            }
            break;
        }
        case REQ_USER:
        {
            if ((client->state != FTP_STATE_CONNECTED) &&
                (client->state != FTP_STATE_READY))
            {
                event_callback(client, FTP_ON_ERROR, "Login rejected, state: %d", client->state);
                break;
            }

            /* Manually trigger login flow */
            send_command(client, FTP_CMD_USER, client->user);
            client->state = FTP_STATE_WAIT_LOGIN;
            break;
        }
        case REQ_PASS:
        {
            if ((client->state != FTP_STATE_CONNECTED) &&
                (client->state != FTP_STATE_WAIT_LOGIN))
            {
                event_callback(client, FTP_ON_ERROR, "Password rejected, state: %d", client->state);
                break;
            }

            send_command(client, FTP_CMD_PASS, client->pass);
            client->state = FTP_STATE_WAIT_LOGIN;
            break;
        }
        case REQ_URL_JOB:
        {
            if (client->state != FTP_STATE_READY)
            {
                event_callback(client, FTP_ON_ERROR, "Request rejected, state: %d", client->state);
                break;
            }

            client->job_cmd = client->req_cmd;
            break;
        }
        case REQ_JOB:
        {
            if (client->state != FTP_STATE_READY)
            {
                event_callback(client, FTP_ON_ERROR, "Request rejected, state: %d", client->state);
                break;
            }

            /* Local file name is in data_buf, open it before data_buf is used for the transfer */
            if ((client->req_cmd == FTP_CMD_RETR) || (client->req_cmd == FTP_CMD_STOR))
            {
                int flags = (client->req_cmd == FTP_CMD_RETR) ?
                    (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;

                client->file_fd = open(client->data_buf, flags, 0666);

                if (client->file_fd < 0)
                {
                    event_callback(client, FTP_ON_ERROR, "Failed to open local file: %s", client->data_buf);
                    break;
                }

                client->file_open = true;
            }

            client->job_cmd = client->req_cmd;
            break;
        }
        default:
        {
            break;
        }
    }
}

/**
 * @brief Free the request slot when the request is finished (ftp_process side)
 * 
 * @param client FTP client object
 */
static void request_finish(ftp_client_t* client)
{
    if (atomic_load_explicit(&client->ack_seq, memory_order_relaxed) == client->req_taken)
    {
        return;
    }

    if ((client->job_cmd == FTP_CMD_NOT_SUPPORTED) &&
        ((client->state == FTP_STATE_IDLE) ||
         (client->state == FTP_STATE_READY) ||
         (client->state == FTP_STATE_CONNECTED) ||
         (client->state == FTP_STATE_ERROR)))
    {
        atomic_store_explicit(&client->ack_seq, client->req_taken, memory_order_release);
    }
}

/*------------------------------------------ Public API ------------------------------------------*/

/**
 * @brief Register a new FTP client
 * 
 * @param client FTP client object
 * @param name Client name
 * @param user User data
 * @param cb Callback function
 * @param resolver Non-blocking host name resolver, NULL to resolve in the caller's
 *                 thread of ftp_connect() (blocking that thread, not ftp_process()).
 *                 Needed only when ftp_connect() runs in the thread of ftp_process().

 * @return bool true on success, false on failure
 *
 * @note Register and unregister before/after ftp_process() runs on the client.
 */
bool ftp_client_register(ftp_client_t* client, const char* name, void* user, ftp_client_cb_t cb, ftp_resolver_t resolver)
{
    assert(client);
    memset(client, 0, sizeof(*client));
    atomic_init(&client->req_seq, 0);
    atomic_init(&client->ack_seq, 0);
    
    if (name)
    {
        SAFE_STRCPY(client->name, name);
    }
    else
    {
        SAFE_STRCPY(client->name, "FTP");
    }

    client->state = FTP_STATE_IDLE;
    client->job_cmd = FTP_CMD_NOT_SUPPORTED;
    client->cmd_sd = FTP_SOCK_INVALID;
    client->data_sd = FTP_SOCK_INVALID;
    client->user_data = user;
    client->resolver = resolver;

    if (cb)
    {
        client->callback = *cb;
    }

    client->next = NULL;

    /* Append to the end of the list */
    ftp_client_t** tail = &client_list;

    while (*tail)
    {
        tail = &(*tail)->next;
    }

    *tail = client;
    client->inited = true;
    atomic_store(&client->abort_req, false);
    atomic_store(&client->state_pub, client->state);
    client->file_fd = -1;
    client->file_open = false;

    return true;
}

/**
 * @brief Unregister an FTP client
 * 
 * @param client FTP client object
 * 
 * @return bool true on success, false on failure
 */
bool ftp_client_unregister(ftp_client_t* client)
{
    assert(client);
    assert(client->inited);

    if (client_list == client)
    {
        client_list = client->next;
    }
    else
    {
        ftp_client_t* prev = client_list;

        while (prev && prev->next != client)
        {
            prev = prev->next;
        }

        if (prev)
        {
            prev->next = client->next;
        }
    }

    ftp_sock_close(&client->cmd_sd);
    ftp_sock_close(&client->data_sd);
    file_close(client);
    memset(client, 0, sizeof(*client));

    return true;
}

/**
 * @brief Abort the transfer requested by ftp_abort() (ftp_process side)
 *
 * @param client FTP client object
 *
 * @note A transfer the server already runs is aborted with ABOR (RFC 959: 426 for
 *       the transfer, then 226); before that only the local side is cancelled.
 */
static void abort_take(ftp_client_t* client)
{
    bool server_side;

    if (!atomic_load_explicit(&client->abort_req, memory_order_acquire))
    {
        return;
    }

    atomic_store_explicit(&client->abort_req, false, memory_order_relaxed);

    /* The transfer command was sent: the server may run the transfer */
    server_side = (client->state == FTP_STATE_LIST) || (client->state == FTP_STATE_RETR) ||
        (client->state == FTP_STATE_STOR) ||
        ((client->state == FTP_STATE_WAIT_RESPONSE) &&
         ((client->job_cmd == FTP_CMD_LIST) || (client->job_cmd == FTP_CMD_NLST) ||
          (client->job_cmd == FTP_CMD_RETR) || (client->job_cmd == FTP_CMD_STOR)));

    if (server_side)
    {
        send_command(client, FTP_CMD_ABOR, NULL);
        ftp_sock_close(&client->data_sd);
        file_close(client);
        client->data_len = 0;
        client->mode = FTP_MODE_NONE;
        client->state = FTP_STATE_ABORT;
        event_callback(client, FTP_ON_TRANSFER_END, "Transfer aborted");
    }
    else if ((client->state == FTP_STATE_PASV_REQUEST) || (client->state == FTP_STATE_PASV_RESPONSE) ||
        (client->state == FTP_STATE_DATA_CONNECT) || (client->state == FTP_STATE_SEND_COMMAND))
    {
        transfer_abort(client);
        event_callback(client, FTP_ON_STATUS, "Transfer cancelled");
    }
    else
    {
        FTP_LOGD("[%s] abort: no transfer in progress", CNAME(client));
    }
}

/**
 * @brief FTP client state machine processing
 * 
 * @param pulse_1ms 1 ms pulse counter for timeout calculation
 */
void ftp_process(uint32_t pulse_1ms)
{
    ftp_client_t* client = client_list;

    /* Only ftp_process() writes it; API threads read it for their logs */
    atomic_store_explicit(&ftp_log_ms,
        atomic_load_explicit(&ftp_log_ms, memory_order_relaxed) + pulse_1ms, memory_order_relaxed);
    
    while (client)
    {
        int code;

        /* 
         * WARNING: 'line' points to 'client->cmd_buf'.
         * 'send_command' also uses 'client->cmd_buf' for constructing the command.
         * THEREFORE: Do NOT access 'line' after calling 'send_command' in the same loop iteration.
         * The content of 'line' will be overwritten by the sent command.
         */
        char* line = client->cmd_buf;

        request_take(client);
        abort_take(client);
        recv_response(client);

        switch (client->state)
        {
            default:
            case FTP_STATE_ERROR:
            case FTP_STATE_CONNECTED:
            case FTP_STATE_IDLE:
            {
                break;
            }
            /* ------------------------------ Connection and Login ------------------------------ */
            case FTP_STATE_ABORT:
            {
                while ((code = get_message(client, line, FTP_CMD_BUFFER_SIZE)) != 0)
                {
                    /* 1xx sent before the abort, 426 for the aborted transfer: wait for the end */
                    if ((code < 200) || (code == FTP_426_CONNECTION_CLOSED))
                    {
                        FTP_LOGV("[%s] <- %s", CNAME(client), line);
                        continue;
                    }

                    client->last_resp_code = code;

                    /* 225/226: aborted (or completed meanwhile); other codes: the abort failed */
                    if (code >= 400)
                    {
                        event_callback(client, FTP_ON_ERROR, "Abort failed: %s", line);
                    }

                    client->job_cmd = FTP_CMD_NOT_SUPPORTED;
                    client->state = FTP_STATE_READY;
                    break;
                }
                break;
            }
            case FTP_STATE_QUIT:
            {
                ftp_sock_close(&client->data_sd);

                if (ftp_sock_valid(client->cmd_sd))
                {
                    send_command(client, FTP_CMD_QUIT, NULL);
                    ftp_sock_close(&client->cmd_sd);
                }

                client->state = FTP_STATE_IDLE;

                file_close(client);
                event_callback(client, FTP_ON_DISCONNECTED, "Disconnected");
                break;
            }
            case FTP_STATE_RESOLVING:
            {
                /* Asynchronous resolver: only look at the result, never wait */
                if (client->dns.state == FTP_RESOLVE_DONE)
                {
                    client->server_ip = client->dns.ip;
                    client->server_ip_valid = true;
                    client->dns.state = FTP_RESOLVE_IDLE;
                    client->state = FTP_STATE_CONNECTING;
                    event_callback(client, FTP_ON_STATUS, "Resolved -> Connecting");
                }
                else if (client->dns.state == FTP_RESOLVE_FAILED)
                {
                    client->dns.state = FTP_RESOLVE_IDLE;
                    client->job_cmd = FTP_CMD_NOT_SUPPORTED;
                    client->state = FTP_STATE_IDLE;
                    event_callback(client, FTP_ON_ERROR, "DNS failed: '%s'", client->host);
                }
                break;
            }
            case FTP_STATE_CONNECTING:
            {
                if (!ftp_sock_valid(client->cmd_sd))
                {
                    client->cmd_sd = socket_create();
                    
                    if (!ftp_sock_valid(client->cmd_sd))
                    {
                        event_callback(client, FTP_ON_ERROR, "Socket create failed");
                        client->state = FTP_STATE_IDLE;
                        break;
                    }

                    int ret = ftp_sock_connect(client->cmd_sd, client->server_ip, client->port);

                    if (ret == 0)
                    {
                        /* already connected */
                        client->state = FTP_STATE_WAIT_GREETING;
                    }
                    else if (ret > 0)
                    {
                        client->state = FTP_STATE_CONNECTING;
                    }
                    else
                    {
                        event_callback(client, FTP_ON_ERROR, "Connect failed");
                        ftp_sock_close(&client->cmd_sd);
                        client->state = FTP_STATE_IDLE;
                        break;
                    }
                    
                    event_callback(client, FTP_ON_STATUS, "Connecting...");
                }
                else
                {
                    int ret = connect_poll(client->cmd_sd);

                    if (ret > 0)
                    {
                        client->state = FTP_STATE_WAIT_GREETING;
                    }
                    else if (ret < 0)
                    {
                        event_callback(client, FTP_ON_ERROR, "Connect error after select");
                        client->state = FTP_STATE_IDLE;
                        ftp_sock_close(&client->cmd_sd);
                    }
                }
                break;
            }
            case FTP_STATE_WAIT_GREETING:
            {
                while ((code = get_message(client, line, FTP_CMD_BUFFER_SIZE)) != 0)
                {
                    if (code == FTP_220_SERVICE_READY)
                    {
                        event_callback(client, FTP_ON_STATUS, "%s", line);
                        
                        if (strlen(client->user) > 0)
                        {
                            client->state = FTP_STATE_WAIT_LOGIN;
                            send_command(client, FTP_CMD_USER, client->user);
                        }
                        else
                        {
                            /* Manual login mode: wait for user command */
                            client->state = FTP_STATE_CONNECTED;
                            event_callback(client, FTP_ON_CONNECTED, "Connected to %s, need login", 
                                client->host);
                        }
                        break;
                    }
                }
                break;
            }
            case FTP_STATE_WAIT_LOGIN:
            {
                while ((code = get_message(client, line, FTP_CMD_BUFFER_SIZE)) != 0)
                {
                    if (code >= 400)
                    {
                        event_callback(client, FTP_ON_ERROR, "Login failed: %s", line);
                        ftp_sock_close(&client->cmd_sd); // Login fails -> usually disconnect
                        client->state = FTP_STATE_IDLE;
                    }
                    else
                    {
                        FTP_LOGV("[%s] <- %s", CNAME(client), line);
                    
                        if (code == FTP_331_USER_NAME_OK)
                        {
                            if (strlen(client->pass) > 0)
                            {
                                send_command(client, FTP_CMD_PASS, client->pass);
                            }
                            else
                            {
                                /* Manual password mode */
                                client->state = FTP_STATE_CONNECTED;
                            }
                        }
                        else if (code == FTP_230_USER_LOGGED_IN) 
                        {
                            send_command(client, FTP_CMD_TYPE, "I");
                        }
                        else if (code == FTP_200_COMMAND_OK)
                        {
                            client->state = FTP_STATE_READY;
                        }
                    }

                    if (client->state != FTP_STATE_WAIT_LOGIN)
                    {
                        break;
                    }
                }
                break;
            }
            case FTP_STATE_READY:
            {
                if (client->job_cmd != FTP_CMD_NOT_SUPPORTED)
                {
                    if (client->job_cmd == FTP_CMD_LIST || 
                        client->job_cmd == FTP_CMD_NLST || 
                        client->job_cmd == FTP_CMD_RETR || 
                        client->job_cmd == FTP_CMD_STOR)
                    {
                        client->state = FTP_STATE_PASV_REQUEST;
                    }
                    else
                    {
                        send_command(client, client->job_cmd, client->path); 
                        client->response_len = 0;
                        client->state = FTP_STATE_WAIT_RESPONSE;
                    }
                }
                break;
            }
            /* ------------------------------  Command transfer ------------------------------ */
            case FTP_STATE_PASV_REQUEST:
            {
                send_command(client, FTP_CMD_PASV, NULL);
                client->state = FTP_STATE_PASV_RESPONSE;
                break;
            }
            case FTP_STATE_PASV_RESPONSE:
            {
                while ((code = get_message(client, line, FTP_CMD_BUFFER_SIZE)) != 0)
                {
                    if (code == FTP_227_PASSIVE_MODE)
                    {
                        if (parse_pasv(line, &client->data_ip, &client->data_port) == 0)
                        {
                            client->state = FTP_STATE_DATA_CONNECT;
                        }
                        else
                        {
                            event_callback(client, FTP_ON_ERROR, "PASV parse failed");
                        }
                    }
                    else if (code >= 400)
                    {
                        event_callback(client, FTP_ON_ERROR, "PASV failed: %s", line);
                        client->state = FTP_STATE_READY;
                        ftp_sock_close(&client->data_sd);
                    }
                    else
                    {
                        FTP_LOGV("[%s] <- %s", CNAME(client), line);
                    }

                    if (client->state != FTP_STATE_PASV_RESPONSE)
                    {
                        break;
                    }
                }
                break;
            }
            case FTP_STATE_DATA_CONNECT:
            {
                if (!ftp_sock_valid(client->data_sd))
                {
                    client->data_sd = socket_create();
                    
                    if (!ftp_sock_valid(client->data_sd))
                    {
                        event_callback(client, FTP_ON_ERROR, "Data socket create failed");
                        transfer_abort(client);
                        break;
                    }

                    int r = ftp_sock_connect(client->data_sd, client->data_ip, client->data_port);

                    if (r == 0)
                    {
                        client->state = FTP_STATE_SEND_COMMAND;
                        break;
                    }
                    else if (r < 0)
                    {
                        event_callback(client, FTP_ON_ERROR, "Data connect failed");
                        transfer_abort(client);
                        break;
                    }
                }
                
                if (ftp_sock_valid(client->data_sd))
                {
                    int ret = connect_poll(client->data_sd);

                    if (ret > 0)
                    {
                        client->state = FTP_STATE_SEND_COMMAND;
                    }
                    else if (ret < 0)
                    {
                        event_callback(client, FTP_ON_ERROR, "Data connect failed");
                        transfer_abort(client);
                    }
                }
                break;
            }
            case FTP_STATE_SEND_COMMAND:
            {
                if (client->job_cmd != FTP_CMD_NOT_SUPPORTED)
                {
                    send_command(client, client->job_cmd, client->path);
                }
                
                client->response_len = 0;
                client->state = FTP_STATE_WAIT_RESPONSE;
                break;
            }
            case FTP_STATE_WAIT_RESPONSE:
            {
                /* One call per reply, multi-line replies included (get_message) */
                while ((code = get_message(client, line, FTP_CMD_BUFFER_SIZE)) != 0)
                {
                    process_response(client, code, line);

                    if (client->state != FTP_STATE_WAIT_RESPONSE)
                    {
                        break;
                    }
                }
                break;
            }
            /* ------------------------------  Command execution ------------------------------ */
            case FTP_STATE_LIST:
            {
                /* Line longer than the buffer: deliver it as is (recv of 0 bytes would look like EOF) */
                if (client->data_len >= sizeof(client->data_buf) - 1)
                {
                    callback(client, FTP_ON_TRANSFER_DATA, client->data_buf, client->data_len);
                    client->data_len = 0;
                }

                /* read from data socket and append to buffer */
                int n = ftp_sock_recv(client->data_sd,
                    client->data_buf + client->data_len,
                    sizeof(client->data_buf) - client->data_len - 1);

                if (n > 0)
                {
                    client->ticks = 0;
                    client->data_len += n;
                    client->data_buf[client->data_len] = '\0';

                    while (pop_line(client->data_buf, &client->data_len, line, FTP_CMD_BUFFER_SIZE))
                    {
                        callback(client, FTP_ON_TRANSFER_DATA, line, strlen(line));
                    }
                }
                else if (n == 0)
                {
                    ftp_sock_close(&client->data_sd);
                    client->state = FTP_STATE_WAIT_RESPONSE;
                    client->data_len = 0; 
                }
                else if (!ftp_sock_would_block())
                {
                    event_callback(client, FTP_ON_ERROR, "List error: %d", errno);
                    transfer_abort(client);
                }
                break;
            }
            case FTP_STATE_RETR:
            {
                /* read from data socket and forward data to callback */
                int n = ftp_sock_recv(client->data_sd, client->data_buf, sizeof(client->data_buf));

                if (n > 0)
                {
                    client->ticks = 0;

                    if (client->file_open)
                    {
                        /* A short write on a regular file means the medium is full */
                        if (write(client->file_fd, client->data_buf, (size_t)n) != (ssize_t)n)
                        {
                            event_callback(client, FTP_ON_ERROR, "File write error");
                            transfer_abort(client);
                        }
                    }
                }
                else if (n == 0)
                {
                    ftp_sock_close(&client->data_sd);
                    client->data_len = 0;
                    client->state = FTP_STATE_WAIT_RESPONSE;
                }
                else if (!ftp_sock_would_block())
                {
                    event_callback(client, FTP_ON_ERROR, "Data recv error");
                    transfer_abort(client);
                }
                break;
            }
            case FTP_STATE_STOR:
            {
                if (client->data_len == 0)
                {
                    if (client->file_open)
                    {
                        ssize_t br = read(client->file_fd, client->data_buf, FTP_DATA_BUFFER_SIZE);

                        if (br < 0)
                        {
                            event_callback(client, FTP_ON_ERROR, "File read error");
                            transfer_abort(client);
                            break;
                        }

                        if (br > 0)
                        {
                            client->data_len = br;
                        }
                        else
                        {
                            /* EOF */
                            ftp_sock_close(&client->data_sd);
                            file_close(client);
                            client->state = FTP_STATE_WAIT_RESPONSE;
                            break;
                        }
                    }
                    else
                    {
                        /* Should not happen if strictly following flow, but safe fallback */
                        ftp_sock_close(&client->data_sd);
                        client->state = FTP_STATE_WAIT_RESPONSE;
                        break;
                    }
                }
                
                if (client->data_len > 0)
                {
                    int sent = ftp_sock_send(client->data_sd, client->data_buf, client->data_len);
                    
                    if (sent > 0)
                    {
                        client->ticks = 0;
                        
                        if ((size_t)sent < client->data_len)
                        {
                            /* Partial send: move remaining data to front */
                            memmove(client->data_buf, client->data_buf + sent, client->data_len - sent);
                            client->data_len -= sent;
                        }
                        else
                        {
                            /* All sent, ready for next chunk */
                            client->data_len = 0;
                        }
                    }
                    else if ((sent < 0) && !ftp_sock_would_block())
                    {
                        event_callback(client, FTP_ON_ERROR, "Send failed (STOR)");
                        ftp_sock_close(&client->data_sd);
                        file_close(client);
                        client->state = FTP_STATE_WAIT_RESPONSE;
                    }
                }
                break;
            }
        }

        /* Command connection broken (see send_command) */
        if (client->cmd_broken)
        {
            client->cmd_broken = false;

            /* Already closed by the state (e.g. QUIT): nothing more to report */
            if (ftp_sock_valid(client->cmd_sd))
            {
                event_callback(client, FTP_ON_ERROR, "Command send failed");
                transfer_abort(client);
                ftp_sock_close(&client->cmd_sd);
                client->state = FTP_STATE_IDLE;
                event_callback(client, FTP_ON_DISCONNECTED, "Disconnected");
            }
        }

        if (client->state != client->last_state)
        {
            client->ticks = 0;
            client->last_state = client->state;
        }
        else
        {
            if(client->ticks < FTP_CMD_TIMEOUT_MS)
            {
                client->ticks += pulse_1ms;
            }
            else if((client->state != FTP_STATE_IDLE) && 
                    (client->state != FTP_STATE_READY))
            {
                /* Transfer timeout ? */
                if (client->mode != FTP_MODE_NONE)
                {
                    event_callback(client, FTP_ON_TRANSFER_END, "Transfer timeout");
                    client->mode = FTP_MODE_NONE;
                }

                event_callback(client, FTP_ON_ERROR, "Operation timeout (State: %d)", client->state);

                /* The session state is unknown, disconnect (reported only once) */
                transfer_abort(client);
                ftp_sock_close(&client->cmd_sd);
                client->state = FTP_STATE_IDLE;
                client->last_state = FTP_STATE_IDLE;
                event_callback(client, FTP_ON_DISCONNECTED, "Disconnected (timeout)");
            }
        }

        /* Publish before request_finish(): an API thread that sees the slot free sees this state */
        atomic_store_explicit(&client->state_pub, client->state, memory_order_relaxed);
        request_finish(client);
        client = client->next;
    }
}

/**
 * @brief Fill and post a connect request (API side, slot already checked free)
 * 
 * @param client FTP client object
 * @param host FTP server host name or IPv4 address
 * @param port FTP server port, 0 for default
 * @param user FTP username, NULL for manual login
 * @param pass FTP password
 * @param job Command after login (URL), FTP_CMD_NOT_SUPPORTED for none
 * @param path Path of the job
 * 
 * @return bool true if the request is accepted
 */
static bool connect_request(ftp_client_t* client, const char* host, uint16_t port,
    const char* user, const char* pass, ftp_cmd_t job, const char* path)
{
    uint32_t ip = 0;
    bool ip_valid;
    ftp_state_t state = api_state(client);

    /* Too long: refused, never truncated (a cut password fails the login silently) */
    if (!host || (strlen(host) == 0) || (strlen(host) >= sizeof(client->host)) ||
        !fits(user, sizeof(client->user)) || !fits(pass, sizeof(client->pass)))
    {
        return false;
    }

    port = port ? port : FTP_CMD_PORT;

    /* Already connected with the same parameters: reuse the connection */
    if ((state == FTP_STATE_READY) &&
        (strcmp(client->host, host) == 0) &&
        (client->port == port) &&
        (strcmp(client->user, user ? user : "") == 0) &&
        (strcmp(client->pass, pass ? pass : "") == 0))
    {
        if (job != FTP_CMD_NOT_SUPPORTED)
        {
            SAFE_STRCPY(client->path, path ? path : "");
            request_post(client, REQ_URL_JOB, job);
        }

        return true;
    }

    if ((state != FTP_STATE_IDLE) &&
        (state != FTP_STATE_QUIT) &&
        (state != FTP_STATE_ERROR))
    {
        FTP_LOGW("[%s] connect rejected, state %s", CNAME(client), state_str[state]);
        return false;
    }

    ip_valid = ftp_sock_ip4_parse(host, &ip);

    /*
        Default: resolve here, in the caller's thread (blocks the caller only,
        not ftp_process()). With a resolver, ftp_process() resolves asynchronously.
    */
    if (!ip_valid && (client->resolver == NULL))
    {
        if (!ftp_sock_resolve(host, &ip))
        {
            FTP_LOGW("[%s] cannot resolve \"%s\"", CNAME(client), host);
            return false;
        }

        ip_valid = true;
    }

    SAFE_STRCPY(client->host, host);
    SAFE_STRCPY(client->user, user ? user : "");
    SAFE_STRCPY(client->pass, pass ? pass : "");
    SAFE_STRCPY(client->path, path ? path : "");
    client->port = port;
    client->server_ip = ip;
    client->server_ip_valid = ip_valid;

    request_post(client, REQ_CONNECT, job);

    return true;
}

/**
 * @brief Connect and login to FTP server
 * 
 * @param client FTP client object
 * @param host FTP server host name or IPv4 address
 * @param port FTP server port
 * @param user FTP username
 * @param pass FTP password
 * 
 * @return bool true if the request is accepted
 *
 * @note With a host name and no resolver, it blocks the calling thread until
 *       the name is resolved. Pass a resolver to ftp_client_register() if this
 *       is called from the thread of ftp_process().
 */
bool ftp_connect(ftp_client_t* client, const char* host, uint16_t port, const char* user, const char* pass)
{
    if (!request_free(client))
    {
        return false;
    }

    return connect_request(client, host, port, user, pass, FTP_CMD_NOT_SUPPORTED, NULL);
}

/**
 * @brief Connect to FTP server using URL
 * 
 * @param client FTP client object
 * @param url URL to connect to
 * 
 * @return bool true if the request is accepted
 */
bool ftp_open_url(ftp_client_t* client, const char* url)
{
    char *user, *pass, *host, *path;
    uint16_t port;
    ftp_cmd_t job;

    assert(url);

    /* data_buf is used by parse_url(), it is free only while the slot is free */
    if (!request_free(client) ||
        !parse_url(client, url, &user, &pass, &host, &port, &path))
    {
        return false;
    }

    if (path && strlen(path) > 0)
    {
        /* Trailing slash -> directory listing */
        job = (path[strlen(path) - 1] == '/') ? FTP_CMD_NLST : FTP_CMD_RETR;
    }
    else
    {
        /* No path -> list root directory */
        job = FTP_CMD_NLST;
    }

    return connect_request(client, host, port, user, pass, job, path);
}

/**
 * @brief Connect to FTP server 
 * 
 * @param client FTP client object
 * @param host FTP server host
 * @param port FTP server port
 * 
 * @return bool true if the request is accepted
 */
bool ftp_open(ftp_client_t* client, const char* host, uint16_t port)
{
    return ftp_connect(client, host, port, NULL, NULL);
}

/**
 * @brief Login to FTP server
 * 
 * @param client FTP client object
 * @param user FTP username
 * @param pass FTP password
 * 
 * @return bool true if the request is accepted
 */
bool ftp_user(ftp_client_t* client, const char* user, const char* pass)
{
    if (!user || !fits(user, sizeof(client->user)) || !fits(pass, sizeof(client->pass)) ||
        !request_free(client) ||
        ((api_state(client) != FTP_STATE_CONNECTED) && (api_state(client) != FTP_STATE_READY)))
    {
        return false;
    }

    SAFE_STRCPY(client->user, user);
    SAFE_STRCPY(client->pass, pass ? pass : "");
    request_post(client, REQ_USER, FTP_CMD_USER);

    return true;
}

/**
 * @brief Login to FTP server (password)
 * 
 * @param client FTP client object
 * @param pass FTP password
 * 
 * @return bool true if the request is accepted
 */
bool ftp_password(ftp_client_t* client, const char* pass)
{
    if (!pass || !fits(pass, sizeof(client->pass)) || !request_free(client) ||
        ((api_state(client) != FTP_STATE_CONNECTED) && (api_state(client) != FTP_STATE_WAIT_LOGIN)))
    {
        return false;
    }

    SAFE_STRCPY(client->pass, pass);
    request_post(client, REQ_PASS, FTP_CMD_PASS);

    return true;
}

/**
 * @brief Request disconnect
 * 
 * @param client FTP client object
 * 
 * @return bool true if the request is accepted
 */
bool ftp_quit(ftp_client_t* client)
{
    return generic_request(client, FTP_CMD_QUIT, NULL);
}

/**
 * @brief Request directory listing
 * 
 * @param client FTP client object
 * @param path Directory path
 * 
 * @return bool true if the request is accepted
 */
bool ftp_ls(ftp_client_t* client, const char* path)
{
    return generic_request(client, FTP_CMD_NLST, path);
}

/**
 * @brief Change directory
 * 
 * @param client FTP client object
 * @param path Directory path
 * 
 * @return bool true if the request is accepted
 */
bool ftp_cd(ftp_client_t* client, const char* path)
{
    if (!path || (strlen(path) == 0))
    {
        return false;
    }
    
    return generic_request(client, FTP_CMD_CWD, path);
}

/**
 * @brief Get current directory
 * 
 * @param client FTP client object
 * 
 * @return bool true if the request is accepted
 */
bool ftp_pwd(ftp_client_t* client)
{
    return generic_request(client, FTP_CMD_PWD, NULL);
}

/**
 * @brief Create directory
 * 
 * @param client FTP client object
 * @param path Directory path
 * 
 * @return bool true if the request is accepted
 */
bool ftp_mkdir(ftp_client_t* client, const char* path)
{
    if (!path || (strlen(path) == 0))
    {
        return false;
    }

    return generic_request(client, FTP_CMD_MKD, path);
}

/**
 * @brief Remove directory
 * 
 * @param client FTP client object
 * @param path Directory path
 * 
 * @return bool true if the request is accepted
 */
bool ftp_rmdir(ftp_client_t* client, const char* path)
{
    if (!path || (strlen(path) == 0))
    {
        return false;
    }
    
    return generic_request(client, FTP_CMD_RMD, path);
}

/**
 * @brief Delete file
 * 
 * @param client FTP client object
 * @param filename File name
 * 
 * @return bool true if the request is accepted
 */
bool ftp_delete(ftp_client_t* client, const char* filename)
{
    if (!filename || (strlen(filename) == 0))
    {
        return false;
    }
    
    return generic_request(client, FTP_CMD_DELE, filename);
}

/**
 * @brief Post a request with a second argument kept in data_buf (API side)
 * 
 * @param client FTP client object
 * @param cmd Command
 * @param arg Remote path
 * @param arg2 Rename target or local file name
 * 
 * @return bool true if the request is accepted
 */
static bool request_with_arg2(ftp_client_t* client, ftp_cmd_t cmd, const char* arg, const char* arg2)
{
    if (!arg || (strlen(arg) == 0) || !arg2 || (strlen(arg2) == 0) ||
        (strlen(arg2) >= sizeof(client->data_buf)) ||
        !request_free(client) || (api_state(client) != FTP_STATE_READY))
    {
        return false;
    }

    SAFE_STRCPY(client->path, arg);
    SAFE_STRCPY(client->data_buf, arg2);
    request_post(client, REQ_JOB, cmd);

    return true;
}

/**
 * @brief Rename file
 * 
 * @param client FTP client object
 * @param from_name Original file name
 * @param to_name New file name
 * 
 * @return bool true if the request is accepted
 */
bool ftp_rename(ftp_client_t* client, const char* from_name, const char* to_name)
{
    return request_with_arg2(client, FTP_CMD_RNFR, from_name, to_name);
}

/**
 * @brief Download file
 * 
 * @param client FTP client object
 * @param remote_file Remote file name
 * @param local_file Local file name
 * 
 * @return bool true if the request is accepted
 *
 * @note The local file is opened by ftp_process(), a failure is reported by FTP_ON_ERROR.
 */
bool ftp_get(ftp_client_t* client, const char* remote_file, const char* local_file)
{
    return request_with_arg2(client, FTP_CMD_RETR, remote_file, local_file);
}

/**
 * @brief Upload file
 * 
 * @param client FTP client object
 * @param local_file Local file name
 * @param remote_file Remote file name
 * 
 * @return bool true if the request is accepted
 *
 * @note The local file is opened by ftp_process(), a failure is reported by FTP_ON_ERROR.
 */
bool ftp_put(ftp_client_t* client, const char* local_file, const char* remote_file)
{
    return request_with_arg2(client, FTP_CMD_STOR, remote_file, local_file);
}

/**
 * @brief Upload file from URL
 * 
 * @param client FTP client object
 * @param url URL of the file to upload
 * 
 * @return bool true if the request is accepted
 */
bool ftp_put_url(ftp_client_t* client, const char* url)
{
    char *user, *pass, *host, *path;
    uint16_t port;

    assert(url);

    if (!request_free(client) ||
        !parse_url(client, url, &user, &pass, &host, &port, &path))
    {
        return false;
    }

    if (!path || strlen(path) == 0)
    {
        /* Cannot upload to root without filename */
        return false;
    }

    return connect_request(client, host, port, user, pass, FTP_CMD_STOR, path);
}

/**
 * @brief Get status of the FTP server
 * 
 * @param client FTP client object
 * 
 * @return bool true on success, false on failure
 */
bool ftp_status(ftp_client_t* client)
{
    return generic_request(client, FTP_CMD_STAT, NULL);
}

/**
 * @brief Abort the transfer in progress (LIST, RETR, STOR)
 *
 * @param client FTP client object
 *
 * @return bool true (the request is recorded)
 *
 * @note Returns at once, any thread. ftp_process() sends ABOR if the server runs the
 *       transfer, closes the data connection and the local file and reports
 *       FTP_ON_TRANSFER_END "Transfer aborted"; the client is ready again after the
 *       server replies. Without a transfer in progress nothing happens.
 */
bool ftp_abort(ftp_client_t* client)
{
    assert(client);
    assert(client->inited);

    atomic_store_explicit(&client->abort_req, true, memory_order_release);

    return true;
}

/**
 * @brief Get status message of the FTP client
 * 
 * @param client FTP client object
 * 
 * @return const char* status message
 *
 * @note The buffer is rewritten by ftp_process(): use it in the callback or in the
 *       thread of ftp_process() (the callback also receives the message).
 */
const char* ftp_client_status_message(ftp_client_t* client)
{
    assert(client);
    assert(client->inited);
    
    return client->status;
}

/**
 * @brief Get state string of the FTP client
 * 
 * @param client FTP client object
 * 
 * @return const char* state string
 */
const char* ftp_client_state_string(ftp_client_t* client)
{
    assert(client);

    if(!client->inited)
    {
        return "Not Inited";
    }
    
    ftp_state_t state = api_state(client);

    if ((state >= 0) && (state < FTP_STATE_MAX))
    {
        return state_str[state];
    }
    
    return "Invalid";
}

/**
 * @brief Get the last response code from the server
 * 
 * @param client FTP client object
 * 
 * @return int Last response code (e.g. 250), or 0 if no response yet
 */
int ftp_last_resp_code(ftp_client_t* client)
{
    assert(client);
    assert(client->inited);
    
    return client->last_resp_code;
}
