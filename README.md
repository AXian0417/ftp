# FTP Module

This module implements a fully featured FTP Server and a non-blocking FTP Client for MCUs. It supports standard FTP commands and passive mode, and uses only the POSIX file and socket APIs (no file system or network stack specific code).

## Features

### FTP Server
- **Standard FTP Protocol**: Supports common commands (`USER`, `PASS`, `LIST`, `NLST`, `PWD`, `CWD`, `RETR`, `STOR`, `APPE`, `DELE`, `MKD`, `RMD`, `RNFR/RNTO`, `SIZE`, `MDTM`, `FEAT`, `OPTS UTF8`, `ABOR`, `MODE S`, `STRU F`, `ALLO`, `EPSV`, `REST`, `MFMT`).
- **Resume**: `REST` (RFC 3659 stream mode) restarts `RETR`/`STOR` at an offset; FEAT lists `REST STREAM`.
- **Passive Modes**: `PASV` and `EPSV` (RFC 2428, port only, works behind address translation); `EPSV ALL` is honoured.
- **Interfaces & NAT**: listens on all interfaces; `PASV` announces the interface the client reached. Behind NAT, `ftp_set_pasv_address()` sets the public address announced to public clients; clients of a local network keep the interface address.
- **Robust Command Lines**: a command split by TCP or several commands in one segment are handled (lines are taken one by one with `MSG_PEEK`, no buffer per client); `ABOR` is accepted during a transfer, other commands wait until it ends.
- **POSIX Files**: File operations use `open/read/write/close/stat/opendir/readdir/closedir/unlink/rmdir/mkdir/rename` only, any file system behind them works.
- **UTF-8 File Names**: Names are passed to the file system unchanged (RFC 2640, `FEAT` announces `UTF8`).
- **Virtual Root**: Virtualizes the file system root (e.g., `/sd/FTP` maps to `/`) for security.
- **Robust Path Handling**: Handles client path anomalies and normalizes paths.
- **Passive Mode (PASV)**: Supports passive data connections.
- **Multiple Clients**: Configurable concurrent client connections.
- **Thread-safe Control**: `ftp_enable()`/`ftp_disable()` may be called from another thread than `ftp_server()`; they only record the request.
- **No malloc Option**: `FTP_STATIC_BUFFERS` puts all client buffers in `.bss`.

### FTP Client
- **Non-blocking Architecture**: Event-driven state machine for background operation, `ftp_process()` never waits.
- **Multi-line Replies**: RFC 959 multi-line replies (`220-`, `230-` ...) are handled as one reply in every state.
- **Thread-safe API**: API functions may be called from another thread than `ftp_process()` (one API thread per client).
- **Flexible Callbacks**: Decoupled protocol logic from UI/Application.

### FTP Console
- **Console Interface**: Built-in CLI for interactive.

## Files and Build

```
include/        public headers, the only directory the application includes
src/            library sources (always compiled), ftp_sock.h is internal
port/           optional: ftp_resolver_lwip.c (see "Host names" below)
Kconfig         configuration options
gen_config.py   generates ftp_config.h for non ESP-IDF projects
CMakeLists.txt  ESP-IDF component
```

- **ESP-IDF**: the directory is a component, add `PRIV_REQUIRES ftp` (or `REQUIRES ftp`). Options are under *Component config → FTP* in `idf.py menuconfig` and come from `sdkconfig.h`; no `ftp_config.h` is used.
- **Other build systems**: compile `src/*.c` (plus `port/*.c` when used), add `include/` and the directory of the generated `ftp_config.h` to the include path.

## Configuration

Options are defined in `Kconfig` and compiled in as `CONFIG_FTP_*` macros.

- **ESP-IDF**: `idf.py menuconfig` → *Component config → FTP*.
- **Other platforms**: generate `ftp_config.h` from the project root:

  ```
  pip3 install kconfiglib
  python3 ftp/gen_config.py            # opens menuconfig
  python3 ftp/gen_config.py --silent   # regenerates the header without the UI
  ```

  The selection is stored in `.ftp_config` in the current working directory, and `ftp_config.h` is written into the directory named by `FTP_CONFIG_HEADER_DIR` (default `.`).

