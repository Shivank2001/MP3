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
#include<sys/wait.h>

#define MAX_FILENAME_LEN 128 //max filename = 128 bytes
#define MAX_MODE_LEN 128 //max length of mode = 128 bytes
#define DATA_SIZE 512 //max data size = 512 bytes
#define TIMEOUT 10 // Timeout in seconds

char bufferi[512];
char buffero[516]; // 4 bytes for opcode and block number + 512 bytes for data
void error_sys(const char *message);

struct ReadWrite_packet {   // Structure to store file read or written
    uint16_t opcode;
    char Filename[MAX_FILENAME_LEN];
    char Mode[MAX_MODE_LEN];
};

struct Data_packet {     //structure to store data packet
    uint16_t opcode;
    uint16_t block_no;
    char Data[DATA_SIZE];
};

struct ACK_packet {    //structure to store message acknowledgement
    uint16_t opcode;
    uint16_t block_no;
};

struct ERROR_packet {    //structure to store error
    uint16_t opcode;
    uint16_t Errorcode;
    char Error_msg[512];
};

// RRQ function
void RRQ(int socket1, struct sockaddr_storage *client_addr, socklen_t client_len, const char *buffer) {
    struct ReadWrite_packet msg;
    msg.opcode = ntohs(*(uint16_t *)buffer);  // converts opcode from client byte order to host byte order

    if (msg.opcode == 1) {  // RRQ Opcode
        const char *filename_start = buffer + 2; //offset of 2 to store filename
        const char *mode_start = filename_start + strlen(filename_start) + 1; //offset of 1 byte to store null

        strncpy(msg.Filename, filename_start, MAX_FILENAME_LEN - 1);
        msg.Filename[MAX_FILENAME_LEN - 1] = '\0'; //storing null byte before mode
        strncpy(msg.Mode, mode_start, MAX_MODE_LEN - 1);
        msg.Mode[MAX_MODE_LEN - 1] = '\0'; //storing null byte after mode
        printf("Received RRQ for filename: %s, mode: %s\n", msg.Filename, msg.Mode);

        FILE *file_fd; //to access fopen
        if (strcmp(msg.Mode, "octet") == 0) {
            // Binary mode
            file_fd = fopen(msg.Filename, "rb");
        } else if (strcmp(msg.Mode, "netascii") == 0) {
            // Netascii mode
            file_fd = fopen(msg.Filename, "r");
        } else {
            // Unsupported mode
            struct ERROR_packet error_msg;
            error_msg.opcode = htons(5);  // ERROR Opcode
            error_msg.Errorcode = htons(4);  // Illegal TFTP operation
            strncpy(error_msg.Error_msg, "Unsupported mode", sizeof(error_msg.Error_msg));
            sendto(socket1, &error_msg, sizeof(error_msg), 0, (struct sockaddr *)client_addr, client_len);
            return;
        }

        if (!file_fd) {
            struct ERROR_packet error_msg;
            error_msg.opcode = htons(5);  // ERROR Opcode
            error_msg.Errorcode = htons(1);  // File not found
            strncpy(error_msg.Error_msg, "File not found", sizeof(error_msg.Error_msg));
            sendto(socket1, &error_msg, sizeof(error_msg), 0, (struct sockaddr *)client_addr, client_len);
            error_sys("File not found");
            return;
        }

        uint16_t block_no = 1;
        ssize_t bytesread;
        fd_set readfds; // monitor multiple conncetions and descriptors using select
        struct timeval timeout; 

        int c;
        int nextchar = -1;  // Helper variable for CR-LF handling
        int count = 0;  // Counts bytes written to buffer
        int consecutive_timeouts = 0;
        const int MAX_TIMEOUTS = 10;

        while (1) {
            // Reading and preparing data packet
            count = 0;  // Reset byte counter for each block
            while (count < DATA_SIZE) {
                if (nextchar >= 0) {
                    // Write the stored nextchar (for newline conversions)
                    buffero[4 + count++] = nextchar; //offset 4 to account for header and increment count 
                    nextchar = -1;
                    continue;
                }

                // Read a character from the file
                c = getc(file_fd);

                if (c == EOF) {
                    if (ferror(file_fd)) {
                        perror("Read error from file");
                    }
                    break;  // End of file or error
                }

                // Handle newline and carriage return for netascii mode
                if (c == '\n') {
                    buffero[4 + count++] = '\r';  // Write CR
                    if (count < DATA_SIZE) {
                        buffero[4 + count++] = '\n';  // Write LF
                    } else {
                        nextchar = '\n';  // Store LF for the next packet
                    }
                } else if (c == '\r') {
                    buffero[4 + count++] = '\r';  // Write CR
                    if (count < DATA_SIZE) {
                        buffero[4 + count++] = '\0';  // Write NULL after CR
                    } else {
                        nextchar = '\0';  // Store NULL for the next packet
                    }
                } else {
                    buffero[4 + count++] = c;  // Regular character
                }
            }

            // Prepare data packet to send
            struct Data_packet data_msg;
            data_msg.opcode = htons(3);  // DATA Opcode
            data_msg.block_no = htons(block_no);
            memcpy(data_msg.Data, buffero + 4, count);

            // Send data packet
            sendto(socket1, &data_msg, count + 4, 0, (struct sockaddr *)client_addr, client_len);

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
                    // Timeout occurred, resend the last data packet
                    consecutive_timeouts++;
                    printf("Timeout occurred, resending block %d (timeout %d/%d)\n", block_no, consecutive_timeouts, MAX_TIMEOUTS);
                    if (consecutive_timeouts >= MAX_TIMEOUTS) {
                        printf("Max timeouts reached. Terminating transfer.\n");
                        fclose(file_fd);
                        return;   
                    } 
                    sendto(socket1, &data_msg, count + 4, 0, (struct sockaddr *)client_addr, client_len);
                } else {
                    // An ACK was received
                    struct ACK_packet ack_msg;
                    socklen_t addr_len = sizeof(*client_addr);
                    recvfrom(socket1, &ack_msg, sizeof(ack_msg), 0, (struct sockaddr *)client_addr, &addr_len);

                    // Check if ACK is for the correct block number
                    ack_msg.block_no = ntohs(ack_msg.block_no);
                    if (ack_msg.opcode == htons(4) && ack_msg.block_no == block_no) {
                        printf("Received ACK for block %d\n", block_no);
                        consecutive_timeouts = 0;
                        block_no = (block_no == 65535) ? 0 : block_no + 1;  //ternary operator, will reset block_no to 0 if exceeding 65535 and increment if not
                        break;  // Exit the resend loop
                    }
                }
            }

            // End of file
            if (count < DATA_SIZE) {
                break;
            }
        }

        fclose(file_fd);  // Close the file after transfer is complete
    }
}

