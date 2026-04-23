CC = gcc
CFLAGS = -Wall -Wextra -pedantic -fPIC
LDFLAGS = -ldl -pthread

UNAME_S := $(shell uname -s)

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
OUT_DIR = out

all: $(LIB_NAME) $(APP_NAME)

$(LIB_NAME): libcaesar.o
	$(CC) $(SHARED_FLAGS) -o $@ $^

libcaesar.o: libcaesar.c libcaesar.h
	$(CC) $(CFLAGS) -c libcaesar.c

$(APP_NAME): main.c
	$(CC) $(CFLAGS) main.c -o $@ $(LDFLAGS)

clean:
	rm -f *.o *.so *.dylib $(APP_NAME) log.txt f*.txt a.txt b.txt c.txt d.txt
	rm -rf $(OUT_DIR)

test-files:
	rm -rf $(OUT_DIR) log.txt
	mkdir -p $(OUT_DIR)
	for i in 1 2 3 4 5 6 7 8 9 10; do echo "test $$i" > f$$i.txt; done

test: all test-files
	./$(APP_NAME) --mode=auto f1.txt f2.txt f3.txt f4.txt f5.txt f6.txt f7.txt f8.txt f9.txt f10.txt $(OUT_DIR) k
	@echo "out files:"
	@ls -1 $(OUT_DIR)
	@echo "log tail:"
	@tail -n 3 log.txt || true

test-seq: all
	rm -rf $(OUT_DIR) log.txt
	mkdir -p $(OUT_DIR)
	echo a > a.txt; echo b > b.txt; echo c > c.txt; echo d > d.txt
	./$(APP_NAME) --mode=sequential a.txt b.txt c.txt d.txt $(OUT_DIR) k

test-par: all test-files
	./$(APP_NAME) --mode=parallel f1.txt f2.txt f3.txt f4.txt f5.txt f6.txt f7.txt f8.txt f9.txt f10.txt $(OUT_DIR) k