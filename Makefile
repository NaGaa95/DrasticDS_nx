#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	$(notdir $(CURDIR))
APP_TITLE	:=	Drastic DS
APP_AUTHOR	:=	naga
APP_VERSION	:=	1.1.1
BUILD		:=	build
SOURCES		:=	source source/hooks source/switch
DATA		:=	data
INCLUDES	:=	source source/switch
DFX_GENERATED ?=
ifneq ($(strip $(DFX_GENERATED)),)
DATA		+=	$(DFX_GENERATED)/data
INCLUDES	+=	$(DFX_GENERATED)/include
endif
STORAGE_BUILD ?= $(TOPDIR)/launcher/dependencies/build
LIBSMB2_INCLUDE ?= $(STORAGE_BUILD)/_deps/libsmb2-src/include
LIBUSBHSFS_INCLUDE ?= $(STORAGE_BUILD)/_deps/libusbhsfs-src/include
MESA_SDK ?= $(TOPDIR)/mesa-switch-sdk

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
OPTIMIZATION := -O3 -flto=auto

# __SWITCH__ for libnx; DRASTIC_NX gates the port-specific host branches.
DEFINES	:=	-D__SWITCH__ -DDRASTIC_NX -DDRASTIC_NX_VERSION='"$(APP_VERSION)"'
ifneq ($(strip $(DFX_GENERATED)),)
DEFINES	+=	-DDRASTIC_DFX_GENERATED
endif

# --- unified renderer host --------------------------------------------------
# The Horizon Mesa SDK provides native NVC0 OpenGL, Zink-on-NVK and loaderless
# Vulkan in one coherent static build.  All renderer implementations are linked
# once and the host selects Vulkan or EGL at runtime from drastic.ini.
DEFINES	+=	-DUSE_VULKAN -DUSE_OPENGL -DUSE_UNIFIED_RENDERER \
			-DVK_USE_PLATFORM_VI_NN
VULKAN_INCLUDE ?= $(MESA_SDK)/include
SOURCES	+=	source/lsfg \
			third_party/lsfg-vk/lsfg-vk-common/src/helpers \
			third_party/lsfg-vk/lsfg-vk-common/src/vulkan \
			third_party/lsfg-vk/lsfg-vk-backend/src \
			third_party/lsfg-vk/lsfg-vk-backend/src/extraction \
			third_party/lsfg-vk/lsfg-vk-backend/src/helpers \
			third_party/lsfg-vk/lsfg-vk-backend/src/shaderchains
INCLUDES	+=	source/lsfg \
			$(VULKAN_INCLUDE) \
			third_party/lsfg-vk/lsfg-vk-common/include \
			third_party/lsfg-vk/lsfg-vk-backend/include \
			third_party/lsfg-vk/lsfg-vk-backend/src

CFLAGS	:=	-Wall -Wextra $(OPTIMIZATION) -DNDEBUG -ffunction-sections -fdata-sections \
			-fno-ident -ffile-prefix-map=$(CURDIR)=. \
			-fmacro-prefix-map=$(CURDIR)=. $(ARCH) $(DEFINES)
CFLAGS	+=	$(INCLUDE)
CXXFLAGS	:= $(CFLAGS) -Wno-missing-field-initializers -std=gnu++20

ASFLAGS	:=	$(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) $(OPTIMIZATION) -Wl,-Map,$(notdir $*.map) \
			-Wl,--gc-sections -Wl,--build-id=sha1

STORAGE_LIBS := $(STORAGE_BUILD)/_deps/libsmb2-build/lib/libsmb2.a \
				$(STORAGE_BUILD)/_deps/libusbhsfs-build/liblibusbhsfs.a

# nx supplies audren, HID, applet, and filesystem services. DraStic's OpenSL ES
# ABI is implemented directly by the audren-backed source/opensles.c layer.
# Unified EGL embeds both NVC0 and Zink, while the Vulkan renderer calls the
# same loaderless NVK archive directly. Keep the complete static dependency set
# in one rescan group so both runtime paths resolve from a single executable.
LIBDIRS	:= $(MESA_SDK) $(PORTLIBS) $(LIBNX)
LIBS	:= -Wl,-u,vk_icdGetInstanceProcAddr \
		-Wl,-u,vk_icdNegotiateLoaderICDInterfaceVersion -pthread \
		-Wl,--start-group $(STORAGE_LIBS) -lminizip \
		-l:libGLESv2.a -l:libEGL.a -l:libvulkan.a -l:libglapi.a \
		-l:libmesa_util_c11.a -l:libblake3.a -l:libmesa_util.a \
		-l:libmesa_util_simd.a -l:libxmlconfig.a \
		-lelf -lexpat -lzstd -lz -lnx -lstdc++ -lm \
		-Wl,--end-group

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

absolute_or_local = $(if $(filter /%,$(1)),$(1),$(CURDIR)/$(1))
export VPATH	:=	$(foreach dir,$(SOURCES),$(call absolute_or_local,$(dir))) \
			$(foreach dir,$(DATA),$(call absolute_or_local,$(dir)))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# link with g++ so mesa's C++ EGL/GLES pulls in libstdc++
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(call absolute_or_local,$(dir))) \
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
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf \
		$(TARGET)_gl.nro $(TARGET)_gl.elf $(TARGET)_gl.map \
		$(TARGET)_vk.nro $(TARGET)_vk.elf $(TARGET)_vk.map \
		$(TARGET)_zink.nro $(TARGET)_zink.elf $(TARGET)_zink.map \
		DrasticDS.nro vulkan
	@rm -f *.o

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
