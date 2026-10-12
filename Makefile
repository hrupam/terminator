CC      = gcc
CFLAGS  = -Wall -Wextra -g -std=gnu11 -pthread
LDLIBS  = -lX11 -pthread -lrt
SRC     = $(wildcard src/*.c)
OBJ     = $(SRC:.c=.o)

terminator: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o terminator

.PHONY: clean