#ifndef _ftp_typedef_h_
#define _ftp_typedef_h_

#include "stdint.h"
#include "stdbool.h"
#include "ftp_config.h"

#define FTP_MAX_USER_LEN                        (32)
#define FTP_MAX_PASS_LEN                        (64)

/* FTP Response Codes (RFC 959) */
/* 1xx: Positive Preliminary */
#define FTP_150_FILE_STATUS_OK                  (150)
#define FTP_125_DATA_CONNECTION_OPEN            (125)

/* 2xx: Positive Completion */
#define FTP_200_COMMAND_OK                      (200)
#define FTP_211_SYSTEM_STATUS                   (211)
#define FTP_213_FILE_STATUS                     (213)
#define FTP_215_SYSTEM_TYPE                     (215)
#define FTP_220_SERVICE_READY                   (220)
#define FTP_221_GOODBYE                         (221)
#define FTP_226_TRANSFER_COMPLETE               (226)
#define FTP_227_PASSIVE_MODE                    (227)
#define FTP_230_USER_LOGGED_IN                  (230)
#define FTP_250_ACTION_COMPLETE                 (250)
#define FTP_257_PATHNAME_CREATED                (257)

/* 3xx: Positive Intermediate */
#define FTP_331_USER_NAME_OK                    (331)
#define FTP_332_NEED_ACCOUNT                    (332)
#define FTP_350_PENDING_FURTHER_INFO            (350)

/* 4xx: Transient Negative Completion */
#define FTP_421_SERVICE_NOT_AVAILABLE           (421)
#define FTP_425_CANT_OPEN_DATA_CONNECTION       (425)
#define FTP_426_CONNECTION_CLOSED               (426)
#define FTP_451_LOCAL_ERROR                     (451)

/* 5xx: Permanent Negative Completion */
#define FTP_502_COMMAND_NOT_IMPLEMENTED         (502)
#define FTP_504_COMMAND_NOT_SUPPORTED           (504)
#define FTP_530_NOT_LOGGED_IN                   (530)
#define FTP_550_ACTION_NOT_TAKEN                (550)
#define FTP_552_EXCEEDED_STORAGE                (552)
#define FTP_553_FILE_NAME_NOT_ALLOWED           (553)

typedef enum ftp_cmd
{
    FTP_CMD_NOT_SUPPORTED = -1,
    FTP_CMD_FEAT = 0,       /** Features */
    FTP_CMD_SYST,           /** System type */
    FTP_CMD_CDUP,           /** Change to root directory */
    FTP_CMD_CWD,            /** Change working directory */
    FTP_CMD_PWD,            /** Print working directory */
    FTP_CMD_XPWD,           /** Print the current working directory */
    FTP_CMD_SIZE,           /** Get the size of the file */
    FTP_CMD_MDTM,           /** Get the last modification time of the file */
    FTP_CMD_MFMT,           /** Modify the last modification time of the file */
    FTP_CMD_TYPE,           /** Set the transmission mode (ASCII/binary) */
    FTP_CMD_USER,           /** Login user */
    FTP_CMD_PASS,           /** Login pass */
    FTP_CMD_PASV,           /** Passive mode */
    FTP_CMD_LIST,           /** List files in the current directory */
    FTP_CMD_RETR,           /** Transfer a copy of the file */
    FTP_CMD_STOR,           /** Receive data and store it as a file on the FTP */
    FTP_CMD_DELE,           /** Delete the file */
    FTP_CMD_RMD,            /** Remove the directory */
    FTP_CMD_MKD,            /** Create the directory */
    FTP_CMD_RNFR,           /** Rename from ... */
    FTP_CMD_RNTO,           /** Rename to ... */
    FTP_CMD_NOOP,           /** No operation, used to maintain the connection */
    FTP_CMD_QUIT,           /** Disconnect */
    FTP_CMD_APPE,           /** Add data to the end of the file */
    FTP_CMD_NLST,           /** Returns a list of file names in the specified directory */
    FTP_CMD_AUTH,           /** Authentication */
    FTP_CMD_STAT,           /** Server Status */
    FTP_CMDS_MAX,           /** Number of commands */
} ftp_cmd_t;

typedef struct
{
    char* str;
} ftp_cmd_str_t;

#endif
