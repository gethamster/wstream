CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -pthread

libwstream.a: wstream.o
	ar rcs $@ $<

wstream.o: wstream.c wstream.h
	$(CC) $(CFLAGS) -c wstream.c -o $@

test: test_wstream.c wstream.o
	$(CC) $(CFLAGS) -o test_wstream test_wstream.c wstream.o
	./test_wstream

clean:
	rm -f wstream.o libwstream.a test_wstream

.PHONY: test clean