| Option | Default | Description |
|-------|---------|-------------|
| `FTP_CLIENTS_MAX` | `3` | Max concurrent server clients |
| `FTP_CMD_PORT` | `21` | FTP control port |
| `FTP_PASSIVE_DATA_PORT` | `2024` | First port for PASV mode |
| `FTP_LIST_NUM` | `64` | Directory entries per listing chunk |
| `FTP_MAX_USER_LEN` / `FTP_MAX_PASS_LEN` | `32` / `64` | User name / password buffer sizes (server roles and client); longer values are refused, never truncated |
| `FTP_USER/MAINT/ADMIN_DEFAULT_PASSWORD` | `"1234"` | Default passwords of the roles (must fit `FTP_MAX_PASS_LEN`, checked at compile time) |
| `FTP_FILE_PATH_LEN` | `128` | Max path string length |
| `FTP_CMD_BUFFER_SIZE` | `256` | Command buffer size |
| `FTP_DATA_BUFFER_SIZE` | `1024` | Data buffer size |
| `FTP_MFMT` | `n` | `MFMT` sets the modification time (needs `utime()`); `n`: MFMT replies 502 |
| `FTP_STATIC_BUFFERS` | `n` | `n`: server client buffers from the heap while connected; `y`: static, no malloc |
| `FTP_CMD_TIMEOUT_MS` | `180000` | Command timeout (3 mins) |
| `FTP_DATA_TIMEOUT_MS` | `30000` | Data transfer timeout (30 s) |
| `FTP_LOG_LEVEL` | `INFO` | `NONE` / `ERROR` / `WARN` / `INFO` / `DEBUG` / `VERBOSE`, lower levels are removed at compile time (see "Logs") |
| `FTP_LOG_CUSTOM_IMPL` | `n` | Logging through the application's `ftp_log_output()` instead of `printf` |
| `FTP_CONFIG_HEADER_DIR` | `"."` | Output directory of `ftp_config.h`, hidden under ESP-IDF |

### Logs

| Level | Content |
|---|---|
| E | The server cannot work: mount point, start or listen failure |
| W | Service lost: connection refused, file read/write error, transfer failed, path outside the root |
| I | Lifecycle: start/stop, connect, login result, disconnect reason, transfer done (size, time) |
| D | One line per command: command (`PASS ****`), reply, resolved path, result |
| V | Traces: listing entries, transfer progress, data connection, server replies on the client |

Default output (`printf`), same layout as ESP-IDF; the time is the milliseconds counted from `pulse_1ms`:

```
I (61230) ftp: #0 192.168.4.2 connected
I (61450) ftp: #0 192.168.4.2 login "admin" failed: password mismatch (got 5 chars, expected 4)
I (62780) ftp: #0 192.168.4.2 sent "/a.bin" 1048576 bytes in 1320 ms
I (241460) ftp: #0 192.168.4.2 disconnected: command timeout
```

`#0` is the client slot (one IP may hold several connections). Passwords are never logged; a failed login logs the reason and the lengths, the client only sees `530 Login incorrect`.

Custom output (`FTP_LOG_CUSTOM_IMPL`): the application defines

```c
/* tag: "ftp" (server) or "ftp-cli" (client); fmt has no line ending */
void ftp_log_output(ftp_log_level_t level, const char* tag, const char* fmt, ...);
```

### Minimal configuration

Every command stays available; only sizes and logs shrink. Options with a platform requirement
(`FTP_MFMT`) stay off.

| Option | Minimal | Default |
|---|---|---|
| `FTP_CLIENTS_MAX` | 1 | 3 |
| `FTP_FILE_PATH_LEN` | 128 | 128 |
| `FTP_CMD_BUFFER_SIZE` | 128 | 256 |
| `FTP_DATA_BUFFER_SIZE` | 512 | 1024 |
| `FTP_MAX_USER_LEN` / `FTP_MAX_PASS_LEN` | 16 / 32 | 32 / 64 |
| `FTP_LOG_LEVEL` | NONE | INFO |
| `FTP_MFMT` | n | n |
| `FTP_STATIC_BUFFERS` | y (RAM known at link time) | n |

