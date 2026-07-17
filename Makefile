# ysf2dmrcon — YSF/EchoLink <-> DMR voice bridge
# Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
# SPDX-License-Identifier: GPL-3.0-or-later
# See LICENSE for the full GNU General Public License v3 text.

CC ?= gcc
CXX ?= g++
CFLAGS ?= -Wall -Wextra -O2 -I. -Ivendor/yyjson -Ivendor/gsm/inc
CXXFLAGS ?= -Wall -Wextra -O2 -std=c++11 -Immdvm
# -MMD -MP: rebuild when headers change (avoids stale offsetof bugs across .o files)
CFLAGS += -MMD -MP
CXXFLAGS += -MMD -MP
LDFLAGS ?= -lcrypto -lm -lpthread /usr/lib/x86_64-linux-gnu/libgsm.so.1

BUILD = build

C_SRCS = ysf2dmrcon.c config.c log.c aliases.c talker_alias.c peer_dmr.c peer_ysf.c \
         peer_echolink.c bridge.c bridge_el.c vocoder_remote.c ysf_fich.c \
         hbp/dmr_hbp.c vendor/yyjson/yyjson.c
CXX_SRCS = mmdvm/ModeConv.cpp mmdvm/Golay24128.cpp mmdvm/modeconv_wrap.cpp \
           mmdvm/YSFPayload.cpp mmdvm/YSFConvolution.cpp mmdvm/CRC.cpp \
           mmdvm/Utils.cpp mmdvm/ysfpayload_wrap.cpp

C_OBJS = $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
CXX_OBJS = $(patsubst %.cpp,$(BUILD)/%.o,$(CXX_SRCS))
OBJS = $(C_OBJS) $(CXX_OBJS)
DEPS = $(OBJS:.o=.d)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

all: ysf2dmrcon

ysf2dmrcon: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

install: ysf2dmrcon
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 ysf2dmrcon $(DESTDIR)$(BINDIR)/
	install -m 644 ysf2dmrcon.example.ini $(DESTDIR)$(BINDIR)/ysf2dmrcon.example.ini

clean:
	rm -rf $(BUILD) ysf2dmrcon

-include $(DEPS)

.PHONY: all install clean
