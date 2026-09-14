CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
LDFLAGS = -pthread -lssl -lcrypto -lsqlite3

# Detect OS to handle macOS Homebrew paths
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	OPENSSL_DIR ?= $(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)
	ifneq ($(OPENSSL_DIR),)
		CFLAGS += -I$(OPENSSL_DIR)/include
		LDFLAGS += -L$(OPENSSL_DIR)/lib
	endif
endif

TARGETS = server stress_test

all: $(TARGETS)

server: server.c ws.c db.c stock.c auth.c logger.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

stress_test: stress_test.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

cert:
	openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt -days 365 -nodes -subj "/CN=localhost"

run: server
	./server

clean:
	rm -f $(TARGETS) *.o
