## fanout

```
▶ ./fanout --help
Usage:
  fanout [OPTIONS] CMD [ARGS...]

Read lines from STDIN, and for each input line start a job running the given
command, with the line (excluding delimiter) written to the job's STDIN. The
command is looked up in PATH. The program waits for all jobs to complete while
forwarding signals (SIGTERM, SIGINT, ...) to the running jobs.

Options:
  -d CHAR    Split input on CHAR (default: newline).
  -n NUM     Run no more than NUM jobs in parallel (default: number of CPUs).
  -t SIG     Terminating signal - stops further job creation irrespective of any
             job exit status; may be given more than once.
             Valid signals are: SIGINT SIGTERM SIGQUIT SIGHUP SIGUSR1 SIGUSR2.
  -v         Increase verbosity; may be given more than once.
  -h,--help  Display this help and exit.

Exit status: 0 if all jobs succeeded; otherwise the first non-zero job status,
or 128 + signal if terminated by a signal.
```

This is similar to `xargs -P`, except that:

- Each line is written to the command's `STDIN` rather than passed as an argument,
  so the size of the line is not limited by `ARG_MAX`;
- Lines can contain any bytes except the delimiter, no special shell quoting is required;
- `fanout` forwards signals it receives to all running jobs while waiting for them
  to complete, to make sure no job is left orphaned.

### Build

Clone the project and run `make` from the project's root directory, then copy
`fanout` binary to any suitable location.

### Details

- Empty strings are skipped.
- `fanout` forwards only the following signals:
  - `SIGINT`
  - `SIGTERM`
  - `SIGQUIT`
  - `SIGHUP`
  - `SIGUSR1`
  - `SIGUSR2`
- `FANOUT_ID` environment variable is set in each job's environment to a sequential
  number starting at 0; this id is also used in log messages.
- `fanout` stops launching new jobs as soon as any job exits with a non-zero status,
  or any terminating signal is received.
- `fanout` is not useful when STDIN is a terminal.

Tested on Linux Mint 22.3.
