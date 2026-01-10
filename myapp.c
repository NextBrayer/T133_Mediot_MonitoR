#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

#define MAX_EVENTS 4

int main() {
    // 1. Create epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        return 1;
    }

    // 2. Open 4 UARTs
    int uart_fds[4];
    const char *uart_devices[] = {
        "/dev/ttyS1",  // Device 0
        "/dev/ttyS2",  // Device 1
        "/dev/ttyS3",  // Device 2
        "/dev/ttyS4"   // Device 3
    };

    // 3. Add each UART to epoll monitoring
    for (int i = 0; i < 4; i++) {
        uart_fds[i] = open(uart_devices[i], O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (uart_fds[i] < 0) {
            perror("open UART");
            continue;
        }

        struct epoll_event event;
        event.events = EPOLLIN;      // We want to know about incoming data
        event.data.fd = uart_fds[i]; // Store FD so we know which UART later
        
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, uart_fds[i], &event) == -1) {
            perror("epoll_ctl");
        }
    }

    // 4. Main event loop
    struct epoll_event events[MAX_EVENTS];
    uint8_t buffer[512];

    while (1) {
        // Wait for ANY UART to have data (blocks here)
        int num_ready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        
        if (num_ready == -1) {
            perror("epoll_wait");
            break;
        }

        // 5. Process each UART that has data
        for (int i = 0; i < num_ready; i++) {
            int ready_fd = events[i].data.fd;
            
            // Figure out which device (0-3)
            int device_id = -1;
            for (int j = 0; j < 4; j++) {
                if (uart_fds[j] == ready_fd) {
                    device_id = j;
                    break;
                }
            }

            // Read data from this UART
            ssize_t bytes_read = read(ready_fd, buffer, sizeof(buffer));
            if (bytes_read > 0) {
                printf("Device %d sent %zd bytes\n", device_id, bytes_read);
                // Process data here...
            }
        }
    }

    // Cleanup
    close(epoll_fd);
    for (int i = 0; i < 4; i++) {
        close(uart_fds[i]);
    }

    return 0;
}