Measured on ESP32 (`-Os`, `ftp_server.c`):

| Configuration | Code | RAM |
|---|---|---|
| Defaults (INFO logs) | 13.9 KB | 1.0 KB static, plus 1.5 KB heap per connected client |
| Defaults, no logs | 9.5 KB | same |
| Minimal | 9.2 KB | 1.4 KB, all static (one client included), no malloc |

A client needs `2 * FTP_FILE_PATH_LEN + FTP_DATA_BUFFER_SIZE + FTP_CMD_BUFFER_SIZE` bytes.
`MFMT` adds about 0.8 KB of code, `ftp_sock.c` about 0.8 KB; the client and the console are
linked only when used. `FTP_FILE_PATH_LEN` includes the mount point: keep it short.

---

## Dependencies and Platform Integration

The library uses only the POSIX file and socket APIs. The platform provides them; nothing in
the library is platform specific. Each requirement below fails at build or start-up when it is
missing, except the ones marked **silent**, which need care.

### Files

POSIX file API only: `open`, `read`, `write`, `close`, `lseek`, `fstat`, `stat`, `opendir`, `readdir`, `closedir`, `unlink`, `rmdir`, `mkdir`, `rename`, and `utime` when `FTP_MFMT` is enabled (a platform without it fails at link time: disable the option).

- `mount_point` and the client's local file names are paths of this API. The application creates the `mount_point` directory; `ftp_init()` returns `false` if it is not an accessible directory (not mounted, a file system specific path such as a drive prefix `0:/FTP`, or no file syscalls). The library does not create it: a mistyped path would otherwise be served as a new empty directory.
- Open files: one per connected server client and one per client transfer, plus whatever the application uses. Directories being listed are open as well.
- Concurrent access to the same file by two sessions (one writes, one reads) is not prevented by the library; enable the file system's own lock.
- **Silent**: UTF-8 file names (announced by `FEAT`) need a file system that stores them. With FAT, long file names and the UTF-8 API encoding must be enabled; otherwise short names are altered or names look garbled on a PC. The library cannot check this through POSIX, so the platform's file module should stop the build when the setting is missing (`#error`).

| Platform | To do |
|---|---|
| Linux, macOS | nothing |
| ESP-IDF | mount on the VFS (`esp_vfs_fat_*_mount`, base path e.g. `/sd`), `max_files` covers the open files above; `CONFIG_FATFS_FS_LOCK`; `CONFIG_FATFS_LFN_*` + `CONFIG_FATFS_API_ENCODING_UTF_8` |
| Bare metal / RTOS with newlib (e.g. STM32 + FatFs) | a syscall layer, see below; `FF_FS_LOCK`; long file names with UTF-8 (`FF_LFN_UNICODE = 2` from FatFs R0.13, older versions: UTF-16 API converted in the syscall layer) |

### Network

POSIX socket API only, all sockets non-blocking: `socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv` (also with `MSG_PEEK`), `close`, `fcntl(F_GETFL/F_SETFL, O_NONBLOCK)`, `select` with a zero timeout (writable check only), `getsockopt(SO_ERROR)`, `setsockopt(SO_REUSEADDR)`, `inet_pton`, `htons/htonl/ntohs/ntohl`, `getaddrinfo/freeaddrinfo`. No `closesocket`, `ioctlsocket` or `MSG_DONTWAIT`.

| Platform | To do |
|---|---|
| Linux, macOS, ESP-IDF | nothing |
| lwIP with an RTOS | add lwIP `src/include/compat/posix` to the include path; `LWIP_SOCKET 1`, `LWIP_COMPAT_SOCKETS 1`, `LWIP_POSIX_SOCKETS_IO_NAMES 0`, `LWIP_SOCKET_OFFSET` above the file descriptors (see "Shared descriptors") |
| lwIP without an RTOS (`NO_SYS 1`) | lwIP has no sockets: a socket emulation layer provides the subset above on the raw API; `getaddrinfo()` may fail, the application then registers a resolver (`port/ftp_resolver_lwip.c`) |

