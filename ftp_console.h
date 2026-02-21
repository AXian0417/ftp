#ifndef _ftp_console_h_
#define _ftp_console_h_

typedef void (*ftp_console_print_t)(const char* fmt, ...);

void ftp_console_init(ftp_console_print_t func);
void ftp_console(int argc, char** argv);

#endif
