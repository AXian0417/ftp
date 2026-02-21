/**
 * @file ftp_client.c
 * @author A Xian (axian0417@gmail.com)
 *
 * @brief FTP client implementation
 *
 * @version 1.0
 * @date 2026-02-22
 *
 * @copyright Copyright (c) 2026 A Xian
 *
 * @license MIT
 *          https://opensource.org/licenses/MIT
 *
 */

#include "ftp_client.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include "assert.h"
#include "lwip/sockets.h"

/* Port range */
#define PORT_MIN                                (1)
#define PORT_MAX                                (65535)

#define CLIENT_LOG(client, fmt, ...) \
    FTP_LOG("[%s] " fmt, (client)->name[0] ? (client)->name : "FTP", ##__VA_ARGS__)

#define SAFE_STRCPY(dest, src) do { \
    strncpy((dest), (src), sizeof(dest) - 1); \
    (dest)[sizeof(dest) - 1] = '\0'; \
} while(0)

extern const ftp_cmd_str_t ftp_cmd_str[FTP_CMDS_MAX];

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
    [FTP_STATE_QUIT]            = "Quit",
    [FTP_STATE_ERROR]           = "Error",
};

/*------------------------------------------ Private API ------------------------------------------*/

/**
 * @brief Set socket to non-blocking mode
 * 
 * @param s Socket descriptor

 * @return int 0 on success, -1 on failure
 */
static int set_nonblocking(int s)
{
    int flags = lwip_fcntl(s, F_GETFL, 0);

    if (flags < 0)
    {
        return -1;
    }

    return lwip_fcntl(s, F_SETFL, flags | O_NONBLOCK);
}

/**
 * @brief Close socket
 * 
 * @param s Pointer to socket descriptor
 */
static void close_socket(int* s)
{
    if (*s >= 0)
    {
        lwip_close(*s);
        *s = -1;
    }
}

/**
 * @brief Create non-blocking TCP socket
 * 
 * @return int Socket descriptor on success, -1 on failure
 */
static int socket_create(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);

    if (s < 0)
    {
        return -1;
    }

    if (set_nonblocking(s) < 0)
    {
        close_socket(&s);
        return -1;
    }

    return s;
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
        f_close(&client->file);
        client->file_open = false;
    }
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
 * @param out Output sockaddr_in structure

 * @return int 0 on success, -1 on failure
 */
