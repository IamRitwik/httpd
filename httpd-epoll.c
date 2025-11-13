/* httpd-epoll.c - Event-driven HTTP/1.1 server using epoll (Linux) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define LISTENADDR "127.0.0.1"
#define MAX_EVENTS 64
#define BUFFER_SIZE 4096

/* HTTP request structure */
struct sHttpRequest {
    char method[16];
    char url[1024];
};
typedef struct sHttpRequest httpreq;

/* Set a socket to non-blocking mode */
/* This allows the server to handle multiple connections without blocking */
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        return -1;
    }
    return 0;
}

/* Create and configure the server socket */
/* Returns -1 on error, else a socket fd */
int srv_init(int portno) {
    int s;
    struct sockaddr_in srv;
    int opt = 1;

    /* Create TCP socket */
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return -1;
    }

    /* Allow address reuse to avoid "address already in use" errors */
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt");
        close(s);
        return -1;
    }

    /* Configure server address */
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    if (inet_pton(AF_INET, LISTENADDR, &srv.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed for %s\n", LISTENADDR);
        close(s);
        return -1;
    }
    srv.sin_port = htons(portno);

    /* Bind socket to address */
    if (bind(s, (struct sockaddr *)&srv, sizeof(srv)) == -1) {
        perror("bind");
        close(s);
        return -1;
    }

    /* Start listening for connections */
    if (listen(s, 128) == -1) {
        perror("listen");
        close(s);
        return -1;
    }

    /* Make the server socket non-blocking */
    if (set_nonblocking(s) == -1) {
        close(s);
        return -1;
    }

    return s;
}

/* Parse HTTP request line to extract method and URL */
httpreq *parse_http(char *str) {
    httpreq *req;
    char *p;
    
    req = malloc(sizeof(httpreq));
    if (!req) return NULL;
    
    memset(req, 0, sizeof(httpreq));
    
    /* Find first space (after method) */
    for (p = str; *p && *p != ' '; p++);
    if (*p != ' ') {
        free(req);
        return NULL;
    }
    *p = 0;
    strncpy(req->method, str, sizeof(req->method) - 1);
    
    /* Find second space (after URL) */
    str = ++p;
    for (; *p && *p != ' ' && *p != '\r' && *p != '\n'; p++);
    *p = 0;
    strncpy(req->url, str, sizeof(req->url) - 1);
    
    return req;
}

/* Send HTTP response to client */
void http_send_response(int fd, int code, const char *status, 
                       const char *contenttype, const char *body) {
    char buf[BUFFER_SIZE];
    
    if (!body) body = "";
    if (!contenttype) contenttype = "text/plain";
    int body_len = strlen(body);

    int n = snprintf(buf, sizeof(buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        code, status, contenttype, body_len, body
    );

    write(fd, buf, n);
}

/* Handle HTTP request and send appropriate response */
void handle_request(int fd, char *request) {
    httpreq *req;
    
    req = parse_http(request);
    if (!req) {
        http_send_response(fd, 400, "Bad Request", "text/plain", 
                          "Malformed request");
        return;
    }
    
    printf("'%s'\t'%s'\n", req->method, req->url);
    
    /* Handle GET requests */
    if (strcmp(req->method, "GET") == 0) {
        if (strcmp(req->url, "/index.html") == 0 || strcmp(req->url, "/") == 0) {
            http_send_response(fd, 200, "OK", "text/html",
                "<html><h4>Hello World!!</h4></html>");
        } 
        else if (strcmp(req->url, "/data.json") == 0) {
            http_send_response(fd, 200, "OK", "application/json",
                "{\"message\": \"Hello World!!!\"}");
        } 
        else {
            http_send_response(fd, 404, "Not Found", "text/plain", 
                              "File not found!");
        }
    } else {
        http_send_response(fd, 405, "Method Not Allowed", "text/plain", 
                          "Only GET supported");
    }
    
    free(req);
}

/* Accept new client connections */
/* In non-blocking mode, we accept all pending connections */
void accept_connections(int server_fd, int epoll_fd) {
    struct sockaddr_in cli;
    socklen_t addrlen;
    int client_fd;
    struct epoll_event event;
    
    while (1) {
        addrlen = sizeof(cli);
        memset(&cli, 0, sizeof(cli));
        
        /* Accept a new connection */
        client_fd = accept(server_fd, (struct sockaddr *)&cli, &addrlen);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* No more pending connections */
                break;
            }
            perror("accept");
            break;
        }
        
        printf("New connection: fd=%d\n", client_fd);
        
        /* Make client socket non-blocking */
        if (set_nonblocking(client_fd) == -1) {
            close(client_fd);
            continue;
        }
        
        /* Register client socket with epoll for read events */
        /* EPOLLIN: monitor for read availability */
        /* EPOLLET: use edge-triggered mode (more efficient, like nginx) */
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = client_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
            perror("epoll_ctl: add client");
            close(client_fd);
        }
    }
}

