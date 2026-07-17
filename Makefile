# BFpilot - reproducible PS5 payload builds.

SHELL := bash

ifeq ($(strip $(PS5_PAYLOAD_SDK)),)
ifeq ($(filter ps5-diag ps5-smoke ps5-storage-audit ps5-archive-perf,$(MAKECMDGOALS)),)
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
# Production HTTP listen port (file manager UI). Override only for local tests:
#   mingw32-make bfpilot WEB_PORT=5910
# Ship / daily use must stay 5905.
WEB_PORT ?= 5905

VERSION_TAG := bfpilot-v0.4.1
BUILD_VERSION ?= v0.4.1

LLVM_CONFIG ?= $(if $(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-llvm-config),$(PS5_PAYLOAD_SDK)/bin/prospero-llvm-config,$(CURDIR)/build-tools/llvm-config)
LLVM_BINDIR ?= $(shell "$(LLVM_CONFIG)" --bindir 2>/dev/null || dirname "$$(command -v clang 2>/dev/null || command -v clang.exe 2>/dev/null || command -v llvm-strip 2>/dev/null || command -v llvm-strip.exe 2>/dev/null || echo clang)" 2>/dev/null || echo .)
export LLVM_BINDIR
export LLVM_CONFIG

BFPILOT_BIN := bfpilot.elf
LAUNCHER_INSTALLER_BIN := bfpilot-launcher-installer.elf
ARCHIVE_WORKER_BIN := bfpilot-archive-worker.elf

WEB_SRCS := src/lite_main.c
WEB_SRCS += src/boot_marker.c
WEB_SRCS += src/diag.c
WEB_SRCS += src/websrv_lite.c
WEB_SRCS += src/asset.c
WEB_SRCS += src/fs.c
WEB_SRCS += src/mime.c
WEB_SRCS += src/notify.c
WEB_SRCS += src/search.c
WEB_SRCS += src/transfer.c
WEB_SRCS += src/payload_launch.c
WEB_SRCS += src/tile_bootstrap.c
WEB_SRCS += src/path_ops.c

LAUNCHER_INSTALLER_SRCS := src/launcher_installer_force_main.c
LAUNCHER_INSTALLER_SRCS += src/boot_marker.c
LAUNCHER_INSTALLER_SRCS += src/notify.c

ARCHIVE_WORKER_SRCS := src/archive_worker.cpp

UNRAR_SRC_DIR := third_party/unrar-ps5/src
LZMA_DIR := third_party/unrar-ps5/lzma2601
LZMA_C_DIR := $(LZMA_DIR)/C
LZMA_CPP_DIR := $(LZMA_DIR)/CPP
SEVENZ_DIR := $(LZMA_CPP_DIR)/7zip
MINIZ_DIR := third_party/miniz

