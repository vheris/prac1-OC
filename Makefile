CC = gcc
CFLAGS = -Wall -Wextra -pedantic -fPIC
LDFLAGS = -ldl

UNAME_S = $(shell uname -s)

ifeq ($(UNAME_S),Linux)
    LIB_EXT = so
    SHARED_FLAGS = -shared
    INSTALL_CMD = sudo cp libcaesar.so /usr/local/lib/ && sudo ldconfig
endif

ifeq ($(UNAME_S),Darwin)
    LIB_EXT = dylib
    SHARED_FLAGS = -dynamiclib
    INSTALL_CMD = sudo cp libcaesar.dylib /usr/local/lib/
endif


LIB_NAME = libcaesar.$(LIB_EXT)

all: $(LIB_NAME)

$(LIB_NAME): libcaesar.o
	$(CC) $(SHARED_FLAGS) -o $(LIB_NAME) libcaesar.o

libcaesar.o: libcaesar.c libcaesar.h
	$(CC) $(CFLAGS) -c libcaesar.c

test_prog: main.c
	$(CC) -Wall -Wextra -pedantic main.c -o test_prog $(LDFLAGS)

install: $(LIB_NAME)
	$(INSTALL_CMD)

test: all test_prog
	echo "Hello XOR Test" > input.txt
	./test_prog ./$(LIB_NAME) A input.txt encrypted.txt
	./test_prog ./$(LIB_NAME) A encrypted.txt decrypted.txt
	cat decrypted.txt

clean:
	rm -f *.o *.so *.dylib test_prog encrypted.txt decrypted.txt input.txt