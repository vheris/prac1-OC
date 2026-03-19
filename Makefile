CC = gcc
CFLAGS = -Wall -Wextra -pedantic -fPIC

UNAME_S = $(shell uname -s)

ifeq ($(UNAME_S),Linux)
    LIB_EXT = so
    SHARED_FLAGS = -shared
endif

ifeq ($(UNAME_S),Darwin)
    LIB_EXT = dylib
    SHARED_FLAGS = -dynamiclib
endif

LIB_NAME = libcaesar.$(LIB_EXT)
APP_NAME = secure_copy

all: $(LIB_NAME) $(APP_NAME)

$(LIB_NAME): libcaesar.o
	$(CC) $(SHARED_FLAGS) -o $(LIB_NAME) libcaesar.o

libcaesar.o: libcaesar.c libcaesar.h
	$(CC) $(CFLAGS) -c libcaesar.c

$(APP_NAME): main.c
	$(CC) -Wall -pthread main.c -o $(APP_NAME) -ldl

clean:
	rm -f *.o *.so *.dylib $(APP_NAME) log.txt