#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE

#include "qcmd.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
/* #include <unistd.h> */
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <pty.h>
#include <termios.h>

int
popen2(struct popen *child, const char *cmdline)
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

struct {
	struct termios tty;
	int slave_fd;
} clients[FD_SETSIZE];

void cleanup_handler(int sig) {
	for (int i = 0; i < FD_SETSIZE; ++i) if (clients[i].slave_fd != -1) {
		tcsetattr(i, TCSANOW, &clients[i].tty);
		close(i);
		clients[i].slave_fd = -1;
	}
}

int
command_pty(int master_fd, struct winsize *ws, struct popen *child, char * const args[])
{
	pid_t p;

	p = fork();
	if(p == 0) { /* child */
		setsid();

		int slave_fd = open(ptsname(master_fd), O_RDWR);
		if (slave_fd == -1) {
			perror("open slave pty");
			exit(EXIT_FAILURE);
		}

		ioctl(slave_fd, TIOCSWINSZ, ws);

		if (ioctl(slave_fd, TIOCSCTTY, NULL) == -1)
			perror("ioctl TIOCSCTTY");

		int flags;
		flags = fcntl(slave_fd, F_GETFL, 0);
		fcntl(slave_fd, F_SETFL, flags | O_NONBLOCK);

		struct termios tty;
		tcgetattr(slave_fd, &tty); // Get current terminal attributes
		tcgetattr(slave_fd, &clients[slave_fd].tty); // Get current terminal attributes
		clients[slave_fd].tty = tty;
		clients[slave_fd].slave_fd = slave_fd;

		cfmakeraw(&tty); // Modify them for raw mode

		/* tty.c_oflag |= (OPOST | ONLCR); */
		/* tty.c_oflag &= ~(OPOST | ONLCR); */
		/* tty.c_oflag |= (OPOST | ONLCR); */

		tcsetattr(slave_fd, TCSANOW, &tty); // Set the attributes to make the changes take effect immediately

		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = cleanup_handler;
		sigaction(SIGTERM, &sa, NULL);

		dup2(slave_fd, STDIN_FILENO);
		dup2(slave_fd, STDOUT_FILENO);
		dup2(slave_fd, STDERR_FILENO);
		close(master_fd);

		execvp(args[0], args);
		perror("execvp");
		exit(99);
	}

	child->pid = p;
	child->in = master_fd;
	child->out = master_fd;

	return p;
}

ssize_t
command(char *prompt, cmd_cb_t callback, void *arg) {
	static char buf[BUFSIZ];
	struct popen child;
	ssize_t len = 0, total = 0;
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
			return -1;
		}

		if (!FD_ISSET(child.out, &read_fds))
			continue;

		len = read(child.out, buf, sizeof(buf));

		if (len > 0) {
			buf[len] = '\0';
			callback(buf, len, &child, arg);
			total += len;
		} else if (len == 0) {
			callback("", 0, &child, arg);
			break;
		} else switch (errno) {
			case EAGAIN:
				continue;
			default:
				perror("Error in read");
				return -1;
		}
	} while (1);

	close(child.in);
	close(child.out);
	kill(child.pid, 0);
	return total;
}


ssize_t
commandf(const char *format, cmd_cb_t callback, void *arg, ...) {
	static char buf[BUFSIZ * 4];
	va_list args;
	va_start(args, arg);
	vsprintf(buf, format, args);
	va_end(args);
	return command(buf, callback, arg);
}

void *command_thread(void *args) {
	struct cmd_args *cmd_args = (struct cmd_args *) args;
	if (command(cmd_args->prompt, cmd_args->callback, cmd_args->arg) < 0)
		return (void *) 0x1;
	pthread_exit(NULL);
	free(cmd_args->arg);
	free(cmd_args);
	return NULL;
}

void tcommand_callback(char *buf, ssize_t len, struct popen *child, void *arg) {
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
_tcommand(struct cmd_args *cmd_args, cmd_cb_t callback, void *arg, cmd_fin_t fin, unsigned millis) {
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

	cmd_args->callback = tcommand_callback;
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
	
struct tcmd_ret tcommand(char *buf, cmd_cb_t callback, void *arg, cmd_fin_t fin, unsigned millis) {
	struct cmd_args *cmd_args = malloc(sizeof(struct cmd_args));
	strcpy(cmd_args->prompt, buf);
	return _tcommand(cmd_args, callback, arg, fin, millis);
}

struct tcmd_ret tvcommandf(char *format, cmd_cb_t callback, void *arg, cmd_fin_t fin, unsigned millis, va_list va) {
	struct cmd_args *cmd_args = malloc(sizeof(struct cmd_args));
	vsprintf(cmd_args->prompt, format, va);
	return _tcommand(cmd_args, callback, arg, fin, millis);
}

struct tcmd_ret tcommandf(char *format, cmd_cb_t callback, void *arg, cmd_fin_t fin, unsigned millis, ...) {
	struct tcmd_ret ret;
	va_list args;
	va_start(args, millis);
	ret = tvcommandf(format, callback, arg, fin, millis, args);
	va_end(args);
	return ret;
}

void easy_fin(union sigval sv) {
	struct tcmd_args *def_arg = (struct tcmd_args *) sv.sival_ptr;
	def_arg->cb("", -1, NULL, def_arg->arg);
}

#define DEFAULT_INTERVAL 333

struct tcmd_ret etcommand(char *buf, cmd_cb_t callback, void *arg) {
	struct cmd_args *cmd_args = malloc(sizeof(struct cmd_args));
	strcpy(cmd_args->prompt, buf);
	return _tcommand(cmd_args, callback, arg, easy_fin, DEFAULT_INTERVAL);
}

struct tcmd_ret etcommandf(char *format, cmd_cb_t callback, void *arg, ...) {
	struct tcmd_ret ret;
	va_list args;
	va_start(args, arg);
	ret = tvcommandf(format, callback, arg, easy_fin, DEFAULT_INTERVAL, args);
	va_end(args);
	return ret;
}
