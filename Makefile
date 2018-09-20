target := tellstick-influxdb
DESTDIR ?= /usr/local

.PHONY: all install
all: $(target)

$(target): main.cc
	g++ -O2 $^ -ltelldus-core -pthread -lcurl -o $@

clean:
	rm $(target)

install: $(target)
	install -m 775 $(target) $(DESTDIR)/bin
