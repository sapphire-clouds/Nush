# nush — New Unix Shell

> A custom UNIX shell written from scratch in **C**, implementing core and advanced shell functionality.

Built as a systems programming project to deeply understand how shells work under the hood including process creation, inter-process communication, job control, and signal handling.

---

## Features

| Category | Feature |
|---|---|
| **Execution** | Run any program via `fork()` + `execvp()` |
| **Pipelines** | Multi-stage pipes: `cmd1 \| cmd2 \| cmd3` |
| **I/O Redirection** | `>`, `>>`, `<` operators |
| **Background Jobs** | Run with `&`; automatic zombie reaping |
| **Job Control** | `jobs`, `fg`, `bg`, Ctrl+Z to suspend |
| **Logical Operators** | `&&` and `\|\|` based on exit codes |
| **Variable Expansion** | `$VAR`, `${VAR}`, `$?` (last exit status) |
| **Signal Handling** | Shell ignores SIGINT/SIGTSTP; children don't |
| **Command History** | Tracks last 100 commands (`history`) |
| **Built-in Commands** | `cd`, `exit`, `help`, `history`, `jobs`, `fg`, `bg` |
| **Custom Prompt** | `user@host:cwd$` with ANSI color; red exit code on failure |

---

## Build & Run

**Requirements:** GCC, Make, Linux or macOS (any POSIX system)

```bash
git clone https://github.com/yourusername/nush.git
cd nush
make
./nush
```

Debug build (AddressSanitizer + UBSan):

```bash
make debug
./nush
```

---

## Usage Examples

```bash
# Pipelines
ls -l | grep ".c" | wc -l

# Logical operators
make && echo "Build succeeded" || echo "Build failed"
test -f config.txt && cat config.txt

# Variable expansion
echo $HOME
echo "User is: ${USER}, last exit: $?"

# I/O Redirection
echo "log entry" >> app.log
sort < unsorted.txt > sorted.txt

# Background & Job Control
sleep 60 &       # start in background
jobs             # list all jobs
fg 1             # bring job 1 to foreground
# Ctrl+Z suspends it
bg 1             # resume in background
```

---

## Architecture

```
nush.c  (code is organized in 11 sections)

Section 1  = History          circular buffer, strdup
Section 2  = Job Control      Job struct, jobs/fg/bg built-ins
Section 3  = Var Expansion    expand_vars(), expand_line()
Section 4  = Signal Handling  SIGINT, SIGCHLD, SIGTSTP
Section 5  = Prompt           color, ~-shortening, $? display
Section 6  = Parsing          tokenize, split_logical, split_pipes
Section 7  = I/O Redirection  parse_redirects, apply_redirects
Section 8  = Built-ins        cd, exit, help, history, jobs, fg, bg
Section 9  = Execution        exec_single, exec_pipeline
Section 10 = Logical Ops      exec_logical: && / ||
Section 11 = Main Loop        REPL: read -> expand -> parse -> execute
```

### Key System Calls

| Call | Purpose |
|---|---|
| `fork()` | Spawn child process for each command |
| `execvp()` | Replace child image with target program |
| `waitpid()` | Reap children; detect stop/exit |
| `pipe()` | Create anonymous pipe between commands |
| `dup2()` | Wire pipe ends and files to stdin/stdout |
| `setpgid()` | Put each job in its own process group |
| `tcsetpgrp()` | Transfer terminal control for fg/bg |
| `kill(-pgid, sig)` | Signal an entire process group |
| `sigaction()` | Register signal handlers safely |
| `getenv()` | Variable expansion |

---

## What This Project Demonstrates

- **Process model**: every program is a child; `fork` + `exec` is how Unix creates processes
- **File descriptors**: stdin/stdout/stderr are integers; `dup2` rewires them
- **Pipes as IPC**: `pipe()` creates two connected fds; managing them without leaks is non-trivial
- **Process groups**: job control requires grouping related processes so signals reach all of them
- **Terminal ownership**: `tcsetpgrp()` hands the terminal to a foreground job
- **Signals**: shell ignores SIGINT; children get it; SIGCHLD reaps background jobs
- **Exit codes**: `&&` / `||` are built entirely on `WEXITSTATUS()`

---

## Possible Further Extensions

- [ ] Tab completion (via `readline` or `termios`)
- [ ] Arrow-key history navigation
- [ ] Environment variable assignment (`VAR=value`)
- [ ] Command substitution (`$(...)`)
- [ ] Glob expansion (`*.c`)
- [ ] `.nushrc` config file
- [ ] Aliases

---

## License

MIT — free to use, study, and modify.