// WRQ function
void WRQ(int sockfd, struct sockaddr *client_addr, socklen_t client_len, const char *buffer) {
    struct ReadWrite_packet msg;
    msg.opcode = ntohs(*(uint16_t*)buffer);

    if (msg.opcode == 2) {  // WRQ Opcode
        const char *filename_start = buffer + 2;
        const char *mode_start = filename_start + strlen(filename_start) + 1;
        strncpy(msg.Filename, filename_start, MAX_FILENAME_LEN - 1);
        msg.Filename[MAX_FILENAME_LEN - 1] = '\0';
        strncpy(msg.Mode, mode_start, MAX_MODE_LEN - 1);
        msg.Mode[MAX_MODE_LEN - 1] = '\0';
        printf("Received WRQ for filename: %s, mode: %s\n", msg.Filename, msg.Mode);

        FILE *fp;
        if (strcmp(msg.Mode, "octet") == 0) {
            fp = fopen(msg.Filename, "wb");
        } else if (strcmp(msg.Mode, "netascii") == 0) {
            fp = fopen(msg.Filename, "w");
        } else {
            struct ERROR_packet error_msg;
            error_msg.opcode = htons(5); //Error opcode
            error_msg.Errorcode = htons(4); //Error code for file not found
            strncpy(error_msg.Error_msg, "Unsupported mode", sizeof(error_msg.Error_msg));
            sendto(sockfd, &error_msg, sizeof(error_msg), 0, client_addr, client_len);
            return;
        }

        if (fp == NULL) {
            struct ERROR_packet err;
            err.opcode = htons(5); 
            err.Errorcode = htons(2); //if file access is not allowed
            strcpy(err.Error_msg, "Access violation");
            sendto(sockfd, &err, sizeof(err), 0, client_addr, client_len);
            return;
        }

        uint16_t block_no = 0;
        int consecutive_timeouts = 0;
        const int MAX_TIMEOUTS = 10;//maximum of 10 timeouts before transmission is cancelled
        fd_set readfds;
        struct timeval timeout;

        struct ACK_packet ack;
        ack.opcode = htons(4);
        ack.block_no = htons(block_no);
        sendto(sockfd, &ack, sizeof(ack), 0, client_addr, client_len);

        while (1) {
            FD_ZERO(&readfds);
            FD_SET(sockfd, &readfds);
            timeout.tv_sec = TIMEOUT;// set timeout value of 10s
            timeout.tv_usec = 0;

            int activity = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
            if (activity < 0) {
                perror("Select error");
                fclose(fp);
                return;
            } else if (activity == 0) {
                consecutive_timeouts++;
                printf("Timeout occurred, resending ACK for block %d (timeout %d/%d)\n", block_no, consecutive_timeouts, MAX_TIMEOUTS);
                if (consecutive_timeouts >= MAX_TIMEOUTS) {
                    printf("Max timeouts reached. Terminating transfer.\n");
                    fclose(fp);
                    return;
                }
                sendto(sockfd, &ack, sizeof(ack), 0, client_addr, client_len);
                continue;
            }

            char buffer[DATA_SIZE + 4];
            int recv_len = recvfrom(sockfd, buffer, DATA_SIZE + 4, 0, client_addr, &client_len);
            if (recv_len < 0) {
                perror("Error receiving data");
                fclose(fp);
                return;
            }

            if (ntohs(*(uint16_t*)buffer) == 3) {  // DATA packet
                uint16_t received_block_no = ntohs(*(uint16_t*)(buffer + 2));
                if (received_block_no == (block_no + 1) % 65536) {  // Implement wraparound
                    block_no = received_block_no;
                    int data_len = recv_len - 4;

                    if (strcmp(msg.Mode, "netascii") == 0) {
                        for (int i = 0; i < data_len; i++) {
                            char c = buffer[i + 4];
                            if (c == '\r') {
                                if (i + 1 < data_len && buffer[i + 5] == '\n') {
                                    fputc('\n', fp);
                                    i++;
                                } else if (i + 1 < data_len && buffer[i + 5] == '\0') {
                                    fputc('\r', fp);
                                    i++;
                                } else {
                                    fputc('\r', fp);
                                }
                            } else {
                                fputc(c, fp);
                            }
                        }
                    } else {
                        fwrite(buffer + 4, 1, data_len, fp);
                    }

                    ack.block_no = htons(block_no);
                    sendto(sockfd, &ack, sizeof(ack), 0, client_addr, client_len);
                    consecutive_timeouts = 0;

                    if (recv_len < DATA_SIZE + 4) break;  // Last packet
                } else {
                    sendto(sockfd, &ack, sizeof(ack), 0, client_addr, client_len);
                }
            }
        }

        fclose(fp);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <address> <port>\n", argv[0]);
        return 1;
    }

    const char *address = argv[1];
    const char *port = argv[2];
   
    int sockfd;
    struct addrinfo hints, *res, *p;
    int status;
    int optval = 1;

    // Setting hints for IPv4 and IPv6
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;      // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_DGRAM;   // UDP socket
    hints.ai_flags = AI_PASSIVE;      // Use my IP

    if ((status = getaddrinfo(address, port, &hints, &res)) != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        return 1;
    }

    // Try to create socket and bind to an available address
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

        printf("Socket successfully bound to %s : %s\n", address, port);
        break;
    }

    if (p == NULL) {
        fprintf(stderr, "Failed to bind socket\n");
        freeaddrinfo(res);
        return 2;
    }

    freeaddrinfo(res);

    // Main loop to handle incoming requests
    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);

