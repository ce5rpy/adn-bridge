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
         peer_echolink.c el_proxy.c vocoder_remote.c ysf_fich.c \
         hbp/dmr_hbp.c vendor/yyjson/yyjson.c \
         media/bridge_util.c media/call_meta.c media/identity.c media/router.c \
         media/peer_bus.c media/codec_plan.c media/log_flow.c media/core.c \
         media/core_ysf_dmr.c media/core_echolink.c \
         adapters/peer_plugin.c \
         engine.c \
         session/dmr_wire.c session/dmr_tx.c session/ysf_tx.c \
         codecs/registry.c codecs/pcm.c codecs/dmr_ambe.c codecs/ysf_ambe.c \
         codecs/pair_modeconv.c adapters/dmr.c adapters/ysf.c adapters/el.c
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
	rm -rf $(BUILD) adn-bridge tests/test_wire tests/test_codecs tests/test_router tests/test_config_peers tests/test_codec_plan tests/test_media_core

test: tests/test_wire tests/test_codecs tests/test_router tests/test_config_peers tests/test_codec_plan tests/test_media_core
	./tests/test_wire
	./tests/test_codecs
	./tests/test_router
	./tests/test_config_peers
	./tests/test_codec_plan
	./tests/test_media_core

$(BUILD)/tests/test_wire.o: tests/test_wire.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_wire: adn-bridge $(BUILD)/tests/test_wire.o
	$(CXX) -o $@ $(BUILD)/tests/test_wire.o \
		$(BUILD)/session/dmr_wire.o $(BUILD)/media/call_meta.o \
		$(BUILD)/media/identity.o $(BUILD)/media/bridge_util.o \
		$(BUILD)/media/log_flow.o $(BUILD)/adapters/peer_plugin.o \
		$(BUILD)/codecs/registry.o \
		$(BUILD)/session/ysf_tx.o $(BUILD)/log.o $(BUILD)/ysf_fich.o \
		$(BUILD)/peer_ysf.o $(BUILD)/aliases.o $(BUILD)/vendor/yyjson/yyjson.o \
		$(filter $(BUILD)/mmdvm/%,$(OBJS)) $(LDFLAGS)

$(BUILD)/tests/test_codecs.o: tests/test_codecs.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_codecs: adn-bridge $(BUILD)/tests/test_codecs.o
	$(CXX) -o $@ $(BUILD)/tests/test_codecs.o \
		$(BUILD)/codecs/registry.o $(BUILD)/codecs/pcm.o \
		$(BUILD)/codecs/pair_modeconv.o $(BUILD)/codecs/ysf_ambe.o \
		$(filter $(BUILD)/mmdvm/%,$(OBJS)) $(LDFLAGS)

$(BUILD)/tests/test_router.o: tests/test_router.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_router: $(BUILD)/tests/test_router.o $(BUILD)/media/router.o
	$(CC) -o $@ $(BUILD)/tests/test_router.o $(BUILD)/media/router.o

$(BUILD)/tests/test_config_peers.o: tests/test_config_peers.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_config_peers: $(BUILD)/tests/test_config_peers.o $(BUILD)/config.o $(BUILD)/log.o $(BUILD)/aliases.o $(BUILD)/vendor/yyjson/yyjson.o $(BUILD)/media/router.o $(BUILD)/media/codec_plan.o $(BUILD)/adapters/peer_plugin.o $(BUILD)/codecs/registry.o
	$(CC) -o $@ $(BUILD)/tests/test_config_peers.o \
		$(BUILD)/config.o $(BUILD)/log.o $(BUILD)/aliases.o \
		$(BUILD)/media/router.o $(BUILD)/media/codec_plan.o \
		$(BUILD)/adapters/peer_plugin.o $(BUILD)/codecs/registry.o \
		$(BUILD)/vendor/yyjson/yyjson.o -lcrypto -lpthread

$(BUILD)/tests/test_codec_plan.o: tests/test_codec_plan.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_codec_plan: $(BUILD)/tests/test_codec_plan.o $(BUILD)/media/router.o $(BUILD)/media/codec_plan.o $(BUILD)/media/log_flow.o $(BUILD)/adapters/peer_plugin.o $(BUILD)/codecs/registry.o
	$(CC) -o $@ $(BUILD)/tests/test_codec_plan.o \
		$(BUILD)/media/router.o $(BUILD)/media/codec_plan.o \
		$(BUILD)/media/log_flow.o \
		$(BUILD)/adapters/peer_plugin.o $(BUILD)/codecs/registry.o

$(BUILD)/tests/test_media_core.o: tests/test_media_core.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_media_core: adn-bridge $(BUILD)/tests/test_media_core.o
	$(CXX) -o $@ $(BUILD)/tests/test_media_core.o \
		$(BUILD)/media/core.o $(BUILD)/media/core_ysf_dmr.o $(BUILD)/media/core_echolink.o \
		$(BUILD)/media/router.o $(BUILD)/media/codec_plan.o \
		$(BUILD)/media/log_flow.o $(BUILD)/media/peer_bus.o \
		$(BUILD)/media/bridge_util.o $(BUILD)/media/call_meta.o \
		$(BUILD)/media/identity.o \
		$(BUILD)/adapters/peer_plugin.o $(BUILD)/adapters/dmr.o $(BUILD)/adapters/ysf.o \
		$(BUILD)/adapters/el.o $(BUILD)/codecs/registry.o \
		$(BUILD)/session/dmr_wire.o $(BUILD)/session/dmr_tx.o $(BUILD)/session/ysf_tx.o \
		$(BUILD)/peer_dmr.o $(BUILD)/peer_ysf.o $(BUILD)/peer_echolink.o $(BUILD)/el_proxy.o \
		$(BUILD)/vocoder_remote.o $(BUILD)/talker_alias.o \
		$(BUILD)/hbp/dmr_hbp.o $(BUILD)/log.o $(BUILD)/ysf_fich.o $(BUILD)/aliases.o \
		$(BUILD)/vendor/yyjson/yyjson.o \
		$(filter $(BUILD)/mmdvm/%,$(OBJS)) $(LDFLAGS)

-include $(DEPS)

.PHONY: all install clean test
