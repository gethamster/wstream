CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -pthread

libwstream.a: wstream.o
	ar rcs $@ $<

wstream.o: wstream.c wstream.h
	$(CC) $(CFLAGS) -c wstream.c -o $@

test: test_wstream.c wstream.o
	$(CC) $(CFLAGS) -o test_wstream test_wstream.c wstream.o
	./test_wstream

PREFIX ?= /usr/local
install: libwstream.a wstream.h
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 644 libwstream.a $(DESTDIR)$(PREFIX)/lib/
	install -m 644 wstream.h $(DESTDIR)$(PREFIX)/include/

clean:
	rm -f wstream.o libwstream.a test_wstream

.PHONY: test install clean
