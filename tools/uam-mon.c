/*
 * uam-mon - a client for ember's monitor (uam-run --monitor SPEC).
 *
 *   uam-mon WHERE                     commands from stdin (a prompt on a terminal)
 *   uam-mon WHERE -c "stop; status"   these commands, then exit
 *
 * WHERE: unix:PATH, a plain PATH, tcp:PORT or tcp:HOST:PORT. Output lines are
 * printed as they come, errors go to stderr, events ("! stopped ...") are shown
 * between replies. -j asks for JSON replies. Exit status: 0, 1 if a command failed,
 * 2 if the monitor could not be reached or went away before answering.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define PROMPT "ember-mon> "

static void usage(void)
{
    fprintf(stderr,
            "usage: uam-mon WHERE [-j] [-c \"COMMAND[; COMMAND...]\"]...\n"
            "  WHERE  unix:PATH, PATH, tcp:PORT or tcp:HOST:PORT (uam-run --monitor SPEC)\n"
            "  -c     run these commands (';' between them) and exit; repeatable\n"
            "  -j     JSON replies (the monitor's \"json on\")\n"
            "Without -c, commands are read from stdin, one per line.\n");
    exit(2);
}

static int connect_to(const char *where)
{
    if (!strncmp(where, "tcp:", 4)) {
        char host[256] = "127.0.0.1";
        const char *port = where + 4, *colon = strrchr(port, ':');
        if (colon) {
            snprintf(host, sizeof(host), "%.*s", (int)(colon - port), port);
            port = colon + 1;
        }
        struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res;
        if (getaddrinfo(host, port, &hints, &res) != 0)
            return -1;
        int fd = -1;
        for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
            fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
                close(fd);
                fd = -1;
            }
        }
        freeaddrinfo(res);
        return fd;
    }
    const char *path = !strncmp(where, "unix:", 5) ? where + 5 : where;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(a.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(a.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

/* Commands to send: from -c, split on ';'. */
static char **queue;
static size_t n_queue, cap_queue, next_queue;

static void enqueue(const char *text)
{
    const char *p = text;
    for (;;) {
        const char *semi = strchr(p, ';');
        size_t n = semi ? (size_t)(semi - p) : strlen(p);
        while (n && (*p == ' ' || *p == '\t')) {
            p++;
            n--;
        }
        while (n && (p[n - 1] == ' ' || p[n - 1] == '\t'))
            n--;
        if (n) {
            if (n_queue == cap_queue) {
                cap_queue = cap_queue ? 2 * cap_queue : 8;
                queue = realloc(queue, cap_queue * sizeof(*queue));
                if (!queue)
                    exit(2);
            }
            queue[n_queue++] = strndup(p, n);
        }
        if (!semi)
            break;
        p = semi + 1;
    }
}

int main(int argc, char **argv)
{
    const char *where = NULL;
    bool json = false, oneshot = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            enqueue(argv[++i]);
            oneshot = true;
        } else if (!strcmp(argv[i], "-j")) {
            json = true;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") || argv[i][0] == '-'
                   || where) {
            usage();
        } else {
            where = argv[i];
        }
    }
    if (!where)
        usage();
    signal(SIGPIPE, SIG_IGN);   /* a monitor that went away is exit status 2, not a signal */
    int fd = connect_to(where);
    if (fd < 0) {
        fprintf(stderr, "uam-mon: cannot reach the monitor at %s: %s\n", where, strerror(errno));
        return 2;
    }
    const bool tty = !oneshot && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    int rc = 0;
    unsigned seq = 0;
    char pending[32] = "";     /* the id of the command waiting for its reply */
    bool stdin_open = !oneshot, got_hello = false;
    char *in = NULL;
    size_t in_len = 0, in_cap = 0;
    if (json) {
        const char *on = "#j json on\n";
        if (!send_all(fd, on, strlen(on)))
            return 2;
        snprintf(pending, sizeof(pending), "j");
    }

    for (;;) {
        /* Send the next command once the last one is answered. */
        if (!pending[0] && next_queue < n_queue) {
            char line[16384];
            snprintf(pending, sizeof(pending), "u%u", ++seq);
            int n = snprintf(line, sizeof(line), "#%s %s\n", pending, queue[next_queue++]);
            if (n < 0 || (size_t)n >= sizeof(line) || !send_all(fd, line, (size_t)n)) {
                fprintf(stderr, "uam-mon: cannot send the command\n");
                return 2;
            }
        }
        if (!pending[0] && next_queue == n_queue) {
            if (oneshot || !stdin_open)
                break;
        }
        struct pollfd pfd[2] = { { .fd = fd, .events = POLLIN },
                                 { .fd = STDIN_FILENO, .events = POLLIN } };
        nfds_t nfd = stdin_open && !pending[0] ? 2 : 1;
        if (poll(pfd, nfd, -1) < 0) {
            if (errno == EINTR)
                continue;
            return 2;
        }
        if (nfd == 2 && pfd[1].revents) {
            char buf[4096];
            if (!fgets(buf, sizeof(buf), stdin)) {
                stdin_open = false;
                continue;
            }
            buf[strcspn(buf, "\r\n")] = 0;
            if (buf[0])
                enqueue(buf);   /* a ';' runs several */
            else if (tty)
                fputs(PROMPT, stdout), fflush(stdout);
            continue;
        }
        if (!pfd[0].revents)
            continue;
        if (in_len + 4096 > in_cap) {
            in_cap = in_cap ? 2 * in_cap : 8192;
            in = realloc(in, in_cap);
            if (!in)
                return 2;
        }
        ssize_t n = read(fd, in + in_len, in_cap - in_len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (pending[0]) {
                fprintf(stderr, "uam-mon: the monitor went away\n");
                return 2;
            }
            break;   /* the run ended */
        }
        in_len += (size_t)n;
        char *line;
        size_t used = 0;
        while ((line = memchr(in + used, '\n', in_len - used))) {
            *line = 0;
            char *l = in + used;
            used = (size_t)(line - in) + 1;
            char tag[40];
            int tl = snprintf(tag, sizeof(tag), "#%s ", pending);
            if (pending[0] && !strncmp(l, tag, (size_t)tl)) {
                /* text reply: the line that ends it */
                const char *end = l + tl;
                if (!strncmp(end, "error: ", 7)) {
                    fprintf(stderr, "%s\n", end);
                    rc = 1;
                }
                pending[0] = 0;
            } else if (pending[0] && l[0] == '{' && strstr(l, "\"id\":\"")
                       && !strncmp(strstr(l, "\"id\":\"") + 6, pending, strlen(pending))
                       && strstr(l, "\"id\":\"")[6 + strlen(pending)] == '"') {
                /* JSON reply */
                if (strcmp(pending, "j"))
                    printf("%s\n", l);
                if (strstr(l, "\"ok\":false"))
                    rc = 1;
                pending[0] = 0;
            } else if (!got_hello && (!strncmp(l, "! hello", 7) || !strncmp(l, "{\"event\":\"hello\"", 16))) {
                got_hello = true;   /* the greeting: not shown */
            } else {
                printf("%s\n", l);
            }
            if (!pending[0] && tty && next_queue == n_queue)
                fputs(PROMPT, stdout);
        }
        memmove(in, in + used, in_len - used);
        in_len -= used;
        fflush(stdout);
    }
    close(fd);
    return rc;
}
