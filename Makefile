CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -pthread

libwstream.a: wstream.o
	ar rcs $@ $<

# Shared library for ctypes/FFI consumers (pMLX binds this).
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
DYLIB := libwstream.dylib
SHFLAGS := -dynamiclib -install_name @rpath/libwstream.dylib
else
DYLIB := libwstream.so
SHFLAGS := -shared -fPIC
endif

dylib: $(DYLIB)
$(DYLIB): wstream.c wstream.h
	$(CC) $(CFLAGS) -fPIC $(SHFLAGS) wstream.c -o $@

wstream.o: wstream.c wstream.h
	$(CC) $(CFLAGS) -c wstream.c -o $@

test: test_wstream.c wstream.o
	$(CC) $(CFLAGS) -o test_wstream test_wstream.c wstream.o
	./test_wstream

bench: bench_wstream.c wstream.o
	$(CC) $(CFLAGS) -o bench_wstream bench_wstream.c wstream.o
	./bench_wstream

PREFIX ?= /usr/local
install: libwstream.a wstream.h
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 644 libwstream.a $(DESTDIR)$(PREFIX)/lib/
	install -m 644 wstream.h $(DESTDIR)$(PREFIX)/include/

clean:
	rm -f wstream.o libwstream.a libwstream.dylib libwstream.so test_wstream bench_wstream

.PHONY: test bench install clean dylib