| Missing | Detected by |
|---|---|
| compat include path | compile error (header not found) |
| `LWIP_POSIX_SOCKETS_IO_NAMES 1` (`close/read/write` become macros) | `#error` in the library sources |
| `FD_SETSIZE` too small | lwIP's own `#error` |
| `fcntl()` not routed to the socket stack | non-blocking setup fails, the server refuses to start and logs it |

### Shared descriptors (bare metal with a socket stack)

Files and sockets share `close()` and `fcntl()`, so the syscall layer routes each descriptor by range:

| Range | Use |
|---|---|
| 0 – 2 | stdin, stdout, stderr |
| 3 – `LWIP_SOCKET_OFFSET - 1` | files |
| `LWIP_SOCKET_OFFSET` and above | sockets (allocated by lwIP) |

The only silent failure is overlapping ranges (`close()` of a socket closing a file). Two guards:

```c
/* L1: the file table size comes from the socket offset, the ranges cannot overlap */
#define FILE_FD_FIRST   3
#define FILE_FD_MAX     (LWIP_SOCKET_OFFSET - FILE_FD_FIRST)
_Static_assert(FILE_FD_MAX > 0, "LWIP_SOCKET_OFFSET must leave room for file descriptors");

int _close(int fd)
{
    if (fd >= LWIP_SOCKET_OFFSET)
    {
        return lwip_close(fd);              /* _fcntl dispatches the same way */
    }
    /* file: fd - FILE_FD_FIRST indexes the file table */
}
```

L2: a descriptor that belongs to no open file or socket asserts in debug builds and returns `EBADF` otherwise.

### Bare-metal syscall layer (newlib)

- Implement `_open/_close/_read/_write/_lseek/_fstat/_stat/_isatty/_unlink`, and provide `rename`, `mkdir`, `rmdir`, `opendir`, `readdir`, `closedir` under their POSIX names (newlib does not always forward them). Map file system results to `errno`.
- Provide `dirent.h` in an include directory searched before the system headers. With FatFs, `#define DIR FF_DIR` before including `ff.h` (both define `DIR`).
- Exclude the generated `syscalls.c` from the build (keep `sysmem.c`); add the layer as source, not as a `.a` (**silent**: weak symbols in the generated file win over strong ones in an archive).
- With an RTOS: lock the descriptor table, enable the file system's reentrancy (`FF_FS_REENTRANT`), and newlib's per-task `errno`/`malloc` support (FreeRTOS `configUSE_NEWLIB_REENTRANT`).
- `stdout`/`stderr` may go to the board's console from `_write` (descriptors 1 and 2).

---

## API Reference

### 1. FTP Server (`ftp_server.h`)

**Initialization & Control**
```c
/* Initialize server; mount_point is copied (at most FTP_FILE_PATH_LEN - 1 characters) */
bool ftp_init(const char* mount_point);

/* De-initialize server and free resources */
bool ftp_deinit(void);

/* Reset server */
void ftp_reset(void);

/* Enable server (Start accepting connections) */
bool ftp_enable(void);

/* Disable server (Stop accepting connections) */
bool ftp_disable(void);

/* Check if server is enabled */
bool ftp_enabled(void);
```

**Task Execution**
```c
/* Main server task function. Call this periodically or in a dedicated task. 
   pulse_1ms: 1ms time elapsed since last call (for timeouts) */
void ftp_server(uint32_t pulse_1ms);
```

**Threads**

| Functions | Where to call |
|---|---|
| `ftp_init`, `ftp_deinit`, `ftp_reset` | Before `ftp_server()` starts or after it stops |
| `ftp_enable`, `ftp_disable`, `ftp_enabled`, `ftp_enable_time`, `ftp_disable_time`, `ftp_set_username`, `ftp_set_password`, `ftp_set_pasv_address` | Any thread, one thread at a time; return at once. `ftp_server()` applies the request in its next call: starts or stops the server (stop signs out all clients); new credentials apply to new logins at once, the clients logged in with that role are signed out (421) |

