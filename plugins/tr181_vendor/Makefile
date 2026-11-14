# Makefile for the OBUSPA TR-181 vendor plug-in (libtr181_vendor.so)
#
# Native Linux build:
#   make                                  # -> libtr181_vendor.so
#   make test                             # build + run the host test harness
#   make check                            # test + verify exported/undefined symbols of the .so
#   make install [DESTDIR=...] [PREFIX=/usr]
#
# Cross-compile:
#   make CROSS_COMPILE=aarch64-linux-gnu- SYSROOT=/path/to/sysroot
#   make CC=arm-openwrt-linux-muslgnueabi-gcc OPENSSL_CFLAGS=-I$STAGING_DIR/usr/include
#
# Other:
#   make env                              # print the toolchain/flags that will be used
#   make gen                              # regenerate src/tr181_dm_gen.c from data/tr181_params.csv (needs python3)
#   make BACKEND=uci                      # build with src/tr181_backend_uci.c instead of the mock backend
#   make V=1                              # show full compiler command lines
#   make WERROR=1                         # treat warnings as errors
#
# Run:
#   obuspa -p -v 4 -r <factory_reset.txt> -x /usr/lib/obuspa/libtr181_vendor.so

#------------------------------------------------------------------------------
# Configuration (override on the command line or in the environment)

OBUSPA_SRC    ?= ../..
BACKEND       ?= mock
NAME          := tr181_vendor
TARGET        := lib$(NAME).so

# Toolchain. CC defaults to $(CROSS_COMPILE)gcc unless CC was set explicitly
CROSS_COMPILE ?=
ifeq ($(origin CC),default)
CC            := $(CROSS_COMPILE)gcc
endif
NM            ?= $(CROSS_COMPILE)nm
STRIP         ?= $(CROSS_COMPILE)strip
PKG_CONFIG    ?= pkg-config
PYTHON        ?= python3
INSTALL       ?= install

# Optional sysroot for cross-compilation
SYSROOT       ?=

# Host compiler, used only for the test harness (it has to run on the build machine)
HOSTCC        ?= cc

# Install locations
PREFIX        ?= /usr
LIBDIR        ?= $(PREFIX)/lib/obuspa
DESTDIR       ?=

# usp_api.h includes <openssl/ssl.h>, so the OpenSSL *headers* are needed (the plug-in does not link OpenSSL).
# Debian/Ubuntu: libssl-dev   RHEL/Fedora: openssl-devel   Alpine: openssl-dev
# When headers are in the default search path pkg-config returns nothing, which is fine.
OPENSSL_CFLAGS      ?= $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
HOST_OPENSSL_CFLAGS ?= $(shell pkg-config --cflags openssl 2>/dev/null)

# Must match the feature defines obuspa itself was built with (they affect vendor_defs.h / usp_api.h).
# Default matches a standard autotools build of obuspa (UDS MTP enabled).
PLUGIN_DEFS   ?= -DENABLE_UDS

# User-tunable flags. Required flags are kept separately below, so a toolchain that sets
# CFLAGS/LDFLAGS (e.g. OpenWrt, Yocto) does not drop -fPIC etc.
CFLAGS        ?= -O2 -g
CPPFLAGS      ?=
LDFLAGS       ?=

# Libraries: the mock backend persists to sqlite (Debian/Ubuntu libsqlite3-dev, RHEL/Fedora sqlite-devel, Alpine sqlite-dev)
LIBS          ?= -lsqlite3

#------------------------------------------------------------------------------
# Derived settings

# Target platform is taken from the compiler (not 'uname'), so cross builds detect it correctly
TARGET_MACHINE := $(shell $(CC) -dumpmachine 2>/dev/null)
ifneq (,$(findstring darwin,$(TARGET_MACHINE)))
TARGET_OS     := darwin
else
TARGET_OS     := linux
endif

ifneq ($(SYSROOT),)
SYSROOT_FLAGS := --sysroot=$(SYSROOT)
endif

INCLUDES      := -I$(OBUSPA_SRC)/src/include -I$(OBUSPA_SRC)/src/vendor -Isrc

WARN_FLAGS    := -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
ifeq ($(WERROR),1)
WARN_FLAGS    += -Werror
endif

REQ_CFLAGS    := -std=gnu99 -D_GNU_SOURCE $(PLUGIN_DEFS) -fPIC -fvisibility=hidden $(WARN_FLAGS) $(INCLUDES)

