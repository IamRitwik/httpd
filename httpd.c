
/* httpd.c */
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#define LISTENADDR "127.0.0.1"
#define REQ_BUFSIZE 8192

struct sHttpRequest {
    char method[16];
    char url[1024];
};
typedef struct sHttpRequest httpreq;

/* global error message */
char error[128];

/* create and prepare the server socket; returns fd or -1 on error */
int srv_init(int portno) {
    int s = -1;
    struct sockaddr_in srv;
    int opt = 1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        snprintf(error, sizeof(error), "socket() error: %s\n", strerror(errno));
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

/* accept wrapper */
int cli_accept(int s) {
    int c;
    socklen_t addrlen;
    struct sockaddr_in cli;
    addrlen = sizeof(cli);
    memset(&cli, 0, sizeof(cli));
    c = accept(s, (struct sockaddr *)&cli, &addrlen);
    if (c < 0) {
        snprintf(error, sizeof(error), "accept() error: %s\n", strerror(errno));
        return -1;
    }
    return c;
}

/* parse minimal HTTP request-line (method and url).
   It modifies the provided buffer (replacing spaces with '\0').
   Returns malloc'd httpreq (must be free()d by caller) or NULL on error.
*/
httpreq *parse_http(char *str, int len) {
    httpreq *req;
    char *p;

    if (!str || len <= 0) return NULL;

    req = malloc(sizeof(httpreq));
    if (!req) return NULL;
    memset(req, 0, sizeof(*req));

    /* parse method */
    for (p = str; *p && *p != ' '; p++);
    if (*p != ' ') {
        snprintf(error, sizeof(error), "parse_http() NOSPACE error\n");
        free(req);
        return NULL;
    }
    *p = '\0';
    strncpy(req->method, str, sizeof(req->method) - 1);

    /* next token is URL */
    str = p + 1; /* point to after first space */
    for (p = str; *p && *p != ' '; p++);
    if (*p != ' ') {
        snprintf(error, sizeof(error), "parse_http() 2NDNOSPACE error\n");
        free(req);
        return NULL;
    }
    *p = '\0';
    /* strip leading slash if present */
    if (str[0] == '/') {
        strncpy(req->url, str + 1, sizeof(req->url) - 1);
    } else {
        strncpy(req->url, str, sizeof(req->url) - 1);
    }

    return req;
}

/* read from client socket into a static buffer.
   Returns pointer to static buffer on success (>0 bytes).
   Returns NULL on EOF, timeout, or error.
*/
char *cli_read(int c) {
    static char buf[REQ_BUFSIZE];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(c, buf, sizeof(buf) - 1);
    if (n == 0) {
        /* client closed connection */
        return NULL;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* timeout */
            return NULL;
        } else {
            /* real error */
            perror("read");
            return NULL;
        }
    }
    /* success */
    /* buf is null-terminated already due to memset */
    return buf;
}

/* build and send response; keep_alive controls Connection header */
void http_send_response(int c, int code, const char *status,
                        const char *content_type, const char *body,
                        int keep_alive) {
    printf("keep-alive = %d\n", keep_alive);
    char header[1024];
    size_t body_len = body ? strlen(body) : 0;
    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "\r\n",
        code, status, content_type ? content_type : "text/plain",
        body_len,
        keep_alive ? "keep-alive" : "close");

    /* send header then body */
    if (len > 0) send(c, header, (size_t)len, 0);
    if (body_len > 0) send(c, body, body_len, 0);
}

/* handle one client connection with simple keep-alive handling */
void cli_conn(int c) {
    char *p;
    httpreq *req;
    int keep_alive = 1;
    struct timeval timeout;

    /* set a reasonable idle timeout for keep-alive connections */
    timeout.tv_sec = 5;   /* change to 75 for nginx-like behavior */
    timeout.tv_usec = 0;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    while (keep_alive) {
        printf("Inside keep-alive while loop!\n");
        p = cli_read(c);
        if (!p) {
            /* timeout or client closed connection */
            printf("timeout or client closed connection\n");
            break;
        }

        /* parse request-line only (we assume the request-line is at the start of buffer) */
        req = parse_http(p, (int)strlen(p));
        if (!req) {
            fprintf(stderr, "%s\n", error[0] ? error : "parse_http failed\n");
            break;
        }

        /* determine if client wants the connection closed explicitly */
        if (strcasestr(p, "Connection: close") != NULL) {
            keep_alive = 0;
        }

        if (strcasecmp(req->method, "GET") == 0) {
            if (strcmp(req->url, "index.html") == 0 || strcmp(req->url, "") == 0) {
                /* treat "/" (empty after stripping '/') as index too */
                http_send_response(c, 200, "OK", "text/html",
                    "<html><h4>Hello World!!</h4></html>", keep_alive);
            } else if (strcmp(req->url, "data.json") == 0) {
                http_send_response(c, 200, "OK", "application/json",
                    "{\"message\": \"Hello World!!!\"}", keep_alive);
            } else {
                http_send_response(c, 404, "Not Found", "text/plain",
                    "File not found!", keep_alive);
            }
        } else {
            http_send_response(c, 405, "Method Not Allowed", "text/plain",
                "Only GET supported", keep_alive);
        }

        printf("'%s'\t'%s'\n", req->method, req->url);

        free(req);

        /* If Connection: close requested, break and close socket.
           Otherwise continue and wait for next request on same socket
           (subject to the SO_RCVTIMEO timeout above). */
    }

    close(c);
    return;
}

int main(int argc, char *argv[]) {
    int s, c;
    char *port;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <listening port>\n", argv[0]);
        return -1;
    } else {
        port = argv[1];
    }

    /* reap children automatically */
    signal(SIGCHLD, SIG_IGN);

    s = srv_init(atoi(port));
    if (s < 0) {
        fprintf(stderr, "%s\n", error);
        return -1;
    }

    printf("Listening on %s:%s\n", LISTENADDR, port);
    while (1) {
        c = cli_accept(s);
        if (c < 0) {
            fprintf(stderr, "%s\n", error);
            continue;
        }
        printf("Incoming connection of client FD#%d!\n", c);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(c);
            continue;
        } else if (pid == 0) {
            /* child */
            close(s); /* child doesn't need listening socket */
            cli_conn(c);
            exit(0);
        } else {
            /* parent */
            close(c); /* parent doesn't need this client socket */
        }
    }

    /* never reached */
    close(s);
    return 0;
}
