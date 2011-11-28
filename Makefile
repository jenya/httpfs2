MAIN_CFLAGS :=  -g -Os -Wall $(shell pkg-config fuse --cflags)
MAIN_CPPFLAGS := -Wall -Wno-unused-function -Wconversion -Wtype-limits -DUSE_AUTH -D_XOPEN_SOURCE=700 -D_ISOC99_SOURCE
THR_CPPFLAGS := -DUSE_THREAD
THR_LDFLAGS := -lpthread
SSL_CPPFLAGS := -DUSE_SSL $(shell pkg-config openssl --cflags)
SSL_LDFLAGS := $(shell pkg-config openssl --libs)
MAIN_LDFLAGS := $(shell pkg-config fuse --libs | sed -e s/-lrt// -e s/-ldl// -e s/-pthread// -e "s/  / /g")

intermediates =

variants = _mt #_ssl _ssl_mt

binbase = httpfs2

binaries = $(addsuffix $(binsuffix),$(binbase))

#manpages = $(addsuffix .1,$(binaries))

intermediates += $(addsuffix .xml,$(manpages))

targets = $(binaries) $(manpages)

full:
	$(MAKE) all $(addprefix all,$(variants))

all: $(targets)

httpfs2$(binsuffix): httpfs2.c
	$(CC) $(MAIN_CPPFLAGS) $(CPPFLAGS) $(MAIN_CFLAGS) $(CFLAGS) httpfs2.c $(MAIN_LDFLAGS) $(LDFLAGS) -o $@

httpfs2%.1: httpfs2.1
	ln -sf httpfs2.1 $@

clean: clean_recursive_full

clean_recursive:
	rm -f $(targets) $(intermediates)

%_full:
	$(MAKE) $* $(addprefix $*,$(variants))

%.1: %.1.txt
	a2x -f manpage $<

%_ssl: $*
	$(MAKE) CPPFLAGS="$(CPPFLAGS) $(SSL_CPPFLAGS)" LDFLAGS="$(LDFLAGS) $(SSL_LDFLAGS)" binsuffix=_ssl$(binsuffix) $*

%_mt: $*
	$(MAKE) CPPFLAGS="$(CPPFLAGS) $(THR_CPPFLAGS)" LDFLAGS="$(LDFLAGS) $(THR_LDFLAGS)" binsuffix=_mt$(binsuffix) $*

%_lstr: $*
	$(MAKE) CPPFLAGS="$(CPPFLAGS) -DNEED_STRNDUP -U_XOPEN_SOURCE -D_XOPEN_SOURCE=500" binsuffix=_lstr$(binsuffix) $*

%_rst: $*
	$(MAKE) CPPFLAGS="$(CPPFLAGS) -DRETRY_ON_RESET" binsuffix=_rst$(binsuffix) $*

