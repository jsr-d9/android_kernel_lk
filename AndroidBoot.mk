#Android makefile to build lk bootloader as a part of Android Build

# Set flags if we need to include security libs
ifeq ($(TARGET_BOOTIMG_SIGNED),true)
  SIGNED_KERNEL := SIGNED_KERNEL=1
else
  SIGNED_KERNEL := SIGNED_KERNEL=0
endif

ifneq ($(strip $(TARGET_BOOTLOADER_PLATFORM_OVERRIDE)),)
  BOOTLOADER_PLATFORM := $(TARGET_BOOTLOADER_PLATFORM_OVERRIDE)
else
  ifeq ($(call is-board-platform,msm8660),true)
    BOOTLOADER_PLATFORM := msm8660_surf
  else
    BOOTLOADER_PLATFORM := $(TARGET_BOARD_PLATFORM)
  endif
endif

ifeq ($(TARGET_BUILD_VARIANT),user)
  USER_SYSTEM := USER_SYSTEM=1
else
  USER_SYSTEM := USER_SYSTEM=0
endif

# NAND variant output
TARGET_NAND_BOOTLOADER := $(PRODUCT_OUT)/appsboot.mbn
NAND_BOOTLOADER_OUT := $(TARGET_OUT_INTERMEDIATES)/NAND_BOOTLOADER_OBJ

# Remove bootloader binary to trigger recompile when source changes
appsbootldr_clean:
	$(hide) rm -f $(TARGET_NAND_BOOTLOADER)

$(NAND_BOOTLOADER_OUT): appsbootldr_clean
	mkdir -p $(NAND_BOOTLOADER_OUT)

# eMMC variant output
TARGET_EMMC_BOOTLOADER := $(PRODUCT_OUT)/emmc_appsboot.mbn
EMMC_BOOTLOADER_OUT := $(TARGET_OUT_INTERMEDIATES)/EMMC_BOOTLOADER_OBJ

emmc_appsbootldr_clean:
	$(hide) rm -f $(TARGET_EMMC_BOOTLOADER)

$(EMMC_BOOTLOADER_OUT): emmc_appsbootldr_clean
	mkdir -p $(EMMC_BOOTLOADER_OUT)

# Top level for NAND variant targets
$(TARGET_NAND_BOOTLOADER): $(NAND_BOOTLOADER_OUT)
	$(MAKE) -C bootable/bootloader/lk BOOTLOADER_OUT=../../../$(NAND_BOOTLOADER_OUT) $(BOOTLOADER_PLATFORM) $(SIGNED_KERNEL)

# Top level for eMMC variant targets
$(TARGET_EMMC_BOOTLOADER): $(EMMC_BOOTLOADER_OUT)
	$(MAKE) -C bootable/bootloader/lk BOOTLOADER_OUT=../../../$(EMMC_BOOTLOADER_OUT) $(BOOTLOADER_PLATFORM) EMMC_BOOT=1 $(SIGNED_KERNEL)  BOARD_USERDATAIMAGE_PARTITION_SIZE=$(BOARD_USERDATAIMAGE_PARTITION_SIZE) BOARD_CACHEIMAGE_PARTITION_SIZE=$(BOARD_CACHEIMAGE_PARTITION_SIZE)

# Keep build NAND & eMMC as default for targets still using TARGET_BOOTLOADER
TARGET_BOOTLOADER := $(PRODUCT_OUT)/EMMCBOOT.MBN
$(TARGET_BOOTLOADER): $(NAND_BOOTLOADER_OUT) $(EMMC_BOOTLOADER_OUT) | $(TARGET_NAND_BOOTLOADER) $(TARGET_EMMC_BOOTLOADER)

#
# Build nandwrite as a part of Android Build for NAND configurations
#
TARGET_NANDWRITE := $(PRODUCT_OUT)/obj/nandwrite/build-$(BOOTLOADER_PLATFORM)_nandwrite/lk
NANDWRITE_OUT := $(TARGET_OUT_INTERMEDIATES)/nandwrite

nandwrite_clean:
	$(hide) rm -f $(TARGET_NANDWRITE)
	$(hide) rm -rf $(NANDWRITE_OUT)

$(NANDWRITE_OUT): nandwrite_clean
	mkdir -p $(NANDWRITE_OUT)

$(TARGET_NANDWRITE): $(NANDWRITE_OUT)
	@echo $(BOOTLOADER_PLATFORM)_nandwrite
	$(MAKE) -C bootable/bootloader/lk BOOTLOADER_OUT=../../../$(NANDWRITE_OUT) $(BOOTLOADER_PLATFORM)_nandwrite BUILD_NANDWRITE=1
