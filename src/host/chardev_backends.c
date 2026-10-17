/*
 * chardev_backends.c - Host backends for uam_chardev (uam/host.h): null,
 * fixed buffer, stdio (optionally raw), pty, TCP/Unix server sockets.
 */
#define _XOPEN_SOURCE 700  /* posix_openpt, grantpt, unlockpt, ptsname */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include "uam/host.h"

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Write everything (output fds may be non-blocking); give up on hard errors. */
static size_t write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n > 0)
            done += (size_t)n;
        else if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        else
            break;
    }
    return len;
}

/* ---- null ---------------------------------------------------------------- */

static size_t null_write(uam_chardev *cd, const void *buf, size_t len)
{
    (void)cd; (void)buf;
    return len;
}

static void free_cd(uam_chardev *cd) { free(cd); }

static const uam_chardev_ops null_ops = { .type = "null", .write = null_write, .destroy = free_cd };

uam_chardev *uam_chardev_null_create(void)
{
    uam_chardev *cd = malloc(sizeof(*cd));
    if (cd)
        uam_chardev_init(cd, &null_ops);
    return cd;
}

/* ---- buffer: FILE * output, fixed input ---------------------------------- */

typedef struct buf_cd {
    uam_chardev cd;
    FILE       *out;
    uint8_t    *in;
    size_t      in_len, in_off;
} buf_cd;

static size_t buf_write(uam_chardev *cd, const void *buf, size_t len)
{
    buf_cd *b = UAM_CONTAINER_OF(cd, buf_cd, cd);
    if (b->out) {
        fwrite(buf, 1, len, b->out);
        if (memchr(buf, '\n', len))
            fflush(b->out);
    }
    return len;
}

static void buf_poll(uam_chardev *cd)
{
    buf_cd *b = UAM_CONTAINER_OF(cd, buf_cd, cd);
    if (b->in_off < b->in_len)
        b->in_off += uam_chardev_input(cd, b->in + b->in_off, b->in_len - b->in_off);
}

static void buf_destroy(uam_chardev *cd)
{
    buf_cd *b = UAM_CONTAINER_OF(cd, buf_cd, cd);
    if (b->out)
        fflush(b->out);
    free(b->in);
    free(b);
}

static const uam_chardev_ops buf_ops = {
    .type = "buffer", .write = buf_write, .poll = buf_poll, .destroy = buf_destroy,
};

uam_chardev *uam_chardev_buffer_create(FILE *out, const void *input, size_t len)
{
    buf_cd *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    uam_chardev_init(&b->cd, &buf_ops);
    b->out = out;
    if (input && len) {
        b->in = malloc(len);
        if (!b->in) {
            free(b);
            return NULL;
        }
        memcpy(b->in, input, len);
        b->in_len = len;
        buf_poll(&b->cd);
    }
    return &b->cd;
}

/* ---- file descriptors: stdio, pty, sockets -------------------------------- */

typedef enum { FD_STDIO, FD_PTY, FD_SERVER } fd_kind;

typedef struct fd_cd {
    uam_chardev    cd;
    fd_kind        kind;
    int            in_fd, out_fd;   /* -1 when closed / not connected */
    int            listen_fd;       /* FD_SERVER */
    int            keep_fd;         /* FD_PTY: slave kept open so the master never sees EIO */
    bool           raw, escape;     /* stdio raw mode; Ctrl-A seen */
    bool           restore_tty;
    struct termios saved_tty;
    int            saved_fl;        /* FD_STDIO: stdin's flags (shared with the shell) */
} fd_cd;

static fd_cd *to_fd(uam_chardev *cd) { return UAM_CONTAINER_OF(cd, fd_cd, cd); }

static size_t fd_write(uam_chardev *cd, const void *buf, size_t len)
{
    fd_cd *f = to_fd(cd);
    if (f->out_fd >= 0)
        write_all(f->out_fd, buf, len);
    return len;
}

static int fd_fd(uam_chardev *cd)
{
    fd_cd *f = to_fd(cd);
    if (f->kind == FD_SERVER && f->in_fd < 0)
        return f->listen_fd;    /* wait for a client */
    return f->in_fd;
}

/* stdio raw mode: Ctrl-A x quits, Ctrl-A Ctrl-A sends Ctrl-A. */
static void stdio_filter(fd_cd *f, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (f->escape) {
            f->escape = false;
            if (c == 'x' || c == 'X') {
                uam_chardev_quit(&f->cd);
                continue;
            }
            if (c != 0x01)
                continue;
        } else if (c == 0x01) {
            f->escape = true;
            continue;
        }
        uam_chardev_input(&f->cd, &c, 1);
    }
}