/* Handle data from client */
void handle_client_data(int client_fd, int epoll_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    /* Read data from client */
    bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    
    if (bytes_read <= 0) {
        /* Connection closed or error */
        if (bytes_read == 0) {
            printf("Connection closed: fd=%d\n", client_fd);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("read");
        }
        /* Remove from epoll and close */
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
        close(client_fd);
        return;
    }
    
    buffer[bytes_read] = '\0';
    
    /* Process HTTP request and send response */
    handle_request(client_fd, buffer);
    
    /* Remove from epoll and close connection after response */
    /* (HTTP/1.1 with Connection: close) */
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
    close(client_fd);
}

int main(int argc, char *argv[]) {
    int server_fd, epoll_fd;
    int port;
    struct epoll_event event, events[MAX_EVENTS];
    int nevents, i;
    
    /* Parse command line arguments */
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }
    port = atoi(argv[1]);
    
    /* Initialize server socket */
    server_fd = srv_init(port);
    if (server_fd < 0) {
        fprintf(stderr, "Failed to initialize server\n");
        return 1;
    }
    
    printf("Server listening on %s:%d\n", LISTENADDR, port);
    printf("Using epoll for event-driven I/O (like Node.js and nginx)\n");
    
    /* Create epoll instance */
    /* epoll is the Linux kernel's scalable I/O event notification mechanism */
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        close(server_fd);
        return 1;
    }
    
    /* Register server socket with epoll to monitor for new connections */
    /* EPOLLIN: monitor for read readiness (new connections) */
    /* EPOLLET: edge-triggered mode (more efficient, requires non-blocking sockets) */
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("epoll_ctl: add server");
        close(server_fd);
        close(epoll_fd);
        return 1;
    }
    
    /* Main event loop - similar to Node.js/nginx event loop */
    /* This is a single-threaded, non-blocking, event-driven architecture */
    printf("Event loop started. Waiting for connections...\n");
    while (1) {
        /* Wait for events (blocks until events are available) */
        /* Returns the number of file descriptors ready for I/O */
        nevents = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nevents == -1) {
            perror("epoll_wait");
            break;
        }
        
        /* Process all ready events */
        for (i = 0; i < nevents; i++) {
            int fd = events[i].data.fd;
            
            /* Check for errors or hangup */
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                fprintf(stderr, "epoll error on fd %d\n", fd);
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                continue;
            }
            
            /* Check if it's the server socket (new connection) */
            if (fd == server_fd) {
                accept_connections(server_fd, epoll_fd);
            } 
            /* Otherwise it's a client socket (data to read) */
            else if (events[i].events & EPOLLIN) {
                handle_client_data(fd, epoll_fd);
            }
        }
    }
    
    /* Cleanup */
    close(server_fd);
    close(epoll_fd);
    return 0;
}
