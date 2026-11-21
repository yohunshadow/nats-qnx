/*
 * nats-server-remote: a stand-in for the `nats-server` executable on a QNX
 * target, used ONLY to run the upstream nats.c unit tests.
 *
 * There is no nats-server for QNX (Go has no GOOS=qnx), so the upstream
 * test-suite's fork()/exec("nats-server ...") is redirected to this shim
 * (via NATS_TEST_SERVER_EXE).  The shim forwards its argv (and the contents
 * of any config file passed with -c/--config) to remote-server-ctl.py on the
 * dev host, which starts a real nats-server there.  Client ports on the
 * target's 127.0.0.1 are tunnelled to the host with `ssh -R`.
 *
 * Protocol (line based, host listens on 10.0.2.2:4299 by default):
 *   shim -> host : "ARGC <n>\n" then n lines "ARG <arg>\n"
 *                  optional "FILE <path> <len>\n<bytes>" for config files
 *                  "GO\n"
 *   host -> shim : "READY\n" | "EXITED <code>\n"
 *   on SIGINT/SIGTERM: shim -> host "STOP\n", host -> shim "STOPPED\n"
 *
 * Copyright 2026 nats-qnx authors. Licensed under the Apache License 2.0.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t stop_req = 0;
static void on_sig(int s) { (void)s; stop_req = 1; }

static int write_all(int fd, const void *b, size_t n)
{
    const char *p = b;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static void send_file(int fd, const char *path)
{
    FILE *f = fopen(path, "rb");
    struct stat st;
    char hdr[1200];
    if (f == NULL || fstat(fileno(f), &st) != 0) { if (f) fclose(f); return; }
    char *buf = malloc((size_t)st.st_size + 1);
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    snprintf(hdr, sizeof(hdr), "FILE %zu %s\n", n, path);
    write_all(fd, hdr, strlen(hdr));
    write_all(fd, buf, n);
    free(buf);
}

int main(int argc, char **argv)
{
    const char *host = getenv("NATS_REMOTE_CTL_HOST");
    const char *port = getenv("NATS_REMOTE_CTL_PORT");
    struct sockaddr_in sa;
    char line[4096];
    int fd, i;

    if (host == NULL) host = "10.0.2.2";
    if (port == NULL) port = "4299";

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)atoi(port));
    inet_pton(AF_INET, host, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        perror("nats-server-remote: connect to controller");
        return 1;
    }

    snprintf(line, sizeof(line), "ARGC %d\n", argc - 1);
    write_all(fd, line, strlen(line));
    for (i = 1; i < argc; i++) {
        snprintf(line, sizeof(line), "ARG %s\n", argv[i]);
        write_all(fd, line, strlen(line));
    }
    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0
            || strcmp(argv[i], "-config") == 0)
            send_file(fd, argv[i + 1]);
    }
    write_all(fd, "GO\n", 3);

    /* Wait for either a signal (test calls _stopServer) or controller msg. */
    for (;;) {
        fd_set rf;
        struct timeval tv = {0, 100000};
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        if (stop_req) break;
        int r = select(fd + 1, &rf, NULL, NULL, &tv);
        if (r > 0) {
            ssize_t n = read(fd, line, sizeof(line) - 1);
            if (n <= 0) return 2;               /* controller went away */
            line[n] = '\0';
            if (strstr(line, "EXITED") != NULL) {
                int code = 1;
                char *p = strstr(line, "EXITED");
                sscanf(p, "EXITED %d", &code);
                return code;
            }
            /* READY: just keep waiting */
        }
    }
    write_all(fd, "STOP\n", 5);
    /* Wait (bounded) for "STOPPED <loglen>\n<log bytes>" so the port is free
     * for the next test, and mirror the server log ("-l file") locally. */
    {
        const char *logpath = NULL;
        size_t have = 0, want = 0, hdr = 0;
        char *acc = NULL;
        for (i = 1; i < argc - 1; i++)
            if (strcmp(argv[i], "-l") == 0) logpath = argv[i + 1];
        for (i = 0; i < 100; i++) {
            fd_set rf;
            struct timeval tv = {0, 100000};
            FD_ZERO(&rf);
            FD_SET(fd, &rf);
            if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) continue;
            ssize_t n = read(fd, line, sizeof(line));
            if (n <= 0) break;
            acc = realloc(acc, have + (size_t)n + 1);
            memcpy(acc + have, line, (size_t)n);
            have += (size_t)n;
            acc[have] = '\0';
            if (hdr == 0) {
                char *st = strstr(acc, "STOPPED");
                char *nl = st ? strchr(st, '\n') : NULL;
                if (nl == NULL) continue;
                want = (size_t)strtoul(st + 7, NULL, 10);
                hdr = (size_t)(nl - acc) + 1;
            }
            if (have - hdr >= want) break;
        }
        if (logpath != NULL && hdr > 0 && have - hdr >= want) {
            FILE *f = fopen(logpath, "wb");
            if (f) { fwrite(acc + hdr, 1, want, f); fclose(f); }
        }
        free(acc);
    }
    close(fd);
    return 0;
}
