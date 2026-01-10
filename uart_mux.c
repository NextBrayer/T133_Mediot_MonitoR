#define _GNU_SOURCE
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pty.h>
#include <termios.h>

#define MAX_EVENTS 10
#define BUFFER_SIZE 512

int pty_master = -1;
int pty_slave = -1;
char pty_name[256];

// Track last data sent from Qt to filter echoes
uint8_t last_sent_from_pty[BUFFER_SIZE];
ssize_t last_sent_size = 0;

int main()
{
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

    // Configure UART settings (disable echo, raw mode)
    struct termios tty;
    if (tcgetattr(uart_fd, &tty) == 0)
    {
        // Disable echo and canonical mode
        tty.c_lflag &= ~(ECHO | ECHOE | ECHONL | ICANON | ISIG | IEXTEN);
        
        // Raw input
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        
        // Raw output
        tty.c_oflag &= ~OPOST;
        
        // Apply settings
        tcsetattr(uart_fd, TCSANOW, &tty);
        printf("UART configured: echo disabled, raw mode\n");
    }
    else
    {
        perror("tcgetattr");
    }

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

    // Main event loop
    struct epoll_event events[MAX_EVENTS];
    uint8_t buffer[BUFFER_SIZE];

    printf("\n=== Configuration ===\n");
    printf("UART: %s\n", uart_device);
    printf("PTY for Qt app: %s\n", pty_name);
    printf("Symlink: %s\n", symlink_path);
    printf("\nStarting event loop...\n\n");

    while (1)
    {
        // Wait for events on UART or PTY
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

            // Check if it's the UART (data from physical device)
            if (ready_fd == uart_fd)
            {
                ssize_t bytes_read = read(ready_fd, buffer, sizeof(buffer));
                if (bytes_read > 0)
                {
                    printf("UART → received %zd bytes\n", bytes_read);

                    // Filter out echo (if it matches what Qt just sent)
                    if (bytes_read == last_sent_size && 
                        memcmp(buffer, last_sent_from_pty, bytes_read) == 0)
                    {
                        printf("  (Ignoring echo)\n");
                        last_sent_size = 0; // Reset
                    }
                    else
                    {
                        // Real data from device - send to PTY (Qt app will receive this)
                        ssize_t written = write(pty_master, buffer, bytes_read);
                        if (written > 0)
                        {
                            printf("  → PTY (Qt app): %zd bytes\n", written);
                        }
                    }
                }
            }
            // Check if it's the PTY (data from Qt app)
            else if (ready_fd == pty_master)
            {
                ssize_t bytes_read = read(ready_fd, buffer, sizeof(buffer));
                if (bytes_read > 0)
                {
                    printf("PTY (Qt app) → received %zd bytes\n", bytes_read);

                    // Save what Qt sent for echo filtering
                    memcpy(last_sent_from_pty, buffer, bytes_read);
                    last_sent_size = bytes_read;

                    // Write to UART
                    ssize_t written = write(uart_fd, buffer, bytes_read);
                    if (written > 0)
                    {
                        printf("  → UART: %zd bytes\n", written);
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

    return 0;
}