while (1) {
        struct sockaddr_storage client_addr;
        socklen_t client_len = sizeof(client_addr);
        ssize_t recv_len = recvfrom(sockfd, bufferi, sizeof(bufferi), 0, (struct sockaddr*)&client_addr, &client_len);
        
        if (recv_len > 0) {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork failed");
                continue;
            } else if (pid == 0) {
                // Child process
                close(sockfd);  // Close the original socket
                
                int new_sockfd = socket(client_addr.ss_family, SOCK_DGRAM, 0);
                if (new_sockfd < 0) {
                    perror("new socket creation failed");
                    exit(EXIT_FAILURE);
                }

                struct sockaddr_storage new_addr;
                memset(&new_addr, 0, sizeof(new_addr));
                ((struct sockaddr_in*)&new_addr)->sin_family = client_addr.ss_family;
                ((struct sockaddr_in*)&new_addr)->sin_addr.s_addr = INADDR_ANY;
                ((struct sockaddr_in*)&new_addr)->sin_port = 0;  // Let the system assign a port

                if (bind(new_sockfd, (struct sockaddr*)&new_addr, sizeof(new_addr)) < 0) {
                    perror("bind failed");
                    close(new_sockfd);
                    exit(EXIT_FAILURE);
                }

                // Determine if it's a RRQ or WRQ and call appropriate function
                uint16_t opcode = ntohs(*(uint16_t*)bufferi);
                if (opcode == 1) {
                    RRQ(new_sockfd, &client_addr, client_len, bufferi);
                } else if (opcode == 2) {
                    WRQ(new_sockfd, (struct sockaddr *)&client_addr, client_len, bufferi);
                } else {
                    // Send error message for unknown opcode
                }

                close(new_sockfd);
                exit(EXIT_SUCCESS);
            } else {
                // Parent process
                waitpid(-1, NULL, WNOHANG);
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