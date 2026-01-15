CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags libusb-1.0 2>/dev/null || echo "-I/opt/homebrew/include")
LDFLAGS = $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo "-L/opt/homebrew/lib -lusb-1.0")
TARGET = flashmd
SRC = flashmd.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
