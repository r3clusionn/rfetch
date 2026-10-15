# Static by default: a static rfetch starts about 0.4 ms faster than a dynamic one.
# Needs glibc's static libraries; for a dynamic build use `make LDFLAGS=`.
CFLAGS ?= -O2 -march=native -pipe
CFLAGS += -std=gnu11 -Wall -Wextra
LDFLAGS ?= -static
PREFIX ?= $(HOME)/.local

rfetch: rfetch.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lm

install: rfetch
	install -Dm755 rfetch $(PREFIX)/bin/rfetch

clean:
	rm -f rfetch
