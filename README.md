# TFTP Server Implementation

WRQ, Fork done by Kyungseok Ryoo

RRQ, Netascii, octet implemntation done by Shivank Vishvanathan

## Overview
This project is a TFTP (Trivial File Transfer Protocol) server implementation with the following features:
1. Handles Read Requests (RRQ) from TFTP clients
2. Handles Write Requests (WRQ) from TFTP clients
3. Supports both 'netascii' and 'octet' transfer modes
4. Handles multiple clients by forking child processes
5. Implements timeout and retransmission mechanisms
6. Supports IPv4 and IPv6 connections

## Architecture
- Server Implementation:
  - Implemented using C
  - Uses UDP sockets to communicate with clients
  - Each client connection is handled separately using fork()
  - Server listens on both IPv4 and IPv6 addresses
  - Implements the TFTP protocol as specified in RFC 1350

## Server Process Flow
```
+------------------------+
|    Socket Creation     |
+------------------------+
            |
+------------------------+
|        Binding         |
+------------------------+
            |
+------------------------+
| Waiting for Requests   |
+------------------------+
            |
+-------------------------------+
|   Forking for Each Request    |
+-------------------------------+
            |
+-------------------------------+
|   Handling TFTP Operations    |
|   (RRQ, WRQ, DATA, ACK)       |
+-------------------------------+
            |
+------------------------+
|    Closing Sockets     |
+------------------------+
```

## Error Handling
- File not found errors are handled and appropriate error messages are sent to clients
- Timeout mechanism implemented to handle lost packets or unresponsive clients
- Maximum retry limit to prevent infinite loops in case of persistent network issues
- Handles unsupported TFTP modes and unknown opcodes

## Usage
Compilation instructions:
1. Compile the server:
   ```
   gcc -o tftp_server tftp_server.c
   ```
2. Run the server:
   ```
   ./tftp_server <address> <port>
   ```
   Example: `./tftp_server 0.0.0.0 69`

## Requirements
- Linux Environment
- GCC Compiler
- TFTP client for testing (e.g., `tftp` command-line tool)

## Features
- Supports both RRQ (Read Request) and WRQ (Write Request) operations
- Implements block number wraparound for large file transfers (>32MB)
- Handles netascii mode conversions
- Implements a timeout and retransmission mechanism for reliability

## Limitations and Future Improvements
- Error logging could be enhanced for better debugging
- Security features like access control are not implemented
- Performance optimization for large file transfers could be considered

## Testing
To test the server, you can use a standard TFTP client. For example:
1. To download a file:
   ```
   tftp <server_ip> <port>
   tftp> get <filename>
   ```
2. To upload a file:
   ```
   tftp <server_ip> <port>
   tftp> put <filename>
   ```
