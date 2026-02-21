/**
 * @file ftp_server.c
 * @author A Xian (axian0417@gmail.com)
 *
 * @brief FTP server implementation
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

#include "ftp_server.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include "assert.h"
#include "lwip/sockets.h"
#include "ff.h"

#if (FF_FS_LOCK < 5)
#error "FTP multi-client operation requires FF_FS_LOCK >= 5"
#endif

#define MAX(a, b)                               ((a) > (b) ? (a) : (b))
#define CMD_SIZE                                (6)
#define UNIX_2000_01_01_00_00_00                (946684800)
#define UNIX_SECONDS_180_DAYS                   (15552000)
#define LOG_TRX_SIZE                            (10240) 
#define SEND_RETRY_MAX                          (20)
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
    FAILED
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
    FIL file;
    FF_DIR dir;
    open_t open_type;

    client_state_t state;
    ftp_role_t role;

    bool active;
    bool name_list;
    bool close_child;

    int cmd_sd;                         /** command socket */
    int data_sd;                        /** data socket */
    int ldata_sd;                       /** listen data socket */

    uint8_t number;                     /** Client number */
    uint32_t ip_addr;                   /** Client IP */
    char ip_str[20];                    /** Client IP (String) */

    uint32_t time;
    uint32_t cmd_timeout;
    uint32_t data_timeout;
    int32_t data_count;
    uint32_t data_total;
    uint32_t data_last;
    uint32_t allowed_cmds;

    char* path;
    uint8_t* data;
} client_t;

typedef struct user
{
    char username[FTP_MAX_USER_LEN];
    char password[FTP_MAX_PASS_LEN];
    const ftp_role_t role;
    const uint32_t allowed_cmds;
} user_t;

/*---------------------------------------------------------------------------------------*/

static user_t users[FTP_ROLE_UNKNOWN] =
{
    [FTP_ROLE_GUEST] =
    { "guest", "", FTP_ROLE_GUEST, GUEST_ALLOW },

    [FTP_ROLE_USER] =
    { "user", FTP_USER_DEFAULT_PASSWORD, FTP_ROLE_USER, USER_ALLOW },

    [FTP_ROLE_MAINTAINER] =
    { "maint", FTP_MAINT_DEFAULT_PASSWORD, FTP_ROLE_MAINTAINER, MAINT_ALLOW},

    [FTP_ROLE_ADMIN] =
    { "admin", FTP_ADMIN_DEFAULT_PASSWORD, FTP_ROLE_ADMIN, ALLOW_ALL },
};

const ftp_cmd_str_t ftp_cmd_str[FTP_CMDS_MAX] =
{
    [FTP_CMD_FEAT] {"FEAT"},
    [FTP_CMD_SYST] {"SYST"},
    [FTP_CMD_CDUP] {"CDUP"},
    [FTP_CMD_CWD] {"CWD" },
    [FTP_CMD_PWD] {"PWD" },
    [FTP_CMD_XPWD] {"XPWD"},
    [FTP_CMD_SIZE] {"SIZE"},
    [FTP_CMD_MDTM] {"MDTM"},
    [FTP_CMD_MFMT] {"MFMT"},
    [FTP_CMD_TYPE] {"TYPE"},
    [FTP_CMD_USER] {"USER"},
    [FTP_CMD_PASS] {"PASS"},
    [FTP_CMD_PASV] {"PASV"},
    [FTP_CMD_LIST] {"LIST"},
    [FTP_CMD_RETR] {"RETR"},
    [FTP_CMD_STOR] {"STOR"},
    [FTP_CMD_DELE] {"DELE"},
    [FTP_CMD_RMD] {"RMD" },
    [FTP_CMD_MKD] {"MKD" },
    [FTP_CMD_RNFR] {"RNFR"},
    [FTP_CMD_RNTO] {"RNTO"},
    [FTP_CMD_NOOP] {"NOOP"},
    [FTP_CMD_QUIT] {"QUIT"},
    [FTP_CMD_APPE] {"APPE"},
    [FTP_CMD_NLST] {"NLST"},
    [FTP_CMD_AUTH] {"AUTH"},
    [FTP_CMD_STAT] {"STAT"},
};

/* 
    Shared static buffers for memory optimization.
    Thread-safety is guaranteed by single-threaded sequential processing.
    Buffers are used as scratchpads (cleared per use), so no collision occurs.
    Note: Does not support TCP fragmentation handling (partial commands are discarded).
 */
static char reply_buf[FTP_CMD_BUFFER_SIZE] = { 0 };
static char temp_buf[FTP_CMD_BUFFER_SIZE] = { 0 };
static char cmd_buffer[FTP_CMD_BUFFER_SIZE + CMD_SIZE] = { 0 };

/* listening command socket descriptor */
static int lc_sd;

static union
{
    struct
    {
        uint8_t init : 1;
        uint8_t enable : 1;
        uint8_t reserve : 6;
    } bits;

    uint8_t all;
} flag;

static ftp_config_t* ftp_config;
static state_t state;
static client_t clients[FTP_CLIENTS_MAX];
static ftp_unix_time_t get_unix_time;

static void send_reply(client_t* client, uint16_t status, char* message);
static void client_sign_out(client_t* client);

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
 * @brief Pops the next word from the string
 *
 * @param str string
 * @param param pop param
 * @param stop_on_space if true, pop stops when a space is encountered
 * @param stop_on_newline if true, pop stops when '\n' is encountered
 * @param len param buffer length
 *
 * @return true if the param buffer is long enough
 * @return false if the param buffer is not long enough
 *
 * @note After each execution, the **str pointer will move to the end of the string
 *       (stop according to the condition) so that the subsequent program can
 *       directly use the subsequent string
 */
