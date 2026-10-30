#---------------------------------------------------------------------------------
# 3DS music player - based on the devkitPro 3DS application template
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET		:=	YoRHA-3ds-MP
BUILD		:=	build
SOURCES		:=	source
INCLUDES	:=	include lib
DATA		:=	data

APP_TITLE	:=	Music Player
APP_DESCRIPTION	:=	YoRHA-3ds-MP
APP_AUTHOR	:=	YoRHa No.001 Type M
APP_PRODUCT_CODE	:=	CTR-H-MUSP
APP_UNIQUE_ID	:=	0xF5A4E
APP_VERSION	:=	1.0.0

ICON		:=	resources/icon.png
BANNER_IMAGE	:=	resources/banner.png
BANNER_AUDIO	:=	resources/banner.wav

VERSION_PARTS	:=	$(subst ., ,$(APP_VERSION))

ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations -ffunction-sections \
			$(ARCH) $(INCLUDE) -D__3DS__ -DAPP_VERSION=\"$(APP_VERSION)\"

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:=	-lturbojpeg -lcitro2d -lcitro3d -lctru -lm
LIBDIRS	:=	$(PORTLIBS) $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
PNGFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.png)))

export LD	:=	$(CC)
export OFILES_BIN	:=	$(PNGFILES:.png=.png.o)
export OFILES_SRC	:=	$(CFILES:.c=.o)
export OFILES	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES	:=	$(PNGFILES:.png=_png.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON	:=	$(CURDIR)/$(ICON)
export _3DSXDEPS	:=	$(OUTPUT).smdh
export _3DSXFLAGS	+=	--smdh=$(OUTPUT).smdh

.PHONY: all clean cia

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

cia: all
	@bannertool makebanner -i $(BANNER_IMAGE) -a $(BANNER_AUDIO) -o $(BUILD)/banner.bnr >/dev/null
	@bannertool makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" \
		-i $(ICON) -o $(BUILD)/icon.icn >/dev/null
	@makerom -f cia -o $(TARGET).cia -elf $(TARGET).elf -rsf cia.rsf -exefslogo \
		-icon $(BUILD)/icon.icn -banner $(BUILD)/banner.bnr \
		-major $(word 1,$(VERSION_PARTS)) -minor $(word 2,$(VERSION_PARTS)) -micro $(word 3,$(VERSION_PARTS)) \
		-DAPP_TITLE="$(APP_TITLE)" -DAPP_PRODUCT_CODE=$(APP_PRODUCT_CODE) -DAPP_UNIQUE_ID=$(APP_UNIQUE_ID)
	@echo built ... $(TARGET).cia

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).cia $(OUTPUT).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)
$(OUTPUT).elf	:	$(OFILES)

# The C files include the generated headers, so build the images first.
$(OFILES_SRC)	:	$(HFILES)
main.o		:	$(TOPDIR)/Makefile

#---------------------------------------------------------------------------------
# embedded images: data/foo.png becomes foo_png[] / foo_png_size in foo_png.h
#---------------------------------------------------------------------------------
%.png.o	%_png.h :	%.png
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