static void fd_poll(uam_chardev *cd)
{
    fd_cd *f = to_fd(cd);
    if (f->kind == FD_SERVER && f->in_fd < 0) {
        int c = accept(f->listen_fd, NULL, NULL);
        if (c >= 0) {
            set_nonblock(c);
            f->in_fd = f->out_fd = c;
            uam_log(UAM_LOG_INFO, "chardev", "client connected");
        }
        return;
    }
    if (f->in_fd < 0)
        return;
    size_t room = UAM_CHARDEV_RXBUF - cd->rx_len;
    uint8_t buf[512];
    while (room) {
        ssize_t n = read(f->in_fd, buf, room < sizeof(buf) ? room : sizeof(buf));
        if (n > 0) {
            if (f->kind == FD_STDIO && f->raw)
                stdio_filter(f, buf, (size_t)n);
            else
                uam_chardev_input(cd, buf, (size_t)n);
            room = UAM_CHARDEV_RXBUF - cd->rx_len;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n < 0 && errno == EIO && f->kind == FD_PTY)
            return;  /* no client on the slave side */
        /* EOF or hard error */
        if (f->kind == FD_SERVER) {
            close(f->in_fd);
            f->in_fd = f->out_fd = -1;
            uam_log(UAM_LOG_INFO, "chardev", "client disconnected");
        } else {
            f->in_fd = -1;  /* input ended; output continues */
        }
        return;
    }
}

static void restore_tty(fd_cd *f)
{
    if (f->restore_tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &f->saved_tty);
        f->restore_tty = false;
    }
}

/* Terminal restore at exit even if the program dies through exit(). */
static fd_cd *g_raw_stdio;
static void restore_tty_atexit(void)
{
    if (g_raw_stdio) {
        restore_tty(g_raw_stdio);
        if (g_raw_stdio->saved_fl >= 0)
            fcntl(STDIN_FILENO, F_SETFL, g_raw_stdio->saved_fl);
    }
}

static void fd_destroy(uam_chardev *cd)
{
    fd_cd *f = to_fd(cd);
    switch (f->kind) {
    case FD_STDIO:
        restore_tty(f);
        if (f->saved_fl >= 0)
            fcntl(STDIN_FILENO, F_SETFL, f->saved_fl);
        if (g_raw_stdio == f)
            g_raw_stdio = NULL;
        break;
    case FD_PTY:
        if (f->in_fd >= 0)
            close(f->in_fd);
        if (f->keep_fd >= 0)
            close(f->keep_fd);
        break;
    case FD_SERVER:
        if (f->in_fd >= 0)
            close(f->in_fd);
        if (f->listen_fd >= 0)
            close(f->listen_fd);
        break;
    }
    free(f);
}

static const uam_chardev_ops fd_ops = {
    .type = "fd", .write = fd_write, .fd = fd_fd, .poll = fd_poll, .destroy = fd_destroy,
};

static fd_cd *fd_new(fd_kind kind)
{
    fd_cd *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    uam_chardev_init(&f->cd, &fd_ops);
    f->kind = kind;
    f->in_fd = f->out_fd = f->listen_fd = f->keep_fd = -1;
    return f;
}

uam_chardev *uam_chardev_stdio_create(bool raw)
{
    fd_cd *f = fd_new(FD_STDIO);
    if (!f)
        return NULL;
    f->in_fd = STDIN_FILENO;
    f->out_fd = STDOUT_FILENO;
    f->saved_fl = fcntl(STDIN_FILENO, F_GETFL);
    set_nonblock(STDIN_FILENO);
    if (raw && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &f->saved_tty) == 0) {
        struct termios t = f->saved_tty;
        cfmakeraw(&t);
        t.c_oflag |= OPOST | ONLCR;   /* keep output readable */
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        f->restore_tty = true;
        f->raw = true;
        g_raw_stdio = f;
        static bool registered;
        if (!registered) {
            atexit(restore_tty_atexit);
            registered = true;
        }
        uam_log(UAM_LOG_WARN, "chardev", "console in raw mode: Ctrl-A x to quit");
    }
    return &f->cd;
}

uam_chardev *uam_chardev_pty_create(void)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) {
        if (m >= 0)
            close(m);
        uam_log(UAM_LOG_ERROR, "chardev", "cannot create a pty: %s", strerror(errno));
        return NULL;
    }
    const char *path = ptsname(m);
    int s = path ? open(path, O_RDWR | O_NOCTTY) : -1;
    if (s >= 0) {
        struct termios t;
        if (tcgetattr(s, &t) == 0) {
            cfmakeraw(&t);
            tcsetattr(s, TCSANOW, &t);
        }
    }
    fd_cd *f = fd_new(FD_PTY);
    if (!f) {
        close(m);
        if (s >= 0)
            close(s);
        return NULL;
    }
    set_nonblock(m);
    f->in_fd = f->out_fd = m;
    f->keep_fd = s;
    uam_log(UAM_LOG_WARN, "chardev", "console on %s", path ? path : "?");
    return &f->cd;
}

