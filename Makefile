# SPDX-License-Identifier: Apache-2.0
# mod_klear — out-of-tree FreeSWITCH module build.
#
# Requires:
#   * FreeSWITCH dev headers     (pkg-config freeswitch)
#   * webrtc-audio-processing-2  (built from source, AEC3 is in v2.x)
#   * libdeepfilter              (built from DeepFilterNet via `cargo cinstall`)
#   * libsndfile                 (only for the offline test harness)

CXX ?= g++
CXXSTD := -std=c++17
OPT := -O2
WARN := -Wall -Wextra -Wno-unused-parameter

# FreeSWITCH
FS_CFLAGS := $(shell pkg-config --cflags freeswitch 2>/dev/null)
FS_LIBS   := $(shell pkg-config --libs   freeswitch 2>/dev/null)
FS_MOD_DIR ?= $(shell pkg-config --variable=modulesdir freeswitch 2>/dev/null)
FS_CONF_DIR ?= /etc/freeswitch/autoload_configs
# Fallback modules dir if pkg-config doesn't supply one
ifeq ($(FS_MOD_DIR),)
  FS_MOD_DIR := /usr/lib/freeswitch/mod
endif

# Third-party audio libs
APM_CFLAGS := $(shell pkg-config --cflags webrtc-audio-processing-2)
APM_LIBS   := $(shell pkg-config --libs   webrtc-audio-processing-2)

DF_CFLAGS := $(shell pkg-config --cflags deepfilter)
DF_LIBS   := $(shell pkg-config --libs   deepfilter)

SOXR_CFLAGS := $(shell pkg-config --cflags soxr 2>/dev/null)
SOXR_LIBS   := $(shell pkg-config --libs   soxr 2>/dev/null)
ifeq ($(SOXR_LIBS),)
  SOXR_LIBS := -lsoxr
endif

SNDFILE_CFLAGS := $(shell pkg-config --cflags sndfile)
SNDFILE_LIBS   := $(shell pkg-config --libs   sndfile)

INCLUDES := -Isrc

COMMON_CXXFLAGS := $(CXXSTD) $(OPT) $(WARN) -fPIC $(INCLUDES) \
                   $(APM_CFLAGS) $(DF_CFLAGS) $(SOXR_CFLAGS)
MOD_CXXFLAGS := $(COMMON_CXXFLAGS) $(FS_CFLAGS)
MOD_LDFLAGS  := -shared -Wl,-soname,mod_klear.so \
                -Wl,-rpath,/usr/local/lib/x86_64-linux-gnu \
                $(FS_LIBS) $(APM_LIBS) $(DF_LIBS) $(SOXR_LIBS)

# ---- source lists -------------------------------------------------------
CORE_SRC := src/processor.cpp src/reframer.cpp \
            src/backends/webrtc_aec.cpp src/backends/df_ns.cpp

MOD_SRC := src/mod_klear.cpp $(CORE_SRC)
TEST_SRC := test/klear_test.cpp $(CORE_SRC)

# ---- targets ------------------------------------------------------------
.PHONY: all module test live-test clean install install-module install-config install-models
all: module test

# Run the live end-to-end smoke test: originates a null/nothing channel,
# attaches mod_klear to it, verifies frames flow through the callback.
# Requires mod_klear to already be loaded in a running FreeSWITCH.
live-test:
	@./test/live_smoke.sh

build:
	mkdir -p build

module: build build/mod_klear.so

build/mod_klear.so: $(MOD_SRC)
	$(CXX) $(MOD_CXXFLAGS) -o $@ $(MOD_SRC) $(MOD_LDFLAGS)

test: build build/klear_test

build/klear_test: $(TEST_SRC)
	$(CXX) $(COMMON_CXXFLAGS) $(SNDFILE_CFLAGS) -o $@ $(TEST_SRC) \
	    $(APM_LIBS) $(DF_LIBS) $(SOXR_LIBS) $(SNDFILE_LIBS) \
	    -Wl,-rpath,/usr/local/lib/x86_64-linux-gnu

install: install-module install-config install-models
	@echo "mod_klear installed"

install-module: build/mod_klear.so
	install -m 0755 -D build/mod_klear.so $(FS_MOD_DIR)/mod_klear.so

install-config: conf/klear.conf.xml
	install -m 0644 -D conf/klear.conf.xml \
	    $(FS_CONF_DIR)/klear.conf.xml

install-models:
	@test -f /usr/local/share/deepfilternet/models/DeepFilterNet3_onnx.tar.gz || \
	    (echo "ERROR: DeepFilterNet3_onnx.tar.gz is missing from /usr/local/share/deepfilternet/models/. Run scripts/install-model.sh first." && false)

clean:
	rm -rf build
