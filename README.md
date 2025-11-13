# HTTP/1.1 Server Implementations

This directory contains three different implementations of a simple HTTP/1.1 server in C, each demonstrating different concurrency models.

## Overview of the Three Implementations

### 1. `httpd.c` - Basic Fork-Based Server
- **Architecture**: Process-per-connection (classic Unix model)
- **Concurrency**: Multiple processes using `fork()`
- **I/O Model**: Blocking I/O
- **Scalability**: Limited by process overhead (~1000 connections max)
- **Memory**: High memory usage (each process has its own memory space)
- **Use Case**: Simple applications, learning, low-traffic servers
- **Zombie Handling**: Manual cleanup required (relies on parent reaping children)

### 2. `http-server.c` - Improved Fork-Based Server
- **Architecture**: Process-per-connection with better error handling
- **Concurrency**: Multiple processes using `fork()`
- **I/O Model**: Blocking I/O
- **Scalability**: Limited by process overhead
- **Memory**: High memory usage (each process has its own memory space)
- **Use Case**: More robust version of httpd.c
- **Zombie Handling**: Uses `signal(SIGCHLD, SIG_IGN)` to automatically reap zombies
- **Features**: Better request parsing, proper header reading until `\r\n\r\n`

### 3. `httpd-epoll.c` - Event-Driven Server (Linux Only)
- **Architecture**: Single-threaded event loop
- **Concurrency**: Multiplexed I/O using Linux's `epoll`
- **I/O Model**: Non-blocking I/O with edge-triggered events
- **Scalability**: Can handle thousands of concurrent connections (10k+)
- **Memory**: Low memory footprint (single process)
- **Use Case**: High-performance servers, high concurrency
- **Similar to**: Node.js, nginx, Redis
- **Platform**: Linux only (epoll is Linux-specific)

---

## Compilation and Running

### 1. httpd.c - Basic Fork Server

**Compile:**
```bash
gcc -o httpd httpd.c -Wall
```

**Run:**
```bash
./httpd 8001
```

**Features:**
- Serves hardcoded responses for `/index.html` and `/data.json`
- Each connection spawns a new child process
- Simple but can leave zombie processes if not careful

### 2. http-server.c - Improved Fork Server

**Compile:**
```bash
gcc -o http-server http-server.c -Wall
```

**Run:**
```bash
./http-server 8001
```

**Features:**
- Better error handling and request parsing
- Reads complete HTTP headers (until `\r\n\r\n`)
- Automatically prevents zombie processes with `signal(SIGCHLD, SIG_IGN)`
- Prints raw request for debugging
- Returns a simple "Hello World!" HTML page for all requests

### 3. httpd-epoll.c - Event-Driven Server (Linux Only)

**Compile (Linux only):**
```bash
gcc -o httpd-epoll httpd-epoll.c -Wall
```

**Run:**
```bash
./httpd-epoll 8001
```

**Note:** This will NOT compile on MacOS because `epoll` is Linux-specific. On MacOS, you would need to use `kqueue` instead.

**Features:**
- Single-threaded event loop using epoll
- Non-blocking I/O
- Edge-triggered mode for maximum efficiency
- Can handle thousands of concurrent connections
- Similar architecture to nginx and Node.js

## Testing

### Testing httpd.c and httpd-epoll.c

Both of these servers respond to specific routes:

**Test index.html:**
```bash
curl -v http://localhost:8001/index.html
```

Expected response:
```
< HTTP/1.1 200 OK
< Content-Type: text/html
< Content-Length: 35
<html><h4>Hello World!!</h4></html>
```

**Test data.json:**
```bash
curl -v http://localhost:8001/data.json
```

Expected response:
```
< HTTP/1.1 200 OK
< Content-Type: application/json
< Content-Length: 30
{"message": "Hello World!!!"}
```

**Test 404:**
```bash
curl -v http://localhost:8001/notfound.txt
```

Expected response:
```
< HTTP/1.1 404 Not Found
< Content-Type: text/plain
File not found!
```

**Test POST method (should fail):**
```bash
curl -X POST http://localhost:8001/index.html
```

Expected response:
```
< HTTP/1.1 405 Method Not Allowed
Only GET supported
```

### Testing http-server.c

This server returns the same response for all routes:

**Test any URL:**
```bash
curl -v http://localhost:8001/
curl -v http://localhost:8001/any/path
curl -v http://localhost:8001/test.html
```

All will return:
```
< HTTP/1.1 200 OK
< Content-Type: text/html
<html><body><h1>Hello World!</h1></body></html>
```

### Load Testing (Concurrent Connections)

Test how the server handles multiple concurrent connections:

**Using Apache Bench (ab):**
```bash
ab -n 1000 -c 100 http://localhost:8001/index.html
```
This sends 1000 requests with 100 concurrent connections.

**Using wrk (more modern):**
```bash
wrk -t4 -c100 -d10s http://localhost:8001/index.html
```
This runs a 10-second benchmark with 4 threads and 100 connections.


## Detailed Architecture Comparison

