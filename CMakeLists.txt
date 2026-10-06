# ESP-IDF component. The configuration comes from Kconfig (sdkconfig.h).
# Other build systems: compile src/*.c (and port/*.c when used), add include/ and
# the directory of the generated ftp_config.h to the include path (see README).

idf_component_register(SRCS "src/ftp_server.c"
                            "src/ftp_client.c"
                            "src/ftp_console.c"
                            "src/ftp_sock.c"
                            "port/ftp_resolver_lwip.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES lwip)
