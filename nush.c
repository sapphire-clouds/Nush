/*
 * nush - New Unix Shell
 * A custom UNIX shell written in C
 *
 * Author: [Your Name]
 * Features: command execution, pipelines, I/O redirection,
 *           background jobs, job control (fg/bg/jobs),
 *           logical operators (&&/||), env var expansion,
 *           history, and built-in commands
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <ctype.h>

/* ─── Constants ─────────────────────────────────────────── */
#define NUSH_INPUT_MAX  2048
#define MAX_ARGS        64
#define MAX_HISTORY     100
#define MAX_PIPES       16
#define MAX_JOBS        64

#define C_GREEN   "\033[1;32m"
#define C_BLUE    "\033[1;34m"
#define C_YELLOW  "\033[1;33m"
#define C_RED     "\033[1;31m"
#define C_RESET   "\033[0m"

/* ═══════════════════════════════════════════════════════════
 * SECTION 1 — HISTORY
 * ═══════════════════════════════════════════════════════════ */

static char *g_history[MAX_HISTORY];
static int   g_history_count = 0;

static void history_add(const char *cmd) {
    if (g_history_count < MAX_HISTORY) {
        g_history[g_history_count++] = strdup(cmd);
    } else {
        free(g_history[0]);
        memmove(g_history, g_history + 1, (MAX_HISTORY - 1) * sizeof(char *));
        g_history[MAX_HISTORY - 1] = strdup(cmd);
    }
}

static void history_print(void) {
    for (int i = 0; i < g_history_count; i++)
        printf("  %3d  %s\n", i + 1, g_history[i]);
}

static void history_free(void) {
    for (int i = 0; i < g_history_count; i++) free(g_history[i]);
}

/* ═══════════════════════════════════════════════════════════
 * SECTION 2 — JOB CONTROL
 * ═══════════════════════════════════════════════════════════ */

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } JobStatus;

typedef struct {
    int       id;
    pid_t     pgid;
    JobStatus status;
    char      cmd[NUSH_INPUT_MAX];
} Job;

static Job g_jobs[MAX_JOBS];
static int g_job_count = 0;

static int job_add(pid_t pgid, const char *cmd) {
    if (g_job_count >= MAX_JOBS) return -1;
    int id = g_job_count + 1;
    g_jobs[g_job_count].id     = id;
    g_jobs[g_job_count].pgid   = pgid;
    g_jobs[g_job_count].status = JOB_RUNNING;
    strncpy(g_jobs[g_job_count].cmd, cmd, NUSH_INPUT_MAX - 1);
    g_job_count++;
    return id;
}

static Job *job_find_by_id(int id) {
    for (int i = 0; i < g_job_count; i++)
        if (g_jobs[i].id == id) return &g_jobs[i];
    return NULL;
}

static void jobs_reap(void) {
    int w = 0;
    for (int i = 0; i < g_job_count; i++) {
        if (g_jobs[i].status != JOB_DONE) g_jobs[w++] = g_jobs[i];
    }
    g_job_count = w;
    for (int i = 0; i < g_job_count; i++) g_jobs[i].id = i + 1;
}

static void builtin_jobs(void) {
    for (int i = 0; i < g_job_count; i++) {
        int wstatus;
        pid_t r = waitpid(-g_jobs[i].pgid, &wstatus, WNOHANG|WUNTRACED|WCONTINUED);
        if (r > 0) {
            if (WIFEXITED(wstatus) || WIFSIGNALED(wstatus)) g_jobs[i].status = JOB_DONE;
            else if (WIFSTOPPED(wstatus))   g_jobs[i].status = JOB_STOPPED;
            else if (WIFCONTINUED(wstatus)) g_jobs[i].status = JOB_RUNNING;
        }
    }
    for (int i = 0; i < g_job_count; i++) {
        const char *st = g_jobs[i].status == JOB_RUNNING ? "Running" :
                         g_jobs[i].status == JOB_STOPPED ? "Stopped" : "Done";
        printf("  [%d] %d  %-10s  %s\n", g_jobs[i].id, g_jobs[i].pgid, st, g_jobs[i].cmd);
    }
    jobs_reap();
}

