#include "qcmd.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct tcmd_args {
	pid_t pid;
	pthread_mutex_t mutex;
	cmd_cb_t cb;
	cmd_fin_t fin;
	char *arg;
	void *ca;
	timer_t timer;
};

int
popen2(struct popen2 *child, const char *cmdline)
{
	pid_t p;
	int pipe_stdin[2], pipe_stdout[2];

	if (pipe(pipe_stdin) || pipe(pipe_stdout))
		return -1;

	p = fork();
	if (p < 0)
		return p;

	if(p == 0) { /* child */
		close(pipe_stdin[1]);
		dup2(pipe_stdin[0], 0);
		close(pipe_stdout[0]);
		dup2(pipe_stdout[1], 1);
		execl("/bin/sh", "sh", "-c", cmdline, NULL);
		perror("execl"); exit(99);
	}

	child->pid = p;
	child->in = pipe_stdin[1];
	child->out = pipe_stdout[0];
	close(pipe_stdin[0]);
	close(pipe_stdout[1]);
	return 0;
}

char *
command(char *prompt, cmd_cb_t callback, char *arg) {
	static char buf[BUFSIZ];
	struct popen2 child;
	ssize_t len = 0;
	int start = 1, cont = 0;

	/* fprintf(stderr, "# %s\n", prompt); */

	popen2(&child, prompt); // should assert it equals 0
	callback("", -1, &child, arg);

	int flags = fcntl(child.out, F_GETFL);
	flags |= O_NONBLOCK;
	fcntl(child.out, F_SETFL, flags);

	fd_set read_fds;
	FD_ZERO(&read_fds);
	FD_SET(child.out, &read_fds);

	int ready_fds;

	do {
		ready_fds = select(child.out + 1, &read_fds, NULL, NULL, NULL);

		if (!ready_fds)
			continue;

		if (ready_fds == -1) {
			perror("Error in select");
			exit(1);
		}

		if (!FD_ISSET(child.out, &read_fds))
			continue;

		len = read(child.out, buf, sizeof(buf));

		if (len > 0) {
			buf[len] = '\0';
			callback(buf, len, &child, arg);
		} else if (len == 0) {
			callback("", 0, &child, arg);
			break;
		} else switch (errno) {
			case EAGAIN:
				continue;
			default:
				perror("Error in read");
				exit(1);
		}
	} while (1);

	close(child.in);
	close(child.out);
	kill(child.pid, 0);
	return buf;
}


char *
commandf(const char *format, cmd_cb_t callback, char *arg, ...) {
	static char buf[BUFSIZ * 4];
	va_list args;
	va_start(args, arg);
	vsprintf(buf, format, args);
	va_end(args);
	return command(buf, callback, arg);
}


void *command_thread(void *args) {
	struct cmd_args *cmd_args = (struct cmd_args *) args;
	char *ret = command(cmd_args->prompt, cmd_args->callback, cmd_args->arg);
	pthread_exit(NULL);
	free(cmd_args);
	return ret;
}

void default_callback2(char *buf, ssize_t len, struct popen2 *child, char *arg) {
	struct tcmd_args *def_arg = (struct tcmd_args *) arg;
	char *s;

	if (len == 0) {
		pthread_mutex_lock(&def_arg->mutex);
		def_arg->pid = 0;
		pthread_mutex_unlock(&def_arg->mutex);
		return;
	} else if (len < 0) {
		pthread_mutex_lock(&def_arg->mutex);
		def_arg->pid = child->pid;
		pthread_mutex_unlock(&def_arg->mutex);
		return;
	}

	def_arg->cb(buf, len, child, def_arg->arg);
}

void check_callback(union sigval sv) {
	struct tcmd_args *def_arg = (struct tcmd_args *) sv.sival_ptr;

	pthread_mutex_lock(&def_arg->mutex);
	if (def_arg->pid) {
		pthread_mutex_unlock(&def_arg->mutex);
		return;
	}
	pthread_mutex_unlock(&def_arg->mutex);

	timer_delete(def_arg->timer);
	def_arg->fin(sv);
}

struct tcmd_ret
_tcommand(struct cmd_args *cmd_args, cmd_cb_t callback, char *arg, cmd_fin_t fin, unsigned millis) {
	struct tcmd_ret ret = { .mutex = NULL };
	struct tcmd_args *cb_arg = malloc(sizeof(struct tcmd_args));
	va_list args;

	if (pthread_mutex_init(&cb_arg->mutex, NULL) != 0)
		return ret;

	struct sigevent sev;

	timer_t timer_id;
	struct itimerspec its;

	// Set up signal handler.
	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_value.sival_ptr = cb_arg;
	sev.sigev_notify_function = check_callback;
	sev.sigev_notify_attributes = NULL;

	if (timer_create(CLOCK_REALTIME, &sev, &timer_id) == -1) {
		perror("timer_create");
		exit(1);
	}

	ret.mutex = &cb_arg->mutex;
	cb_arg->pid = 0;
	cb_arg->cb = callback;
	cb_arg->arg = arg;
	cb_arg->fin = fin;
	cb_arg->ca = cmd_args;
	cb_arg->timer = timer_id;

	// had vsprintf here

	cmd_args->callback = default_callback2;
	cmd_args->arg = (char *) cb_arg;

	its.it_value.tv_sec = millis / 1000;
	its.it_value.tv_nsec = (millis % 1000) * 1000;
	its.it_interval.tv_sec = 3;
	its.it_interval.tv_nsec = 0;

	if (timer_settime(timer_id, 0, &its, NULL) == -1) {
		perror("timer_settime");
		exit(1);
	}

	// Create a thread and pass the cmd_args as an argument
	if (pthread_create(&ret.thread, NULL, command_thread, cmd_args) != 0) {
		perror("Thread creation failed");
		exit(1);
	}

	return ret;

}
	
struct tcmd_ret tcommand(char *buf, cmd_cb_t callback, char *arg, cmd_fin_t fin, unsigned millis) {
	struct cmd_args *cmd_args = malloc(sizeof(struct cmd_args));
	strcpy(cmd_args->prompt, buf);
	return _tcommand(cmd_args, callback, arg, fin, millis);
}

struct tcmd_ret tcommandf(char *format, cmd_cb_t callback, char *arg, cmd_fin_t fin, unsigned millis, ...) {
	struct cmd_args *cmd_args = malloc(sizeof(struct cmd_args));
	va_list args;
	va_start(args, millis);
	vsprintf(cmd_args->prompt, format, args);
	va_end(args);
	return _tcommand(cmd_args, callback, arg, fin, millis);
}

void easy_fin(union sigval sv) {}

#define DEFAULT_INTERVAL 333

struct tcmd_ret etcommand(char *buf, cmd_cb_t callback, char *arg) {
	struct cmd_args *cmd_args = malloc(sizeof(struct cmd_args));
	strcpy(cmd_args->prompt, buf);
	return _tcommand(cmd_args, callback, arg, easy_fin, DEFAULT_INTERVAL);
}

struct tcmd_ret etcommandf(char *format, cmd_cb_t callback, char *arg, ...) {
	struct cmd_args *cmd_args = malloc(sizeof(struct cmd_args));
	va_list args;
	va_start(args, arg);
	vsprintf(cmd_args->prompt, format, args);
	va_end(args);
	return _tcommand(cmd_args, callback, arg, easy_fin, DEFAULT_INTERVAL);
}
