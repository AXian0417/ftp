#ifndef _ftp_console_h_
#define _ftp_console_h_

typedef void (*ftp_console_print_t)(const char* fmt, ...);

/* func NULL: vprintf (stdout) */
void ftp_console_init(ftp_console_print_t func);
/* main() arguments: argv[0] is the command name; returns 0 on success */
int ftp_console(int argc, char** argv);

#endif