ARCHIVE_UNRAR_SRCS := \
	$(UNRAR_SRC_DIR)/strlist.cpp $(UNRAR_SRC_DIR)/strfn.cpp $(UNRAR_SRC_DIR)/pathfn.cpp $(UNRAR_SRC_DIR)/smallfn.cpp \
	$(UNRAR_SRC_DIR)/global.cpp $(UNRAR_SRC_DIR)/file.cpp $(UNRAR_SRC_DIR)/filefn.cpp $(UNRAR_SRC_DIR)/filcreat.cpp \
	$(UNRAR_SRC_DIR)/archive.cpp $(UNRAR_SRC_DIR)/arcread.cpp $(UNRAR_SRC_DIR)/unicode.cpp $(UNRAR_SRC_DIR)/system.cpp \
	$(UNRAR_SRC_DIR)/crypt.cpp $(UNRAR_SRC_DIR)/crc.cpp $(UNRAR_SRC_DIR)/rawread.cpp $(UNRAR_SRC_DIR)/encname.cpp \
	$(UNRAR_SRC_DIR)/resource.cpp $(UNRAR_SRC_DIR)/match.cpp $(UNRAR_SRC_DIR)/timefn.cpp $(UNRAR_SRC_DIR)/rdwrfn.cpp \
	$(UNRAR_SRC_DIR)/consio.cpp $(UNRAR_SRC_DIR)/options.cpp $(UNRAR_SRC_DIR)/errhnd.cpp $(UNRAR_SRC_DIR)/rarvm.cpp \
	$(UNRAR_SRC_DIR)/secpassword.cpp $(UNRAR_SRC_DIR)/rijndael.cpp $(UNRAR_SRC_DIR)/getbits.cpp $(UNRAR_SRC_DIR)/sha1.cpp \
	$(UNRAR_SRC_DIR)/sha256.cpp $(UNRAR_SRC_DIR)/blake2s.cpp $(UNRAR_SRC_DIR)/hash.cpp $(UNRAR_SRC_DIR)/extinfo.cpp \
	$(UNRAR_SRC_DIR)/extract.cpp $(UNRAR_SRC_DIR)/volume.cpp $(UNRAR_SRC_DIR)/list.cpp $(UNRAR_SRC_DIR)/find.cpp \
	$(UNRAR_SRC_DIR)/unpack.cpp $(UNRAR_SRC_DIR)/headers.cpp $(UNRAR_SRC_DIR)/threadpool.cpp $(UNRAR_SRC_DIR)/rs16.cpp \
	$(UNRAR_SRC_DIR)/cmddata.cpp $(UNRAR_SRC_DIR)/ui.cpp $(UNRAR_SRC_DIR)/largepage.cpp \
	$(UNRAR_SRC_DIR)/filestr.cpp $(UNRAR_SRC_DIR)/recvol.cpp $(UNRAR_SRC_DIR)/rs.cpp $(UNRAR_SRC_DIR)/scantree.cpp \
	$(UNRAR_SRC_DIR)/qopen.cpp $(UNRAR_SRC_DIR)/ps5_7z.cpp

ARCHIVE_SEVENZ_C_SRCS := \
	$(LZMA_C_DIR)/7zCrc.c $(LZMA_C_DIR)/7zCrcOpt.c $(LZMA_C_DIR)/7zStream.c $(LZMA_C_DIR)/Aes.c \
	$(LZMA_C_DIR)/Alloc.c $(LZMA_C_DIR)/Bcj2.c $(LZMA_C_DIR)/Bra.c $(LZMA_C_DIR)/Bra86.c \
	$(LZMA_C_DIR)/BraIA64.c $(LZMA_C_DIR)/CpuArch.c $(LZMA_C_DIR)/Delta.c $(LZMA_C_DIR)/Lzma2Dec.c \
	$(LZMA_C_DIR)/Lzma2DecMt.c $(LZMA_C_DIR)/LzmaDec.c $(LZMA_C_DIR)/MtCoder.c $(LZMA_C_DIR)/MtDec.c \
	$(LZMA_C_DIR)/Sha256.c $(LZMA_C_DIR)/SwapBytes.c $(LZMA_C_DIR)/Threads.c \
	$(UNRAR_SRC_DIR)/ps5_7z_stubs.c $(MINIZ_DIR)/miniz.c \
	$(MINIZ_DIR)/miniz_tdef.c $(MINIZ_DIR)/miniz_tinfl.c

