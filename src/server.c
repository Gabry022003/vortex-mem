/*
 * Standalone C Web Server
 * Handles HTTP connections to serve the React dashboard, listens for UDP telemetry,
 * and routes real-time data to connected browsers via Server-Sent Events (SSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <errno.h>
#include <ctype.h>

#define DEFAULT_HTTP_PORT 8000
#define DEFAULT_UDP_PORT 8001
#define MAX_CLIENTS 128
#define BUFFER_SIZE 8192

static int http_port = DEFAULT_HTTP_PORT;
static int udp_port = DEFAULT_UDP_PORT;

static int sse_clients[MAX_CLIENTS];
static int num_clients = 0;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static char dist_dir[512] = "./frontend/dist";

static int udp_sock = -1;

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    char a, b;
    size_t i = 0;
    while (*src && i + 1 < dst_len)
    {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b)))
        {
            if (a >= 'a' && a <= 'f')
                a -= 'a' - 'A';
            if (a >= 'A' && a <= 'F')
                a -= 'A' - 10;
            else if (a >= '0' && a <= '9')
                a -= '0';

            if (b >= 'a' && b <= 'f')
                b -= 'a' - 'A';
            if (b >= 'A' && b <= 'F')
                b -= 'A' - 10;
            else if (b >= '0' && b <= '9')
                b -= '0';

            dst[i++] = (char)(16 * a + b);
            src += 3;
        }
        else if (*src == '+')
        {
            dst[i++] = ' ';
            src++;
        }
        else
        {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 02000000
#endif

static inline bool is_client_alive(int fd)
{
    char tmp[1];
    ssize_t r = recv(fd, tmp, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r == 0)
        return false;
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        return false;
    return true;
}

static bool send_all_nonblock(int fd, const char *buf, size_t len)
{
    size_t total = 0;
    while (total < len)
    {
        ssize_t sent = send(fd, buf + total, len - total, MSG_NOSIGNAL);
        if (sent < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(fd, &wfds);
                struct timeval tv = {0, 10000};
                if (select(fd + 1, NULL, &wfds, NULL, &tv) > 0)
                {
                    continue;
                }
                return false;
            }
            return false;
        }
        total += sent;
    }
    return true;
}

static void *event_loop(void *arg)
{
    (void)arg;
    udp_sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (udp_sock < 0)
        udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0)
        return NULL;

    fcntl(udp_sock, F_SETFD, FD_CLOEXEC);

    int opt = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(udp_port);

    if (bind(udp_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "Vortex Server: UDP bind failed on port %d (already in use?). Set VORTEX_UDP_PORT to customize.\n", udp_port);
        return NULL;
    }

    printf("UDP Telemetry listening on port %d\n", udp_port);

    char buffer[4096];
    char sse_msg[BUFFER_SIZE];

    while (1)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(udp_sock, &readfds);
        int max_fd = udp_sock;
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        int sel_ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (sel_ret < 0)
        {
            continue;
        }

        if (sel_ret == 0)
        {
            const char *ping = ": keepalive\n\n";
            size_t ping_len = strlen(ping);
            pthread_mutex_lock(&clients_lock);
            for (int i = 0; i < num_clients; i++)
            {
                int fd = sse_clients[i];
                if (!is_client_alive(fd) || !send_all_nonblock(fd, ping, ping_len))
                {
                    close(fd);
                    sse_clients[i] = sse_clients[num_clients - 1];
                    num_clients--;
                    i--;
                }
            }
            pthread_mutex_unlock(&clients_lock);
            continue;
        }

        if (FD_ISSET(udp_sock, &readfds))
        {
            ssize_t len = recv(udp_sock, buffer, sizeof(buffer) - 1, 0);
            if (len > 0)
            {
                buffer[len] = '\0';
                snprintf(sse_msg, sizeof(sse_msg), "data: %s\n\n", buffer);
                size_t msg_len = strlen(sse_msg);

                pthread_mutex_lock(&clients_lock);
                for (int i = 0; i < num_clients; i++)
                {
                    int fd = sse_clients[i];
                    if (!is_client_alive(fd) || !send_all_nonblock(fd, sse_msg, msg_len))
                    {
                        close(fd);
                        sse_clients[i] = sse_clients[num_clients - 1];
                        num_clients--;
                        i--;
                    }
                }
                pthread_mutex_unlock(&clients_lock);
            }
        }
    }
    return NULL;
}

static void broadcast_sse(const char *message)
{
    char sse_msg[BUFFER_SIZE];
    snprintf(sse_msg, sizeof(sse_msg), "data: %s\n\n", message);
    size_t len = strlen(sse_msg);

    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < num_clients; i++)
    {
        int fd = sse_clients[i];
        if (!is_client_alive(fd) || !send_all_nonblock(fd, sse_msg, len))
        {
            close(fd);
            sse_clients[i] = sse_clients[num_clients - 1];
            num_clients--;
            i--;
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

static const char *get_mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot)
    {
        if (strcmp(dot, ".html") == 0)
            return "text/html";
        if (strcmp(dot, ".css") == 0)
            return "text/css";
        if (strcmp(dot, ".js") == 0)
            return "application/javascript";
        if (strcmp(dot, ".json") == 0)
            return "application/json";
        if (strcmp(dot, ".ico") == 0)
            return "image/x-icon";
        if (strcmp(dot, ".svg") == 0)
            return "image/svg+xml";
        if (strcmp(dot, ".png") == 0)
            return "image/png";
        if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
            return "image/jpeg";
    }
    return "text/plain";
}

static void serve_file(int client_fd, const char *raw_req_path)
{
    char req_path[1024];
    url_decode(req_path, raw_req_path, sizeof(req_path));

    char filepath[2048];

    if (req_path[0] != '/' || strstr(req_path, "..") || strchr(req_path, '\\'))
    {
        const char *forbidden = "HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\n\r\nForbidden";
        send(client_fd, forbidden, strlen(forbidden), 0);
        close(client_fd);
        return;
    }

    const char *target_path = req_path;
    if (strcmp(target_path, "/") == 0)
    {
        target_path = "/index.html";
    }

    if (strstr(target_path, ".json"))
    {
        char clean_path[256];
        strncpy(clean_path, target_path + 1, sizeof(clean_path) - 1);
        clean_path[sizeof(clean_path) - 1] = '\0';
        char *q = strchr(clean_path, '?');
        if (q)
            *q = '\0';
        snprintf(filepath, sizeof(filepath), "./%s", clean_path);
    }
    else
    {
        snprintf(filepath, sizeof(filepath), "%s%s", dist_dir, target_path);
        char *q = strchr(filepath, '?');
        if (q)
            *q = '\0';
    }

    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0 && strstr(target_path, ".json"))
    {
        snprintf(filepath, sizeof(filepath), "%s%s", dist_dir, target_path);
        char *q = strchr(filepath, '?');
        if (q)
            *q = '\0';
        file_fd = open(filepath, O_RDONLY);
    }
    if (file_fd < 0)
    {
        if (!strstr(req_path, ".json") && !strstr(req_path, ".css") && !strstr(req_path, ".js"))
        {
            snprintf(filepath, sizeof(filepath), "%s/index.html", dist_dir);
            file_fd = open(filepath, O_RDONLY);
        }
    }

    if (file_fd < 0)
    {
        const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
        send(client_fd, not_found, strlen(not_found), 0);
        close(client_fd);
        return;
    }

    struct stat st;
    fstat(file_fd, &st);

    if (S_ISDIR(st.st_mode))
    {
        close(file_fd);
        const char *forbidden = "HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\n\r\nForbidden";
        send(client_fd, forbidden, strlen(forbidden), 0);
        close(client_fd);
        return;
    }

    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Connection: close\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Cache-Control: no-cache\r\n\r\n",
             get_mime_type(filepath), (long)st.st_size);

    send(client_fd, header, strlen(header), 0);

    char buf[BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(file_fd, buf, sizeof(buf))) > 0)
    {
        if (send(client_fd, buf, bytes_read, MSG_NOSIGNAL) < 0)
            break;
    }

    close(file_fd);
    close(client_fd);
}

static void *handle_client(void *arg)
{
    int client_fd = (int)(intptr_t)arg;
    char buffer[BUFFER_SIZE];

    ssize_t bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0)
    {
        close(client_fd);
        return NULL;
    }
    buffer[bytes] = '\0';

    char method[16], path[1024], protocol[16];
    if (sscanf(buffer, "%15s %1023s %15s", method, path, protocol) != 3)
    {
        close(client_fd);
        return NULL;
    }

    if (strcmp(method, "GET") != 0)
    {
        close(client_fd);
        return NULL;
    }

    if (strcmp(path, "/live") == 0)
    {
        const char *sse_header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n";
        send(client_fd, sse_header, strlen(sse_header), 0);

        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        pthread_mutex_lock(&clients_lock);
        if (num_clients < MAX_CLIENTS)
        {
            sse_clients[num_clients++] = client_fd;
        }
        else
        {
            close(client_fd);
        }
        pthread_mutex_unlock(&clients_lock);
    }
    else if (strcmp(path, "/resolve") == 0)
    {
        broadcast_sse("{\"action\": \"reload\"}");

        const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
        send(client_fd, resp, strlen(resp), 0);
        close(client_fd);
    }
    else
    {
        serve_file(client_fd, path);
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <dist_dir> [--port <http_port>] [--udp-port <udp_port>]\n", argv[0]);
        return 1;
    }

    const char *http_env = getenv("VORTEX_HTTP_PORT");
    if (http_env && *http_env)
        http_port = atoi(http_env);
    const char *udp_env = getenv("VORTEX_UDP_PORT");
    if (udp_env && *udp_env)
        udp_port = atoi(udp_env);

    bool bind_all = false;
    const char *bind_env = getenv("VORTEX_BIND_ALL");
    if (bind_env && atoi(bind_env) != 0)
        bind_all = true;

    for (int i = 1; i < argc; i++)
    {
        if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc)
        {
            http_port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--udp-port") == 0 && i + 1 < argc)
        {
            udp_port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--bind-all") == 0)
        {
            bind_all = true;
        }
        else if (argv[i][0] != '-')
        {
            strncpy(dist_dir, argv[i], sizeof(dist_dir) - 1);
            dist_dir[sizeof(dist_dir) - 1] = '\0';
        }
    }

    if (http_port <= 0 || http_port > 65535)
        http_port = DEFAULT_HTTP_PORT;
    if (udp_port <= 0 || udp_port > 65535)
        udp_port = DEFAULT_UDP_PORT;

    pthread_t udp_tid;
    pthread_create(&udp_tid, NULL, event_loop, NULL);
    pthread_detach(udp_tid);

    int server_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd < 0)
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("Socket failed");
        return 1;
    }
    fcntl(server_fd, F_SETFD, FD_CLOEXEC);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = bind_all ? INADDR_ANY : inet_addr("127.0.0.1");
    addr.sin_port = htons(http_port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "Vortex Server Error: Could not bind to HTTP port %d (already in use?). Set VORTEX_HTTP_PORT to customize.\n", http_port);
        return 1;
    }

    if (listen(server_fd, 10) < 0)
    {
        perror("Listen failed");
        return 1;
    }

    printf("Vortex C-Server running at http://%s:%d\n", bind_all ? "0.0.0.0" : "localhost", http_port);

    while (1)
    {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0)
        {
            fcntl(client_fd, F_SETFD, FD_CLOEXEC);
            pthread_t tid;
            if (pthread_create(&tid, NULL, handle_client, (void *)(intptr_t)client_fd) != 0)
            {
                close(client_fd);
            }
            else
            {
                pthread_detach(tid);
            }
        }
    }

    return 0;
}
