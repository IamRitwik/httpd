
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define LISTENADDR "127.0.0.1"
#define REQ_BUFSIZE 8192

struct sHttpRequest {
    char method[16];
    char url[1024];
};
typedef struct sHttpRequest httpreq;

/* returns socket fd >= 0 on success, -1 on error */
int srv_init(int portno) {
    int s = -1;
    struct sockaddr_in srv;
    int opt = 1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt");
        close(s);
        return -1;
    }

    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    if (inet_pton(AF_INET, LISTENADDR, &srv.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed for %s\n", LISTENADDR);
        close(s);
        return -1;
    }
    srv.sin_port = htons(portno);

    if (bind(s, (struct sockaddr *)&srv, sizeof(srv)) == -1) {
        perror("bind");
        close(s);
        return -1;
    }

    if (listen(s, 16) == -1) {
        perror("listen");
        close(s);
        return -1;
    }

    return s;
}

/* Accept wrapper: returns client fd >= 0 on success, -1 on error */
int cli_accept(int s) {
    int c;
    struct sockaddr_in cli;
    socklen_t addrlen = sizeof(cli);

    memset(&cli, 0, sizeof(cli));
    c = accept(s, (struct sockaddr *)&cli, &addrlen);
    if (c < 0) {
        if (errno != EINTR) perror("accept");
        return -1;
    }
    return c;
}

/* Read HTTP request headers into a newly allocated buffer.
 * Returns pointer to malloc'ed buffer (caller must free) and sets *out_len,
 * or returns NULL on error.
 * It reads until it sees "\r\n\r\n" or fills the buffer.
 */
char *read_request_headers(int c, ssize_t *out_len) {
    char *buf = malloc(REQ_BUFSIZE);
    if (!buf) return NULL;
    ssize_t total = 0;

    while (total < REQ_BUFSIZE - 1) {
        ssize_t n = recv(c, buf + total, REQ_BUFSIZE - 1 - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            free(buf);
            return NULL;
        }
        if (n == 0) break; /* peer closed */
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL) break;
    }

    buf[total] = '\0';
    if (out_len) *out_len = total;
    return buf;
}

/* Very small, safe HTTP request-line parser.
 * Expects a buffer whose first line is like: METHOD SP URL SP HTTP/1.1\r\n
 * Returns malloc'ed httpreq* (caller frees) or NULL on parse failure.
 */
httpreq *parse_http(const char *buf, ssize_t buflen) {
    if (!buf || buflen <= 0) return NULL;

    /* find first line */
    const char *line_end = strstr(buf, "\r\n");
    if (!line_end) return NULL;
    size_t linelen = line_end - buf;

    /* copy the line to a temporary, nul-terminated string we can tokenize */
    char *line = malloc(linelen + 1);
    if (!line) return NULL;
    memcpy(line, buf, linelen);
    line[linelen] = '\0';

    char method[32], url[2048], version[32];
    /* sscanf with width limits */
    int scanned = sscanf(line, "%31s %2047s %31s", method, url, version);
    free(line);

    if (scanned < 2) return NULL;

    httpreq *req = malloc(sizeof *req);
    if (!req) return NULL;

    /* safe copy with bounds */
    strncpy(req->method, method, sizeof(req->method)-1);
    req->method[sizeof(req->method)-1] = '\0';
    strncpy(req->url, url, sizeof(req->url)-1);
    req->url[sizeof(req->url)-1] = '\0';

    return req;
}

/* Minimal handler: echo method/url and send a tiny HTTP response.
 * child should close server_fd and exit after running this.
 */
void cli_conn(int server_fd, int client_fd) {
    ssize_t n;
    char *reqbuf = NULL;
    ssize_t buflen = 0;

    reqbuf = read_request_headers(client_fd, &buflen);
    if (!reqbuf) {
        fprintf(stderr, "Failed to read from client\n");
        close(client_fd);
        return;
    }

    printf("raw request (%zd bytes):\n", buflen);
    /* print safely even if it contains NULs: use fwrite */
    fwrite(reqbuf, 1, (buflen < 8192 ? buflen : 8192), stdout);
    printf("\n--- end preview ---\n");

    httpreq *r = parse_http(reqbuf, buflen);
    if (!r) {
        fprintf(stderr, "parse_http failed\n");
        free(reqbuf);
        close(client_fd);
        return;
    }

    printf("Parsed: method='%s' url='%s'\n", r->method, r->url);

    /* simple response */
    const char *body = "<html><body><h1>Hello World!</h1></body></html>\n";
    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n", strlen(body));

    send(client_fd, header, header_len, 0);
    send(client_fd, body, strlen(body), 0);

    free(r);
    free(reqbuf);
    close(client_fd);
}

/* main with SIGCHLD ignored to avoid zombies (simple approach).
 * Proper production code should reap children properly or use a thread pool.
 */
int main(int argc, char *argv[]) {
    int server_fd, client_fd;
    int port;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }
    port = atoi(argv[1]);
    if (port <= 0) { fprintf(stderr, "Bad port\n"); return 1; }

    /* avoid zombies (simple) */
    signal(SIGCHLD, SIG_IGN);

    server_fd = srv_init(port);
    if (server_fd < 0) return 1;
    printf("Listening on %s:%d\n", LISTENADDR, port);

    for (;;) {
        client_fd = cli_accept(server_fd);
        if (client_fd < 0) continue;

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(client_fd);
            continue;
        }
        if (pid == 0) {
            /* child */
            close(server_fd);       /* child doesn't accept more clients */
            cli_conn(server_fd, client_fd);
            _exit(0);
        } else {
            /* parent */
            close(client_fd);       /* parent doesn't handle this client */
        }
    }

    close(server_fd);
    return 0;
}