static bool pop_param(char** str, char* param, bool stop_on_space, bool stop_on_newline, int len)
{
    char lastc = '\0';
    int count = 0;

    while ((**str != '\0') && (count < (len - 1)))
    {
        if (stop_on_space && (**str == ' '))
        {
            break;
        }

        if ((**str == '\r') || (**str == '\n'))
        {
            if (!stop_on_newline)
            {
                (*str)++;
                continue;
            }
            else
            {
                break;
            }
        }

        if ((**str == '/') && (lastc == '/'))
        {
            (*str)++;
            continue;
        }

        lastc = **str;
        *param++ = **str;
        (*str)++;
        count++;
    }

    *param = '\0';

    if (count == (len - 1))
    {
        return false;
    }

    return true;
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
    char cmd[CMD_SIZE];

    pop_param(buf, cmd, true, true, CMD_SIZE);
    stoupper(cmd);

    for (i = 0; i < FTP_CMDS_MAX; i++)
    {
        if (!strcmp(cmd, ftp_cmd_str[i].str))
        {
            /* move one step further to skip the space */
            (*buf)++;
            return i;
        }
    }

    FTP_LOG("Command not supported [%s]\r\n", cmd);

    return FTP_CMD_NOT_SUPPORTED;
}

/**
 * @brief default time (unix type)
 *
 * @return time_t
 *
 * @note If system no time function (RTC, Network...),
 *       the file time will be using this function.
 */