uam_chardev *uam_chardev_socket_create(const char *spec)
{
    int fd = -1;
    if (strncmp(spec, "unix:", 5) == 0) {
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        if (strlen(spec + 5) >= sizeof(a.sun_path))
            return NULL;
        strcpy(a.sun_path, spec + 5);
        unlink(a.sun_path);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0)
            goto fail;
    } else if (strncmp(spec, "tcp:", 4) == 0) {
        const char *hp = spec + 4, *colon = strrchr(hp, ':');
        char host[256] = "127.0.0.1";
        const char *port = hp;
        if (colon) {
            size_t n = (size_t)(colon - hp);
            if (n >= sizeof(host))
                return NULL;
            memcpy(host, hp, n);
            host[n] = 0;
            port = colon + 1;
        }
        struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
                                  .ai_flags = AI_PASSIVE }, *res = NULL;
        if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
            return NULL;
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        int one = 1;
        if (fd >= 0)
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        int rc = fd >= 0 ? bind(fd, res->ai_addr, res->ai_addrlen) : -1;
        freeaddrinfo(res);
        if (rc != 0)
            goto fail;
    } else {
        return NULL;
    }
    if (listen(fd, 1) != 0)
        goto fail;
    set_nonblock(fd);
    fd_cd *f = fd_new(FD_SERVER);
    if (!f)
        goto fail;
    f->listen_fd = fd;
    uam_log(UAM_LOG_WARN, "chardev", "console: waiting for a client on %s", spec);
    return &f->cd;
fail:
    uam_log(UAM_LOG_ERROR, "chardev", "cannot listen on %s: %s", spec, strerror(errno));
    if (fd >= 0)
        close(fd);
    return NULL;
}

/* ---- spec parser ---------------------------------------------------------- */

/* Decode \n \r \t \\ \xHH escapes. Returns the decoded length. */
static size_t unescape(const char *s, uint8_t *out)
{
    size_t n = 0;
    while (*s) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n': out[n++] = '\n'; s++; break;
            case 'r': out[n++] = '\r'; s++; break;
            case 't': out[n++] = '\t'; s++; break;
            case 'x': {
                char hex[3] = { 0 };
                if (s[1] && s[2]) {
                    hex[0] = s[1];
                    hex[1] = s[2];
                    out[n++] = (uint8_t)strtoul(hex, NULL, 16);
                    s += 3;
                } else {
                    s++;
                }
                break;
            }
            default: out[n++] = (uint8_t)*s++; break;
            }
        } else {
            out[n++] = (uint8_t)*s++;
        }
    }
    return n;
}

static uam_chardev *open_file_spec(const char *arg)
{
    char *copy = strdup(arg);
    if (!copy)
        return NULL;
    char *in_path = NULL, *comma = strstr(copy, ",in=");
    if (comma) {
        *comma = 0;
        in_path = comma + 4;
    }
    FILE *out = strcmp(copy, "-") == 0 ? stdout : fopen(copy, "w");
    uint8_t *in = NULL;
    size_t in_len = 0;
    if (in_path) {
        FILE *fi = fopen(in_path, "rb");
        if (fi) {
            fseek(fi, 0, SEEK_END);
            long sz = ftell(fi);
            rewind(fi);
            in = malloc(sz > 0 ? (size_t)sz : 1);
            if (in && sz > 0)
                in_len = fread(in, 1, (size_t)sz, fi);
            fclose(fi);
        } else {
            uam_log(UAM_LOG_ERROR, "chardev", "cannot open %s", in_path);
        }
    }
    uam_chardev *cd = out ? uam_chardev_buffer_create(out, in, in_len) : NULL;
    if (!out)
        uam_log(UAM_LOG_ERROR, "chardev", "cannot open %s", copy);
    free(in);
    free(copy);
    return cd;
}

uam_chardev *uam_chardev_open(const char *spec)
{
    if (!strcmp(spec, "null"))
        return uam_chardev_null_create();
    if (!strcmp(spec, "stdio"))
        return uam_chardev_stdio_create(false);
    if (!strcmp(spec, "stdio-raw"))
        return uam_chardev_stdio_create(true);
    if (!strcmp(spec, "pty"))
        return uam_chardev_pty_create();
    if (!strncmp(spec, "tcp:", 4) || !strncmp(spec, "unix:", 5))
        return uam_chardev_socket_create(spec);
    if (!strncmp(spec, "file:", 5))
        return open_file_spec(spec + 5);
    if (!strncmp(spec, "string:", 7)) {
        uint8_t *buf = malloc(strlen(spec + 7) + 1);
        if (!buf)
            return NULL;
        size_t n = unescape(spec + 7, buf);
        uam_chardev *cd = uam_chardev_buffer_create(stdout, buf, n);
        free(buf);
        return cd;
    }
    uam_log(UAM_LOG_ERROR, "chardev", "unknown backend '%s'", spec);
    return NULL;
}
