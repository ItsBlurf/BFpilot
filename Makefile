# BFpilot - reproducible PS5 payload builds.

SHELL := bash

ifeq ($(strip $(PS5_PAYLOAD_SDK)),)
ifeq ($(filter ps5-diag ps5-smoke,$(MAKECMDGOALS)),)
$(error PS5_PAYLOAD_SDK is required, e.g. export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk)
endif
else
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

HOST_UNAME := $(shell uname -s 2>/dev/null || echo unknown)
HOST_IS_WINDOWS := 0
ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(HOST_UNAME)))
HOST_IS_WINDOWS := 1
endif

PS5_HOST ?= ps5
PS5_PORT ?= 9021
PYTHON ?= python3
WEB_PORT ?= 5905

VERSION_TAG := bfpilot-v0.3
BUILD_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

LLVM_BINDIR ?= $(shell dirname "$$(command -v clang 2>/dev/null || command -v clang.exe 2>/dev/null || command -v llvm-strip 2>/dev/null || command -v llvm-strip.exe 2>/dev/null || echo clang)" 2>/dev/null || echo .)
LLVM_CONFIG ?= $(CURDIR)/build-tools/llvm-config
export LLVM_BINDIR
export LLVM_CONFIG

BFPILOT_BIN := bfpilot.elf
LAUNCHER_INSTALLER_BIN := bfpilot-launcher-installer.elf

WEB_SRCS := src/lite_main.c
WEB_SRCS += src/boot_marker.c
WEB_SRCS += src/diag.c
WEB_SRCS += src/websrv_lite.c
WEB_SRCS += src/asset.c
WEB_SRCS += src/fs.c
WEB_SRCS += src/mime.c
WEB_SRCS += src/notify.c
WEB_SRCS += src/transfer.c

LAUNCHER_INSTALLER_SRCS := src/launcher_installer_force_main.c
LAUNCHER_INSTALLER_SRCS += src/boot_marker.c
LAUNCHER_INSTALLER_SRCS += src/notify.c

