#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <fcntl.h>

#define TFTP_PORT 6969  // Default port for the server
#define BUFFER_SIZE 516  // TFTP data packets are 512 bytes plus 4 bytes for header
#define DATA_SIZE 512
#define RRQ 1
#define DATA 3
#define ACK 4
#define ERROR 5
#define TIMEOUT 1  // 1 second timeout
#define MAX_TIMEOUTS 10

// Error messages
void send_error(int sockfd, struct sockaddr_in *client_addr, socklen_t client_len, int error_code, const char *error_msg) {
    unsigned char error_packet[BUFFER_SIZE];
    error_packet[0] = 0;  // Opcode for ERROR
    error_packet[1] = ERROR;
    error_packet[2] = 0;  // Error code
    error_packet[3] = error_code;
    strcpy((char *)&error_packet[4], error_msg);

    sendto(sockfd, error_packet, 4 + strlen(error_msg) + 1, 0, (struct sockaddr *)client_addr, client_len);
}

// Handle read request (RRQ)
void handle_rrq(int sockfd, struct sockaddr_in *client_addr, socklen_t client_len, char *filename) {
    unsigned char buffer[BUFFER_SIZE];
    int file_fd = open(filename, O_RDONLY);
    if (file_fd < 0) {
        send_error(sockfd, client_addr, client_len, 1, "File not found");
        return;
    }

    int block = 1;
    ssize_t bytes_read;
    unsigned char ack[4] = {0, ACK, 0, 0};

    while ((bytes_read = read(file_fd, buffer + 4, DATA_SIZE)) >= 0) {
        buffer[0] = 0;
        buffer[1] = DATA;
        buffer[2] = block >> 8;
        buffer[3] = block & 0xFF;

        ssize_t bytes_sent = sendto(sockfd, buffer, bytes_read + 4, 0, (struct sockaddr *)client_addr, client_len);
        if (bytes_sent < 0) {
            perror("sendto failed");
            break;
        }

        // Wait for ACK
        struct timeval timeout;
        timeout.tv_sec = TIMEOUT;
        timeout.tv_usec = 0;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);

        int retries = 0;
        while (retries < MAX_TIMEOUTS) {
            int select_result = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
            if (select_result > 0) {
                // Receive ACK
                ssize_t ack_size = recvfrom(sockfd, ack, sizeof(ack), 0, (struct sockaddr *)client_addr, &client_len);
                if (ack_size == 4 && ack[1] == ACK && ((ack[2] << 8) | ack[3]) == block) {
                    break;
                }
            } else if (select_result == 0) {
                // Timeout
                retries++;
                sendto(sockfd, buffer, bytes_read + 4, 0, (struct sockaddr *)client_addr, client_len);
            } else {
                perror("select error");
                break;
            }
        }

        if (retries == MAX_TIMEOUTS) {
            printf("Transfer failed due to timeouts\n");
            break;
        }

        if (bytes_read < DATA_SIZE) {
            // End of file transfer
            break;
        }

        block++;
    }

    close(file_fd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket failed");
        exit(1);
    }

    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(1);
    }

    while (1) {
        unsigned char buffer[BUFFER_SIZE];
        ssize_t bytes_received = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&client_addr, &client_len);
        if (bytes_received < 0) {
            perror("recvfrom failed");
            continue;
        }

        if (buffer[1] == RRQ) {
            char *filename = (char *)&buffer[2];
            if (fork() == 0) {
                handle_rrq(sockfd, &client_addr, client_len, filename);
                exit(0);
            }
        }
    }

    close(sockfd);
    return 0;
}