static int parse_pasv(const char* resp, struct sockaddr_in* out)
{
    const char* p = strchr(resp, '(');

    if (!p)
    {
        return -1;
    }

    int h1, h2, h3, h4, p1, p2;

    if (sscanf(p + 1, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6)
    {
        return -1;
    }

    unsigned int ip = (h1 << 24) | (h2 << 16) | (h3 << 8) | h4;
    uint16_t port = (p1 << 8) | p2;

    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    out->sin_addr.s_addr = htonl(ip);

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
    CLIENT_LOG(client, "%s\r\n", client->status);
}

/**
 * @brief DNS resolution callback
 * 
 * @param name Resolved hostname
 * @param ipaddr Resolved IP address
 * @param arg User argument
 */
static void dns_found_cb(const char* name, const ip_addr_t* ipaddr, void* arg)
{
    ftp_client_t* client = (ftp_client_t*)arg;

    /* Safety: check if disconnect was requested during DNS resolution */
    if (client->disconnecting)
    {
        client->dns_pending = false;
        return;
    }

    if (ipaddr && (IP_IS_V4(ipaddr)))
    {
        client->resolved_ip = *ipaddr;
        /* move to connecting state; next poll will create socket and connect */
        client->state = FTP_STATE_CONNECTING;
        event_callback(client, FTP_ON_STATUS, "DNS resolved -> connecting");
    }
    else
    {
        event_callback(client, FTP_ON_ERROR, "DNS failed");
        client->state = FTP_STATE_IDLE;
    }

    client->dns_pending = false;
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
 * @brief Pop a line from the command buffer, parse the code
 * 
 * @param client FTP client object
 * @param line Output line buffer
 * @param max_len Maximum output length

 * @return int Parsed code, or 0 if no line
 */
static int get_message(ftp_client_t* client, char* line, size_t max_len)
{
    while (pop_line(client->response_buf, &client->response_len, line, max_len))
    {
        if (strlen(line) > 0)
        {
            return parse_code(line, strlen(line));
        }
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
    if (client->cmd_sd < 0)
    {
        return -1;
    }

    int len;
    memset(client->cmd_buf, 0, FTP_CMD_BUFFER_SIZE);
    client->last_resp_code = 0;

    if (arg && *arg)
    {
        len = snprintf(client->cmd_buf, FTP_CMD_BUFFER_SIZE, 
            "%s %s\r\n", ftp_cmd_str[cmd].str, arg); 
    }
    else
    {
        len = snprintf(client->cmd_buf, FTP_CMD_BUFFER_SIZE,  
            "%s\r\n", ftp_cmd_str[cmd].str);
    }

    /* Check for buffer overflow */
    if (len < 0 || len >= FTP_CMD_BUFFER_SIZE)
    {
        CLIENT_LOG(client, "send_command: buffer overflow prevented\r\n");
        return -1;
    }

    client->ticks = 0;
    client->multi_line_code = 0;

    if (send(client->cmd_sd, client->cmd_buf, len, 0) < 0)
    {
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
    if (client->cmd_sd < 0) 
    {
        return;
    }

    ssize_t n = recv(client->cmd_sd, 
                client->response_buf + client->response_len, 
                sizeof(client->response_buf) - client->response_len - 1, 0);

    if (n > 0)
    {
        client->response_len += n;
        client->response_buf[client->response_len] = '\0';
    }
    else if (n == 0)
    {
        /* Connection closed by server */
        /* Only report if we aren't intentionally disconnecting */
        if (client->state != FTP_STATE_IDLE)
        {
            close_socket(&client->cmd_sd);
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
        close_socket(&client->data_sd);
        close_socket(&client->cmd_sd);
        client->state = FTP_STATE_READY; 
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
                        CLIENT_LOG(client, "%s\r\n", line);
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
                        cmd_name = ftp_cmd_str[client->job_cmd].str;
                    }

                    event_callback(client, FTP_ON_ERROR, "Command %s failed: %s", cmd_name, line);
                    file_close(client);

                    client->mode = FTP_MODE_NONE;
                    client->state = FTP_STATE_READY;
                    client->job_cmd = FTP_CMD_NOT_SUPPORTED;
                    close_socket(&client->data_sd);
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
                close_socket(&client->data_sd);
                client->state = FTP_STATE_READY;
                client->mode = FTP_MODE_NONE;
                client->job_cmd = FTP_CMD_NOT_SUPPORTED;
            }   
            break;
        }
        case FTP_STATE_READY:
        {
            CLIENT_LOG(client, "Ignored response in READY state: %s\r\n", line);
            break;
        }
    }
}

/**
 * @brief Generic request for simple commands
 * 
 * @param client FTP client object
 * @param cmd Command to send
 * @param arg Command arguments

 * @return bool true on success, false on failure
 */
static bool generic_request(ftp_client_t* client, ftp_cmd_t cmd, const char* arg)
{
    assert(client);
    assert(client->inited);

    if (client->state != FTP_STATE_READY)
    {
        return false;
    }

    SAFE_STRCPY(client->path, arg ? arg : "");
    client->job_cmd = cmd;
    return true;
}

/*------------------------------------------ Public API ------------------------------------------*/

/**
 * @brief Register a new FTP client
 * 
 * @param client FTP client object
 * @param name Client name
 * @param user User data
 * @param cb Callback function

 * @return bool true on success, false on failure
 */
bool ftp_client_register(ftp_client_t* client, const char* name, void* user, ftp_client_cb_t cb)
{
    assert(client);
    memset(client, 0, sizeof(*client));
    
    if (name)
    {
        SAFE_STRCPY(client->name, name);
    }
    else
    {
        SAFE_STRCPY(client->name, "FTP");
    }

    client->state = FTP_STATE_IDLE;
    client->cmd_sd = -1;
    client->data_sd = -1;
    client->user_data = user;

    if (cb)
    {
        client->callback = *cb;
    }

    client->next = NULL;

    while (client_list)
    {
        client_list = client_list->next;
    }

    client_list = client;
    client->inited = true;
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

    close_socket(&client->cmd_sd);
    close_socket(&client->data_sd);
    file_close(client);
    memset(client, 0, sizeof(*client));

    return true;
}

/**
 * @brief FTP client state machine processing
 * 
 * @param pulse_1ms 1 ms pulse counter for timeout calculation
 */
void ftp_process(uint32_t pulse_1ms)
{
    ftp_client_t* client = client_list;
    
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
            case FTP_STATE_QUIT:
            {
                close_socket(&client->data_sd);

                if (client->cmd_sd >= 0)
                {
                    send_command(client, FTP_CMD_QUIT, NULL);
                    close_socket(&client->cmd_sd);
                }

                client->state = FTP_STATE_IDLE;

                file_close(client);
                event_callback(client, FTP_ON_DISCONNECTED, "Disconnected");
                break;
            }
            case FTP_STATE_RESOLVING:
            {
                if (client->dns_pending) 
                {
                    break;
                }

                client->dns_pending = true;
                event_callback(client, FTP_ON_STATUS, "Resolving:'%s'", client->host);

                err_t err = dns_gethostbyname(client->host, &client->resolved_ip, dns_found_cb, client);

                if (err == ERR_OK)
                {
                    client->dns_pending = false;
                    client->state = FTP_STATE_CONNECTING;
                    event_callback(client, FTP_ON_STATUS, "Resolved -> Connecting");
                }
                else if (err == ERR_INPROGRESS)
                {
                    event_callback(client, FTP_ON_STATUS, "Resolving... (async)");
                }
                else
                {
                    client->dns_pending = false;
                    event_callback(client, FTP_ON_ERROR, "DNS failed: %d", err);
                    client->state = FTP_STATE_IDLE;
                }
                break;
            }
            case FTP_STATE_CONNECTING:
            {
                if (client->cmd_sd < 0)
                {
                    client->cmd_sd = socket_create();
                    
                    if (client->cmd_sd < 0)
                    {
                        event_callback(client, FTP_ON_ERROR, "Socket create failed");
                        client->state = FTP_STATE_IDLE;
                        break;
                    }

                    struct sockaddr_in serv;
                    memset(&serv, 0, sizeof(serv));
                    serv.sin_family = AF_INET;
                    serv.sin_port = htons(client->port);
                    serv.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(&client->resolved_ip));

                    int ret = connect(client->cmd_sd, (struct sockaddr*)&serv, sizeof(serv));

                    if (ret == 0)
                    {
                        /* already connected */
                        client->state = FTP_STATE_WAIT_GREETING;
                    }
                    else if (errno == EINPROGRESS)
                    {
                        client->state = FTP_STATE_CONNECTING;
                    }
                    else
                    {
                        event_callback(client, FTP_ON_ERROR, "Connect failed");
                        close_socket(&client->cmd_sd);
                        client->state = FTP_STATE_IDLE;
                        break;
                    }
                    
                    event_callback(client, FTP_ON_STATUS, "Connecting...");
                }
                else
                {
                    fd_set wfds;
                    struct timeval tv = { 0,0 };
                    FD_ZERO(&wfds);
                    FD_SET(client->cmd_sd, &wfds);

                    int ret = select(client->cmd_sd + 1, NULL, &wfds, NULL, &tv);

                    if (ret > 0 && FD_ISSET(client->cmd_sd, &wfds))
                    {
                        int err = 0;
                        socklen_t len = sizeof(err);
                        
                        getsockopt(client->cmd_sd, SOL_SOCKET, SO_ERROR, &err, &len);

                        if (err == 0)
                        {
                            client->state = FTP_STATE_WAIT_GREETING;
                        }
                        else
                        {
                            event_callback(client, FTP_ON_ERROR, "Connect error after select");
                            client->state = FTP_STATE_IDLE;
                            close_socket(&client->cmd_sd);
                        }
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
                        close_socket(&client->cmd_sd); // Login fails -> usually disconnect
                        client->state = FTP_STATE_IDLE;
                    }
                    else
                    {
                        CLIENT_LOG(client, "%s\r\n", line);
                    
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
                        if (parse_pasv(line, &client->data_addr) == 0)
                        {
                            // CLIENT_LOG(client, "%s", line); // Suppress PASV response log
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
                        close_socket(&client->data_sd);
                    }
                    else
                    {
                        CLIENT_LOG(client, "%s", line);
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
                if (client->data_sd < 0)
                {
                    client->data_sd = socket_create();
                    
                    if (client->data_sd < 0)
                    {
                        event_callback(client, FTP_ON_ERROR, "Data socket create failed");
                        break;
                    }
                    
                    int r = connect(client->data_sd, (struct sockaddr*)&client->data_addr, sizeof(client->data_addr));
                    
                    if (r == 0)
                    {
                        client->state = FTP_STATE_SEND_COMMAND;
                        break;
                    }
                    else if (errno != EINPROGRESS)
                    {
                        event_callback(client, FTP_ON_ERROR, "Data connect failed");
                        break;
                    }
                }
                
                if (client->data_sd >= 0)
                {
                    fd_set wfds;
                    struct timeval tv = {0, 0};
                    FD_ZERO(&wfds);
                    FD_SET(client->data_sd, &wfds);
                    int sel = select(client->data_sd + 1, NULL, &wfds, NULL, &tv);
                    
                    if (sel > 0 && FD_ISSET(client->data_sd, &wfds))
                    {
                        int error = 0;
                        socklen_t len = sizeof(error);
                        if ((getsockopt(client->data_sd, SOL_SOCKET, SO_ERROR, &error, &len) == 0) && 
                            (error == 0))
                        {
                            client->state = FTP_STATE_SEND_COMMAND;
                        }
                        else
                        {
                            event_callback(client, FTP_ON_ERROR, "Data connect failed");
                            close_socket(&client->data_sd);
                            client->state = FTP_STATE_READY; 
                        }
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
                while ((code = get_message(client, line, FTP_CMD_BUFFER_SIZE)) != 0)
                {
                    /* Handle multi-line responses (RFC 959) */
                    if (client->multi_line_code > 0)
                    {
                        char end_tag[8];

                        snprintf(end_tag, sizeof(end_tag), "%d ", client->multi_line_code);

                        if (strncmp(line, end_tag, strlen(end_tag)) == 0)
                        {
                            client->multi_line_code = 0; 
                        }
                        else
                        {
                            CLIENT_LOG(client, "%s\r\n", line);
                            continue;
                        }
                    }
                    else if ((strlen(line) > 3) && (line[3] == '-'))
                    {
                        client->multi_line_code = code;
                        CLIENT_LOG(client, "%s\r\n", line);
                        continue;
                    }

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
                /* read from data socket and append to buffer */
                ssize_t n = recv(client->data_sd, 
                    client->data_buf + client->data_len, 
                    sizeof(client->data_buf) - client->data_len - 1, 0);
                
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
                    close_socket(&client->data_sd);
                    client->state = FTP_STATE_WAIT_RESPONSE;
                    client->data_len = 0; 
                }
                else if (errno != EAGAIN)
                {
                    event_callback(client, FTP_ON_ERROR, "List error: %d", errno);
                    client->state = FTP_STATE_READY;
                }
                break;
            }
            case FTP_STATE_RETR:
            {
                /* read from data socket and forward data to callback */
                ssize_t n = recv(client->data_sd, client->data_buf, sizeof(client->data_buf), 0);

                if (n > 0)
                {
                    client->ticks = 0;

                    if (client->file_open)
                    {
                        UINT bw;
                        f_write(&client->file, client->data_buf, n, &bw);
                    }
                }
                else if (n == 0)
                {
                    close_socket(&client->data_sd);
                    client->data_len = 0; 
                    client->state = FTP_STATE_WAIT_RESPONSE;
                }
                else if (errno != EAGAIN)
                {
                    event_callback(client, FTP_ON_ERROR, "Data recv error");
                    client->state = FTP_STATE_READY;
                }
                break;
            }
            case FTP_STATE_STOR:
            {
                if (client->data_len == 0)
                {
                    if (client->file_open)
                    {
                        UINT br;

                        f_read(&client->file, client->data_buf, FTP_DATA_BUFFER_SIZE, &br);
                        
                        if (br > 0)
                        {
                            client->data_len = br;
                        }
                        else
                        {
                            /* EOF or Error */
                            close_socket(&client->data_sd);
                            file_close(client);
                            client->state = FTP_STATE_WAIT_RESPONSE;
                            break;
                        }
                    }
                    else
                    {
                        /* Should not happen if strictly following flow, but safe fallback */
                        close_socket(&client->data_sd);
                        client->state = FTP_STATE_WAIT_RESPONSE;
                        break;
                    }
                }
                
                if (client->data_len > 0)
                {
                    ssize_t sent = send(client->data_sd, client->data_buf, client->data_len, 0);
                    
                    if (sent > 0)
                    {
                        client->ticks = 0;
                        
                        if (sent < client->data_len)
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
                    else if (sent < 0 && errno != EAGAIN)
                    {
                        event_callback(client, FTP_ON_ERROR, "Send failed (STOR)");
                        close_socket(&client->data_sd);
                        file_close(client);
                        client->state = FTP_STATE_WAIT_RESPONSE;
                    }
                }
                break;
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
            }
        }

        client = client->next;
    }
}

/**
 * @brief Connect and login to FTP server
 * 
 * @param client FTP client object
 * @param host FTP server host
 * @param port FTP server port
 * @param user FTP username
 * @param pass FTP password
 * 
 * @return bool true on success, false on failure
 */
bool ftp_connect(ftp_client_t* client, const char* host, uint16_t port, const char* user, const char* pass)
{
    assert(client);
    assert(client->inited);
    assert(host);

    if ((client->state != FTP_STATE_IDLE) &&
        (client->state != FTP_STATE_QUIT) &&
        (client->state != FTP_STATE_ERROR))
    {
        /* If already connected and params match, reuse connection */
        if (client->state == FTP_STATE_READY)
        {
            if (strcmp(client->host, host) == 0 &&
                client->port == (port ? port : FTP_CMD_PORT) &&
                strcmp(client->user, user ? user : "") == 0 &&
                strcmp(client->pass, pass ? pass : "") == 0)
            {
                /* Reuse existing connection */
                client->path[0] = '\0';
                client->job_cmd = FTP_CMD_NOT_SUPPORTED;
                return true;
            }
        }
        
        CLIENT_LOG(client, "Connect rejected, state: %d\r\n", client->state);
        return false;
    }


    SAFE_STRCPY(client->host, host);
    SAFE_STRCPY(client->user, user ? user : "");
    SAFE_STRCPY(client->pass, pass ? pass : "");

    client->response_len = 0;
    client->data_len = 0;
    client->dns_pending = false;
    client->disconnecting = false;
    client->path[0] = '\0';
    client->port = port ? port : FTP_CMD_PORT;
    client->state = FTP_STATE_RESOLVING;
    client->job_cmd = FTP_CMD_NOT_SUPPORTED;

    return true;
}

/**
 * @brief Connect to FTP server using URL
 * 
 * @param client FTP client object
 * @param url URL to connect to
 * 
 * @return bool true on success, false on failure
 */
bool ftp_open_url(ftp_client_t* client, const char* url)
{
    assert(client);
    assert(client->inited);
    assert(url);

    char *user, *pass, *host, *path;
    uint16_t port;

    if (!parse_url(client, url, &user, &pass, &host, &port, &path))
    {
        return false;
    }

    /* Connect first (this resets client->path) */
    if (!ftp_connect(client, host, port, user, pass))
    {
        return false;
    }

    /* Store path if present */
    if (path && strlen(path) > 0)
    {   
        SAFE_STRCPY(client->path, path);
        
        /* Check for trailing slash -> directory listing */
        if (client->path[strlen(client->path) - 1] == '/')
        {
            client->job_cmd = FTP_CMD_NLST;
        }
        else
        {
            client->job_cmd = FTP_CMD_RETR;
        }
    }
    else
    {
        /* No path -> list root directory */
        client->job_cmd = FTP_CMD_NLST;
    }

    return true;
}

/**
 * @brief Connect to FTP server 
 * 
 * @param client FTP client object
 * @param host FTP server host
 * @param port FTP server port
 * 
 * @return bool true on success, false on failure
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
 * @return bool true on success, false on failure
 */
bool ftp_user(ftp_client_t* client, const char* user, const char* pass)
{
    assert(client);
    assert(client->inited);

    if ((client->state != FTP_STATE_CONNECTED) && 
        (client->state != FTP_STATE_READY)) 
    {
        return false;
    }

    SAFE_STRCPY(client->user, user);
    SAFE_STRCPY(client->pass, pass ? pass : "");

    /* Manually trigger login flow */
    send_command(client, FTP_CMD_USER, client->user);
    client->state = FTP_STATE_WAIT_LOGIN;

    return true;
}

/**
 * @brief Login to FTP server (password)
 * 
 * @param client FTP client object
 * @param pass FTP password
 * 
 * @return bool true on success, false on failure
 */
bool ftp_password(ftp_client_t* client, const char* pass)
{
    assert(client);
    assert(client->inited);

    /* 
        Should be in CONNECTED or WAIT_LOGIN state technically, 
        but if 331 received we might be in CONNECTED 
    */

    if ((client->state != FTP_STATE_CONNECTED) &&
        (client->state != FTP_STATE_WAIT_LOGIN)) 
    {
        return false;
    }
    
    SAFE_STRCPY(client->pass, pass);
    send_command(client, FTP_CMD_PASS, client->pass);

    /* Still in WAIT_LOGIN state */
    client->state = FTP_STATE_WAIT_LOGIN;

    return true;
}

/**
 * @brief Request disconnect
 * 
 * @param client FTP client object
 * 
 * @return bool true on success, false on failure
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
 * @return bool true on success, false on failure
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
 * @return bool true on success, false on failure
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
 * @return bool true on success, false on failure
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
 * @return bool true on success, false on failure
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
 * @return bool true on success, false on failure
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
 * @return bool true on success, false on failure
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
 * @brief Rename file
 * 
 * @param client FTP client object
 * @param from_name Original file name
 * @param to_name New file name
 * 
 * @return bool true on success, false on failure
 */
bool ftp_rename(ftp_client_t* client, const char* from_name, const char* to_name)
{
    assert(client);
    assert(client->inited);
    
    if ((client->state != FTP_STATE_READY) || 
        (!from_name || strlen(from_name) == 0) || 
        (!to_name || strlen(to_name) == 0))
    {
        return false;
    }

    SAFE_STRCPY(client->path, from_name);
    SAFE_STRCPY(client->data_buf, to_name);
    client->job_cmd = FTP_CMD_RNFR;
    return true;
}

/**
 * @brief Download file
 * 
 * @param client FTP client object
 * @param remote_file Remote file name
 * @param local_file Local file name
 * 
 * @return bool true on success, false on failure
 */
bool ftp_get(ftp_client_t* client, const char* remote_file, const char* local_file)
{
    if (!remote_file || !local_file) 
    {
        return false;
    }

    if (f_open(&client->file, local_file, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        CLIENT_LOG(client, "Failed to open local file: %s", local_file);
        return false;
    }

    client->file_open = true;
    
    return generic_request(client, FTP_CMD_RETR, remote_file);
}

/**
 * @brief Upload file
 * 
 * @param client FTP client object
 * @param local_file Local file name
 * @param remote_file Remote file name
 * 
 * @return bool true on success, false on failure
 */

bool ftp_put(ftp_client_t* client, const char* local_file, const char* remote_file)
{
    if (!remote_file || !local_file) 
    {
        return false;
    }

    if (f_open(&client->file, local_file, FA_READ) != FR_OK)
    {
        CLIENT_LOG(client, "Failed to open local file: %s", local_file);
        return false;
    }

    client->file_open = true;

    return generic_request(client, FTP_CMD_STOR, remote_file);
}

/**
 * @brief Upload file from URL
 * 
 * @param client FTP client object
 * @param url URL of the file to upload
 * 
 * @return bool true on success, false on failure
 */
bool ftp_put_url(ftp_client_t* client, const char* url)
{
    assert(client);
    assert(client->inited);
    assert(url);

    char *user, *pass, *host, *path;
    uint16_t port;

    if (!parse_url(client, url, &user, &pass, &host, &port, &path))
    {
        return false;
    }

    if (!path || strlen(path) == 0)
    {
        /* Cannot upload to root without filename */
        return false;
    }

    if (!ftp_connect(client, host, port, user, pass))
    {
        return false;
    }

    SAFE_STRCPY(client->path, path);
    client->job_cmd = FTP_CMD_STOR;

    return true;
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
 * @brief Get status message of the FTP client
 * 
 * @param client FTP client object
 * 
 * @return const char* status message
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
    
    if ((client->state >= 0) && (client->state < FTP_STATE_MAX))
    {
        return state_str[client->state];
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