### Fork-Based Architecture (httpd.c & http-server.c)

```
                  ┌─────────────┐
                  │   Parent    │
                  │   Process   │
                  │             │
                  │  accept()   │
                  └──────┬──────┘
                         │
         ┌───────────────┼───────────────┐
         │               │               │
         ▼               ▼               ▼
    ┌─────────┐    ┌─────────┐    ┌─────────┐
    │ Child 1 │    │ Child 2 │    │ Child N │
    │ Process │    │ Process │    │ Process │
    │         │    │         │    │         │
    │ Client1 │    │ Client2 │    │ ClientN │
    └─────────┘    └─────────┘    └─────────┘

- Each connection = new process
- Blocking I/O per process
- High memory overhead
- Simple to understand
```

### Event-Driven Architecture (httpd-epoll.c)

```
┌─────────────────────────────────────────┐
│         Application (main)               │
│  - Creates server socket                 │
│  - Creates epoll instance                │
│  - Runs event loop                       │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│      epoll_wait() [Event Loop]          │
│  Waits for I/O events on registered     │
│  file descriptors                        │
└──────────────┬──────────────────────────┘
               │
               ▼
    ┌──────────┴──────────┐
    │                     │
    ▼                     ▼
┌─────────┐        ┌──────────────┐
│ Server  │        │   Client     │
│ Socket  │        │   Sockets    │
│         │        │   (1000s)    │
│ New     │        │ Data ready   │
│ conn.   │        │ to read      │
└─────────┘        └──────────────┘
    │                     │
    ▼                     ▼
accept()            read() & process
    │                     │
    ▼                     ▼
Add client          Send response
to epoll            & close

- Single process handles all connections
- Non-blocking I/O
- Low memory overhead
- More complex but scales better
```

## Code Flow by Implementation

### httpd.c Flow

1. **Initialization** (`main`):
   - Create and bind server socket
   - Start listening for connections

2. **Main Loop**:
   - Call `accept()` to wait for new connection (blocks here)
   - When client connects, call `fork()`
   
