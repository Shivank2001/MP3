#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include <dirent.h>

#define MAX_FILENAME_LEN 128 // max filename length
#define MAX_MODE_LEN 128 // max mode length
#define DATA_SIZE 512 // max data size
#define TIMEOUT 10 // Timeout in seconds
#define MAX_TIMEOUTS 10 // Max timeouts before terminating transfer
#define MAX_CLIENTS 10 // Max number of concurrent clients

// Buffer sizes
char bufferi[DATA_SIZE + 4];
char buffero[DATA_SIZE + 4]; // Buffer for outgoing packets

// Function declarations
void error_sys(const char *message);
void RRQ(int socket1, struct sockaddr_storage *client_addr, socklen_t client_len, const char *buffer);
void WRQ(int sockfd, struct sockaddr *client_addr, socklen_t client_len, const char *buffer);
void log_event(const char *event_msg);
void handle_timeout(int block_no, int consecutive_timeouts);
void list_files(int sockfd, struct sockaddr *client_addr, socklen_t client_len);
void handle_client(int client_sockfd, struct sockaddr_storage *client_addr, socklen_t client_len, const char *buffer);
void show_help();

// Structures for packet definitions
struct ReadWrite_packet {
    uint16_t opcode;
    char Filename[MAX_FILENAME_LEN];
    char Mode[MAX_MODE_LEN];
};

struct Data_packet {
    uint16_t opcode;
    uint16_t block_no;
    char Data[DATA_SIZE];
};

struct ACK_packet {
    uint16_t opcode;
    uint16_t block_no;
};

struct ERROR_packet {
    uint16_t opcode;
    uint16_t Errorcode;
    char Error_msg[512];
};

// Log event to a file
void log_event(const char *event_msg) {
    FILE *log_file = fopen("tftp_server.log", "a");
    if (log_file) {
        time_t now = time(NULL);
        fprintf(log_file, "[%s] %s\n", ctime(&now), event_msg);
        fclose(log_file);
    }
}

// Handle timeouts
void handle_timeout(int block_no, int consecutive_timeouts) {
    printf("Timeout occurred, resending block %d (timeout %d/%d)\n", block_no, consecutive_timeouts, MAX_TIMEOUTS);
    log_event("Timeout occurred, resending block.");
}

// List files in the current directory
void list_files(int sockfd, struct sockaddr *client_addr, socklen_t client_len) {
    struct Data_packet data_msg;
    data_msg.opcode = htons(3);
    uint16_t block_no = 1;
    
    DIR *dir = opendir(".");
    if (!dir) {
        perror("Unable to open directory");
        return;
    }

    struct dirent *entry;
    char file_list[DATA_SIZE];
    int total_length = 0;

    // Read directory entries
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) { // Regular file
            int len = snprintf(file_list + total_length, DATA_SIZE - total_length, "%s\n", entry->d_name);
            if (len < 0 || total_length + len >= DATA_SIZE) break; // Avoid overflow
            total_length += len;
        }
    }
    closedir(dir);

    // Send files as data packets
    while (total_length > 0) {
        int chunk_size = total_length > DATA_SIZE ? DATA_SIZE : total_length;
        memcpy(data_msg.Data, file_list, chunk_size);
        data_msg.block_no = htons(block_no);
        
        sendto(sockfd, &data_msg, chunk_size + 4, 0, client_addr, client_len);
        total_length -= chunk_size;
        block_no++;
    }

    log_event("File list sent to client.");
}

