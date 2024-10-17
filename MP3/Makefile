
init_server:server

SERVER_ADDR ?=127.0.0.1
PORT?=8080
USERNAME?=ANYTHING
NO_OF_CLIENTS?=3



server:server.o
	gcc server.o -o server

all:server.o 
	gcc server.o -o server
	

server.o: server.c
	gcc -c server.c


 
clean:
	rm -f all server.o server

echos:
	@./server $(SERVER_ADDR) $(PORT)