**User Management**
```c
/* Set username/password for specific roles */
bool ftp_set_username(ftp_role_t role, const char* new_username);
bool ftp_set_password(ftp_role_t role, const char* new_password);
```

**Passive Address (NAT)**
```c
/* Address announced by PASV to public clients, NULL or "" for none (default) */
bool ftp_set_pasv_address(const char* ipv4);
```
Without it, `PASV` announces the local address the client connected to, so every interface works without settings. Behind a NAT router, forward `FTP_CMD_PORT` and `FTP_PASSIVE_DATA_PORT` ... `FTP_PASSIVE_DATA_PORT + FTP_CLIENTS_MAX - 1` with the same external port numbers, and set the router's public address. Clients from private, loopback or link-local addresses still get the local address. `EPSV` announces no address and needs no setting.

**Time Synchronization**
```c
/* Enable/Disable file timestamp synchronization */
void ftp_enable_time(const ftp_unix_time_t get_time);
void ftp_disable_time(void);
```

---

### 2. FTP Client (`ftp_client.h`)

**Core Functions**
```c
/* Register a new client instance (resolver: NULL by default, see "Host names") */
bool ftp_client_register(ftp_client_t* client, const char* name, void* user, ftp_client_cb_t cb, ftp_resolver_t resolver);

/* Unregister client */
bool ftp_client_unregister(ftp_client_t* client);

/* Main client processing function. Call this periodically. */
void ftp_process(uint32_t pulse_1ms);
```

**Connection & Authentication**
```c
/* Connect to server (Host/IP, Port, User, Pass) */
bool ftp_connect(ftp_client_t* client, const char* host, uint16_t port, const char* user, const char* pass);

/* Connect via URL (ftp://user:pass@host:port) */
bool ftp_open_url(ftp_client_t* client, const char* url);

/* Disconnect */
bool ftp_quit(ftp_client_t* client);
```

API functions only post a request and return at once (`false` if the client is busy
or in the wrong state); `ftp_process()` executes it and reports through the callback.

**Host names**

| Where `ftp_connect()` is called | Resolver to register | Behaviour |
|---|---|---|
| Another thread than `ftp_process()` (default) | `NULL` | Resolved in the caller's thread with `getaddrinfo()`, blocks the caller only |
| The thread of `ftp_process()` (single thread, no RTOS, FTP callbacks) | `ftp_resolver_lwip` (add `port/ftp_resolver_lwip.c`) | Resolved asynchronously, nothing waits |
| IPv4 address only | `NULL` | No DNS |

The DNS servers configured by the application (DHCP or static) are used in all cases.

**File Operations**
```c
bool ftp_ls(ftp_client_t* client, const char* path);        // List directory
bool ftp_cd(ftp_client_t* client, const char* path);        // Change directory
bool ftp_pwd(ftp_client_t* client);                         // Print working directory
bool ftp_mkdir(ftp_client_t* client, const char* path);     // Make directory
bool ftp_rmdir(ftp_client_t* client, const char* path);     // Remove directory
bool ftp_delete(ftp_client_t* client, const char* filename);// Delete file
bool ftp_rename(ftp_client_t* client, const char* from, const char* to); // Rename
bool ftp_get(ftp_client_t* client, const char* remote, const char* local); // Download
bool ftp_put(ftp_client_t* client, const char* local, const char* remote); // Upload
bool ftp_abort(ftp_client_t* client);                       // Abort the transfer in progress
```

`ftp_abort()` may be called while a transfer runs (the request slot is busy then). It sends
`ABOR` when the server already runs the transfer, closes the data connection and the local
file (a partial download stays) and reports `FTP_ON_TRANSFER_END "Transfer aborted"`.

`ftp_client_state_string()` and `ftp_last_resp_code()` may be called from any thread;
`ftp_client_status_message()` only in the callback or the thread of `ftp_process()`.

---

### 3. FTP Console (`ftp_console.h`)