3. **Child Process** (`cli_conn`):
   - Close server socket (child doesn't need it)
   - Read HTTP request from client
   - Parse method and URL
   - Send appropriate response based on URL
   - Close client socket and exit

4. **Parent Process**:
   - Close client socket (parent doesn't handle it)
   - Loop back to accept next connection
   - **Note**: Zombie processes may accumulate if not reaped

### http-server.c Flow

1. **Initialization** (`main`):
   - Set `signal(SIGCHLD, SIG_IGN)` to auto-reap zombies
   - Create and bind server socket
   - Start listening for connections

2. **Main Loop**:
   - Call `accept()` to wait for new connection (blocks here)
   - When client connects, call `fork()`
   
3. **Child Process** (`cli_conn`):
   - Close server socket
   - Read complete HTTP headers using `read_request_headers()` (reads until `\r\n\r\n`)
   - Parse request line with safer `parse_http()` using `sscanf`
   - Print raw request for debugging
   - Send HTTP 200 response with "Hello World!" HTML
   - Close client socket and exit

4. **Parent Process**:
   - Close client socket
   - Loop back to accept next connection
   - Zombies are automatically reaped by the kernel

### httpd-epoll.c Flow

1. **Initialization** (`main`):
   - Create server socket (non-blocking)
   - Create epoll instance with `epoll_create1()`
   - Add server socket to epoll with `EPOLLIN | EPOLLET`

2. **Event Loop** (`main`):
   - Call `epoll_wait()` to wait for events (blocks here)
   - When events occur, iterate through ready file descriptors
   - If server socket → call `accept_connections()`
   - If client socket → call `handle_client_data()`

3. **Accept Connections** (`accept_connections`):
   - Loop accepting all pending connections (until `EAGAIN`)
   - Set each client socket to non-blocking
   - Add each client to epoll with `EPOLLIN | EPOLLET`

4. **Handle Client Data** (`handle_client_data`):
   - Read HTTP request (non-blocking)
   - Parse and process request
   - Send HTTP response
   - Remove from epoll and close connection

## Performance Comparison

Compare the three implementations under load:

```bash
# Test httpd.c (basic fork)
./httpd 8001
ab -n 10000 -c 100 http://localhost:8001/index.html
# Stop with Ctrl+C

# Test http-server.c (improved fork)
./http-server 8001
ab -n 10000 -c 100 http://localhost:8001/
# Stop with Ctrl+C

# Test httpd-epoll.c (event-driven) - LINUX ONLY
./httpd-epoll 8001
ab -n 10000 -c 100 http://localhost:8001/index.html
# Stop with Ctrl+C
```

**Expected Results:**

| Server | Requests/sec | Memory | CPU | Concurrency |
|--------|-------------|---------|-----|-------------|
| httpd.c | ~500-1000 | High | High | Low |
| http-server.c | ~500-1000 | High | High | Low |
| httpd-epoll.c | ~5000-10000+ | Low | Low | High |

The epoll version should:
- Handle 5-10x more requests per second
- Use significantly less memory
- Scale better with high concurrency
- Be more efficient under heavy load

## Key Concepts Explained

### What is epoll?

`epoll` (Edge Poll) is Linux's scalable I/O event notification mechanism. It allows a program to monitor multiple file descriptors to see if I/O is possible on any of them.

**Advantages over traditional approaches:**
- O(1) performance (doesn't scan all file descriptors)
- Edge-triggered mode for efficiency
- Can handle tens of thousands of connections
- Used by nginx, Redis, Node.js (via libuv)

### Blocking vs Non-Blocking I/O

**Blocking I/O (httpd.c, http-server.c):**
- `accept()`, `read()`, `write()` block until complete
- Process/thread does nothing while waiting
- Simple to program
- Requires one process/thread per connection

**Non-Blocking I/O (httpd-epoll.c):**
- Operations return immediately with `EAGAIN` if not ready
- Process can do other work while waiting
- More complex to program
- Single process can handle many connections

### Edge-Triggered vs Level-Triggered

**Level-Triggered (default):**
- Notified as long as condition is true
- Safer, easier to use
- Can be less efficient

**Edge-Triggered (EPOLLET):**
- Notified only when state changes
- More efficient (fewer wakeups)
- Requires careful programming (must read until EAGAIN)
- Used by nginx for maximum performance

### Zombie Processes

**Problem (httpd.c):**
When a child process exits, it becomes a "zombie" until the parent calls `wait()` to reap it. If not reaped, zombies accumulate.

**Solution 1 (http-server.c):**
```c
signal(SIGCHLD, SIG_IGN);  // Kernel automatically reaps children
```

**Solution 2 (httpd-epoll.c):**
No child processes, so no zombies!

## Limitations

### All Implementations

- Only supports GET requests (no POST, PUT, DELETE, etc.)
- No HTTPS/TLS support
- No compression (gzip, brotli)
- Basic HTTP parsing (doesn't handle all edge cases)
- No request body handling
- No HTTP header parsing beyond request line

### httpd.c Specific

- Zombie process accumulation (no zombie reaping)
- Hardcoded responses only
- Limited to ~1000 concurrent connections
- High memory usage under load

### http-server.c Specific

- Returns same response for all URLs
- Limited to ~1000 concurrent connections  
- High memory usage under load
- Better than httpd.c but still process-per-connection

### httpd-epoll.c Specific

- **Linux only** (won't compile on MacOS/BSD)
- Closes connection after each response (no keep-alive)
- Single-threaded (can't utilize multiple CPU cores)
- No timeout handling for idle connections

## Further Enhancements

### For Fork-Based Servers (httpd.c, http-server.c)

- Add worker pool instead of fork-per-connection
- Use threads instead of processes (lower overhead)
- Implement proper zombie reaping with `waitpid()`
- Add signal handling for graceful shutdown
- Serve actual files from disk
- Parse and validate HTTP headers

### For Event-Driven Server (httpd-epoll.c)

- **Persistent connections** (HTTP keep-alive)
- **Multi-threading** (like nginx worker processes)
  - Create worker threads, each with its own epoll instance
  - Distribute connections across workers
  - Utilize multiple CPU cores
- **Timeout handling** for idle connections
  - Add timer events to epoll
  - Close connections that are idle too long
- **Static file serving** from disk
  - Use `sendfile()` for zero-copy transfers
  - Add MIME type detection
- **Buffering** for large requests/responses
  - Handle partial reads/writes
  - Queue data when socket buffer is full
- **HTTP/1.1 pipelining** support
- **Logging** to files with rotation
- **Configuration** file support (like nginx.conf)
- **TLS/SSL** support using OpenSSL
- **Port to kqueue** for MacOS/BSD support

## Quick Reference

### Which Server Should I Use?

**Learning C network programming?**
→ Start with `httpd.c` (simplest)

**Need better zombie handling?**
→ Use `http-server.c`

**Need high performance on Linux?**
→ Use `httpd-epoll.c`

**Building production server?**
→ None of these! Use nginx, Apache, or a proper framework

### Common Commands

```bash
# Compile all
gcc -o httpd httpd.c -Wall
gcc -o http-server http-server.c -Wall
gcc -o httpd-epoll httpd-epoll.c -Wall  # Linux only

# Run any server
./httpd 8001
./http-server 8001
./httpd-epoll 8001

# Test with curl
curl -v http://localhost:8001/index.html

# Load test
ab -n 10000 -c 100 http://localhost:8001/index.html

# Monitor processes (fork-based servers)
watch -n1 'ps aux | grep httpd'

# Check for zombie processes
ps aux | grep 'Z'

# Monitor connections (epoll server)
ss -tn | grep :8001 | wc -l
```

### Key Files Summary

| File | Lines | Complexity | Best For |
|------|-------|------------|----------|
| httpd.c | ~220 | Simple | Learning |
| http-server.c | ~240 | Medium | Understanding better patterns |
| httpd-epoll.c | ~340 | Complex | High performance (Linux) |

---

**Created as a learning resource for C network programming and different concurrency models.**
# httpd
