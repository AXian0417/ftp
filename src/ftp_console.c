// SPDX-License-Identifier: MIT
/**
 * @file ftp_console.c
 * @author A Xian (axian0417@gmail.com)
 *
 * @brief Command line front end for the FTP client.
 * @details Maps text commands (open, user, pass, ls, cd, pwd, mkdir, rmdir, delete, rename,
 *          get, put, abort, status, cstatus, bye) to the FTP client API on one static
 *          client instance and prints the results through the print function given to
 *          ftp_console_init() (vprintf if NULL). ftp_console() takes main() arguments,
 *          so a shell can register it directly. Commands return at once; ftp_process()
 *          executes them.
 *
 * @copyright Copyright (c) 2026 A Xian
 *
 */

#include "ftp_console.h"
#include "ftp_client.h"
#include "string.h"
#include "stdio.h"
#include "stdarg.h"
#include "stdlib.h"

static ftp_client_t client;
static ftp_client_t* p_client = &client;
static ftp_console_print_t print_function = NULL;
static bool inited;

typedef struct 
{
    const char* name;
    void (*handler)(int argc, char** argv);
} cmd_entry_t;

/*------------------------------------------ Private API ------------------------------------------*/

/**
 * @brief Check if a string is a number
 * 
 * @param str String to check
 * 
 * @return true if the string is a number, false otherwise
 */
static bool is_number(const char* str)
{
    while(*str)
    {
        if ((*str < '0') || (*str > '9')) 
        {
            return false;
        }
        str++;
    }

    return true;
}

/**
 * @brief Print a formatted string to the console
 * 
 * @param fmt Format string
 * @param ... Arguments for the format string
 */
static void console_print(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);

    if (print_function)
    {
        char buf[256];

        vsnprintf(buf, sizeof(buf), fmt, ap);
        print_function("%s", buf);
    }
    else
    {
        vprintf(fmt, ap);
    }

    va_end(ap);
}

/**
 * @brief Print an error message to the console
 * 
 * @param cmd Command that failed
 */
static void cmd_error(const char* cmd)
{
    console_print("'%s' failed\r\n", cmd);
}

/**
 * @brief Callback function for FTP events
 * 
 * @param client FTP client object
 * @param event FTP event
 * @param data Event data
 * @param len Event data length
 */
static void callback(ftp_client_t* client, ftp_events_t event, void* data, size_t len)
{
    switch(event)
    {
        default: { break; }
        case FTP_ON_TRANSFER_DATA:
        {
            if (client->mode == FTP_MODE_LISTING)
            {
                if(data) { console_print("%.*s\r\n", (int)len, (char*)data); }
            }
            break;
        }
    }
}

/**
 * @brief Command: help
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_help(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    console_print("FTP Client Commands:\r\n");
    console_print("  open <url> | <host> [port]     - Connect to FTP\r\n");
    console_print("  user <name> [pass]             - Send USER command\r\n");
    console_print("  pass <pass>                    - Send PASS command\r\n");
    console_print("  bye                            - Disconnect and exit\r\n");
    console_print("  quit                           - Disconnect and exit\r\n");
    console_print("  close                          - Disconnect\r\n");
    console_print("  ls [path]                      - List directory\r\n");
    console_print("  cd <path>                      - Change directory\r\n");
    console_print("  pwd                            - Print working directory\r\n");
    console_print("  mkdir <path>                   - Make directory\r\n");
    console_print("  rmdir <path>                   - Remove directory\r\n");
    console_print("  delete <file>                  - Delete file/directory\r\n");
    console_print("  rename <from> <to>             - Rename or move file/directory\r\n");
    console_print("  get <remote file> [local file] - Download file\r\n");
    console_print("  put <local file> [remote file] - Upload file\r\n");
    console_print("  abort                          - Abort the transfer in progress\r\n");
    console_print("  status                         - Show server status\r\n");
    console_print("  cstatus                        - Show client status\r\n");
}

/**
 * @brief Command: open
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_open(int argc, char** argv)
{
    if ((argc < 2) || !argv[1])
    {
        console_print("Usage: open <url> | <host> [port]\r\n");
        return;
    }

    char* arg1 = argv[1];

    if (strncmp(arg1, "ftp://", 6) == 0)
    {
        if (!ftp_open_url(p_client, arg1))
        {
            console_print("Failed to open URL %s\r\n", arg1);
        }
    }
    else
    {
        /* Host mode logic */
        const char* host = arg1;
        uint16_t port = FTP_CMD_PORT;
        
        /* Check if argv[2] is a port number */
        if ((argc >= 3) && is_number(argv[2]))
        {
            port = atoi(argv[2]);
        }
        
        if (!ftp_open(p_client, host, port))
        {
            console_print("Failed to connect to %s\r\n", host);
        }
    }
}

