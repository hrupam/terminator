CC      = g++
CFLAGS  = -Wall -Wextra -g -std=c++17 -pthread
LDLIBS  = -lX11 -pthread -lrt
SRC     = $(wildcard src/*.cpp)
OBJ     = $(SRC:.cpp=.o)

terminator: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.cpp $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o terminator

.PHONY: clean