ifeq ($(TARGET_OS),darwin)
# USP_* symbols are resolved from the obuspa executable at dlopen() time
REQ_LDFLAGS   := -shared -undefined dynamic_lookup
else
# NOTE: USP_* symbols are intentionally left undefined; they are resolved from the obuspa
# executable (built with -rdynamic) when it dlopen()s the plug-in. So do not link with -Wl,-z,defs.
REQ_LDFLAGS   := -shared -Wl,-soname,$(TARGET) -Wl,--version-script=exports.map
endif

SRCS          := src/tr181_plugin.c src/tr181_dm_gen.c src/tr181_backend_$(BACKEND).c
OBJS          := $(SRCS:.c=.o)
DEPS          := $(OBJS:.o=.d)

TEST_BIN      := test/test_plugin
TEST_SRCS     := test/test_plugin.c test/usp_stub.c $(SRCS)

ifeq ($(V),1)
Q :=
else
Q := @
endif

#------------------------------------------------------------------------------
# Targets

all: $(TARGET)

$(TARGET): $(OBJS) exports.map
	@echo "  LD      $@"
	$(Q)$(CC) $(SYSROOT_FLAGS) $(REQ_LDFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

src/%.o: src/%.c
	@echo "  CC      $<"
	$(Q)$(CC) $(SYSROOT_FLAGS) $(CPPFLAGS) $(REQ_CFLAGS) $(OPENSSL_CFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

# Regenerate the data model tables from the CSV
gen:
	$(PYTHON) tools/gen_tr181_dm.py data/tr181_params.csv src/tr181_dm_gen.c

# Test harness: plug-in sources linked against stubs of the USP API, built and run on the build machine
test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRCS) $(wildcard src/*.h test/*.h)
	@echo "  HOSTCC  $@"
	$(Q)$(HOSTCC) -std=gnu99 -D_GNU_SOURCE $(PLUGIN_DEFS) $(WARN_FLAGS) $(INCLUDES) $(HOST_OPENSSL_CFLAGS) -O1 -g -o $@ $(TEST_SRCS) $(LIBS)

# Verify the .so exports only the VENDOR_* entry points, and list what it expects from obuspa
check: test $(TARGET)
ifeq ($(TARGET_OS),linux)
	@echo "Exported symbols:"
	@$(NM) -D --defined-only $(TARGET) | awk '$$2 ~ /^[TDBR]$$/ {print "  " $$3}'
	@if $(NM) -D --defined-only $(TARGET) | awk '$$2 ~ /^[TDBR]$$/ {print $$3}' | grep -qv '^VENDOR_'; then \
		echo "ERROR: unexpected exported symbols"; exit 1; fi
	@echo "Symbols resolved from obuspa at load time:"
	@$(NM) -D --undefined-only $(TARGET) | awk '{print $$NF}' | grep -E '^(USP_|enable_callstack_debug)' | sed 's/^/  /'
	@echo "check OK"
else
	@echo "Symbol check is only implemented for Linux targets"
endif

# Print toolchain configuration (useful when debugging cross builds)
env:
	@echo "CC             = $(CC)"
	@echo "TARGET_MACHINE = $(TARGET_MACHINE)"
	@echo "TARGET_OS      = $(TARGET_OS)"
	@echo "SYSROOT        = $(SYSROOT)"
	@echo "OBUSPA_SRC     = $(OBUSPA_SRC)"
	@echo "BACKEND        = $(BACKEND)"
	@echo "PLUGIN_DEFS    = $(PLUGIN_DEFS)"
	@echo "OPENSSL_CFLAGS = $(OPENSSL_CFLAGS)"
	@echo "CFLAGS         = $(CFLAGS)"
	@echo "LDFLAGS        = $(LDFLAGS)"
	@echo "LIBS           = $(LIBS)"
	@printf '#include <openssl/ssl.h>\n' | $(CC) $(SYSROOT_FLAGS) $(OPENSSL_CFLAGS) -E -x c - >/dev/null 2>&1 \
		&& echo "openssl/ssl.h  = found" \
		|| echo "openssl/ssl.h  = NOT FOUND (install libssl-dev / openssl-devel, or set OPENSSL_CFLAGS)"
	@test -f $(OBUSPA_SRC)/src/include/usp_api.h \
		&& echo "usp_api.h      = found" \
		|| echo "usp_api.h      = NOT FOUND (set OBUSPA_SRC to the obuspa source tree)"

install: $(TARGET)
	$(INSTALL) -d $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 0755 $(TARGET) $(DESTDIR)$(LIBDIR)/$(TARGET)

install-strip: install
	$(STRIP) --strip-unneeded $(DESTDIR)$(LIBDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(TARGET)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) $(TEST_BIN)
	rm -rf $(TEST_BIN).dSYM

.PHONY: all gen test check env install install-strip uninstall clean