/**
 * @brief Command: user
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_user(int argc, char** argv)
{
    if (argc < 2 || !argv[1])
    {
        console_print("Usage: user <name> [password]\r\n");
        return;
    }
    
    const char* pass = (argc > 2) ? argv[2] : NULL;

    if (!ftp_user(p_client, argv[1], pass))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: pass
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_pass(int argc, char** argv)
{
    const char* arg = (argc > 1) ? argv[1] : NULL;

    if (!arg)
    {
        console_print("Usage: pass <password>\r\n");
        return;
    }
    
    if (!ftp_password(p_client, arg))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: bye
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_bye(int argc, char** argv)
{
    (void)argc;

    if (!ftp_quit(p_client))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: ls
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_ls(int argc, char** argv)
{
    const char* arg = (argc > 1) ? argv[1] : NULL;
    
    if (!ftp_ls(p_client, arg))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: cd
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_cd(int argc, char** argv)
{
    const char* arg = (argc > 1) ? argv[1] : NULL;

    if (!arg) 
    {
        console_print("Usage: cd <path>\r\n");
        return;
    }

    if (!ftp_cd(p_client, arg))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: pwd
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_pwd(int argc, char** argv)
{
    (void)argc;
    
    if (!ftp_pwd(p_client))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: mkdir
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_mkdir(int argc, char** argv)
{
    const char* arg = (argc > 1) ? argv[1] : NULL;
    if (!arg) 
    {
        console_print("Usage: mkdir <path>\r\n");
        return;
    }

    if (!ftp_mkdir(p_client, arg))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: rmdir
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_rmdir(int argc, char** argv)
{
    const char* arg = (argc > 1) ? argv[1] : NULL;

    if (!arg) 
    {
        console_print("Usage: rmdir <path>\r\n");
        return;
    }

    if (!ftp_rmdir(p_client, arg))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: delete
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_delete(int argc, char** argv)
{
    const char* arg = (argc > 1) ? argv[1] : NULL;

    if (!arg)
    {
        console_print("Usage: delete <file>\r\n");
        return;
    }
    
    if (!ftp_delete(p_client, arg))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: rename
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_rename(int argc, char** argv)
{
    const char* from = (argc > 1) ? argv[1] : NULL;
    const char* to = (argc > 2) ? argv[2] : NULL;

    if (!from || !to)
    {
        console_print("Usage: rename <from> <to>\r\n");
        return;
    }
    
    if (!ftp_rename(p_client, from, to))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: get
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_get(int argc, char** argv)
{
    const char* remote_file = (argc > 1) ? argv[1] : NULL;
    const char* local_file = (argc > 2) ? argv[2] : NULL;
    const char* target_local = NULL;

    if (!remote_file)
    {
        console_print("Usage: get <remote file> [local file]\r\n");
        return;
    }

    /* Determine local filename */
    if (local_file)
    {
        /* User specified local name */
        target_local = local_file;
    }
    else
    {
        /* Use basename of remote file */
        const char* p = strrchr(remote_file, '/');
        target_local = p ? p + 1 : remote_file;
    }
    
    if (!ftp_get(p_client, remote_file, target_local))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: put
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_put(int argc, char** argv)
{
    const char* local_file = (argc > 1) ? argv[1] : NULL;
    const char* remote_file = (argc > 2) ? argv[2] : NULL;
    const char* target_remote = NULL;

    if (!local_file)
    {
        console_print("Usage: put <local file> [remote file]\r\n");
        return;
    }

    if (remote_file)
    {
        target_remote = remote_file;
    }
    else
    {
        const char* p = strrchr(local_file, '/');
        target_remote = p ? p + 1 : local_file;
    }

    if (!ftp_put(p_client, local_file, target_remote))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: abort
 *
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_abort(int argc, char** argv)
{
    (void)argc;

    if (!ftp_abort(p_client))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: status
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_status(int argc, char** argv)
{
    (void)argc;

    if (!ftp_status(p_client))
    {
        cmd_error(argv[0]);
    }
}

/**
 * @brief Command: cstatus
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 */
static void cmd_cstatus(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if (p_client && p_client->inited) 
    {
        console_print("--- FTP Client Status ---\r\n");
        console_print("Host: %s:%d\r\n", p_client->host, p_client->port);
        console_print("User: %s\r\n", p_client->user);
        console_print("State: %s\r\n", ftp_client_state_string(p_client));
        console_print("Last Message: %s\r\n", p_client->status);
        console_print("-------------------------\r\n");
    } 
    else 
    {
        console_print("Client not initialized\r\n");
    }
}

static const cmd_entry_t cmd_table[] = 
{
    {"help", cmd_help},
    {"open", cmd_open},
    {"user", cmd_user},
    {"pass", cmd_pass},
    {"bye", cmd_bye},
    {"quit", cmd_bye},
    {"close", cmd_bye},
    {"ls", cmd_ls},
    {"dir", cmd_ls},
    {"cd", cmd_cd},
    {"pwd", cmd_pwd},
    {"mkdir", cmd_mkdir},
    {"rmdir", cmd_rmdir},
    {"delete", cmd_delete},
    {"rename", cmd_rename},
    {"get", cmd_get},
    {"put", cmd_put},
    {"abort", cmd_abort},
    {"status", cmd_status},
    {"cstatus", cmd_cstatus},
    {NULL, NULL}
};

/*------------------------------------------ Public API ------------------------------------------*/

/**
 * @brief Initialize the FTP console
 * 
 * @param func Output function, NULL for vprintf (stdout)
 */
void ftp_console_init(ftp_console_print_t func)
{
    print_function = func;
    /* Console commands run in the console task, not in the ftp_process() loop: default resolver */
    ftp_client_register(p_client, "ConsoleFTP", NULL, callback, NULL);
    inited = true;
}

/**
 * @brief Run one console command
 * 
 * @param argc Number of arguments, including the command name
 * @param argv Arguments as main(): argv[0] is the command name (e.g. "ftp"),
 *             argv[1] the console command
 * 
 * @return 0 on success, 1 if the command is unknown or the console is not initialized
 */
int ftp_console(int argc, char** argv)
{
    if (!inited)
    {
        console_print("ftp_console_init() missing\r\n");
        return 1;
    }

    /* No console command: help */
    if ((argc < 2) || (argv[1] == NULL))
    {
        cmd_help(0, NULL);
        return 0;
    }

    const char* cmd = argv[1];
    const cmd_entry_t* entry = cmd_table;
    
    while (entry->name)
    {
        if (strcmp(entry->name, cmd) == 0)
        {
            /* Handlers see the console command as argv[0] */
            entry->handler(argc - 1, argv + 1);
            return 0;
        }
        entry++;
    }

    console_print("Unknown command: %s (Try 'help')\r\n", cmd);
    return 1;
}
