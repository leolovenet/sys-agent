.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET		:=	sys-botbase
BUILD		:=	build
SOURCES		:=	source
FTP_SOURCE_DIR := $(TOPDIR)/../third_party/ftpsrv/src
DATA		:=	data
INCLUDES	:=	include
EXEFS_SRC	:=	exefs_src
ATMOSPHERE_TITLE_ID := 430000000000000B
SD_LAYOUT_DIR := $(CURDIR)/$(ATMOSPHERE_TITLE_ID)

ARCH	:=	-march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__
CFLAGS  +=  -DFTP_PATHNAME_SIZE=769 -DFTP_MAX_SESSIONS=4 -DFTP_FILE_BUFFER_SIZE=65536 \
			-DFTP_VFS_HEADER='"ftp_vfs_sd.h"' \
			-DFTP_SOCKET_HEADER='"platform/nx/socket_nx.h"' \
			-DHAVE_LSTAT=0 -DHAVE_READLINK=0 -DHAVE_GETPWUID=0 -DHAVE_GETGRGID=0 \
			-DHAVE_STRNCASECMP=1 -DHAVE_LOCALTIME_R=1 -DHAVE_GMTIME_R=1 -DHAVE_POLL=1 \
			-DHAVE_IPTOS_THROUGHPUT=0 -DHAVE_TCP_NODELAY=1 -DHAVE_SO_KEEPALIVE=1 \
			-DHAVE_SO_REUSEADDR=1 \
			-DFTPSRV_VERSION_MAJOR=1 -DFTPSRV_VERSION_MINOR=2 -DFTPSRV_VERSION_PATCH=2 \
			-DFTPSRV_VERSION_STR='"1.2.2"' -DFTPSRV_VERSION_HASH='"1.2.2-7c82402"'

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lm -lnx

LIBDIRS	:= $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
			$(FTP_SOURCE_DIR)

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CFILES		+=	ftpsrv.c
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(TOPDIR)/source \
			-I$(FTP_SOURCE_DIR) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export BUILD_EXEFS_SRC := $(TOPDIR)/$(EXEFS_SRC)

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

.PHONY: $(BUILD) clean all sd-layout

all: sd-layout

sd-layout: $(BUILD)
	@mkdir -p "$(SD_LAYOUT_DIR)/flags"
	@[ -e "$(SD_LAYOUT_DIR)/flags/boot2.flag" ] || touch "$(SD_LAYOUT_DIR)/flags/boot2.flag"
	@cp -f "$(TARGET).nsp" "$(SD_LAYOUT_DIR)/exefs.nsp"
	@echo updated ... $(ATMOSPHERE_TITLE_ID)/exefs.nsp

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).npdm $(TARGET).nso $(TARGET).elf "$(SD_LAYOUT_DIR)"

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nsp

ifeq ($(strip $(APP_JSON)),)
$(OUTPUT).nsp	:	$(OUTPUT).nso
else
$(OUTPUT).nsp	:	$(OUTPUT).nso $(OUTPUT).npdm
endif

$(OUTPUT).nso	:	$(OUTPUT).elf

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
