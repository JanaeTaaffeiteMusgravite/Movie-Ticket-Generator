CC     ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
TARGET  = bin/movie

.PHONY: all run clean

all: $(TARGET)

$(TARGET): backend/movie.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf bin
