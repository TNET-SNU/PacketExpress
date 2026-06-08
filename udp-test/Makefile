CC = gcc
CFLAGS = -Wall -O3 -g

# Targets
all: udp-receiver

# udp-receiver target
udp-receiver: udp-receiver.c
	$(CC) $(CFLAGS) -o udp-receiver udp-receiver.c

# Clean up
clean:
	rm -f udp-receiver

.PHONY: all clean

