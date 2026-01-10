#define _GNU_SOURCE
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdint.h>
#include <pty.h>

#define MAX_EVENTS 10
#define MAX_CLIENTS 10
#define BUFFER_SIZE 512
#define NETWORK_PORT 5000

typedef struct
{
    int fd;
    int active;
} client_t;

client_t clients[MAX_CLIENTS];
int num_clients = 0;
int pty_master = -1;
int pty_slave = -1;
char pty_name[256];

int setup_network_server(int port)
{
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket failed");
        return -1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("setsockopt");
        close(server_fd);
        return -1;
    }

    // Make server socket non-blocking
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind failed");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 5) < 0)
    {
        perror("listen");
        close(server_fd);
        return -1;
    }

    printf("Network server listening on port %d\n", port);
    return server_fd;
}

void add_client(int epoll_fd, int client_fd)
{
    // Make client socket non-blocking
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    // Add to clients array
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].active)
        {
            clients[i].fd = client_fd;
            clients[i].active = 1;
            num_clients++;
            printf("Client connected (slot %d), total clients: %d\n", i, num_clients);

            // Add client to epoll for monitoring disconnects
            struct epoll_event event;
            event.events = EPOLLIN | EPOLLRDHUP;
            event.data.fd = client_fd;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event);
            return;
        }
    }

    // No space for new client
    printf("Max clients reached, rejecting connection\n");
    close(client_fd);
}

void remove_client(int epoll_fd, int client_fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active && clients[i].fd == client_fd)
        {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
            close(client_fd);
            clients[i].active = 0;
            num_clients--;
            printf("Client disconnected (slot %d), total clients: %d\n", i, num_clients);
            return;
        }
    }
}

void broadcast_to_clients(uint8_t *data, ssize_t len)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active)
        {
            ssize_t sent = send(clients[i].fd, data, len, MSG_NOSIGNAL);
            if (sent < 0)
            {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    printf("Send failed to client %d, will be removed\n", i);
                    clients[i].active = 0; // Mark for removal
                }
            }
        }
    }
}

int main()
{
    // Initialize clients array
    memset(clients, 0, sizeof(clients));

    // 1. Create epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
    {
        perror("epoll_create1");
        return 1;
    }

    // 2. Open 1 UART for testing
    int uart_fd;
    const char *uart_device = "/dev/ttyS1"; // Test with first UART

    uart_fd = open(uart_device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (uart_fd < 0)
    {
        perror("open UART");
        return 1;
    }
    printf("Opened UART: %s\n", uart_device);

    // 3. Create PTY for Qt app
    if (openpty(&pty_master, &pty_slave, pty_name, NULL, NULL) < 0)
    {
        perror("openpty");
        close(uart_fd);
        return 1;
    }

    // Make PTY master non-blocking
    int flags = fcntl(pty_master, F_GETFL, 0);
    fcntl(pty_master, F_SETFL, flags | O_NONBLOCK);

    printf("Created PTY: %s\n", pty_name);

    // Create symbolic link /dev/ttyAS1 -> PTY
    const char *symlink_path = "/dev/ttyAS1";

    // Remove old symlink if exists
    unlink(symlink_path);

    // Create new symlink
    if (symlink(pty_name, symlink_path) == 0)
    {
        printf("Created symlink: %s -> %s\n", symlink_path, pty_name);
        printf("*** Point your Qt app to: %s ***\n", symlink_path);
    }
    else
    {
        perror("Failed to create symlink (run as root?)");
        printf("*** Point your Qt app to: %s ***\n", pty_name);
    }

    // 4. Add UART to epoll monitoring
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = uart_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, uart_fd, &event) == -1)
    {
        perror("epoll_ctl UART");
        return 1;
    }

    // 5. Add PTY master to epoll (to receive data from Qt app)
    event.events = EPOLLIN;
    event.data.fd = pty_master;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pty_master, &event) == -1)
    {
        perror("epoll_ctl PTY");
        return 1;
    }

    // 6. Setup network server
    int server_fd = setup_network_server(NETWORK_PORT);
    if (server_fd < 0)
    {
        return 1;
    }

    // 7. Add server socket to epoll
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1)
    {
        perror("epoll_ctl server");
        return 1;
    }

    // 8. Main event loop
    struct epoll_event events[MAX_EVENTS];
    uint8_t buffer[BUFFER_SIZE];

    printf("\n=== Configuration ===\n");
    printf("UART: %s\n", uart_device);
    printf("PTY for Qt app: %s\n", pty_name);
    printf("Network port: %d\n", NETWORK_PORT);
    printf("\nStarting event loop...\n");
    printf("Connect with: nc localhost %d\n\n", NETWORK_PORT);

    while (1)
    {
        // Wait for events on UART, PTY, server socket, or client sockets
        int num_ready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        if (num_ready == -1)
        {
            perror("epoll_wait");
            break;
        }

        // Process each ready file descriptor
        for (int i = 0; i < num_ready; i++)
        {
            int ready_fd = events[i].data.fd;

            // Check if it's the server socket (new connection)
            if (ready_fd == server_fd)
            {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

                if (client_fd >= 0)
                {
                    add_client(epoll_fd, client_fd);
                }
            }
            // Check if it's the UART (data from physical device)
            else if (ready_fd == uart_fd)
            {
                ssize_t bytes_read = read(ready_fd, buffer, sizeof(buffer));
                if (bytes_read > 0)
                {
                    printf("UART → received %zd bytes\n", bytes_read);

                    // Send to PTY (Qt app will receive this)
                    ssize_t written = write(pty_master, buffer, bytes_read);
                    if (written > 0)
                    {
                        printf("  → PTY (Qt app): %zd bytes\n", written);
                    }

                    // Broadcast to all network clients
                    broadcast_to_clients(buffer, bytes_read);
                    printf("  → Network clients: broadcasted\n");
                }
            }
            // Check if it's the PTY (data from Qt app)
            else if (ready_fd == pty_master)
            {
                ssize_t bytes_read = read(ready_fd, buffer, sizeof(buffer));
                if (bytes_read > 0)
                {
                    printf("PTY (Qt app) → received %zd bytes\n", bytes_read);

                    // Write back to UART
                    ssize_t written = write(uart_fd, buffer, bytes_read);
                    if (written > 0)
                    {
                        printf("  → UART: %zd bytes\n", written);
                    }
                }
            }
            // Otherwise it's a client socket
            else
            {
                // Check for hangup or error
                if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
                {
                    remove_client(epoll_fd, ready_fd);
                }
                else
                {
                    // Client sent data - write to UART
                    uint8_t client_buffer[256];
                    ssize_t n = read(ready_fd, client_buffer, sizeof(client_buffer));
                    if (n <= 0)
                    {
                        remove_client(epoll_fd, ready_fd);
                    }
                    else
                    {
                        printf("Network client → received %zd bytes\n", n);
                        // Write to UART
                        ssize_t written = write(uart_fd, client_buffer, n);
                        if (written > 0)
                        {
                            printf("  → UART: %zd bytes\n", written);
                        }
                    }
                }
            }
        }
    }

    // Cleanup
    close(epoll_fd);
    close(uart_fd);
    close(pty_master);
    close(pty_slave);
    close(server_fd);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active)
        {
            close(clients[i].fd);
        }
    }

    return 0;
}