static int builtin_fg(char **argv) {
    int id = (argv[1]) ? atoi(argv[1]) : (g_job_count > 0 ? g_jobs[g_job_count-1].id : -1);
    Job *j = job_find_by_id(id);
    if (!j) { fprintf(stderr, C_RED "nush: fg: no such job\n" C_RESET); return 1; }
    j->status = JOB_RUNNING;
    printf("%s\n", j->cmd);
    tcsetpgrp(STDIN_FILENO, j->pgid);
    kill(-j->pgid, SIGCONT);
    int wstatus;
    waitpid(-j->pgid, &wstatus, WUNTRACED);
    tcsetpgrp(STDIN_FILENO, getpgrp());
    if (WIFSTOPPED(wstatus)) {
        j->status = JOB_STOPPED;
        printf(C_YELLOW "\n[%d] Stopped  %s\n" C_RESET, j->id, j->cmd);
    } else {
        j->status = JOB_DONE;
        jobs_reap();
    }
    return 0;
}

static int builtin_bg(char **argv) {
    int id = (argv[1]) ? atoi(argv[1]) : (g_job_count > 0 ? g_jobs[g_job_count-1].id : -1);
    Job *j = job_find_by_id(id);
    if (!j) { fprintf(stderr, C_RED "nush: bg: no such job\n" C_RESET); return 1; }
    if (j->status == JOB_RUNNING) { fprintf(stderr, "nush: bg: already running\n"); return 0; }
    j->status = JOB_RUNNING;
    kill(-j->pgid, SIGCONT);
    printf("[%d] %s &\n", j->id, j->cmd);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * SECTION 3 — ENV VAR EXPANSION
 * ═══════════════════════════════════════════════════════════ */

static int g_last_status = 0;

static void expand_vars(const char *src, char *dst, size_t dst_size) {
    size_t di = 0;
    const char *p = src;
    while (*p && di < dst_size - 1) {
        if (*p != '$') { dst[di++] = *p++; continue; }
        p++;
        if (*p == '?') {
            char num[16];
            snprintf(num, sizeof(num), "%d", g_last_status);
            size_t nl = strlen(num);
            if (di + nl < dst_size - 1) { memcpy(dst + di, num, nl); di += nl; }
            p++; continue;
        }
        int braced = (*p == '{');
        if (braced) p++;
        char varname[256]; int vi = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') && vi < 255)
            varname[vi++] = *p++;
        varname[vi] = '\0';
        if (braced && *p == '}') p++;
        const char *val = (vi > 0) ? getenv(varname) : NULL;
        if (val) {
            size_t vl = strlen(val);
            if (di + vl < dst_size - 1) { memcpy(dst + di, val, vl); di += vl; }
        }
    }
    dst[di] = '\0';
}

static void expand_line(const char *src, char *dst, size_t dst_size) {
    char tmp[NUSH_INPUT_MAX];
    size_t di = 0;
    const char *p = src;
    while (*p && di < dst_size - 1) {
        if (*p == '\'') {
            dst[di++] = *p++;
            while (*p && *p != '\'') dst[di++] = *p++;
            if (*p) dst[di++] = *p++;
        } else if (*p == '"') {
            dst[di++] = *p++;
            size_t ti = 0;
            while (*p && *p != '"') tmp[ti++] = *p++;
            tmp[ti] = '\0';
            if (*p) p++;
            char ex[NUSH_INPUT_MAX];
            expand_vars(tmp, ex, sizeof(ex));
            size_t el = strlen(ex);
            if (di + el < dst_size - 1) { memcpy(dst + di, ex, el); di += el; }
            dst[di++] = '"';
        } else {
            size_t ti = 0;
            while (*p && *p != '\'' && *p != '"') tmp[ti++] = *p++;
            tmp[ti] = '\0';
            char ex[NUSH_INPUT_MAX];
            expand_vars(tmp, ex, sizeof(ex));
            size_t el = strlen(ex);
            if (di + el < dst_size - 1) { memcpy(dst + di, ex, el); di += el; }
        }
    }
    dst[di] = '\0';
}

/* ═══════════════════════════════════════════════════════════
 * SECTION 4 — SIGNALS
 * ═══════════════════════════════════════════════════════════ */

static void sigint_handler(int sig) {
    (void)sig;
    write(STDOUT_FILENO, "\n", 1);
}

static void sigchld_handler(int sig) {
    (void)sig;
    int wstatus; pid_t pid;
    while ((pid = waitpid(-1, &wstatus, WNOHANG|WUNTRACED)) > 0) {
        for (int i = 0; i < g_job_count; i++) {
            if (getpgid(pid) == g_jobs[i].pgid) {
                if (WIFEXITED(wstatus) || WIFSIGNALED(wstatus))
                    g_jobs[i].status = JOB_DONE;
                else if (WIFSTOPPED(wstatus)) {
                    g_jobs[i].status = JOB_STOPPED;
                    printf(C_YELLOW "\n[%d]+ Stopped  %s\n" C_RESET, g_jobs[i].id, g_jobs[i].cmd);
                    fflush(stdout);
                }
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * SECTION 5 — PROMPT
 * ═══════════════════════════════════════════════════════════ */

static void print_prompt(void) {
    char cwd[PATH_MAX], host[256];
    struct passwd *pw = getpwuid(getuid());
    const char *user  = pw ? pw->pw_name : "user";
    gethostname(host, sizeof(host));
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
    const char *home = getenv("HOME");
    char dcwd[PATH_MAX];
    if (home && strncmp(cwd, home, strlen(home)) == 0)
        snprintf(dcwd, sizeof(dcwd), "~%s", cwd + strlen(home));
    else strncpy(dcwd, cwd, sizeof(dcwd));
    if (g_last_status != 0) printf(C_RED "[%d] " C_RESET, g_last_status);
    printf(C_GREEN "%s@%s" C_RESET ":" C_BLUE "%s" C_RESET "$ ", user, host, dcwd);
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════
 * SECTION 6 — PARSING
 * ═══════════════════════════════════════════════════════════ */

static int tokenize(char *line, char **argv) {
    int argc = 0; char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char q = *p++; argv[argc++] = p;
            while (*p && *p != q) p++;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
        if (argc >= MAX_ARGS - 1) break;
    }
    argv[argc] = NULL; return argc;
}

#define MAX_LOGICAL 32
static int split_logical(char *line, char *segs[], char ops[], int max) {
    int count = 0; char *p = line; char *seg = line;
    ops[0] = '\0';
    while (*p && count < max - 1) {
        if (*p == '\'' || *p == '"') {
            char q = *p++; while (*p && *p != q) p++; if (*p) p++; continue;
        }
        if ((p[0]=='&' && p[1]=='&') || (p[0]=='|' && p[1]=='|')) {
            char op = p[0]; *p = '\0';
            segs[count] = seg; ops[count+1] = op; count++;
            p += 2; seg = p;
        } else p++;
    }
    segs[count++] = seg; return count;
}

static int split_pipes(char *line, char *segs[], int max) {
    int count = 0; char *seg = line;
    for (char *p = line; *p; p++) {
        if (*p == '\'' || *p == '"') {
            char q = *p++; while (*p && *p != q) p++; if (!*p) break; continue;
        }
        if (*p == '|' && p[1] != '|' && (p == line || p[-1] != '|')) {
            *p = '\0'; segs[count++] = seg; seg = p + 1;
            if (count >= max) break;
        }
    }
    segs[count++] = seg; return count;
}

/* ═══════════════════════════════════════════════════════════
 * SECTION 7 — I/O REDIRECTION
 * ═══════════════════════════════════════════════════════════ */

typedef struct { char *infile, *outfile; int append; } Redirect;

static int parse_redirects(char **argv, int argc, Redirect *r) {
    r->infile = r->outfile = NULL; r->append = 0; int na = 0;
    for (int i = 0; i < argc; i++) {
        if      (!strcmp(argv[i],"<")  && i+1<argc) r->infile  = argv[++i];
        else if (!strcmp(argv[i],">>") && i+1<argc) { r->outfile = argv[++i]; r->append = 1; }
        else if (!strcmp(argv[i],">")  && i+1<argc) r->outfile = argv[++i];
        else argv[na++] = argv[i];
    }
    argv[na] = NULL; return na;
}

static void apply_redirects(const Redirect *r) {
    if (r->infile) {
        int fd = open(r->infile, O_RDONLY);
        if (fd < 0) { perror("nush: open"); exit(EXIT_FAILURE); }
        dup2(fd, STDIN_FILENO); close(fd);
    }
    if (r->outfile) {
        int flags = O_WRONLY|O_CREAT|(r->append ? O_APPEND : O_TRUNC);
        int fd = open(r->outfile, flags, 0644);
        if (fd < 0) { perror("nush: open"); exit(EXIT_FAILURE); }
        dup2(fd, STDOUT_FILENO); close(fd);
    }
}

/* ═══════════════════════════════════════════════════════════
 * SECTION 8 — BUILT-INS
 * ═══════════════════════════════════════════════════════════ */

static int builtin_cd(char **argv) {
    const char *dir = argv[1] ? argv[1] : getenv("HOME");
    if (!dir) dir = "/";
    if (chdir(dir) != 0) {
        fprintf(stderr, C_RED "nush: cd: %s: %s\n" C_RESET, dir, strerror(errno));
        return 1;
    }
    return 0;
}

static int builtin_help(void) {
    printf("\n  " C_GREEN "nush" C_RESET " — New Unix Shell\n\n");
    printf("  Built-in commands:\n");
    printf("    cd [dir]         Change directory (default: $HOME)\n");
    printf("    history          Show command history\n");
    printf("    jobs             List background/stopped jobs\n");
    printf("    fg [n]           Bring job n to foreground\n");
    printf("    bg [n]           Resume stopped job n in background\n");
    printf("    help             Show this help\n");
    printf("    exit [code]      Exit the shell\n\n");
    printf("  Operators:\n");
    printf("    cmd1 | cmd2      Pipe\n");
    printf("    cmd1 && cmd2     Run cmd2 only if cmd1 succeeds\n");
    printf("    cmd1 || cmd2     Run cmd2 only if cmd1 fails\n");
    printf("    cmd > file       Redirect stdout (overwrite)\n");
    printf("    cmd >> file      Redirect stdout (append)\n");
    printf("    cmd < file       Redirect stdin\n");
    printf("    cmd &            Run in background\n\n");
    printf("  Variables:\n");
    printf("    $VAR / ${VAR}    Expand environment variable\n");
    printf("    $?               Last exit status\n\n");
    return 0;
}

static int handle_builtin(char **argv, int argc, int *ret) {
    if (!argv[0]) return 0;
    if (!strcmp(argv[0],"exit"))    { history_free(); exit(argc>1?atoi(argv[1]):0); }
    if (!strcmp(argv[0],"cd"))      { *ret=builtin_cd(argv);   return 1; }
    if (!strcmp(argv[0],"history")) { history_print(); *ret=0; return 1; }
    if (!strcmp(argv[0],"help"))    { *ret=builtin_help();      return 1; }
    if (!strcmp(argv[0],"jobs"))    { builtin_jobs(); *ret=0;  return 1; }
    if (!strcmp(argv[0],"fg"))      { *ret=builtin_fg(argv);   return 1; }
    if (!strcmp(argv[0],"bg"))      { *ret=builtin_bg(argv);   return 1; }
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * SECTION 9 — EXECUTION
 * ═══════════════════════════════════════════════════════════ */

static int exec_pipeline(char *segs[], int nseg, int background, const char *full_cmd);

static int exec_single(char **argv, int argc, int background, const char *full_cmd) {
    Redirect redir;
    argc = parse_redirects(argv, argc, &redir);
    if (!argc || !argv[0]) return 0;
    int ret = 0;
    if (handle_builtin(argv, argc, &ret)) return ret;

    pid_t pid = fork();
    if (pid < 0) { perror("nush: fork"); return 1; }
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL); signal(SIGTSTP, SIG_DFL); signal(SIGTTOU, SIG_DFL);
        apply_redirects(&redir);
        execvp(argv[0], argv);
        fprintf(stderr, C_RED "nush: %s: %s\n" C_RESET, argv[0], strerror(errno));
        exit(EXIT_FAILURE);
    }
    setpgid(pid, pid);
    if (!background) {
        tcsetpgrp(STDIN_FILENO, pid);
        int status; waitpid(pid, &status, WUNTRACED);
        tcsetpgrp(STDIN_FILENO, getpgrp());
        if (WIFSTOPPED(status)) {
            int jid = job_add(pid, full_cmd ? full_cmd : argv[0]);
            g_jobs[jid-1].status = JOB_STOPPED;
            printf(C_YELLOW "\n[%d]+ Stopped  %s\n" C_RESET, jid, full_cmd ? full_cmd : argv[0]);
            return 0;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    int jid = job_add(pid, full_cmd ? full_cmd : argv[0]);
    printf("[%d] %d\n", jid, pid);
    return 0;
}

static int exec_pipeline(char *segs[], int nseg, int background, const char *full_cmd) {
    if (nseg == 1) {
        char *argv[MAX_ARGS]; int argc = tokenize(segs[0], argv);
        return exec_single(argv, argc, background, full_cmd);
    }
    int pipes[MAX_PIPES][2]; pid_t pids[MAX_PIPES+1]; pid_t pgid = 0;
    for (int i = 0; i < nseg-1; i++)
        if (pipe(pipes[i]) < 0) { perror("nush: pipe"); return 1; }

    for (int i = 0; i < nseg; i++) {
        char *argv[MAX_ARGS]; int argc = tokenize(segs[i], argv);
        Redirect redir; argc = parse_redirects(argv, argc, &redir);
        pids[i] = fork();
        if (pids[i] < 0) { perror("nush: fork"); return 1; }
        if (pids[i] == 0) {
            setpgid(0, pgid ? pgid : getpid());
            signal(SIGINT, SIG_DFL); signal(SIGTSTP, SIG_DFL); signal(SIGTTOU, SIG_DFL);
            if (i > 0)        dup2(pipes[i-1][0], STDIN_FILENO);
            if (i < nseg-1)   dup2(pipes[i][1],   STDOUT_FILENO);
            for (int j = 0; j < nseg-1; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            apply_redirects(&redir);
            if (argv[0]) execvp(argv[0], argv);
            exit(EXIT_FAILURE);
        }
        if (pgid == 0) pgid = pids[i];
        setpgid(pids[i], pgid);
    }
    for (int i = 0; i < nseg-1; i++) { close(pipes[i][0]); close(pipes[i][1]); }

    int ret = 0;
    if (!background) {
        tcsetpgrp(STDIN_FILENO, pgid);
        for (int i = 0; i < nseg; i++) {
            int status; waitpid(pids[i], &status, WUNTRACED);
            if (i == nseg-1) ret = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        tcsetpgrp(STDIN_FILENO, getpgrp());
    } else {
        int jid = job_add(pgid, full_cmd ? full_cmd : segs[0]);
        printf("[%d] %d\n", jid, pgid);
    }
    return ret;
}

/* ═══════════════════════════════════════════════════════════
 * SECTION 10 — LOGICAL OPERATORS (&& / ||)
 * ═══════════════════════════════════════════════════════════ */

static int exec_logical(char *line, int background) {
    char *segs[MAX_LOGICAL]; char ops[MAX_LOGICAL]; char buf[NUSH_INPUT_MAX];
    strncpy(buf, line, sizeof(buf)-1);
    int nseg = split_logical(buf, segs, ops, MAX_LOGICAL);
    int status = 0;
    for (int i = 0; i < nseg; i++) {
        if (i > 0) {
            if (ops[i] == '&' && status != 0) continue;
            if (ops[i] == '|' && status == 0) continue;
        }
        char pb[NUSH_INPUT_MAX]; strncpy(pb, segs[i], sizeof(pb)-1);
        char *ps[MAX_PIPES+1]; int np = split_pipes(pb, ps, MAX_PIPES);
        status = exec_pipeline(ps, np, background, segs[i]);
    }
    return status;
}

/* ═══════════════════════════════════════════════════════════
 * SECTION 11 — MAIN LOOP
 * ═══════════════════════════════════════════════════════════ */

int main(void) {
    setpgid(0, 0);
    tcsetpgrp(STDIN_FILENO, getpgrp());

    struct sigaction sa_int  = { .sa_handler = sigint_handler,  .sa_flags = SA_RESTART };
    struct sigaction sa_chld = { .sa_handler = sigchld_handler, .sa_flags = SA_RESTART|SA_NOCLDSTOP };
    struct sigaction sa_ign  = { .sa_handler = SIG_IGN };
    sigemptyset(&sa_int.sa_mask); sigemptyset(&sa_chld.sa_mask); sigemptyset(&sa_ign.sa_mask);
    sigaction(SIGINT,  &sa_int,  NULL);
    sigaction(SIGCHLD, &sa_chld, NULL);
    sigaction(SIGTSTP, &sa_ign,  NULL);
    sigaction(SIGTTOU, &sa_ign,  NULL);

    printf("\n  " C_GREEN "nush" C_RESET " — New Unix Shell  (type " C_GREEN "help" C_RESET " for usage)\n\n");

    char raw[NUSH_INPUT_MAX], expanded[NUSH_INPUT_MAX];

    while (1) {
        print_prompt();
        if (!fgets(raw, sizeof(raw), stdin)) { printf("\nexit\n"); break; }
        raw[strcspn(raw, "\n")] = '\0';
        char *t = raw;
        while (*t == ' ' || *t == '\t') t++;
        if (!*t) continue;

        expand_line(t, expanded, sizeof(expanded));
        history_add(expanded);

        int background = 0;
        size_t len = strlen(expanded);
        if (len > 0 && expanded[len-1] == '&' && (len < 2 || expanded[len-2] != '&')) {
            background = 1; expanded[--len] = '\0';
            while (len > 0 && expanded[len-1] == ' ') expanded[--len] = '\0';
        }

        g_last_status = exec_logical(expanded, background);
    }
    history_free();
    return 0;
}
