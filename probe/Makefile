# sdt_probe: hardware probe for the Sonic Dream Team port.
# Answers two questions on the console before port code depends on the answers:
#   1. which code-memory permission changes Horizon allows (the game's loader
#      writes code into mapped memory and then makes it executable in place);
#   2. what the NVK Vulkan driver offers (version, features, formats, extensions).
# Build:  make NVK_SDK=/path/to/mesa-sdk/opt/devkitpro/portlibs/switch
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules
NVK_SDK ?= $(TOPDIR)/../../work/mesa/opt/devkitpro/portlibs/switch
TARGET := sdt_probe
BUILD  := build
APP_TITLE := SDT probe
APP_AUTHOR := sdt_nx
APP_VERSION := 0.1
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -O2 -ffunction-sections $(ARCH) -D__SWITCH__ -DVK_USE_PLATFORM_VI_NN \
          -I$(NVK_SDK)/include -I$(LIBNX)/include
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) \
          -Wl,--allow-multiple-definition -Wl,--no-relax
ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(CURDIR)/source
export DEPSDIR := $(CURDIR)/$(BUILD)
export OFILES := probe.o
export LD := $(CXX)
export NVK_SDK
.PHONY: all clean
all:
	@mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
# The SDK archive's index leaves out its Rust members: index a copy and link it
# in a group (the Rust objects reference each other in every order).
LIBS := -Wl,--start-group nvk/libvulkan.a -L$(PORTLIBS)/lib -lexpat -lzstd -lz -L$(LIBNX)/lib -lnx -lm -Wl,--end-group
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES) nvk/libvulkan.a
nvk/libvulkan.a: $(NVK_SDK)/lib/libvulkan.a
	@mkdir -p nvk && cp $< $@ && $(AR:ar=ranlib) $@
-include $(OFILES:.o=.d)
endif