ARCHIVE_SEVENZ_CPP_SRCS := \
	$(LZMA_CPP_DIR)/Common/CRC.cpp $(LZMA_CPP_DIR)/Common/IntToString.cpp \
	$(LZMA_CPP_DIR)/Common/MyWindows.cpp $(LZMA_CPP_DIR)/Common/MyString.cpp \
	$(LZMA_CPP_DIR)/Common/MyVector.cpp $(LZMA_CPP_DIR)/Common/StringConvert.cpp \
	$(LZMA_CPP_DIR)/Common/StringToInt.cpp $(LZMA_CPP_DIR)/Common/UTFConvert.cpp \
	$(LZMA_CPP_DIR)/Windows/PropVariant.cpp $(LZMA_CPP_DIR)/Windows/System.cpp \
	$(SEVENZ_DIR)/Common/CreateCoder.cpp $(SEVENZ_DIR)/Common/CWrappers.cpp \
	$(SEVENZ_DIR)/Common/FilterCoder.cpp $(SEVENZ_DIR)/Common/LimitedStreams.cpp \
	$(SEVENZ_DIR)/Common/MethodId.cpp $(SEVENZ_DIR)/Common/MethodProps.cpp \
	$(SEVENZ_DIR)/Common/ProgressUtils.cpp $(SEVENZ_DIR)/Common/PropId.cpp \
	$(SEVENZ_DIR)/Common/StreamBinder.cpp $(SEVENZ_DIR)/Common/StreamObjects.cpp \
	$(SEVENZ_DIR)/Common/StreamUtils.cpp $(SEVENZ_DIR)/Common/VirtThread.cpp \
	$(SEVENZ_DIR)/Archive/Common/CoderMixer2.cpp $(SEVENZ_DIR)/Archive/Common/DummyOutStream.cpp \
	$(SEVENZ_DIR)/Archive/Common/HandlerOut.cpp $(SEVENZ_DIR)/Archive/Common/InStreamWithCRC.cpp \
	$(SEVENZ_DIR)/Archive/Common/MultiStream.cpp $(SEVENZ_DIR)/Archive/Common/ParseProperties.cpp \
	$(SEVENZ_DIR)/Compress/Bcj2Coder.cpp $(SEVENZ_DIR)/Compress/Bcj2Register.cpp \
	$(SEVENZ_DIR)/Compress/BcjCoder.cpp $(SEVENZ_DIR)/Compress/BcjRegister.cpp \
	$(SEVENZ_DIR)/Compress/BranchMisc.cpp $(SEVENZ_DIR)/Compress/BranchRegister.cpp \
	$(SEVENZ_DIR)/Compress/ByteSwap.cpp $(SEVENZ_DIR)/Compress/CopyCoder.cpp \
	$(SEVENZ_DIR)/Compress/CopyRegister.cpp $(SEVENZ_DIR)/Compress/DeltaFilter.cpp \
	$(SEVENZ_DIR)/Compress/Lzma2Decoder.cpp $(SEVENZ_DIR)/Compress/Lzma2Register.cpp \
	$(SEVENZ_DIR)/Compress/LzmaDecoder.cpp $(SEVENZ_DIR)/Compress/LzmaRegister.cpp \
	$(SEVENZ_DIR)/Crypto/7zAes.cpp $(SEVENZ_DIR)/Crypto/7zAesRegister.cpp \
	$(SEVENZ_DIR)/Crypto/MyAes.cpp $(SEVENZ_DIR)/Crypto/MyAesReg.cpp \
	$(SEVENZ_DIR)/Archive/7z/7zCompressionMode.cpp $(SEVENZ_DIR)/Archive/7z/7zDecode.cpp \
	$(SEVENZ_DIR)/Archive/7z/7zExtract.cpp $(SEVENZ_DIR)/Archive/7z/7zHandler.cpp \
	$(SEVENZ_DIR)/Archive/7z/7zHeader.cpp $(SEVENZ_DIR)/Archive/7z/7zIn.cpp \
	$(SEVENZ_DIR)/Archive/7z/7zProperties.cpp $(SEVENZ_DIR)/Archive/7z/7zSpecStream.cpp

