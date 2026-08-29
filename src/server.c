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

#define HTTP_PORT 8000
#define UDP_PORT 8001
#define MAX_CLIENTS 128
#define BUFFER_SIZE 8192

static int sse_clients[MAX_CLIENTS];
static int num_clients = 0;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static char dist_dir[512];

static int udp_sock = -1;

static void *event_loop(void *arg)
{
    (void)arg;
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0)
        return NULL;

    int opt = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(UDP_PORT);

    if (bind(udp_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("Vortex Server: UDP bind failed");
        return NULL;
    }

    printf("UDP Telemetry listening on port %d\n", UDP_PORT);

    char buffer[4096];
    char sse_msg[BUFFER_SIZE];

    while (1)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(udp_sock, &readfds);
        int max_fd = udp_sock;

        if (select(max_fd + 1, &readfds, NULL, NULL, NULL) < 0)
        {
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
                    ssize_t sent = send(fd, sse_msg, msg_len, MSG_NOSIGNAL);
                    if (sent < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            continue;
                        }
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
        if (send(fd, sse_msg, len, MSG_NOSIGNAL) < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
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
    if (strstr(path, ".html"))
        return "text/html";
    if (strstr(path, ".css"))
        return "text/css";
    if (strstr(path, ".js"))
        return "application/javascript";
    if (strstr(path, ".json"))
        return "application/json";
    if (strstr(path, ".ico"))
        return "image/x-icon";
    if (strstr(path, ".svg"))
        return "image/svg+xml";
    return "text/plain";
}

static void serve_file(int client_fd, const char *req_path)
{
    char filepath[2048];

    if (strstr(req_path, ".."))
    {
        const char *forbidden = "HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\n\r\nForbidden";
        send(client_fd, forbidden, strlen(forbidden), 0);
        close(client_fd);
        return;
    }

    if (strcmp(req_path, "/") == 0)
    {
        req_path = "/index.html";
    }

    if (strncmp(req_path, "/vortex_report", 14) == 0 && strstr(req_path, ".json"))
    {
        char clean_path[256];
        strncpy(clean_path, req_path + 1, sizeof(clean_path) - 1);
        clean_path[sizeof(clean_path) - 1] = '\0';
        char *q = strchr(clean_path, '?');
        if (q)
            *q = '\0';
        snprintf(filepath, sizeof(filepath), "./%s", clean_path);
    }
    else
    {
        snprintf(filepath, sizeof(filepath), "%s%s", dist_dir, req_path);
        char *q = strchr(filepath, '?');
        if (q)
            *q = '\0';
    }

    int file_fd = open(filepath, O_RDONLY);
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

    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
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
        fprintf(stderr, "Usage: %s <dist_dir>\n", argv[0]);
        return 1;
    }

    strncpy(dist_dir, argv[1], sizeof(dist_dir) - 1);
    dist_dir[sizeof(dist_dir) - 1] = '\0';

    pthread_t udp_tid;
    pthread_create(&udp_tid, NULL, event_loop, NULL);
    pthread_detach(udp_tid);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("Socket failed");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTP_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("Bind failed");
        return 1;
    }

    if (listen(server_fd, 10) < 0)
    {
        perror("Listen failed");
        return 1;
    }

    printf("Vortex C-Server running at http://localhost:%d\n", HTTP_PORT);

    while (1)
    {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0)
        {
            pthread_t tid;
            pthread_create(&tid, NULL, handle_client, (void *)(intptr_t)client_fd);
            pthread_detach(tid);
        }
    }

    return 0;
}
