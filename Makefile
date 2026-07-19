# adn-bridge — YSF/EchoLink <-> DMR voice bridge
# Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
# SPDX-License-Identifier: GPL-3.0-or-later
# See LICENSE for the full GNU General Public License v3 text.
#
# Default install layout (self-contained under /opt):
#   /opt/adn-bridge/adn-bridge
#   /opt/adn-bridge/config/*.example.ini
#   /opt/adn-bridge/data/          (aliases cache)
# Systemd units stay in examples/ — copy manually when needed.

CC ?= gcc
CXX ?= g++
CFLAGS ?= -Wall -Wextra -O2 -I. -Ivendor/yyjson -Ivendor/gsm/inc
CXXFLAGS ?= -Wall -Wextra -O2 -std=c++11 -Immdvm
# -MMD -MP: rebuild when headers change (avoids stale offsetof bugs across .o files)
CFLAGS += -MMD -MP
CXXFLAGS += -MMD -MP
LDFLAGS ?= -lcrypto -lm -lpthread -lz -lgsm

BUILD = build

C_SRCS = adn_bridge.c config.c log.c aliases.c talker_alias.c peer_dmr.c peer_ysf.c \
         peer_echolink.c el_proxy.c bridge.c bridge_el.c vocoder_remote.c ysf_fich.c \
         hbp/dmr_hbp.c vendor/yyjson/yyjson.c
CXX_SRCS = mmdvm/ModeConv.cpp mmdvm/Golay24128.cpp mmdvm/modeconv_wrap.cpp \
           mmdvm/YSFPayload.cpp mmdvm/YSFConvolution.cpp mmdvm/CRC.cpp \
           mmdvm/Utils.cpp mmdvm/ysfpayload_wrap.cpp

C_OBJS = $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
CXX_OBJS = $(patsubst %.cpp,$(BUILD)/%.o,$(CXX_SRCS))
OBJS = $(C_OBJS) $(CXX_OBJS)
DEPS = $(OBJS:.o=.d)

PREFIX ?= /opt/adn-bridge
CONFDIR ?= $(PREFIX)/config

all: adn-bridge

adn-bridge: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

install: adn-bridge
	install -d $(DESTDIR)$(PREFIX)
	install -d $(DESTDIR)$(CONFDIR)
	install -d $(DESTDIR)$(PREFIX)/data
	install -m 755 adn-bridge $(DESTDIR)$(PREFIX)/
	install -m 644 examples/adn-bridge.example.ini $(DESTDIR)$(CONFDIR)/
	install -m 644 examples/adn-bridge-ysf-dmr.example.ini $(DESTDIR)$(CONFDIR)/
	install -m 644 examples/adn-bridge-echolink-dmr.example.ini $(DESTDIR)$(CONFDIR)/
	install -m 644 examples/adn-bridge-echolink-ysf.example.ini $(DESTDIR)$(CONFDIR)/

clean:
	rm -rf $(BUILD) adn-bridge

-include $(DEPS)

.PHONY: all install clean
