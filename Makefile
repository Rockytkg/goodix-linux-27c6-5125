# SPDX-License-Identifier: GPL-2.0-or-later
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Iinclude $(shell pkg-config --cflags libusb-1.0)
# openssl: pkg-config available on Debian/Ubuntu
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS   := $(shell pkg-config --libs openssl 2>/dev/null)
ifneq ($(strip $(OPENSSL_LIBS)),)
  CFLAGS += $(OPENSSL_CFLAGS)
  LIBS   += $(OPENSSL_LIBS)
else
  LIBS   += -lcrypto
endif
# mbedtls: often no .pc file -> link directly
LIBS += -lmbedtls -lmbedx509 -lmbedcrypto
LIBS += $(shell pkg-config --libs libusb-1.0)
LIBS += -lz
# goodix_imgproc.c 的 SIGFM 反锐化增强用 expf/ceilf/lroundf
LIBS += -lm

SRCS := src/transport.c src/goodix_frame.c src/goodix_cmd.c \
        src/goodix_psk.c src/goodix_tls.c src/goodix_fwupdate.c \
        src/goodix_init.c src/goodix_capture.c src/goodix_base.c \
        src/goodix_otp.c src/goodix_imgproc.c src/main.c
OBJS := $(SRCS:.c=.o)

all: goodix-cli

goodix-cli: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c include/goodix.h include/goodix_imgproc.h include/goodix_fw.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) goodix-cli image-*.pgm

.PHONY: all clean
