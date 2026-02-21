# FTP Module

This module implements a fully featured FTP Server and a non-blocking FTP Client for MCUs. It supports standard FTP commands, passive mode, and integration with the FATFS file system.

## Features

### FTP Server
- **Standard FTP Protocol**: Supports common commands (`USER`, `PASS`, `LIST`, `PWD`, `CWD`, `RETR`, `STOR`, `DELE`, `MKD`, `RMD`, `RNFR/RNTO`).
- **FATFS Integration**: Maps FTP operations to the MCU's virtual file system (e.g., SD Card).
- **Virtual Root**: Virtualizes the file system root (e.g., `0:/FTP` maps to `/`) for security.
- **Robust Path Handling**: Handles client path anomalies and normalizes paths.
- **Passive Mode (PASV)**: Supports passive data connections.
- **Multiple Clients**: Configurable concurrent client connections.

### FTP Client
- **Non-blocking Architecture**: Event-driven state machine for background operation.
- **Flexible Callbacks**: Decoupled protocol logic from UI/Application.

### FTP Console
- **Console Interface**: Built-in CLI for interactive.

## Configuration

The file `config_file/ftp_config.h` is a **template**. You should copy it to your project's include path or ensure the compiler can find it as `ftp_config.h`.

| Macro | Default | Description |
|-------|---------|-------------|
| `FTP_CLIENTS_MAX` | `3` | Max concurrent server clients |
| `FTP_CMD_PORT` | `21` | FTP Control Port |
| `FTP_PASSIVE_DATA_PORT` | `2024` | Starting port for PASV mode |
| `FTP_CMD_BUFFER_SIZE` | `256` | Command socket buffer size |
| `FTP_DATA_BUFFER_SIZE` | `1024` | Data socket buffer size |
| `FTP_CMD_TIMEOUT_MS` | `180000` | Command timeout (3 mins) |
| `FTP_DATA_TIMEOUT_MS` | `30000` | Data transfer timeout (30s) |
| `FTP_FILE_PATH_LEN` | `128` | Max path string length |

---

## Dependencies

- **FATFS**: The file system must be mounted before starting the FTP Server or using Client file operations.
- **LwIP**: Uses standard sockets API.

---

## API Reference

### 1. FTP Server (`ftp_server.h`)

**Initialization & Control**
```c
/* Initialize server with configuration (IP, Mount Point) */
bool ftp_init(const ftp_config_t* config);

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

**User Management**
```c
/* Set username/password for specific roles */
bool ftp_set_username(ftp_role_t role, const char* new_username);
bool ftp_set_password(ftp_role_t role, const char* new_password);
```

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
/* Register a new client instance */
bool ftp_client_register(ftp_client_t* client, const char* name, void* user, ftp_client_cb_t cb);

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
```

---

### 3. FTP Console (`ftp_console.h`)

Before using console commands, initialize the console with a print function.

```c
/* Initialize console with a custom print callback */
void ftp_console_init(ftp_console_print_t func);

/* Process command line arguments (e.g., from UART input) */
void ftp_console(int argc, char** argv);
```

**Available Commands**: `open`, `user`, `pass`, `ls`, `cd`, `pwd`, `mkdir`, `rmdir`, `delete`, `rename`, `get`, `put`, `status`, `bye`.

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
    ftp_config_t config = 
    {
        .server_ip = "192.168.4.1",
        .mount_point = "0:/FTP"
    };

    /* Initialize and Enable */
    if (ftp_init(&config)) 
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
    /* Register Client */
    ftp_client_register(&my_client, "MyFTP", NULL, my_ftp_callback);

    /* Initiate Connection */
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

The `ftp_console` acts as a bridge. You typically call it when you receive a complete command string from UART or USB.

```c
#include "ftp_console.h"

void my_console_print(const char* fmt, ...) 
{
    // Print to UART/USB
}

void on_uart_line_received(char* line_buffer) 
{
    /* Parse command line arguments */
    int argc = 0;
    char* argv[10];
    // ... parse line_buffer into argv ...

    /* 
        Note: If your parser includes the root command (e.g. "ftp") in argv[0], 
        shift arguments to pass the subcommand (e.g. "open") as the first argument. 
    */
    /* Pass to FTP Console */
    ftp_console(argc - 1, argv + 1);
}

void main(void) 
{
    // ... Client Init ...
    
    /* Initialize Console */
    ftp_console_init(my_console_print);

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
