#ifndef CMD_H
#define CMD_H

#include <sys/types.h>
#include <signal.h>
#include <stdio.h>

struct popen2 {
	int in, out, pid;
};

typedef void (*cmd_cb_t)(char *buf, ssize_t len, struct popen2 *child, char *arg);
typedef void (*cmd_fin_t)(union sigval sv);

struct cmd_args {
	char prompt[BUFSIZ * 4];
	cmd_cb_t callback;
	char *arg;
};

struct tcmd_ret {
	pthread_t thread;
	pthread_mutex_t *mutex;
};

int popen2(struct popen2 *child, const char *cmdline);
char *command(char *prompt, cmd_cb_t callback, char *arg);
char *commandf(const char *format, cmd_cb_t callback, char *arg, ...);
struct tcmd_ret tcommand(char *buf, cmd_cb_t callback, char *arg, cmd_fin_t fin, unsigned millis);
struct tcmd_ret tcommandf(char *format, cmd_cb_t callback, char *arg, cmd_fin_t fin, unsigned millis, ...);

#endif
