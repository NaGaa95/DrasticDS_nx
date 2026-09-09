#---------------------------------------------------------------------------------
# Drastic DS launcher -- the complete SDL GUI adapted for Nintendo DS.
# It writes drastic.ini and chainloads one unified Vulkan/OpenGL host via
# envSetNextLoad. The host selects NVK, native NVC0 or Zink at runtime. Kept OUT of the emulator
# source tree so its SDL2/Mesa link never executes the Drastic core.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET		:=	DrasticDS
BUILD		:=	build
SOURCES		:=	source ../source/switch
DATA		:=	data
INCLUDES	:=	include ../source ../source/switch
STORAGE_BUILD ?= $(TOPDIR)/dependencies/build
LIBSMB2_INCLUDE ?= $(STORAGE_BUILD)/_deps/libsmb2-src/include
LIBUSBHSFS_INCLUDE ?= $(STORAGE_BUILD)/_deps/libusbhsfs-src/include
ROMFS_DIR	?=	$(CURDIR)/romfs	# build_all.sh overrides this with a temporary directory

APP_TITLE	:=	Drastic DS
APP_AUTHOR	:=	naga
APP_VERSION	:=	1.1.1

#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
OPTIMIZATION := -O3 -flto=auto

PKGS	:=	sdl2 SDL2_ttf SDL2_image libcurl

CFLAGS	:=	`$(PREFIX)pkg-config --cflags $(PKGS)` -Wall -Wextra $(OPTIMIZATION) -DNDEBUG \
			-ffunction-sections -fdata-sections -fno-ident \
			-ffile-prefix-map=$(CURDIR)=. -fmacro-prefix-map=$(CURDIR)=. \
			$(ARCH) $(DEFINES)
CFLAGS	+=	$(INCLUDE) -D__SWITCH__ -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 \
			-DDRASTIC_NX_VERSION='"$(APP_VERSION)"'

ifeq ($(UPDATE_TEST),1)
CFLAGS += -DDRASTIC_NX_UPDATE_TEST
endif

CXXFLAGS	:= $(CFLAGS) -fno-rtti -std=gnu++17

ASFLAGS	:=	$(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) $(OPTIMIZATION) -Wl,-Map,$(notdir $*.map) \
			-Wl,--gc-sections -Wl,--build-id=none

# SDL2_image (covers) + libcurl (SteamGridDB; libnx TLS backend, no cacert needed) +
# turbojpeg (forwarder NACP icon encoding).
STORAGE_LIBS := $(STORAGE_BUILD)/_deps/libsmb2-build/lib/libsmb2.a \
				$(STORAGE_BUILD)/_deps/libusbhsfs-build/liblibusbhsfs.a
LIBS	:=	$(STORAGE_LIBS) `$(PREFIX)pkg-config --libs $(PKGS)` -lturbojpeg -lz -lGLESv2 -lnx

#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
			$(CURDIR)/../source

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(filter-out SwitchStorageBridge.cpp,$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp))))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ and libEGL dependent projects
#---------------------------------------------------------------------------------
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(LIBSMB2_INCLUDE) -I$(LIBUSBHSFS_INCLUDE) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(strip $(ROMFS_DIR)),)
	export NROFLAGS += --romfsdir=$(ROMFS_DIR)
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
	@rm -fr romfs/cores romfs/emu romfs/fwd romfs/res

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