// RRQ function
void RRQ(int socket1, struct sockaddr_storage *client_addr, socklen_t client_len, const char *buffer) {
    struct ReadWrite_packet msg;
    msg.opcode = ntohs(*(uint16_t *)buffer);

    if (msg.opcode == 1) {
        const char *filename_start = buffer + 2;
        const char *mode_start = filename_start + strlen(filename_start) + 1;

        strncpy(msg.Filename, filename_start, MAX_FILENAME_LEN - 1);
        msg.Filename[MAX_FILENAME_LEN - 1] = '\0';
        strncpy(msg.Mode, mode_start, MAX_MODE_LEN - 1);
        msg.Mode[MAX_MODE_LEN - 1] = '\0';
        printf("Received RRQ for filename: %s, mode: %s\n", msg.Filename, msg.Mode);
        log_event("Received RRQ.");

        FILE *file_fd = (strcmp(msg.Mode, "octet") == 0) ? fopen(msg.Filename, "rb") : fopen(msg.Filename, "r");

        if (!file_fd) {
            struct ERROR_packet error_msg;
            error_msg.opcode = htons(5);
            error_msg.Errorcode = htons(1);
            strncpy(error_msg.Error_msg, "File not found", sizeof(error_msg.Error_msg));
            sendto(socket1, &error_msg, sizeof(error_msg), 0, (struct sockaddr *)client_addr, client_len);
            log_event("File not found.");
            return;
        }

        uint16_t block_no = 1;
        ssize_t bytesread;
        fd_set readfds;
        struct timeval timeout;
        int consecutive_timeouts = 0;

        while (1) {
            bytesread = fread(buffero + 4, 1, DATA_SIZE, file_fd);
            struct Data_packet data_msg;
            data_msg.opcode = htons(3);
            data_msg.block_no = htons(block_no);
            memcpy(data_msg.Data, buffero + 4, bytesread);

            // Send data packet
            sendto(socket1, &data_msg, bytesread + 4, 0, (struct sockaddr *)client_addr, client_len);

            // Wait for ACK
            while (1) {
                FD_ZERO(&readfds);
                FD_SET(socket1, &readfds);
                timeout.tv_sec = TIMEOUT;
                timeout.tv_usec = 0;

                int activity = select(socket1 + 1, &readfds, NULL, NULL, &timeout);
                if (activity < 0) {
                    perror("Select error");
                    fclose(file_fd);
                    return;
                } else if (activity == 0) {
                    handle_timeout(block_no, ++consecutive_timeouts);
                    if (consecutive_timeouts >= MAX_TIMEOUTS) {
                        printf("Max timeouts reached. Terminating transfer.\n");
                        fclose(file_fd);
                        return;
                    }
                    sendto(socket1, &data_msg, bytesread + 4, 0, (struct sockaddr *)client_addr, client_len);
                } else {
                    struct ACK_packet ack_msg;
                    socklen_t addr_len = sizeof(*client_addr);
                    recvfrom(socket1, &ack_msg, sizeof(ack_msg), 0, (struct sockaddr *)client_addr, &addr_len);
                    ack_msg.block_no = ntohs(ack_msg.block_no);
                    if (ack_msg.opcode == htons(4) && ack_msg.block_no == block_no) {
                        printf("Received ACK for block %d\n", block_no);
                        log_event("Received ACK.");
                        consecutive_timeouts = 0;
                        block_no = (block_no == 65535) ? 0 : block_no + 1;
                        break;
                    }
                }
            }

            // End of file
            if (bytesread < DATA_SIZE) {
                break;
            }
        }

        fclose(file_fd);
    }
}

