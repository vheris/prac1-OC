.DEFAULT_GOAL := all

CC = gcc
CFLAGS = -Wall -Wextra -pedantic -fPIC
LDFLAGS = -ldl -pthread

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
  LIB_EXT = so
  SHARED_FLAGS = -shared
  CFLAGS += -D_XOPEN_SOURCE=500
endif

ifeq ($(UNAME_S),Darwin)
  LIB_EXT = dylib
  SHARED_FLAGS = -dynamiclib
endif

LIB_NAME = librc4.$(LIB_EXT)
APP_NAME = secure_copy
APP_DEMO = secure_copy_demo
OUT_DIR = out

.PHONY: all demo clean test-files test test-seq test-par

all: $(LIB_NAME) $(APP_NAME)

$(LIB_NAME): librc4.o
	$(CC) $(SHARED_FLAGS) -o $@ $^

librc4.o: librc4.c librc4.h
	$(CC) $(CFLAGS) -c librc4.c

$(APP_NAME): main.c librc4.h $(LIB_NAME)
	$(CC) $(CFLAGS) main.c -o $@ $(LDFLAGS)

$(APP_DEMO): main.c librc4.h $(LIB_NAME)
	$(CC) $(CFLAGS) -DDEMO_SEGV main.c -o $@ $(LDFLAGS)

demo: $(LIB_NAME) $(APP_DEMO)

clean:
	rm -f *.o *.so *.dylib $(APP_NAME) $(APP_DEMO) log.txt f*.txt a.txt b.txt c.txt d.txt img_f*.txt extracted_*.txt *.img
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