ASSETS := $(wildcard assets/*)
GEN_SRCS := $(patsubst assets/%,gen/assets/%,$(ASSETS:=.c))
APP_ASSETS := assets-app/param.json assets-app/icon0.png

BFPILOT_OBJS := $(patsubst %.c,build/bfpilot/%.o,$(WEB_SRCS) $(GEN_SRCS))
LAUNCHER_INSTALLER_OBJS := $(patsubst %.c,build/launcher-installer/%.o,$(LAUNCHER_INSTALLER_SRCS))

COMMON_CFLAGS := -Os -Wall -Werror -Isrc
COMMON_CFLAGS += -ffunction-sections -fdata-sections -flto
COMMON_CFLAGS += -DVERSION_TAG=\"$(VERSION_TAG)\"
COMMON_CFLAGS += -DBUILD_VERSION=\"$(BUILD_VERSION)\"
COMMON_CFLAGS += -DBFPILOT_SDK_PATH=\"$(PS5_PAYLOAD_SDK)\"

BFPILOT_CFLAGS := $(COMMON_CFLAGS)
BFPILOT_CFLAGS += -DBFPILOT_PAYLOAD_NAME=\"bfpilot\"
BFPILOT_CFLAGS += -DBFPILOT_BUILD_MODE=\"file-manager\"
BFPILOT_CFLAGS += -DBFPILOT_WEB_PORT=$(WEB_PORT)
BFPILOT_CFLAGS += -DBFPILOT_DEBUG_NOTIFICATIONS=0
BFPILOT_CFLAGS += -DBFPILOT_ENABLE_LAUNCHER=0
BFPILOT_CFLAGS += -DBFPILOT_DISABLE_LAUNCHER=1

LAUNCHER_INSTALLER_CFLAGS := $(COMMON_CFLAGS)
LAUNCHER_INSTALLER_CFLAGS += -DBFPILOT_PAYLOAD_NAME=\"bfpilot-launcher-installer\"
LAUNCHER_INSTALLER_CFLAGS += -DBFPILOT_BUILD_MODE=\"launcher-installer-force\"

COMMON_LDFLAGS := -Wl,--gc-sections -flto
PRIVILEGED_APPINST_LDLIBS := -lkernel_sys -lSceSystemService
PRIVILEGED_APPINST_LDLIBS += -lSceUserService -lSceAppInstUtil

CC_CMD := "$(CC)"
STRIP_CMD := "$(STRIP)"
LD_CMD := "$(LD)"
DEPLOY_CMD := "$(PS5_DEPLOY)"

ifeq ($(HOST_IS_WINDOWS),1)
CURDIR_POSIX := $(shell pwd)
PS5_PAYLOAD_SDK_POSIX := $(shell cygpath -u "$(PS5_PAYLOAD_SDK)" 2>/dev/null || printf '%s' "$(PS5_PAYLOAD_SDK)")
LLVM_CONFIG_POSIX := $(shell cygpath -u "$(LLVM_CONFIG)" 2>/dev/null || printf '%s' "$(LLVM_CONFIG)")
LLVM_BINDIR_POSIX := $(shell cygpath -u "$(LLVM_BINDIR)" 2>/dev/null || printf '%s' "$(LLVM_BINDIR)")
RUN_ENV := cd "$(CURDIR_POSIX)" && export LLVM_CONFIG="$(LLVM_CONFIG_POSIX)" && export LLVM_BINDIR="$(LLVM_BINDIR_POSIX)"
STRIP_CMD := "$(LLVM_BINDIR_POSIX)/llvm-strip"
WINDOWS_LINK_PREFIX := --gc-sections --sysroot="$(PS5_PAYLOAD_SDK_POSIX)"
WINDOWS_LINK_PREFIX += -L"$(PS5_PAYLOAD_SDK_POSIX)/target/lib"
WINDOWS_LINK_PREFIX += -L"$(PS5_PAYLOAD_SDK_POSIX)/target/user/homebrew/lib"
WINDOWS_LINK_PREFIX += -l:crt1.o -l:crti.o -l:crtbegin.o -lc
WINDOWS_WEB_SUFFIX := -lkernel_web -lSceLibcInternal -lSceNet
WINDOWS_WEB_SUFFIX += -lc_stub_weak -lkernel_stub_weak
WINDOWS_WEB_SUFFIX += -l:crtend.o -l:crtn.o
WINDOWS_APPINST_SUFFIX := -lkernel_sys -lSceSystemService
WINDOWS_APPINST_SUFFIX += -lSceUserService -lSceAppInstUtil
WINDOWS_APPINST_SUFFIX += -lSceLibcInternal -lc_stub_weak -lkernel_stub_weak
WINDOWS_APPINST_SUFFIX += -l:crtend.o -l:crtn.o
define run
bash -lc '$(RUN_ENV) && $(1)'
endef
else
define run
$(1)
endef
endif

all: bfpilot launcher-installer

bfpilot: $(BFPILOT_BIN)

launcher-installer: $(LAUNCHER_INSTALLER_BIN)

gen/assets:
	$(call run,mkdir -p $@)

gen/assets/%.c: assets/% | gen/assets
	$(call run,$(PYTHON) gen-asset-module.py --path $* $< > $@)

build/bfpilot/%.o: %.c Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CC_CMD) $(BFPILOT_CFLAGS) -c $< -o $@)

build/launcher-installer/%.o: %.c Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CC_CMD) $(LAUNCHER_INSTALLER_CFLAGS) -c $< -o $@)

ifeq ($(HOST_IS_WINDOWS),1)
$(BFPILOT_BIN): $(BFPILOT_OBJS)
	$(call run,$(LD_CMD) -o $@ $(WINDOWS_LINK_PREFIX) $(BFPILOT_OBJS) $(WINDOWS_WEB_SUFFIX))
	$(call run,$(STRIP_CMD) --strip-all $@)
	$(call run,$(PYTHON) scripts/scrub_main_payload.py $@)

$(LAUNCHER_INSTALLER_BIN): $(LAUNCHER_INSTALLER_OBJS) $(APP_ASSETS)
	$(call run,$(LD_CMD) -o $@ $(WINDOWS_LINK_PREFIX) $(LAUNCHER_INSTALLER_OBJS) $(WINDOWS_APPINST_SUFFIX))
	$(call run,$(STRIP_CMD) --strip-all $@)
else
$(BFPILOT_BIN): $(BFPILOT_OBJS)
	$(call run,$(CC_CMD) $(BFPILOT_CFLAGS) $(COMMON_LDFLAGS) -o $@ $(BFPILOT_OBJS))
	$(call run,$(STRIP_CMD) --strip-all $@)
	$(call run,$(PYTHON) scripts/scrub_main_payload.py $@)

$(LAUNCHER_INSTALLER_BIN): $(LAUNCHER_INSTALLER_OBJS) $(APP_ASSETS)
	$(call run,$(CC_CMD) $(LAUNCHER_INSTALLER_CFLAGS) $(COMMON_LDFLAGS) -o $@ $(LAUNCHER_INSTALLER_OBJS) $(PRIVILEGED_APPINST_LDLIBS))
	$(call run,$(STRIP_CMD) --strip-all $@)
endif

inspect-imports: all
	$(call run,bash scripts/inspect_imports.sh $(BFPILOT_BIN) $(LAUNCHER_INSTALLER_BIN))

deploy-bfpilot: bfpilot
	$(call run,$(DEPLOY_CMD) -h $(PS5_HOST) -p $(PS5_PORT) $(BFPILOT_BIN))

deploy-launcher-installer: launcher-installer
	$(call run,$(DEPLOY_CMD) -h $(PS5_HOST) -p $(PS5_PORT) $(LAUNCHER_INSTALLER_BIN))

ps5-diag:
	$(call run,$(PYTHON) scripts/ps5_diag.py)

ps5-smoke:
	$(call run,$(PYTHON) scripts/ps5_smoke.py)

clean:
	$(call run,rm -rf $(BFPILOT_BIN) $(LAUNCHER_INSTALLER_BIN) build gen)

.SECONDARY: $(GEN_SRCS)
.PHONY: all bfpilot launcher-installer inspect-imports clean deploy-bfpilot deploy-launcher-installer ps5-diag ps5-smoke
