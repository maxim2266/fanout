/*
BSD 3-Clause License

Copyright (c) 2026 Maxim Konakov
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stddef.h>
#include <signal.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

// macro definitions ------------------------------------------------------------------------------
#ifndef VER
	#error "program version constant is not defined"
#endif

#ifndef MAX_JOBS
	#define MAX_JOBS 256
#endif

// macro argument to string literal
#define STR(s)	#s
#define XSTR(s)	STR(s)

// logging ----------------------------------------------------------------------------------------
static
unsigned log_level = 0u;	// 0 - errors only, 1 - errors and warnings, > 1 - all

#define log_info(fmt, ...) ({	\
	if(log_level > 1u)	\
		warnx("[info] " fmt __VA_OPT__(,) __VA_ARGS__);	\
})

#define log_info_errno(fmt, ...) ({	\
	if(log_level > 1u)	\
		warn("[info] " fmt __VA_OPT__(,) __VA_ARGS__);	\
})

#define log_warn(fmt, ...) ({	\
	if(log_level > 0u)	\
		warnx("[warn] " fmt __VA_OPT__(,) __VA_ARGS__);	\
})

#define log_warn_errno(fmt, ...) ({	\
	if(log_level > 0u)	\
		warn("[warn] " fmt __VA_OPT__(,) __VA_ARGS__);	\
})

#define log_err(fmt, ...)		 warnx("[error] " fmt __VA_OPT__(,) __VA_ARGS__)
#define log_err_errno(fmt, ...)	 warn("[error] " fmt __VA_OPT__(,) __VA_ARGS__)

// termination
#define die(fmt, ...)		errx(EXIT_FAILURE, "[error] " fmt __VA_OPT__(,) __VA_ARGS__)
#define die_errno(fmt, ...)	err(EXIT_FAILURE, "[error] " fmt __VA_OPT__(,) __VA_ARGS__)

// state variables --------------------------------------------------------------------------------
static
int delim = '\n', term_signal = SIGTERM;

static
volatile int exit_code = 0;

static
volatile unsigned num_jobs = 0;

static
unsigned max_jobs = 0;

static
pid_t child_procs[MAX_JOBS];

static
uint64_t job_id = 0;

// signals ----------------------------------------------------------------------------------------
static
sigset_t orig_sig_mask, blocked_sig_mask;

static inline
void sig_block(void) {
	sigprocmask(SIG_SETMASK, &blocked_sig_mask, NULL);
}

static inline
void sig_unblock(void) {
	sigprocmask(SIG_SETMASK, &orig_sig_mask, NULL);
}

static
void init_signal_masks(void) {
	// save existing mask
	if (sigprocmask(0, NULL, &orig_sig_mask))
		die_errno("cannot read signal mask");

	sigdelset(&orig_sig_mask, term_signal);
	sigdelset(&orig_sig_mask, SIGCHLD);

	// blocked signal mask
	blocked_sig_mask = orig_sig_mask;

	sigaddset(&blocked_sig_mask, SIGINT);
	sigaddset(&blocked_sig_mask, SIGTERM);
	sigaddset(&blocked_sig_mask, SIGQUIT);
	sigaddset(&blocked_sig_mask, SIGHUP);
	sigaddset(&blocked_sig_mask, SIGUSR1);
	sigaddset(&blocked_sig_mask, SIGUSR2);
	sigaddset(&blocked_sig_mask, SIGCHLD);
}

static
void reset_signal_handlers(void) {
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGUSR1, SIG_DFL);
	signal(SIGUSR2, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
}

static
void forward_signal(const int sig) {
	const int e = errno;

	for (unsigned i = 0; i < num_jobs; ++i)
		kill(-child_procs[i], sig);

	if (sig == term_signal && exit_code == 0)
		exit_code = 128 + term_signal;

	errno = e;
}

#define WRITE_ERR_MSG(msg)	\
	(void)(write(STDERR_FILENO, msg "\n", sizeof(msg)) + 1)

static
void on_sig_child(const int _ __attribute__((unused))) {
	const int e = errno;

	for (;;) {
		int status;
		const pid_t pid = waitpid(-1, &status, WNOHANG);

		if (pid == 0)
			break;

		if (pid < 0) {
			switch (errno) {
				case EINTR:		// just in case
					continue;
				case EAGAIN:	// should never happen
					break;
				case ECHILD:	// no more children left
					num_jobs = 0;
					break;
				default:		// wait failed
					WRITE_ERR_MSG("*** wait failed ***");
					break;
			}

			break;	// loop
		}

		// exit status
		if (WIFEXITED(status))
			status = WEXITSTATUS(status);
		else if (WIFSIGNALED(status))
			status = 128 +  WTERMSIG(status);
		else
			continue;

		// find pid
		unsigned i;

		for (i = 0; i < num_jobs && pid != child_procs[i]; ++i);

		if (i == num_jobs)	// wild pid - ignore
			continue;

		// remove pid
		if (i < --num_jobs)
			child_procs[i] = child_procs[num_jobs];

		// set exit code
		if (status != 0 && exit_code == 0)
			exit_code = status;
	}

	errno = e;
}

#undef WRITE_ERR_MSG

static
void install_signal_handlers(void) {
	signal(SIGPIPE, SIG_IGN);

	struct sigaction act = {
		.sa_handler = forward_signal,
		.sa_mask = blocked_sig_mask,
		// .sa_flags = SA_RESTART
	};

	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGQUIT, &act, NULL);
	sigaction(SIGHUP, &act, NULL);
	sigaction(SIGUSR1, &act, NULL);
	sigaction(SIGUSR2, &act, NULL);

	act.sa_handler = on_sig_child;
	act.sa_flags |= SA_NOCLDSTOP;

	sigaction(SIGCHLD, &act, NULL);
}

// input reader -----------------------------------------------------------------------------------
#define CHUNK_SIZE		4096
#define MAX_LINE_SIZE	(1024 * 1024 * 1024)

static
struct reader {
	size_t nb, pos;
	int status;
	uint8_t arena[CHUNK_SIZE];
} reader = { 0 };

static
void read_chunk(void) {
	if (reader.status)
		return;

	memset(&reader, 0, offsetof(struct reader, arena));

	ssize_t n;

	while ((n = read(STDIN_FILENO, reader.arena, CHUNK_SIZE)) < 0 && errno == EINTR);

	if (n > 0)
		reader.nb = (size_t)n;
	else
		reader.status = (n < 0) ? errno : -1;
}

struct buffer {
	size_t len, cap;
	uint8_t* ptr;
};

static
int append_bytes(struct buffer* const buff, const uint8_t* const p, const size_t n) {
	size_t len;

	if (__builtin_add_overflow(buff->len, n, &len))
		return E2BIG;

	if (len > MAX_LINE_SIZE)
		return ENOBUFS;

	if (len > buff->cap) {
		size_t cap = buff->cap ? (2 * buff->cap) : CHUNK_SIZE;

		while (len > cap)
			cap *= 2;

		buff->cap = cap;
		buff->ptr = realloc(buff->ptr, cap);
	}

	memcpy(buff->ptr + buff->len, p, n);
	buff->len = len;
	return 0;
}

static
int next_line(struct buffer* const buff) {
	buff->len = 0;

	if (reader.status)
		return reader.status;

	for (;;) {
		// refill
		if (reader.pos >= reader.nb) {
			read_chunk();

			switch (reader.status) {
				case -1:	// EOF
					return (buff->len > 0) ? 0 : -1;
				case 0:		// ok
					break;
				default:
					return reader.status;
			}

			// delimiter at the start of the buffer
			if (buff->len > 0 && reader.arena[0] == (uint8_t)delim)
				return 0;
		}

		// scroll to the start of the next string
		const uint8_t* s = reader.arena + reader.pos;
		const uint8_t* end = reader.arena + reader.nb;

		while (s < end && *s == (uint8_t)delim)
			++s;

		reader.pos = (size_t)(s - reader.arena);

		if (s < end) {
			// find next separator
			end = memchr(s, delim, end - s) ?: end;

			// append chunk
			if ((reader.status = append_bytes(buff, s, end - s)) != 0)
				return reader.status;

			// see if we are done
			if ((reader.pos += (size_t)(end - s)) < reader.nb)
				return 0;
		}
	}
}

// job control ------------------------------------------------------------------------------------
static
int set_env_job_id(void) {
	// number to string conversion
	char buff[24];	// 21 used
	char* p = buff + sizeof(buff) - 1;

	*p = 0;

	uint64_t val = job_id;

	do {
		*--p = val % 10 + '0';
		val /= 10;
	} while (val > 0);

	// set environment variable
	return setenv("FANOUT_ID", p, 1);
}

#define SV_WARN(msg, ...)	\
	log_warn("supervisor (%" PRIu64 "): " msg, job_id __VA_OPT__(,) __VA_ARGS__)

#define SV_ERR_ERRNO(msg, ...)	\
	log_err_errno("supervisor (%" PRIu64 "): " msg, job_id __VA_OPT__(,) __VA_ARGS__)

#define SV_DIE_ERRNO(msg, ...)	\
	die_errno("supervisor (%" PRIu64 "): " msg, job_id __VA_OPT__(,) __VA_ARGS__);

// job runner
static __attribute__((noreturn))
void run_job(char** cmd, const struct buffer* const str) {
	// signals
	reset_signal_handlers();

	// process group
	if (setpgid(0, 0))
		SV_DIE_ERRNO("setting process group");

	// pipe
	int pfd[2];

	if (pipe(pfd) < 0)
		SV_DIE_ERRNO("creating pipe");

	// start job
	const pid_t pid = fork();

	if (pid < 0)
		SV_DIE_ERRNO("forking process");

	// child
	if (pid == 0) {
		close(pfd[1]);

		// input redirect
		if (dup2(pfd[0], STDIN_FILENO) < 0)
			SV_DIE_ERRNO("redirecting STDIN");

		close(pfd[0]);

		// job id
		if (set_env_job_id() < 0)
			SV_DIE_ERRNO("environment setup");

		// final bits
		signal(SIGPIPE, SIG_DFL);
		sig_unblock();

		// exec
		execvp(cmd[0], cmd);
		SV_DIE_ERRNO("starting job");
	}

	// parent
	close(pfd[0]);
	fclose(stdin);
	fclose(stdout);

	// pump the data
	const uint8_t* ptr = str->ptr;
	const uint8_t* const end = ptr + str->len;
	ssize_t n = 0;

	while (ptr < end) {
		while ((n = write(pfd[1], ptr, end - ptr)) < 0 && errno == EINTR);

		if (n < 0) {
			SV_ERR_ERRNO("writing job input");
			kill(pid, term_signal);
			break;
		}

		ptr += n;
	}

	if (close(pfd[1]) < 0 && n >= 0) {
		SV_ERR_ERRNO("writing job input");
		kill(pid, term_signal);
		n = (ssize_t)-1;
	}

	// wait
	int status;

	for (;;) {
		pid_t ret;

		while ((ret = wait(&status)) < 0 && errno == EINTR);

		if (ret < 0)
			SV_DIE_ERRNO("wait failed");

		if (ret != pid)
			continue;

		// exit code
		if (WIFEXITED(status)) {
			status = WEXITSTATUS(status);
			break;
		}

		if (WIFSIGNALED(status)) {
			status = 128 + WTERMSIG(status);
			break;
		}
	}

	if (status == 0 && n < 0)
		status = EXIT_FAILURE;

	if (status != 0)
		SV_WARN("job exit code %d", status);

	exit(status);
}

#undef SV_WARN
#undef SV_ERR_ERRNO
#undef SV_DIE_ERRNO

// start supervisor
static
int start_sv(char** cmd, const struct buffer* const str) {
	fflush(NULL);

	const pid_t pid = fork();

	if (pid < 0) {
		log_err_errno("starting supervisor process (%" PRIu64 ")", job_id);
		return -1;
	}

	// child
	if (pid == 0)
		run_job(cmd, str);

	// parent
	setpgid(pid, pid);
	child_procs[num_jobs++] = pid;
	++job_id;

	return 0;
}

// main loop
static __attribute__((noreturn))
void run(char** cmd) {
	log_info("running up to %u jobs in parallel", max_jobs);

	struct buffer line = { 0 };
	int e;

	// read first line
	if ((e = next_line(&line)) != 0) {
		if (e > 0)
			die("reading input: %s", strerror(e));

		exit(0);	// no input
	}

	// start first supervisor
	sig_block();
	install_signal_handlers();
	e = start_sv(cmd, &line);
	sig_unblock();

	// main loop
	while (e == 0) {
		// next input line
		if ((e = next_line(&line)) != 0) {
			if (e > 0)
				log_err("reading input: %s", strerror(e));

			break;	// no more input
		}

		// wait for concurrency limit or an error
		sig_block();

		while (exit_code == 0 && num_jobs == max_jobs)
			sigsuspend(&orig_sig_mask);

		// start supervisor
		e = exit_code ? -1 : start_sv(cmd, &line);
		sig_unblock();
	}

	// cleanup before final wait
	free((void*)line.ptr);
	fclose(stdout);
	sig_block();

	// wait for the remaining jobs to terminate
	while (num_jobs > 0)
		sigsuspend(&orig_sig_mask);

	reset_signal_handlers();
	sig_unblock();

	log_info("completed %" PRIu64 " jobs; exit code %d", job_id, exit_code);
	exit(exit_code);
}

// setup ------------------------------------------------------------------------------------------
static
void setup(void) {
	init_signal_masks();

	// make STDERR line-buffered
	setvbuf(stderr, NULL, _IOLBF, 0);

	// disable buffering for other streams
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);

	// max. jobs to run in parallel
	if (max_jobs == 0) {
		const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

		max_jobs = (ncpu > 0)? (unsigned)ncpu : 2;

		if (max_jobs > MAX_JOBS)	// just in case
			max_jobs = MAX_JOBS;
	}
}

// application ------------------------------------------------------------------------------------
// usage
static
const char usage_string[] =
"Usage:\n"
"  fanout [OPTIONS] CMD [ARGS...]\n"
"\n"
"Read lines from STDIN, and for each input line start a job running the given\n"
"command, with the line (excluding delimiter) written to the job's STDIN. The\n"
"command is looked up in PATH. The program waits for all jobs to complete while\n"
"forwarding signals (SIGTERM, SIGINT, ...) to the running jobs.\n"
"\n"
"Options:\n"
"  -d CHAR    Split input on CHAR (default: newline).\n"
"  -n NUM     Run no more than NUM jobs in parallel (default: number of CPUs).\n"
"  -t SIG     Terminating signal (default: SIGTERM). Stops further job creation.\n"
"  -v         Increase verbosity; may be given more than once.\n"
"  -h,--help  Display this help and exit.\n"
"\n"
"Exit status: 0 if all jobs succeeded; otherwise the first non-zero job status,\n"
"or 128 + signal if terminated by a signal.\n"
"\n"
"Version: " XSTR(VER) "\n";

static __attribute__((noreturn))
void usage_exit(void) {
	fwrite(usage_string, sizeof(usage_string) - 1, 1, stderr);
	exit(EXIT_FAILURE);
}

// parse unsigned integer
static
int parse_uint(unsigned* const p, const char* s) {
	unsigned val;

	switch (*s) {
		case '0':
			return (s[1] == 0) ? (*p = 0) : 1;

		case '1' ... '9':
			val = *s - '0';
			break;

		default:
			return 1;
	}

	for (++s; ; ++s) {
		switch (*s) {
			case 0:
				*p = val;
				return 0;

			case '0' ... '9':
				if (__builtin_umul_overflow(val, 10, &val))
					return 1;

				if (__builtin_uadd_overflow(val, *s - '0', &val))
					return 1;

				break;

			default:
				return 1;
		}
	}
}

// argument parsing and validation
static
char** parse_args(int argc, char** argv) {
	if (argc > 1 && strcmp(argv[1], "--help") == 0)
		usage_exit();

	int c;

	while ((c = getopt(argc, argv, "+:hvt:n:d:")) != -1) {
		switch (c) {
			case 'h':
				// help
				usage_exit();

			case 'v':
				// log level
				++log_level;
				break;

			case 't': {
				// terminating signal
				const char* s = (optarg[0] == 'S' && optarg[1] == 'I' && optarg[2] == 'G')
							  ? optarg + 3
							  : optarg;

				term_signal = (strcmp(s, "INT") == 0)	? SIGINT
							: (strcmp(s, "TERM") == 0)	? SIGTERM
							: (strcmp(s, "QUIT") == 0)	? SIGQUIT
							: (strcmp(s, "HUP") == 0)	? SIGHUP
							: (strcmp(s, "USR1") == 0)	? SIGUSR1
							: (strcmp(s, "USR2") == 0)	? SIGUSR2
							: 0;

				if (term_signal == 0)
					die("unrecognised signal name: `%s`", optarg);

				break;
			}

			case 'n':
				// max. number of jobs
				if (parse_uint(&max_jobs, optarg))
					die("invalid value for option `-n`: `%s`", optarg);

				if (max_jobs > MAX_JOBS) {
					log_warn(
						"cannot run %u jobs in parallel, " XSTR(MAX_JOBS) " is the maximum",
						max_jobs
					);

					max_jobs = MAX_JOBS;
				}

				break;

			case 'd':
				// delimiter
				switch (strlen(optarg)) {
					case 1:
						delim = optarg[0];
						continue;

					case 2:
						if (optarg[0] == '\\') {
							switch (optarg[1]) {
								case 'n':
									delim = '\n';
									continue;

								case 't':
									delim = '\t';
									continue;

								case '0':
									delim = 0;
									continue;
							}
						}
				}

				die("invalid delimiter: `%s`", optarg);

			case ':':
				die("missing argument for option `-%c`", optopt);

			case '?':
				die("unrecognised option `-%c`", optopt);
		}
	}

	// check for command
	if (optind >= argc)
		die("missing command");

	return argv + optind;
}

int main(int argc, char** argv) {
	char** const cmd = parse_args(argc, argv);

	if (isatty(STDIN_FILENO))
		die("this program is not very useful when its input comes from terminal");

	setup();
	run(cmd);
}
