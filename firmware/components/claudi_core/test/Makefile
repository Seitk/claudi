# Host build for the claudi_core unit test. No ESP-IDF required.
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g -I../include
BIN     := test_claudi_core

.PHONY: all test clean
all: test

$(BIN): test_claudi_core.c ../claudi_core.c ../include/claudi_core.h
	$(CC) $(CFLAGS) -o $(BIN) test_claudi_core.c ../claudi_core.c

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