// WRQ function
void WRQ(int sockfd, struct sockaddr *client_addr, socklen_t client_len, const char *buffer) {
    struct ReadWrite_packet msg;
    msg.opcode = ntohs(*(uint16_t *)buffer);

    if (msg.opcode == 2) {
        const char *filename_start = buffer + 2;
        const char *mode_start = filename_start + strlen(filename_start) + 1;
        strncpy(msg.Filename, filename_start, MAX_FILENAME_LEN - 1);
        msg.Filename[MAX_FILENAME_LEN - 1] = '\0';
        strncpy(msg.Mode, mode_start, MAX_MODE_LEN - 1);
        msg.Mode[MAX_MODE_LEN - 1] = '\0';
        printf("Received WRQ for filename: %s, mode: %s\n", msg.Filename, msg.Mode);
        log_event("Received WRQ.");

        FILE *fp = (strcmp(msg.Mode, "octet") == 0) ? fopen(msg.Filename, "wb") : fopen(msg.Filename, "w");

        if (fp == NULL) {
            struct ERROR_packet err;
            err.opcode = htons(5);
            err.Errorcode = htons(2);
            strncpy(err.Error_msg, "Access violation", sizeof(err.Error_msg));
            sendto(sockfd, &err, sizeof(err), 0, client_addr, client_len);
            log_event("Access violation.");
            return;
        }

        uint16_t block_no = 0;
        fd_set readfds;
        struct timeval timeout;
        int consecutive_timeouts = 0;

        while (1) {
            struct Data_packet data_msg;
            socklen_t addr_len = sizeof(*client_addr);
            int recv_len = recvfrom(sockfd, &data_msg, sizeof(data_msg), 0, (struct sockaddr *)client_addr, &addr_len);
            if (recv_len < 0) {
                perror("Receive error");
                fclose(fp);
                return;
            }

            if (data_msg.opcode == htons(3)) {
                block_no = ntohs(data_msg.block_no);
                fwrite(data_msg.Data, 1, recv_len - 4, fp);
                struct ACK_packet ack;
                ack.opcode = htons(4);
                ack.block_no = data_msg.block_no;

                sendto(sockfd, &ack, sizeof(ack), 0, client_addr, client_len);
                printf("ACK sent for block %d\n", block_no);
                log_event("ACK sent for received block.");
                
                // Check if it's the last packet
                if (recv_len < DATA_SIZE + 4) {
                    break; // Last packet received
                }
            }
        }

        fclose(fp);
    }
}

// Handle client requests
void handle_client(int client_sockfd, struct sockaddr_storage *client_addr, socklen_t client_len, const char *buffer) {
    uint16_t opcode = ntohs(*(uint16_t *)buffer);
    if (opcode == 1) {
        RRQ(client_sockfd, client_addr, client_len, buffer);
    } else if (opcode == 2) {
        WRQ(client_sockfd, (struct sockaddr *)client_addr, client_len, buffer);
    } else if (opcode == 3) {
        list_files(client_sockfd, (struct sockaddr *)client_addr, client_len);
    } else {
        struct ERROR_packet error_msg;
        error_msg.opcode = htons(5);
        error_msg.Errorcode = htons(0);
        strncpy(error_msg.Error_msg, "Unknown TFTP operation", sizeof(error_msg.Error_msg));
        sendto(client_sockfd, &error_msg, sizeof(error_msg), 0, (struct sockaddr *)client_addr, client_len);
        log_event("Unknown TFTP operation.");
    }
}

// Show help for command-line arguments
void show_help() {
    printf("TFTP Server Usage:\n");
    printf("  <address> <port>\n");
    printf("  <address> - IP address to bind the server to\n");
    printf("  <port> - Port number to listen for requests\n");
}

// Main function
int main(int argc, char *argv[]) {
    if (argc != 3) {
        show_help();
        return 1;
    }

    const char *address = argv[1];
    const char *port = argv[2];

    int sockfd;
    struct addrinfo hints, *res, *p;
    int status;
    int optval = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    if ((status = getaddrinfo(address, port, &hints, &res)) != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        return 1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("socket");
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval) == -1) {
            perror("setsockopt");
            close(sockfd);
            continue;
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            perror("bind");
            close(sockfd);
            continue;
        }

        printf("Socket successfully bound to %s:%s\n", address, port);
        log_event("Socket successfully bound.");
        break;
    }

    if (p == NULL) {
        fprintf(stderr, "Failed to bind socket\n");
        freeaddrinfo(res);
        return 2;
    }

    freeaddrinfo(res);

    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (1) {
        ssize_t recv_len = recvfrom(sockfd, bufferi, sizeof(bufferi), 0, (struct sockaddr *)&client_addr, &client_len);
        if (recv_len > 0) {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork failed");
                continue;
            } else if (pid == 0) {
                close(sockfd);
                handle_client(sockfd, &client_addr, client_len, bufferi);
                exit(EXIT_SUCCESS);
            } else {
                waitpid(-1, NULL, WNOHANG); // Cleanup zombie processes
            }
        }
    }

    close(sockfd);
    return 0;
}

void error_sys(const char *message) {
    perror(message);
    exit(EXIT_FAILURE);
}