ASSETS := $(wildcard assets/*)
GEN_SRCS := $(patsubst assets/%,gen/assets/%,$(ASSETS:=.c))
APP_ASSETS := assets-app/param.json assets-app/icon0.png

BFPILOT_OBJS := $(patsubst %.c,build/bfpilot/%.o,$(WEB_SRCS) $(GEN_SRCS))
LAUNCHER_INSTALLER_OBJS := $(patsubst %.c,build/launcher-installer/%.o,$(LAUNCHER_INSTALLER_SRCS))
ARCHIVE_WORKER_OBJS := $(patsubst %.cpp,build/archive-worker/%.o,$(ARCHIVE_WORKER_SRCS) $(ARCHIVE_UNRAR_SRCS) $(ARCHIVE_SEVENZ_CPP_SRCS))
ARCHIVE_WORKER_OBJS += $(patsubst %.c,build/archive-worker/%.o,$(ARCHIVE_SEVENZ_C_SRCS))
BFPILOT_ARCHIVE_OBJS := $(patsubst %.cpp,build/bfpilot-archive/%.o,$(ARCHIVE_WORKER_SRCS) $(ARCHIVE_UNRAR_SRCS) $(ARCHIVE_SEVENZ_CPP_SRCS))
BFPILOT_ARCHIVE_OBJS += $(patsubst %.c,build/bfpilot-archive/%.o,$(ARCHIVE_SEVENZ_C_SRCS))

COMMON_CFLAGS := -Os -Wall -Werror -Isrc
COMMON_CFLAGS += -ffunction-sections -fdata-sections
COMMON_CFLAGS += -DVERSION_TAG=\"$(VERSION_TAG)\"
COMMON_CFLAGS += -DBUILD_VERSION=\"$(BUILD_VERSION)\"
COMMON_CFLAGS += -DBFPILOT_SDK_PATH=\"$(PS5_PAYLOAD_SDK)\"

# Integrated RAR/7z/ZIP extract (~1.2+ MiB of unrar+7z+miniz). Set 0 for lite ELF:
#   mingw32-make bfpilot ENABLE_ARCHIVE=0
ENABLE_ARCHIVE ?= 1

BFPILOT_CFLAGS := $(COMMON_CFLAGS)
BFPILOT_CFLAGS += -DBFPILOT_PAYLOAD_NAME=\"bfpilot\"
BFPILOT_CFLAGS += -DBFPILOT_BUILD_MODE=\"file-manager\"
BFPILOT_CFLAGS += -DBFPILOT_WEB_PORT=$(WEB_PORT)
BFPILOT_CFLAGS += -DBFPILOT_DEBUG_NOTIFICATIONS=0
BFPILOT_CFLAGS += -DBFPILOT_ENABLE_LAUNCHER=0
BFPILOT_CFLAGS += -DBFPILOT_DISABLE_LAUNCHER=1
BFPILOT_CFLAGS += -DBFPILOT_ENABLE_INTEGRATED_ARCHIVE=$(ENABLE_ARCHIVE)

ifeq ($(ENABLE_ARCHIVE),1)
BFPILOT_LINK_ARCHIVE_OBJS := $(BFPILOT_ARCHIVE_OBJS)
else
BFPILOT_LINK_ARCHIVE_OBJS :=
endif

LAUNCHER_INSTALLER_CFLAGS := $(COMMON_CFLAGS)
LAUNCHER_INSTALLER_CFLAGS += -DBFPILOT_PAYLOAD_NAME=\"bfpilot-launcher-installer\"
LAUNCHER_INSTALLER_CFLAGS += -DBFPILOT_BUILD_MODE=\"launcher-installer-force\"

COMMON_LDFLAGS := -Wl,--gc-sections
PRIVILEGED_APPINST_LDLIBS := -lkernel_sys -lSceSystemService
PRIVILEGED_APPINST_LDLIBS += -lSceUserService -lSceAppInstUtil
ARCHIVE_WORKER_DEFINES := -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_UNIX
ARCHIVE_WORKER_DEFINES += -DPS5_PAYLOAD -DSILENT -DRAR_SMP -DNDEBUG
ARCHIVE_SEVENZ_DEFINES := -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_UNIX
ARCHIVE_SEVENZ_DEFINES += -DZ7_EXTRACT_ONLY -DZ7_PROG_VARIANT_R -DZ7_AFFINITY_DISABLE
ARCHIVE_WORKER_INCLUDES := -I$(UNRAR_SRC_DIR) -I$(MINIZ_DIR)
ARCHIVE_WORKER_INCLUDES += -I$(LZMA_C_DIR) -I$(LZMA_CPP_DIR) -I$(SEVENZ_DIR)
ARCHIVE_WORKER_CXXFLAGS := -O3 -std=c++11 -Wall -fexceptions
ARCHIVE_WORKER_CXXFLAGS += -Wno-logical-op-parentheses -Wno-switch
ARCHIVE_WORKER_CXXFLAGS += -Wno-dangling-else -Wno-unused-parameter -Wno-reorder
ARCHIVE_WORKER_CXXFLAGS += -Wno-unused-function
ARCHIVE_WORKER_CXXFLAGS += -Wno-unused-variable -Wno-unused-but-set-variable
ARCHIVE_WORKER_CXXFLAGS += -Wno-missing-braces -Wno-nontrivial-memcall
ARCHIVE_WORKER_CFLAGS := -O3 -Wall -Wno-unused-parameter
ARCHIVE_WORKER_LDFLAGS := -pthread -lSceNotification

CC_CMD := $(PYTHON) compile_wrapper.py "$(CC)"
CXX_CMD := $(PYTHON) compile_wrapper.py "$(CXX)"
STRIP_CMD := "$(STRIP)"
LD_CMD := "$(LD)"
DEPLOY_CMD := "$(PS5_DEPLOY)"

ifeq ($(HOST_IS_WINDOWS),1)
CURDIR_POSIX := $(shell pwd)
PS5_PAYLOAD_SDK_POSIX := $(shell cygpath -u "$(PS5_PAYLOAD_SDK)" 2>/dev/null || printf '%s' "$(PS5_PAYLOAD_SDK)")
LLVM_CONFIG_POSIX := $(shell cygpath -u "$(LLVM_CONFIG)" 2>/dev/null || printf '%s' "$(LLVM_CONFIG)")
LLVM_BINDIR_POSIX := $(shell cygpath -u "$(LLVM_BINDIR)" 2>/dev/null || printf '%s' "$(LLVM_BINDIR)")
PS5_PAYLOAD_SDK_SHELL := $(subst \,/,$(PS5_PAYLOAD_SDK))
LLVM_CONFIG_SHELL := $(subst \,/,$(LLVM_CONFIG))
LLVM_BINDIR_SHELL := $(subst \,/,$(LLVM_BINDIR))
RUN_ENV := cd "$(CURDIR_POSIX)" && export LLVM_CONFIG="$(LLVM_CONFIG_SHELL)" && export LLVM_BINDIR="$(LLVM_BINDIR_SHELL)" && export PATH="$(LLVM_BINDIR_SHELL):$$PATH"
STRIP_CMD := "$(LLVM_BINDIR_SHELL)/llvm-strip"
LD_CMD := "$(PS5_PAYLOAD_SDK_SHELL)/win/prospero-lld.exe"
WINDOWS_LINK_PREFIX := --gc-sections --sysroot="$(PS5_PAYLOAD_SDK_SHELL)"
WINDOWS_LINK_PREFIX += -L"$(PS5_PAYLOAD_SDK_SHELL)/target/lib"
WINDOWS_LINK_PREFIX += -L"$(PS5_PAYLOAD_SDK_SHELL)/target/user/homebrew/lib"
WINDOWS_LINK_PREFIX += -l:crt1.o -l:crti.o -l:crtbegin.o -lc
WINDOWS_WEB_SUFFIX := -lkernel_web -lSceLibcInternal -lSceNet
WINDOWS_WEB_SUFFIX += -lc_stub_weak -lkernel_stub_weak
WINDOWS_WEB_SUFFIX += -l:crtend.o -l:crtn.o
WINDOWS_APPINST_SUFFIX := -lkernel_sys -lSceSystemService
WINDOWS_APPINST_SUFFIX += -lSceUserService -lSceAppInstUtil
WINDOWS_APPINST_SUFFIX += -lSceLibcInternal -lc_stub_weak -lkernel_stub_weak
WINDOWS_APPINST_SUFFIX += -l:crtend.o -l:crtn.o
WINDOWS_CXX_LD_CMD := "$(PS5_PAYLOAD_SDK_SHELL)/win/prospero-lld.exe"
WINDOWS_CXX_LINK_PREFIX := -m elf_x86_64_fbsd --sysroot="$(PS5_PAYLOAD_SDK_SHELL)"
WINDOWS_CXX_LINK_PREFIX += -pie --eh-frame-hdr --hash-style=sysv --build-id=uuid
WINDOWS_CXX_LINK_PREFIX += --unresolved-symbols=report-all -z now
WINDOWS_CXX_LINK_PREFIX += -z start-stop-visibility=hidden -z rodynamic
WINDOWS_CXX_LINK_PREFIX += -z common-page-size=0x4000 -z max-page-size=0x4000
WINDOWS_CXX_LINK_PREFIX += -z dead-reloc-in-nonalloc=.debug_*=0xffffffffffffffff
WINDOWS_CXX_LINK_PREFIX += -z dead-reloc-in-nonalloc=.debug_ranges=0xfffffffffffffffe
WINDOWS_CXX_LINK_PREFIX += -z dead-reloc-in-nonalloc=.debug_loc=0xfffffffffffffffe
WINDOWS_CXX_LINK_PREFIX += --default-script main.script --lto=full
WINDOWS_CXX_LINK_PREFIX += -plugin-opt=-emit-jump-table-sizes-section
WINDOWS_CXX_LINK_PREFIX += -L"$(PS5_PAYLOAD_SDK_SHELL)/target/lib"
WINDOWS_CXX_LINK_PREFIX += -L"$(PS5_PAYLOAD_SDK_SHELL)/target/user/homebrew/lib" -L.
WINDOWS_CXX_LINK_PREFIX += -l:crt1.o -l:crti.o -l:crtbegin.o
WINDOWS_CXX_LINK_PREFIX += -lunwind -lc++abi -lc++ -lc
WINDOWS_CXX_LINK_SUFFIX := -lSceNotification -lkernel_web -lSceLibcInternal
WINDOWS_CXX_LINK_SUFFIX += -lSceNet -lpthread -lc_stub_weak -lkernel_stub_weak
WINDOWS_CXX_LINK_SUFFIX += -l:crtend.o -l:crtn.o
define run
$(RUN_ENV) && $(1)
endef
else
define run
$(1)
endef
endif

all: bfpilot launcher-installer archive-worker

bfpilot: $(BFPILOT_BIN)

launcher-installer: $(LAUNCHER_INSTALLER_BIN)

archive-worker: $(ARCHIVE_WORKER_BIN)

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

build/archive-worker/%.o: %.cpp Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CXX_CMD) $(ARCHIVE_WORKER_CXXFLAGS) $(ARCHIVE_WORKER_DEFINES) $(ARCHIVE_SEVENZ_DEFINES) $(ARCHIVE_WORKER_INCLUDES) -DUNRAR -c $< -o $@)

build/archive-worker/%.o: %.c Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CC_CMD) $(ARCHIVE_WORKER_CFLAGS) $(ARCHIVE_WORKER_DEFINES) $(ARCHIVE_SEVENZ_DEFINES) $(ARCHIVE_WORKER_INCLUDES) -c $< -o $@)

build/bfpilot-archive/%.o: %.cpp Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CXX_CMD) $(ARCHIVE_WORKER_CXXFLAGS) $(ARCHIVE_WORKER_DEFINES) $(ARCHIVE_SEVENZ_DEFINES) $(ARCHIVE_WORKER_INCLUDES) -DUNRAR -DBFPILOT_ARCHIVE_NO_MAIN -DBFPILOT_ARCHIVE_INTEGRATED=1 -c $< -o $@)

build/bfpilot-archive/%.o: %.c Makefile
	$(call run,mkdir -p $(dir $@))
	$(call run,$(CC_CMD) $(ARCHIVE_WORKER_CFLAGS) $(ARCHIVE_WORKER_DEFINES) $(ARCHIVE_SEVENZ_DEFINES) $(ARCHIVE_WORKER_INCLUDES) -DBFPILOT_ARCHIVE_INTEGRATED=1 -c $< -o $@)

ifeq ($(HOST_IS_WINDOWS),1)
$(BFPILOT_BIN): $(BFPILOT_OBJS) $(if $(filter 1,$(ENABLE_ARCHIVE)),$(BFPILOT_ARCHIVE_OBJS))
	$(file >build/bfpilot-link.rsp,$(BFPILOT_OBJS) $(BFPILOT_LINK_ARCHIVE_OBJS))
	$(call run,$(WINDOWS_CXX_LD_CMD) -o $@ $(WINDOWS_CXX_LINK_PREFIX) @build/bfpilot-link.rsp $(WINDOWS_CXX_LINK_SUFFIX))
	$(call run,$(STRIP_CMD) --strip-all $@)
	$(call run,$(PYTHON) scripts/scrub_main_payload.py $@)

$(LAUNCHER_INSTALLER_BIN): $(LAUNCHER_INSTALLER_OBJS) $(APP_ASSETS)
	$(call run,$(LD_CMD) -o $@ $(WINDOWS_LINK_PREFIX) $(LAUNCHER_INSTALLER_OBJS) $(WINDOWS_APPINST_SUFFIX))
	$(call run,$(STRIP_CMD) --strip-all $@)

$(ARCHIVE_WORKER_BIN): $(ARCHIVE_WORKER_OBJS)
	$(file >build/archive-worker-link.rsp,$(ARCHIVE_WORKER_OBJS))
	$(call run,$(WINDOWS_CXX_LD_CMD) -o $@ $(WINDOWS_CXX_LINK_PREFIX) @build/archive-worker-link.rsp $(WINDOWS_CXX_LINK_SUFFIX))
	$(call run,$(STRIP_CMD) --strip-all $@)
else
$(BFPILOT_BIN): $(BFPILOT_OBJS) $(if $(filter 1,$(ENABLE_ARCHIVE)),$(BFPILOT_ARCHIVE_OBJS))
	$(call run,$(CXX_CMD) $(COMMON_LDFLAGS) -o $@ $(BFPILOT_OBJS) $(BFPILOT_LINK_ARCHIVE_OBJS) $(ARCHIVE_WORKER_LDFLAGS))
	$(call run,$(STRIP_CMD) --strip-all $@)
	$(call run,$(PYTHON) scripts/scrub_main_payload.py $@)

$(LAUNCHER_INSTALLER_BIN): $(LAUNCHER_INSTALLER_OBJS) $(APP_ASSETS)
	$(call run,$(CC_CMD) $(LAUNCHER_INSTALLER_CFLAGS) $(COMMON_LDFLAGS) -o $@ $(LAUNCHER_INSTALLER_OBJS) $(PRIVILEGED_APPINST_LDLIBS))
	$(call run,$(STRIP_CMD) --strip-all $@)

$(ARCHIVE_WORKER_BIN): $(ARCHIVE_WORKER_OBJS)
	$(call run,$(CXX_CMD) -o $@ $(ARCHIVE_WORKER_OBJS) $(ARCHIVE_WORKER_LDFLAGS))
	$(call run,$(STRIP_CMD) --strip-all $@)
endif

# Lite file-manager only (no integrated unrar/7z/miniz) — near historical ~200 KiB class.
bfpilot-lite:
	$(MAKE) bfpilot ENABLE_ARCHIVE=0 BUILD_VERSION=$(BUILD_VERSION)-lite

inspect-imports: all
	$(call run,bash scripts/inspect_imports.sh $(BFPILOT_BIN) $(LAUNCHER_INSTALLER_BIN) $(ARCHIVE_WORKER_BIN))

deploy-bfpilot: bfpilot
	$(call run,$(DEPLOY_CMD) -h $(PS5_HOST) -p $(PS5_PORT) $(BFPILOT_BIN))

deploy-launcher-installer: launcher-installer
	$(call run,$(DEPLOY_CMD) -h $(PS5_HOST) -p $(PS5_PORT) $(LAUNCHER_INSTALLER_BIN))

ps5-diag:
	$(call run,$(PYTHON) scripts/ps5_diag.py)

ps5-smoke:
	$(call run,$(PYTHON) scripts/ps5_smoke.py)

ps5-storage-audit:
	$(call run,$(PYTHON) scripts/ps5_storage_audit.py)

ps5-archive-perf:
	$(call run,$(PYTHON) scripts/ps5_archive_perf.py)

clean:
	$(call run,rm -rf $(BFPILOT_BIN) $(LAUNCHER_INSTALLER_BIN) $(ARCHIVE_WORKER_BIN) build gen)

.SECONDARY: $(GEN_SRCS)
.PHONY: all bfpilot bfpilot-lite launcher-installer archive-worker inspect-imports clean deploy-bfpilot deploy-launcher-installer ps5-diag ps5-smoke ps5-storage-audit ps5-archive-perf
