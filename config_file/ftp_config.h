#ifndef _ftp_config_h_
#define _ftp_config_h_

/**
 * @brief Max number of FTP clients
 */
#define FTP_CLIENTS_MAX                         (3) 

/**
 * @brief FTP file path length
 */
#define FTP_FILE_PATH_LEN                       (128)

/**
 * @brief FTP command buffer size
 */
#define FTP_CMD_BUFFER_SIZE                     (256)

/**
 * @brief FTP data buffer size
 */
#define FTP_DATA_BUFFER_SIZE                    (1024)

/**
 * @brief FTP command socket port
 */
#define FTP_CMD_PORT                            (21)

/**
 * @brief FTP passive data port
 */
#define FTP_PASSIVE_DATA_PORT                   (2024)

/**
 * @brief FTP command timeout in milliseconds
 */
#define FTP_CMD_TIMEOUT_MS                      (180 * 1000)

/**
 * @brief FTP data timeout in milliseconds
 * 
 * @note Recommend at least 30 seconds
 */
#define FTP_DATA_TIMEOUT_MS                     (30 * 1000)

/**
 * @brief Max number of files in a directory listing
 */
#define FTP_LIST_NUM                            (64) 

/**
 * @brief Default password for user role
 */
#define FTP_USER_DEFAULT_PASSWORD               "1234"

/**
 * @brief Default password for maintainer role
 */
#define FTP_MAINT_DEFAULT_PASSWORD              "1234"

/**
 * @brief Default password for admin role
 */
#define FTP_ADMIN_DEFAULT_PASSWORD              "1234"

/**
 * @brief FTP server log function
 *
 * @note This macro can be replaced with a custom logging function.
 */
#define FTP_LOG(...)                            

#endif