Before using console commands, initialize the console. `ftp_console()` takes `main()` arguments (`argv[0]` is the command name, e.g. `"ftp"`), so a shell with `int (*)(int, char**)` handlers registers it directly.

```c
/* Initialize console; func NULL prints with vprintf (stdout) */
void ftp_console_init(ftp_console_print_t func);

/* Run "ftp <command> [args]"; no command prints the help. Returns 0 on success */
int ftp_console(int argc, char** argv);
```

**Available Commands**: `open`, `user`, `pass`, `ls`, `cd`, `pwd`, `mkdir`, `rmdir`, `delete`, `rename`, `get`, `put`, `abort`, `status`, `cstatus`, `bye`.

---

## Usage Example

These examples demonstrate a **Bare Metal (No OS)** approach. If you are using an RTOS (FreeRTOS, Zephyr, etc.), you can simply replace `get_tick_ms` with your system tick function and add `thread_yield()` or `delay()` inside the loop to save power.

### Common Helper

```c
/* Platform specific: Get system time in milliseconds */
uint32_t get_tick_ms(void) {
    // Return milliseconds from a hardware timer or system tick
    // Example: return HAL_GetTick(); 
    return 0; 
}
```

### 1. Server Setup

```c
#include "ftp_server.h"

void main(void) 
{
    /* Initialize and Enable; POSIX path, must exist */
    if (ftp_init("/sd/FTP")) 
    {
        ftp_enable();
    }

    uint32_t last_time = get_tick_ms();

    while (1) 
    {
        uint32_t now = get_tick_ms();

        /* Execute logic every 1ms */
        if (now - last_time >= 1) 
        {
            uint32_t elapsed = now - last_time;

            last_time = now;
            
            /* Run Server */
            ftp_server(elapsed); 
        }
        
        /* Check/Process Hardware Interfaces (Ethernet, SD Card, etc.) */
    }
}
```

### 2. Client Setup

```c
#include "ftp_client.h"

static ftp_client_t my_client;

/* Application Callback */
void my_ftp_callback(ftp_client_t* client, ftp_events_t event, void* data, size_t len) 
{
    if (event == FTP_ON_TRANSFER_DATA) 
    {
        // Handle list data or file data
    }
}

void main(void) 
{
    /* Register Client (single thread: pass ftp_resolver_lwip to use host names) */
    ftp_client_register(&my_client, "MyFTP", NULL, my_ftp_callback, NULL);

    /* Initiate Connection (IPv4 address: no DNS needed) */
    ftp_connect(&my_client, "192.168.4.1", 21, "user", "pass");

    uint32_t last_time = get_tick_ms();

    while (1) 
    {
        uint32_t now = get_tick_ms();

        /* Execute logic every 1ms */
        if (now - last_time >= 1) 
        {
            uint32_t elapsed = now - last_time;

            last_time = now;

            /* Run Client */
            ftp_process(elapsed);
        }
    }
}
```

### 3. Console Helper Integration

`ftp_console()` has the `main()` signature: a shell registers it as the handler of the `ftp` command, or the application calls it with a line split into words (`argv[0]` = `"ftp"`).

```c
#include "ftp_console.h"

/* A shell with int (*)(int, char**) handlers, e.g. ESP-IDF esp_console */
const esp_console_cmd_t cmd = { .command = "ftp", .help = "FTP console", .func = ftp_console };
esp_console_cmd_register(&cmd);

/* Own line input, e.g. UART */
void on_uart_line_received(char* line_buffer) 
{
    int argc = 0;
    char* argv[10];
    // ... split line_buffer into argv: "ftp open host 21" ...

    ftp_console(argc, argv);
}

void main(void) 
{
    // ... Client Init ...
    
    /* Initialize Console: NULL prints to stdout, or pass a print function (e.g. a shell's) */
    ftp_console_init(NULL);

    while (1) 
    {
        // ... Client Loop ...
        
        /* Check UART for new commands */
        if (uart_data_available()) 
        {
            // read line and call on_uart_line_received()
        }
    }
}
```