static time_t default_unix_time(void)
{
    time_t time_value = 0;

    return time_value;
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
    
    /* Check for backslash (Windows-style path separator) */
    if (strchr(path, '\\') != NULL)
    {
        return false;
    }
    
    /* Path should not end with multiple slashes */
    size_t len = strlen(path);
    if (len > 1 && path[len-1] == '/' && path[len-2] == '/')
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
 * @param ip_str Specify IP address, if NULL, it is not specified
 * @param port listening port
 *
 * @return true if listening successful
 * @return false if listening failed
 */
static bool listening_socket(int* sd, const char* ip_str, uint32_t port)
{
    struct sockaddr_in addr;
    int _sd, result;

    /* open a socket for ftp data listen */
    *sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    _sd = *sd;

    if (_sd >= 0)
    {
        /* enable non-blocking mode */
        int option = fcntl(_sd, F_GETFL, 0) | O_NONBLOCK;

        fcntl(_sd, F_SETFL, option);

        /* enable address reusing */
        option = 1;
        result = setsockopt(_sd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

        /* bind the socket to a port number */
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_len = sizeof(addr);

        if (ip_str != NULL)
        {
            if (inet_pton(AF_INET, ip_str, &addr.sin_addr) != 1)
            {
                closesocket(_sd);
                return false;
            }
        }
        else
        {
            addr.sin_addr.s_addr = INADDR_ANY;
        }

        result |= bind(_sd, (const struct sockaddr*)&addr, sizeof(addr));
        result |= listen(_sd, FTP_CLIENTS_MAX);

        if (!result)
        {
            return true;
        }

        closesocket(*sd);
    }

    return false;
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
    struct sockaddr_in client_addr;
    socklen_t addr_size = sizeof(client_addr);

    int new_sd = accept(lc_sd, (struct sockaddr*)&client_addr, &addr_size);

    if (new_sd < 0)
    {
        return;
    }

    fcntl(new_sd, F_SETFL, (fcntl(new_sd, F_GETFL, 0) | O_NONBLOCK));

    uint32_t ip = ntohl(client_addr.sin_addr.s_addr);

    /* Check if it is the same client */
    for (i = 0; i < FTP_CLIENTS_MAX; i++)
    {
        if (clients[i].active && (ip == clients[i].ip_addr))
        {
            client_sign_out(&clients[i]);
            break;
        }
    }

    for (i = 0; i < FTP_CLIENTS_MAX; i++)
    {
        if (!clients[i].active)
        {
            clients[i].path = (char*)malloc(FTP_FILE_PATH_LEN);

            if (clients[i].path == NULL)
            {
                send_reply(&clients[i], FTP_552_EXCEEDED_STORAGE, "Hardware error");
                closesocket(new_sd);
                break;
            }

            clients[i].data = (uint8_t*)malloc(FTP_DATA_BUFFER_SIZE);

            if (clients[i].data == NULL)
            {
                send_reply(&clients[i], FTP_552_EXCEEDED_STORAGE, "Hardware error");
                free(clients[i].path);
                clients[i].path = NULL;
                closesocket(new_sd);
                break;
            }

            clients[i].number = i;
            clients[i].active = true;
            clients[i].cmd_sd = new_sd;
            clients[i].ldata_sd = -1;
            clients[i].data_sd = -1;
            clients[i].ip_addr = ntohl(client_addr.sin_addr.s_addr);
            clients[i].cmd_timeout = 0;
            clients[i].role = FTP_ROLE_UNKNOWN;
            clients[i].allowed_cmds = 0;
            clients[i].open_type = NOTHING_OPEN;

            strcpy(clients[i].path, ftp_config->mount_point);
            snprintf(clients[i].ip_str,
                sizeof(clients[i].ip_str),
                "%u.%u.%u.%u",
                (uint8_t)(clients[i].ip_addr >> 24) & 0xFFU,
                (uint8_t)(clients[i].ip_addr >> 16) & 0xFFU,
                (uint8_t)(clients[i].ip_addr >> 8) & 0xFFU,
                (uint8_t)clients[i].ip_addr & 0xFFU);

            FTP_LOG("User %d connected from %s\n", i, clients[i].ip_str);

            send_reply(&clients[i], FTP_220_SERVICE_READY, "FTP server");
            return;
        }
    }

    closesocket(new_sd);
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
static result_t data_socket_connection(int ld_sd, int* d_sd)
{
    struct sockaddr_in addr;
    socklen_t addr_size;

    (*d_sd) = accept(ld_sd, (struct sockaddr*)&addr, &addr_size);

    if ((*d_sd) < 0)
    {
        if (errno == EAGAIN)
        {
            return CONTINUE;
        }

        return FAILED;
    }

    fcntl((*d_sd), F_SETFL, (fcntl((*d_sd), F_GETFL, 0) | O_NONBLOCK));

    return OK;
}

/**
 * @brief Close socket
 *
 * @param sd Socket that needs to be close
 */
static void close_socket(int* sd)
{
    closesocket((*sd));
    (*sd) = -1;
}

/**
 * @brief Send data to the socket
 *
 * @param sd Socket data
 * @param data Data to send
 * @param size Data size
 * 
 * @return result_t OK if all data sent successfully, FAILED otherwise
 */
static result_t send_data(int sd, uint8_t* data, uint32_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sent, total_sent = 0;
    uint8_t retry = 0;

    while (total_sent < size)
    {
        sent = (uint32_t)send(sd, (p + total_sent), (size - total_sent), 0);

        if (sent > 0)
        {
            total_sent += sent;
            retry = 0;
            continue;
        }
        else
        {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
            {
                if (++retry < SEND_RETRY_MAX)
                {
                    continue;
                }
            }
        }

        FTP_LOG("Error sending data\r\n");
        return FAILED;
    }
    
    return OK;
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
static result_t receive_data(int sd, uint8_t* data, uint32_t size, int32_t* actual_size)
{
    if (sd < 0)
    {
        return FAILED;
    }

    (*actual_size) = recv(sd, data, size, 0);

    if (*actual_size > 0)
    {
        return OK;
    }
    else if (errno != EAGAIN)
    {
        return FAILED;
    }

    return CONTINUE;
}

/*---------------------------------------------------------------------------------------*/

/**
 * @brief Open file
 *
 * @param client Client object
 * @param mode Open mode
 *
 * @return true if file is opened
 * @return false if file can't open
 */
static bool open_file(client_t* client, uint8_t mode)
{
    FRESULT res;

    res = f_open(&client->file, client->path, mode);

    if (res == FR_OK)
    {
        client->open_type = FILE_OPEN;
    }

    FTP_LOG("Client: %s, open file -> \"%s\" %s(%d)\r\n",
        client->ip_str, client->path,
        (res == FR_OK) ? "ok" : "failed", res);

    return (res == FR_OK);
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
    FRESULT res;

    f_closedir(&client->dir);

    res = f_opendir(&client->dir, client->path);

    if (res == FR_OK)
    {
        client->open_type = DIR_OPEN;
    }

    FTP_LOG("Client:%s, open directory -> \"%s\" %s(%d)\r\n",
        client->ip_str, client->path,
        (res == FR_OK) ? "ok" : "failed", res);

    return (res == FR_OK);
}

/**
 * @brief Close file or directory
 *
 * @param client Client object
 */
static void close_files_dir(client_t* client)
{
    if (client->open_type == FILE_OPEN)
    {
        f_close(&client->file);
    }
    else if (client->open_type == DIR_OPEN)
    {
        f_closedir(&client->dir);
    }

    client->open_type = NOTHING_OPEN;
}

/**
 * @brief Read file
 *
 * @param client Client object
 * @param data Data buffer
 * @param size Size you want to read
 *
 * @return int32_t -1 = read error, other = actual size of read
 */
static int32_t read_file(client_t* client, uint8_t* data, uint32_t size)
{
    FRESULT res = FR_OK;
    UINT actual_size;

    res = f_read(&client->file, data, (UINT)size, &actual_size);

    if (res != FR_OK)
    {
        close_files_dir(client);
        return -1;
    }
    else if (actual_size < size)
    {
        close_files_dir(client);
    }

    return (int32_t)actual_size;
}

/**
 * @brief write file
 *
 * @param client Client object
 * @param data Data you want to write
 * @param size Size of the data
 *
 * @return int32_t -1 = write error, other = actual size of write
 */
static int32_t write_file(client_t* client, uint8_t* data, uint32_t size)
{
    FRESULT res = FR_OK;
    UINT actual_size;

    res = f_write(&client->file, data, (UINT)size, &actual_size);

    if ((actual_size != size) || (res != FR_OK))
    {
        close_files_dir(client);
        return -1;
    }

    return actual_size;
}

/**
 * @brief Get the file time
 *
 * @param fno FILINFO object
 *
 * @return time_t file time(converted)
 */
static time_t get_file_time(FILINFO* fno)
{
    struct tm t;

    t.tm_year = ((fno->fdate >> 9) & 0x7F) + 80;
    t.tm_mon = ((fno->fdate >> 5) & 0x0F) - 1;
    t.tm_mday = (fno->fdate & 0x1F);
    t.tm_hour = ((fno->ftime >> 11) & 0x1F);
    t.tm_min = ((fno->ftime >> 5) & 0x3F);
    t.tm_sec = ((fno->ftime & 0x1F) * 2);
    t.tm_isdst = 0;

    /* Converter to time_t type */
    return mktime(&t);
}

/**
 * @brief format list item for the LIST
 *
 * @param client Client object
 * @param buf Buffer of the format
 * @param size Buffer size
 * @param fno FILINFO object
 *
 * @return uint32_t Size of a item
 */
static uint32_t format_list_item(client_t* client, char* buf, uint32_t size, FILINFO* fno)
{
    time_t time_val, time_now;
    char str_time[32];
    const char* type = (fno->fattrib & AM_DIR) ? "d" : "-";

    if (client->name_list)
    {
        /* Just want the file name */
        return snprintf(buf, size, "%s\r\n", fno->fname);
    }

    /* Get file time */
    time_val = get_file_time(fno);

    /* Get the current time */
    time_now = get_unix_time();

    if (time(&time_now) < 0)
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

    int add_size = snprintf(buf, size,
        "%srw-rw-rw- 1 root root %9lu %s %s\r\n",
        type, (unsigned long)fno->fsize, str_time, fno->fname);

    if (add_size >= size)
    {
        FTP_LOG("Content exceeds buffer size [%d > %d]\r\n",
            add_size, (int)size);

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
 */
static result_t list_directory(client_t* client, uint32_t* size)
{
    uint32_t next = 0;
    uint8_t list_count = 0;
    FRESULT res;
    FILINFO fno;
    result_t result = CONTINUE;

    FTP_LOG("Client:%s, add to directory list:\r\n", client->ip_str);

    /* read up to FTP_LIST_NUM directory items */
    while (((FTP_DATA_BUFFER_SIZE - next) > LISTING_BUFFER_MARGIN) &&
        (list_count < FTP_LIST_NUM))
    {
        /* Read a directory item */
        res = f_readdir(&client->dir, &fno);

        if (res != FR_OK)
        {
            result = FAILED;
            FTP_LOG("Client:%s, failed to read directory: %d\r\n",
                client->ip_str, res);
            break;
        }

        if (fno.fname[0] == 0)
        {
            result = OK;
            FTP_LOG("Client:%s, end of directory\r\n",
                client->ip_str);
            break;
        }

        /* Ignore . or .. entry */
        if ((strcmp(fno.fname, ".") == 0) ||
            (strcmp(fno.fname, "..") == 0))
        {
            continue;
        }

        next += format_list_item(client, (char*)(client->data + next),
            (FTP_DATA_BUFFER_SIZE - next), &fno);

        /* add the entry to the list */
        FTP_LOG("-> \"%s\" (%"PRIu32")\r\n", fno.fname, next);
        // FTP_LOG("%s\r\n", client->data);

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
 * @brief send reply
 *
 * @param client Client object
 * @param status reply code
 * @param message reply message(option)
 *
 * @note If "status" is 221, 550..., it will be close socket and fs too
 */
static void send_reply(client_t* client, uint16_t status, char* message)
{
    int size;

    if (!message)
    {
        message = "";
    }

    size = snprintf((char*)reply_buf, FTP_CMD_BUFFER_SIZE,
        "%u %s\r\n", status, message);

    send_data(client->cmd_sd, (uint8_t*)reply_buf, (uint32_t)size);

    switch (status)
    {
        case 221:	/* Server closed connection */
        {
            close_socket(&client->cmd_sd);
            close_socket(&client->data_sd);
            close_socket(&client->ldata_sd);
            close_fs(client);
            break;
        }
        case 426:	/* The connection has been closed, the transfer failed */
        case 451:	/* Requested action aborted, local error in processing */
        case 550:	/* The requested action failed, the file is invalid
                       (e.g. no such file found, unacceptable) */
        case 553:	/* The requested action failed, invalid file name */
        {
            close_socket(&client->data_sd);
            close_fs(client);
            break;
        }
        default: { break; }
    }

    FTP_LOG("Send reply: [%u] %s (%u)\r\n", status, message, size);
}

/**
 * @brief Build full file path from client's current directory and filename
 *
 * @param client Client object
 * @param filename Filename to append
 * @param out_path Output buffer for the full path
 * @param max_len Maximum length of output buffer
 * 
 * @return true if path built successfully
 * @return false if path would exceed buffer size
 */
static bool build_file_path(client_t* client, const char* filename, 
                            char* out_path, size_t max_len)
{
    size_t path_len = strlen(client->path);
    const char* sep = "/";

    // Skip leading slashes in filename to prevent double slashes if client sends //file
    while (*filename == '/') 
    {
        filename++;
    }

    if (path_len > 0 && client->path[path_len - 1] == '/')
    {
        sep = "";
    }

    int len = snprintf(out_path, max_len, "%s%s%s", client->path, sep, filename);
    
    if (len < 0 || len >= max_len)
    {
        FTP_LOG("Client:%s, file path too long\r\n", client->ip_str);
        return false;
    }
    
    return true;
}

/**
 * @brief Open child path (directory)
 *
 * @param client Client object
 * @param dir directory
 * 
 * @return true if path is valid and within buffer limits
 * @return false if path would exceed buffer size or is invalid
 */
static bool open_child(client_t* client, char* dir)
{
    size_t current_len, dir_len;
    
    if (strlen(dir) > 0)
    {
        if (dir[0] == '/')
        {
            strcpy(client->path, ftp_config->mount_point);

            if (dir[1] != '\0')
            {
                /* absolute path */
                current_len = strlen(client->path);
                dir_len = strlen(dir);
                
                char* p_dir = dir;

                while (*p_dir == '/') 
                {
                    p_dir++;
                }

                if (current_len + strlen(p_dir) + 2 >= FTP_FILE_PATH_LEN)
                {
                    FTP_LOG("Client:%s, path too long\r\n", client->ip_str);
                    strcpy(client->path, ftp_config->mount_point);
                    return false;
                }

                if (client->path[current_len - 1] != '/')
                {
                    strcat(client->path, "/");
                }
                
                strcat(client->path, p_dir);
            }
        }
        else
        {
            /* relative path */
            current_len = strlen(client->path);
            dir_len = strlen(dir);
            
            /* add trailing '/' if needed */
            if ((current_len > 1) &&
                (client->path[current_len - 1] != '/') &&
                (dir[0] != '/'))
            {
                if (current_len + dir_len + 2 >= FTP_FILE_PATH_LEN)
                {
                    FTP_LOG("Client:%s, path too long\r\n", client->ip_str);
                    strcpy(client->path, ftp_config->mount_point);
                    return false;
                }
                
                strncat(client->path, "/", FTP_FILE_PATH_LEN - current_len - 1);
                current_len++;
            }
            else
            {
                if (current_len + dir_len + 1 >= FTP_FILE_PATH_LEN)
                {
                    FTP_LOG("Client:%s, path too long\r\n", client->ip_str);
                    strcpy(client->path, ftp_config->mount_point);
                    return false;
                }
            }

            /* append directory / file name */
            strncat(client->path, dir, FTP_FILE_PATH_LEN - strlen(client->path) - 1);
        }
    }

    /* Validate path is safe and within mount point */
    if (!is_path_safe(ftp_config->mount_point, client->path))
    {
        FTP_LOG("Client:%s, attempted path traversal: \"%s\"\r\n",
            client->ip_str, client->path);
        strcpy(client->path, ftp_config->mount_point);
        return false;
    }

    FTP_LOG("Client:%s, new path working directory = \"%s\"\r\n",
        client->ip_str, client->path);
    
    return true;
}

/**
 * @brief Close child path (directory)
 *
 * @param client Client object
 *
 * @note It will be return to previous directory
 */
static void close_child(client_t* client)
{
    size_t len = strlen(client->path);

    if (client->path[len - 1] == '/')
    {
        client->path[len - 1] = '\0';
        len--;

        if (len == 0)
        {
            strcpy(client->path, ftp_config->mount_point);
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

        if (len == 0)
        {
            strcpy(client->path, ftp_config->mount_point);
        }
    }

    FTP_LOG("Client:%s, new pwd = \"%s\"\r\n",
        client->ip_str, client->path);
}

/**
 * @brief Get the param and open child path
 *
 * @param client Client object
 * @param buffer Buffer pointer of the param
 *
 * @return true if param is valid
 * @return false if param is invalid
 */
static bool get_param_and_open_child(client_t* client, char** buffer)
{
    if (pop_param(buffer, temp_buf, false, false, FTP_CMD_BUFFER_SIZE) == true)
    {
        if (!open_child(client, temp_buf))
        {
            return false;
        }

        client->close_child = true;
        return true;
    }

    return false;
}

/**
 * @brief Check path is valid and not '/' end
 *
 * @param client Client object
 *
 * @return true if path is valid
 * @return false if path is invalid
 */
static bool check_path(client_t* client)
{
    return ((strlen(client->path) > 0) &&
        (client->path[strlen(client->path) - 1] != '/'));
}

/**
 * @brief Authenticate client role
 *
 * @param client Client object
 * @param user User id from client input
 * @param pass User password from client input
 *
 * @return true if client authenticate ok
 * @return false if client authenticate failed
 */
static bool authenticate(client_t* client, char* user, char* pass)
{
    uint8_t i;

    for (i = 0; i < FTP_ROLE_UNKNOWN; i++)
    {
        if ((strcmp(user, users[i].username) == 0) &&
            (strcmp(pass, users[i].password) == 0))
        {
            client->role = users[i].role;
            client->allowed_cmds = users[i].allowed_cmds;
            return true;
        }
    }

    return false;
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
    bool has = ((client->allowed_cmds & cmd_flag) != 0);

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
 */
static void client_sign_out(client_t* client)
{
    if (!client) 
    {
        return;
    }
    
    close_fs(client);
    
    /* Close all sockets */
    close_socket(&client->cmd_sd);
    close_socket(&client->data_sd);
    close_socket(&client->ldata_sd);
    
    /* Free dynamically allocated memory */
    if (client->path)
    {
        free(client->path);
        client->path = NULL;
    }
    
    if (client->data)
    {
        free(client->data);
        client->data = NULL;
    }
    
    /* Reset the client structure */
    memset(client, 0, sizeof(client_t));
    client->cmd_sd = -1;
    client->data_sd = -1;
    client->ldata_sd = -1;
    client->active = false;
}

/**
 * @brief Client command process
 *
 * @param client Client object
 */
static void client_command(client_t* client)
{
    FRESULT res;
    FILINFO fno;
    char* buf = (char*)cmd_buffer;

    if (client->state > END_TRANSFER)
    {
        return;
    }

    /* Check for command channel timeout */
    if (client->cmd_timeout > FTP_CMD_TIMEOUT_MS)
    {
        FTP_LOG("Client:%s, command channel timeout (%"PRIu32" ms)\r\n",
            client->ip_str, client->cmd_timeout);
        send_reply(client, FTP_421_SERVICE_NOT_AVAILABLE, 
            "Timeout - closing control connection");
        client_sign_out(client);
        return;
    }

    memset(buf, 0, FTP_CMD_BUFFER_SIZE + CMD_SIZE);
    memset(temp_buf, 0, FTP_CMD_BUFFER_SIZE);
    client->close_child = false;

    int len = recv(client->cmd_sd,
        cmd_buffer,
        FTP_CMD_BUFFER_SIZE + CMD_SIZE,
        MSG_DONTWAIT);

    if (len <= 0)
    {
        return;
    }

    cmd_buffer[len] = '\0';

    /* buffer is moved as commands are being popped */
    ftp_cmd_t cmd = pop_command(&buf);

    if (((cmd != FTP_CMD_USER) && (cmd != FTP_CMD_PASS) &&
        (cmd != FTP_CMD_QUIT) && (cmd != FTP_CMD_FEAT) &&
        (cmd != FTP_CMD_AUTH)) && (client->role == FTP_ROLE_UNKNOWN))
    {
        send_reply(client, FTP_332_NEED_ACCOUNT, NULL);
        return;
    }

    if ((cmd >= 0) && (cmd < FTP_CMDS_MAX))
    {
        FTP_LOG("Client:%s, command: %s\r\n",
            client->ip_str, ftp_cmd_str[cmd].str);
    }

    switch (cmd)
    {
        case FTP_CMD_FEAT:
        {
            send_reply(client, FTP_502_COMMAND_NOT_IMPLEMENTED, "no-features");
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
            close_child(client);
            send_reply(client, FTP_250_ACTION_COMPLETE, NULL);
            break;
        }
        case FTP_CMD_CWD:
        {
            if (pop_param(&buf, temp_buf, false, false, FTP_CMD_BUFFER_SIZE))
            {
                if (strlen(temp_buf) > 0)
                {
                    /* Check for backslash (reject Windows-style paths) */
                    if (strchr(temp_buf, '\\') != NULL)
                    {
                        FTP_LOG("Client:%s, attempted path with '\': %s\r\n", 
                               client->ip_str, temp_buf);
                        send_reply(client, FTP_550_ACTION_NOT_TAKEN, "Invalid path");
                        break;
                    }
                    
                    // Ignore . entry
                    if (strcmp(temp_buf, ".") == 0)
                    {
                        send_reply(client, FTP_250_ACTION_COMPLETE, NULL);
                        break;
                    }

                    // Ignore .. entry
                    if (strcmp(temp_buf, "..") == 0)
                    {
                        close_child(client);
                        send_reply(client, FTP_250_ACTION_COMPLETE, NULL);
                        break;
                    }
                    else
                    {
                        open_child(client, temp_buf);
                    }
                }

                /* Check if already at mount point (avoid f_stat on root) */
                if (strcmp(client->path, ftp_config->mount_point) == 0)
                {
                    send_reply(client, FTP_250_ACTION_COMPLETE, NULL);
                }
                else
                {
                    /* Verify path exists and is a directory */
                    if ((f_stat(client->path, &fno) == FR_OK) &&
                        (fno.fattrib & AM_DIR))
                    {
                        send_reply(client, FTP_250_ACTION_COMPLETE, NULL);
                    }
                    else
                    {
                        close_child(client);
                        send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
                    }
                }
            }
            else
            {
                send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
            }
            break;
        }
        case FTP_CMD_PWD:
        case FTP_CMD_XPWD:
        {
            char* path = client->path;
            const char* mp = ftp_config->mount_point;
            size_t mp_len = strlen(mp);

            if (strncmp(path, mp, mp_len) == 0)
            {
                path += mp_len;
            }
            
            if (*path == '\0')
            {
                path = "/";
            }
            
            send_reply(client, FTP_257_PATHNAME_CREATED, path);
            break;
        }
        case FTP_CMD_SIZE:
        {
            if (pop_param(&buf, temp_buf, false, false, FTP_CMD_BUFFER_SIZE))
            {
                if (!build_file_path(client, temp_buf, (char*)client->data, FTP_DATA_BUFFER_SIZE))
                {
                    send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
                    break;
                }

                if ((f_stat((char*)client->data, &fno) == FR_OK) &&
                    !(fno.fattrib & AM_DIR))
                {
                    snprintf(temp_buf, FTP_CMD_BUFFER_SIZE,
                        "%"PRIu32, (uint32_t)fno.fsize);

                    FTP_LOG("client:%s, file \"%s\" size = %"PRIu32" bytes\r\n",
                        client->ip_str, client->data, (uint32_t)fno.fsize);

                    send_reply(client, FTP_213_FILE_STATUS, temp_buf);
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
        case FTP_CMD_MDTM:
        {
            if (pop_param(&buf, temp_buf, false, false, FTP_CMD_BUFFER_SIZE))
            {
                if (!build_file_path(client, temp_buf, (char*)client->data, FTP_DATA_BUFFER_SIZE))
                {
                    send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
                    break;
                }

                if ((f_stat((char*)client->data, &fno) == FR_OK) &&
                    !(fno.fattrib & AM_DIR))
                {
                    time_t time = get_file_time(&fno);
                    struct tm* tm = localtime(&time);

                    strftime((char*)client->data, FTP_DATA_BUFFER_SIZE,
                        "%Y%m%d%H%M%S", tm);

                    send_reply(client, FTP_213_FILE_STATUS, (char*)client->data);
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
        case FTP_CMD_MFMT:
        {
            char date_time[15] = { 0 };

            /* Get date time */
            if (pop_param(&buf, date_time, false, false, 15))
            {
                if (pop_param(&buf, temp_buf, false, false, FTP_CMD_BUFFER_SIZE))
                {
                    if (!build_file_path(client, temp_buf, (char*)client->data, FTP_DATA_BUFFER_SIZE))
                    {
                        send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
                        break;
                    }

                    if (0)  //TODO To be updated...
                    {
                        send_reply(client, FTP_213_FILE_STATUS, (char*)client->data);
                        break;
                    }
                }
            }

            send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
            break;
        }
        case FTP_CMD_TYPE:
        {
            send_reply(client, FTP_200_COMMAND_OK, NULL);
            break;
        }
        case FTP_CMD_USER:
        {
            pop_param(&buf, (char*)client->data, true, true, FTP_CMD_BUFFER_SIZE);

            FTP_LOG("Client:%s, user: %s\r\n", client->ip_str, client->data);
            send_reply(client, FTP_331_USER_NAME_OK, NULL);
            break;
        }
        case FTP_CMD_PASS:
        {
            pop_param(&buf, temp_buf, true, true, FTP_CMD_BUFFER_SIZE);

            FTP_LOG("Client:%s, pass: %s\r\n", client->ip_str, temp_buf);

            if (authenticate(client, (char*)client->data, temp_buf))
            {
                send_reply(client, FTP_230_USER_LOGGED_IN, "Login successful");
            }
            else
            {
                send_reply(client, FTP_530_NOT_LOGGED_IN, "Login incorrect");
            }
            break;
        }
        case FTP_CMD_PASV:
        {
            bool socket_created = true;
            uint16_t port = FTP_PASSIVE_DATA_PORT + client->number;

            // some servers (e.g. google chrome) send PASV several times very quickly
            close_socket(&client->ldata_sd);
            close_socket(&client->data_sd);

            client->state = DISCONNECTED;
            socket_created = listening_socket(&client->ldata_sd, NULL, port);

            if (socket_created)
            {
                uint32_t ip_addr = inet_addr(ftp_config->server_ip);
                uint8_t* pip = (uint8_t*)&ip_addr;

                client->data_timeout = 0;

                snprintf(temp_buf,
                    FTP_CMD_BUFFER_SIZE,
                    "(%u,%u,%u,%u,%u,%u)",
                    pip[0], pip[1], pip[2], pip[3],
                    (port >> 8), (port & 0xFFU));

                client->state = LISTEN_FOR_DATA;

                FTP_LOG("Client:%s, Data socket created on port %u\r\n", 
                        client->ip_str, port);

                send_reply(client, FTP_227_PASSIVE_MODE, temp_buf);
            }
            else
            {
                FTP_LOG("Client:%s, Error creating data socket on port %u (may be in use)\r\n", 
                        client->ip_str, port);
                send_reply(client, FTP_425_CANT_OPEN_DATA_CONNECTION, NULL);
            }
            break;
        }
        case FTP_CMD_LIST:
        case FTP_CMD_NLST:
        {
            if (get_param_and_open_child(client, &buf))
            {
                client->name_list = (cmd == FTP_CMD_LIST) ? false : true;

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
            uint8_t mode;
            uint32_t allow;
            client_state_t state;

            if (get_param_and_open_child(client, &buf))
            {
                client->time = 0;
                client->data_last = 0;
                client->data_count = 0;
                client->data_total = 0;

                if (check_path(client))
                {
                    if (cmd == FTP_CMD_RETR)
                    {
                        mode = FA_READ;
                        allow = ALLOW_RETR;
                        state = CONTINUE_FILE_TX;
                    }
                    else if (cmd == FTP_CMD_STOR)
                    {
                        mode = FA_WRITE | FA_CREATE_ALWAYS;
                        allow = ALLOW_STOR;
                        state = CONTINUE_FILE_RX;
                    }
                    else if (cmd == FTP_CMD_APPE)
                    {
                        mode = FA_WRITE | FA_OPEN_APPEND;
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

                    FTP_LOG("Client:%s, %s path -> \"%s\"\r\n",
                        client->ip_str, ftp_cmd_str[cmd].str, client->path);

                    if (open_file(client, mode))
                    {
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

            if (get_param_and_open_child(client, &buf))
            {
                if (check_path(client))
                {
                    res = f_unlink(client->path);

                    FTP_LOG("Client: %s, delete -> \"%s\" %s\r\n",
                        client->ip_str, client->path,
                        ((res == FR_OK) ? "ok" : "failed"));

                    send_reply(client, (res == FR_OK) ? 250 : 550, NULL);
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

            if (get_param_and_open_child(client, &buf))
            {
                if (check_path(client))
                {
                    res = f_mkdir(client->path);

                    FTP_LOG("Client: %s, making a directory -> \"%s\" %s(%d)\r\n",
                        client->ip_str, client->path,
                        ((res == FR_OK) ? "ok" : "failed"), res);

                    send_reply(client, (res == FR_OK) ? 250 : 550, NULL);
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

            if (get_param_and_open_child(client, &buf))
            {
                res = f_stat(client->path, &fno);

                if (res == FR_OK)
                {
                    // save the path of the file to rename
                    strcpy((char*)client->data, client->path);
                    send_reply(client, FTP_350_PENDING_FURTHER_INFO, NULL);
                }
                else
                {
                    send_reply(client, FTP_550_ACTION_NOT_TAKEN, NULL);
                }

                FTP_LOG("Client: %s, rename from -> \"%s\" %s(%d)\r\n",
                    client->ip_str, client->path,
                    ((res == FR_OK) ? "ok" : "failed"), res);

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

            if (get_param_and_open_child(client, &buf))
            {
                res = f_rename((const char*)client->data,
                    (const char*)client->path);

                FTP_LOG("Client: %s, rename to -> \"%s\" %s(%d)\r\n",
                    client->ip_str, client->path,
                    ((res == FR_OK) ? "ok" : "failed"), res);

                send_reply(client, (res == FR_OK) ? 250 : 550, NULL);
                break;
            }
            send_reply(client, FTP_553_FILE_NAME_NOT_ALLOWED, NULL);
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
            char *msg = (char*)client->data; // Reuse data buffer for large response
            int len = 0;
            int connected_count = 0;

            for (int i = 0; i < FTP_CLIENTS_MAX; i++)
            {
                if (clients[i].active)
                {
                    connected_count++;
                }
            }

            len += snprintf(msg + len, FTP_DATA_BUFFER_SIZE - len, 
                "211- FTP Server Status:\r\n"
                "      IP: %s:%d\r\n"
                "   Mount: %s\r\n"
                " Clients: %d/%d\r\n",
                ftp_config->server_ip, 
                FTP_CMD_PORT, 
                "/",
                connected_count, 
                FTP_CLIENTS_MAX);
            
            len += snprintf(msg + len, FTP_DATA_BUFFER_SIZE - len, "211 End of status.\r\n");

            send_data(client->cmd_sd, (uint8_t*)msg, len);
            break;
        }
        case FTP_CMD_QUIT:
        {
            send_reply(client, FTP_221_GOODBYE, NULL);
            client_sign_out(client);
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

                FTP_LOG("Client:%s, data socket connected\r\n", client->ip_str);
            }
            else if (client->data_timeout > FTP_DATA_TIMEOUT_MS)
            {
                FTP_LOG("Client:%s, Waiting for data connection timeout (%" PRIu32")\r\n",
                    client->ip_str, client->data_timeout);

                /* close the listening socket */
                close_socket(&client->ldata_sd);
                send_reply(client, FTP_421_SERVICE_NOT_AVAILABLE, "Data connection timeout");
                client_sign_out(client);
            }
            break;
        }
        case DATA_CONNECTED:
        {
            if (client->data_timeout > FTP_DATA_TIMEOUT_MS)
            {
                /* close the listening and the data socket */
                close_socket(&client->data_sd);
                close_socket(&client->ldata_sd);
                close_fs(client);
                client->state = DISCONNECTED;

                FTP_LOG("Client:%s, data connection timeout\r\n", client->ip_str);
            }
            break;
        }
        case END_TRANSFER:
        {
            if (client->ldata_sd >= 0)
            {
                close_socket(&client->ldata_sd);
            }

            if (client->data_sd >= 0)
            {
                close_socket(&client->data_sd);
            }

            /* Restore working directory if file operation was performed */
            if (client->close_child)
            {
                close_child(client);
                client->close_child = false;
            }

            client->state = DISCONNECTED;
            break;
        }
        case CONTINUE_LISTING:
        {
            result = list_directory(client, &size);

            if (size > 0)
            {
                if (send_data(client->data_sd, client->data, size) == FAILED)
                {
                    FTP_LOG("Client:%s, failed to send directory listing\r\n", client->ip_str);
                    send_reply(client, FTP_426_CONNECTION_CLOSED, NULL);
                    client->state = END_TRANSFER;
                    break;
                }
            }

            if (result == OK)
            {
                send_reply(client, FTP_226_TRANSFER_COMPLETE, NULL);
                client->state = END_TRANSFER;
            }

            client->cmd_timeout = 0;
            break;
        }
        case CONTINUE_FILE_TX:
        {
            /* read and send the next block from the file */
            client->data_timeout = 0;

            if (client->data_count == 0)
            {
                client->data_count =
                    read_file(client, client->data, FTP_DATA_BUFFER_SIZE);

                if (client->data_count < 0)
                {
                    send_reply(client, FTP_451_LOCAL_ERROR, NULL);
                    client->state = END_TRANSFER;
                    break;
                }
            }

            /* Check for transfer completion (EOF or last block) */
            bool transfer_complete = (client->data_count == 0) || 
                                    (client->data_count < FTP_DATA_BUFFER_SIZE);

            /* Send data if any */
            if (client->data_count > 0)
            {
                send_data(client->data_sd, client->data, (uint32_t)client->data_count);
                client->data_total += client->data_count;

                if ((client->data_total - client->data_last) > LOG_TRX_SIZE)
                {
                    client->data_last = client->data_total;
                    FTP_LOG("Client:%s, sent total: %"PRIu32"\r\n",
                        client->ip_str, client->data_total);
                }
            }

            if (transfer_complete)
            {
                FTP_LOG("Client:%s, file sent (%"PRIi32" bytes in %"PRIu32" ms)\r\n",
                    client->ip_str, client->data_total, client->time);

                /* Restore working directory */
                if (client->close_child)
                {
                    close_child(client);
                    client->close_child = false;
                }
                                
                send_reply(client, FTP_226_TRANSFER_COMPLETE, NULL);
                client->state = END_TRANSFER;
            }
            client->data_count = 0;
            break;
        }
        case CONTINUE_FILE_RX:
        {
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
                    FTP_LOG("Client:%s, error writing to file\r\n", client->ip_str);
                }
                else
                {
                    client->data_total += client->data_count;

                    if ((client->data_total - client->data_last) > LOG_TRX_SIZE)
                    {
                        client->data_last = client->data_total;
                        FTP_LOG("Client:%s, received total: %"PRIu32"\r\n",
                            client->ip_str, client->data_total);
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
                    FTP_LOG("Client:%s, receiving to file timeout\r\n", client->ip_str);
                }
            }
            else
            {
                FTP_LOG("Client:%s, file received (""%" PRIu32" bytes in ""%" PRIu32" ms)\r\n",
                    client->ip_str, client->data_total, client->time);

                /* File received (FAILED) */
                close_files_dir(client);
                
                /* Restore working directory immediately */
                if (client->close_child)
                {
                    close_child(client);
                    client->close_child = false;
                }
                
                send_reply(client, FTP_226_TRANSFER_COMPLETE, NULL);
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
 * @brief FTP initialization
 *
 * @param config Config object
 *
 * @return true if initialization is successful
 * @return false if initialization is failed
 */
bool ftp_init(const ftp_config_t* config)
{
    assert(config);

    ftp_deinit();

    /* The last character of mount_point can't be "/" */
    if (config->mount_point[strlen(config->mount_point) - 1] == '/')
    {
        FTP_LOG("FTP server mount point must not end with '/'\r\n");
        return false;
    }

    ftp_config = config;

    lc_sd = -1;
    state = DISABLED;
    flag.all = 0x0U;
    flag.bits.init = 1;

    return true;
}

/**
 * @brief FTP deinitialization
 *
 * @return true if deinitialization is successful
 * @return false if deinitialization is failed
 *
 * @note It will be sign out all client too.
 */
bool ftp_deinit(void)
{
    uint8_t i;

    flag.all = 0;
    ftp_config = NULL;

    for (i = 0; i < FTP_CLIENTS_MAX; i++)
    {
        client_sign_out(&clients[i]);
    }

    ftp_disable_time();

    return true;
}

/**
 * @brief FTP reset
 *
 * @note It will be reinitialization
 */
void ftp_reset(void)
{
    const ftp_config_t* config = ftp_config;
    const ftp_unix_time_t get_time = get_unix_time;

    ftp_init(config);
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

    get_unix_time = get_time;
}

/**
 * @brief FTP disable time function
 *
 * @note FTP file and directory time use default time
 */
void ftp_disable_time(void)
{
    get_unix_time = default_unix_time;
}

/**
 * @brief FTP set role username
 *
 * @param role Role
 * @param new_username new username
 *
 * @return true if new username is set successful
 * @return false if new username is set failed
 */
bool ftp_set_username(ftp_role_t role, const char* new_username)
{
    if ((state != RUNNING) && (role < FTP_ROLE_UNKNOWN))
    {
        strncpy(users[role].username, new_username, sizeof(users[role].username) - 1);
        users[role].username[sizeof(users[role].username) - 1] = '\0';
        return true;
    }

    return false;
}

/**
 * @brief FTP set role password
 *
 * @param role Role
 * @param new_password new password
 *
 * @return true if new password is set successful
 * @return false if new password is set failed
 */
bool ftp_set_password(ftp_role_t role, const char* new_password)
{
    if ((state != RUNNING) && (role < FTP_ROLE_UNKNOWN))
    {
        strncpy(users[role].password, new_password, sizeof(users[role].password) - 1);
        users[role].password[sizeof(users[role].password) - 1] = '\0';
        return true;
    }

    return false;
}

/**
 * @brief FTP enable
 *
 * @return true if enable is successful
 * @return false if enable is failed
 */
bool ftp_enable(void)
{
    bool res = false;

    if ((state == DISABLED) && (flag.bits.init == 1))
    {
        flag.bits.enable = 1;
        res = true;
    }
    else
    {
        FTP_LOG("Failed to enable FTP server (state=%d, init=%d)\n", 
               state, flag.bits.init);
    }

    return res;
}

/**
 * @brief FTP is enable or not
 *
 * @return true is enable
 * @return false is disable
 */
bool ftp_enabled(void)
{
    return (flag.bits.enable == 1);
}

/**
 * @brief FTP disable
 *
 * @return true if disable is successful
 * @return false if disable is failed
 *
 * @note It will be sign out all client too.
 */
bool ftp_disable(void)
{
    uint8_t i;
    bool res = false;

    if (state == RUNNING)
    {
        for (i = 0; i < FTP_CLIENTS_MAX; i++)
        {
            send_reply(&clients[i], FTP_221_GOODBYE, NULL);
            client_sign_out(&clients[i]);
        }

        flag.bits.enable = 0;
        state = DISABLED;
        res = true;
    }

    return res;
}

/**
 * @brief FTP server
 *
 * @param pulse_1ms 1ms Clock for FTP running
 */
void ftp_server(uint32_t pulse_1ms)
{
    switch (state)
    {
        case DISABLED:
        {
            if (flag.bits.enable)
            {
                state = START;
            }
            break;
        }
        case START:
        {
            /* listening command socket descriptor */
            if (listening_socket(&lc_sd, ftp_config->server_ip, FTP_CMD_PORT))
            {
                state = RUNNING;
                
                /* Log successful startup with IP and port info */
                FTP_LOG("\n");
                FTP_LOG("========================================\n");
                FTP_LOG("FTP Server Started Successfully\n");
                FTP_LOG("========================================\n");
                FTP_LOG("  IP Address : %s\n", ftp_config->server_ip);
                FTP_LOG("  Port       : %d\n", FTP_CMD_PORT);
                FTP_LOG("  Max Clients: %d\n", FTP_CLIENTS_MAX);
                FTP_LOG("  Mount Point: %s\n", ftp_config->mount_point);
                FTP_LOG("========================================\n");
                FTP_LOG("Ready to accept connections...\n\n");
            }
            else
            {
                FTP_LOG("Failed to start FTP server on %s:%d\n", 
                       ftp_config->server_ip, FTP_CMD_PORT);
            }
            break;
        }
        case RUNNING:
        {
            uint8_t i;

            connection();

            for (i = 0; i < FTP_CLIENTS_MAX; i++)
            {
                if (clients[i].active)
                {
                    clients[i].time += pulse_1ms;
                    clients[i].cmd_timeout += pulse_1ms;
                    clients[i].data_timeout += pulse_1ms;

                    client_state(&clients[i]);
                    client_command(&clients[i]);
                }
            }
            break;
        }
    }

}
