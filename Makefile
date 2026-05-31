CC = gcc
CFLAGS = -Iinclude -Wall -Wextra -pthread
LDFLAGS = -lpthread -lrt

SERVER_SRC = $(wildcard src/*.c)
CLIENT_SRC = src/client/client.c
TOOLS_DIR = tools

.PHONY: all clean tools

all: server client tools

server: $(SERVER_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

client: $(CLIENT_SRC)
	$(CC) $(CFLAGS) $< -o $@

tools: replay wal_clr

replay: $(TOOLS_DIR)/replay.c
	$(CC) $(CFLAGS) $< -o $@

wal_clr: $(TOOLS_DIR)/wal_clr.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f server client replay wal_clr *.log *.dat *_app *_test
