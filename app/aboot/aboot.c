/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2012 The Linux Foundation. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Code Aurora nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <app.h>
#include <debug.h>
#include <arch/arm.h>
#include <dev/udc.h>
#include <string.h>
#include <kernel/thread.h>
#include <arch/ops.h>

#include <dev/flash.h>
#include <lib/ptable.h>
#include <dev/keys.h>
#include <dev/fbcon.h>
#include <baseband.h>
#include <target.h>
#include <mmc.h>
#include <partition_parser.h>
#include <platform.h>
#include <crypto_hash.h>
#include <smem.h> //ML

#if DEVICE_TREE
#include <libfdt.h>
#endif

#include "image_verify.h"
#include "recovery.h"
#include "bootimg.h"
#include "fastboot.h"
#include "sparse_format.h"
#include "mmc.h"
#include "devinfo.h"
#include "board.h"

#include "scm.h"

#define EXPAND(NAME) #NAME
#define TARGET(NAME) EXPAND(NAME)
#define DEFAULT_CMDLINE "mem=100M console=null";

#ifdef MEMBASE
#define EMMC_BOOT_IMG_HEADER_ADDR (0xFF000+(MEMBASE))
#else
#define EMMC_BOOT_IMG_HEADER_ADDR 0xFF000
#endif

#define RECOVERY_MODE   0x77665502
#define FASTBOOT_MODE   0x77665500

#if DEVICE_TREE
#define DEV_TREE_SUCCESS        0
#define DEV_TREE_MAGIC          "QCDT"
#define DEV_TREE_VERSION        1
#define DEV_TREE_HEADER_SIZE    12


struct dt_entry{
	uint32_t platform_id;
	uint32_t variant_id;
	uint32_t soc_rev;
	uint32_t offset;
	uint32_t size;
};

struct dt_table{
	uint32_t magic;
	uint32_t version;
	unsigned num_entries;
};
struct dt_entry * get_device_tree_ptr(struct dt_table *);
int update_device_tree(const void *, char *, void *, unsigned);
#endif

static const char *emmc_cmdline = " androidboot.emmc=true";
static const char *usb_sn_cmdline = " androidboot.serialno=";
static const char *boot_up_mode_charger		   = " androidboot.mode=charger";
static const char *boot_up_mode_fastmmi_pcba   = " androidboot.mode=fastmmi_pcba";
static const char *boot_up_mode_fastmmi_full   = " androidboot.mode=fastmmi_full";
static const char *boot_up_mode_normal   	   = " androidboot.mode=normal";
static const char *boot_up_mode_recovery       = " androidboot.mode=recovery";
static const char *boot_up_mode_ftm            = " androidboot.mode=ftm";

static const char *auth_kernel = " androidboot.authorized_kernel=true";

static const char *reboot_mode_normal = " reboot=h";
static const char *reboot_mode_recovery = " reboot=i";

static const char *baseband_apq     = " androidboot.baseband=apq";
static const char *baseband_msm     = " androidboot.baseband=msm";
static const char *baseband_csfb    = " androidboot.baseband=csfb";
static const char *baseband_svlte2a = " androidboot.baseband=svlte2a";
static const char *baseband_mdm     = " androidboot.baseband=mdm";
static const char *baseband_sglte   = " androidboot.baseband=sglte";

/* Assuming unauthorized kernel image by default */
static int auth_kernel_img = 0;

/* TODO: The size is hard coded */
static unsigned char cpr_buf[4096];

static device_info device = {DEVICE_MAGIC, 0, 0};

static struct udc_device surf_udc_device = {
	.vendor_id	= 0x18d1,
	.product_id	= 0xD00D,
	.version_id	= 0x0100,
	.manufacturer	= "Google",
	.product	= "Android",
};

struct atag_ptbl_entry
{
	char name[16];
	unsigned offset;
	unsigned size;
	unsigned flags;
};

enum cpr_status {
	CPR_DISABLED = 0,
	CPR_ENABLED,
	CPR_STATUS,
	CPR_UNKNOWN
};

enum cpr_cmd {
	ENABLE_CPR = 0,
	DISABLE_CPR,
	GET_CPR_STATUS
};

/* The misc partition layout */
enum misc_region_type {
	TYPE_BOOT_REASON = 0,
	TYPE_SWITCH_CTL,
	TYPE_LOG,
	TYPE_CPR,
	TYPE_RESERVE,
};

enum misc_region_mode {
	MODE_RDONLY = 0,
	MODE_WRONLY,
	MODE_RW,
	MODE_UNKNOWN
};


/* Struct to describe partition regions */
struct ptn_rgn {
	int type; // Usage purpose
	int mode; // Operation mode
	unsigned long long offset; // Offset of the region
	unsigned long long size; // Size of the region
	int reserve[2]; // 8 bytes reserve space
};

#define CPR_FLAG0 0x900df1a9 // good flag
#define CPR_FLAG1 0x45525043 // CPRE


#define SZ_MISC_TOTAL 0x100000 // 1M
#define SZ_MISC_BOOT_REASON 0x4000 // 16K
#define SZ_MISC_SWITCH_CTL 0x4000
#define SZ_MISC_LOG 0x4000
#define SZ_MISC_CPR 0x1000 // 4K
#define SZ_MISC_RESERVE 0x0F3000 // 972K

#if (SZ_MISC_BOOT_REGION + SZ_MISC_SWITCH_CTL + SZ_MISC_LOG + SZ_MISC_CPR) > SZ_MISC_TOTAL
#error misc partition exceeded its max size
#endif

static struct ptn_rgn misc_regions[] = {
	{
		.type = TYPE_BOOT_REASON,
		.mode = MODE_RW,
		.offset = 0,
		.size = SZ_MISC_BOOT_REASON // 16K
	},
	{
		.type = TYPE_SWITCH_CTL,
		.mode = MODE_RW,
		.offset = SZ_MISC_BOOT_REASON, // 16K
		.size = SZ_MISC_SWITCH_CTL // 16K
	},
	{
		.type = TYPE_LOG,
		.mode = MODE_RW,
		.offset = SZ_MISC_BOOT_REASON + SZ_MISC_SWITCH_CTL, // 32K
		.size = SZ_MISC_LOG // 16K
	},
	{
		.type = TYPE_CPR,
		.mode = MODE_RW,
		.offset = SZ_MISC_BOOT_REASON + SZ_MISC_SWITCH_CTL + SZ_MISC_LOG, //48K
		.size = SZ_MISC_CPR
	},
	{
		.type = TYPE_RESERVE,
		.mode = MODE_RW,
		.offset = SZ_MISC_BOOT_REASON + SZ_MISC_SWITCH_CTL +
			SZ_MISC_LOG + SZ_MISC_CPR,
		.size = SZ_MISC_RESERVE
	}
};

struct cpr_status_info
{
	unsigned int flag[2];
	unsigned int status;
	int reserve;
};



char sn_buf[13];

extern int emmc_recovery_init(void);

#if NO_KEYPAD_DRIVER
extern int fastboot_trigger(void);
#endif

boot_mode_type get_boot_mode_from_misc()
{
	int res;
	struct boot_mode_message msg;
	res = emmc_get_fastmmi_msg(&msg);
	if (res < 0){
		dprintf(CRITICAL,"emmc get fastmmi msg failure\n");
		return BOOT_MODE_NORMAL;
	}

	if(msg.magic == BOOT_MODE_MAGIN_NUM){
		return msg.boot_mode;
	}else{
		return BOOT_MODE_NORMAL;
	}
}

boot_mode_type get_boot_mode()
{
	unsigned res;
	boot_mode_type bootmode;
	bootmode = get_boot_mode_from_misc();

	if(bootmode == BOOT_MODE_FASTMMI_PCBA){
		return BOOT_MODE_FASTMMI_PCBA;
	}else if(bootmode == BOOT_MODE_FASTMMI_FULL){
		return BOOT_MODE_FASTMMI_FULL;
	}
	
	res = target_pause_for_battery_charge();
	if(boot_into_recovery == 1){
		return BOOT_MODE_RECOVERY;
	}else if (res == PWR_ON_EVENT_FTM) {
		return BOOT_MODE_FTM;
	}else if (res == PWR_ON_EVENT_USB_CHG){
		return BOOT_MODE_USB_CHG;
	}
	return bootmode;
}

static void ptentry_to_tag(unsigned **ptr, struct ptentry *ptn)
{
	struct atag_ptbl_entry atag_ptn;

	memcpy(atag_ptn.name, ptn->name, 16);
	atag_ptn.name[15] = '\0';
	atag_ptn.offset = ptn->start;
	atag_ptn.size = ptn->length;
	atag_ptn.flags = ptn->flags;
	memcpy(*ptr, &atag_ptn, sizeof(struct atag_ptbl_entry));
	*ptr += sizeof(struct atag_ptbl_entry) / sizeof(unsigned);
}

unsigned char *update_cmdline(const char * cmdline)
{
	int cmdline_len = 0;
	int have_cmdline = 0;
	unsigned char *cmdline_final = NULL;
	boot_mode_type boot_mode;

	if (cmdline && cmdline[0]) {
		cmdline_len = strlen(cmdline);
		have_cmdline = 1;
	}
	if (target_is_emmc_boot()) {
		cmdline_len += strlen(emmc_cmdline);
	}

	cmdline_len += strlen(usb_sn_cmdline);
	cmdline_len += strlen(sn_buf);

	boot_mode = get_boot_mode();

	switch(boot_mode)
	{
		case BOOT_MODE_FASTMMI_PCBA:
			cmdline_len += strlen(boot_up_mode_fastmmi_pcba);
			break;
		case BOOT_MODE_FASTMMI_FULL:
			cmdline_len += strlen(boot_up_mode_fastmmi_full);
			break;
		case BOOT_MODE_FTM:
			cmdline_len += strlen(boot_up_mode_ftm);
			break;
		case BOOT_MODE_USB_CHG:
			cmdline_len += strlen(boot_up_mode_charger);
			break;
		case BOOT_MODE_RECOVERY:
			cmdline_len += strlen(boot_up_mode_recovery);
			break;
		case BOOT_MODE_BOOTLOADER:
			break;
			
		case BOOT_MODE_NORMAL:
		default:
			cmdline_len += strlen(boot_up_mode_normal);
	}

	if(target_use_signed_kernel() && auth_kernel_img) {
		cmdline_len += strlen(auth_kernel);
	}

	/* Determine correct androidboot.baseband to use */
	switch(target_baseband())
	{
		case BASEBAND_APQ:
			cmdline_len += strlen(baseband_apq);
			break;

		case BASEBAND_MSM:
			cmdline_len += strlen(baseband_msm);
			break;

		case BASEBAND_CSFB:
			cmdline_len += strlen(baseband_csfb);
			break;

		case BASEBAND_SVLTE2A:
			cmdline_len += strlen(baseband_svlte2a);
			break;

		case BASEBAND_MDM:
			cmdline_len += strlen(baseband_mdm);
			break;

		case BASEBAND_SGLTE:
			cmdline_len += strlen(baseband_sglte);
			break;
	}

	if (cmdline_len > 0) {
		const char *src;
		char *dst = malloc((cmdline_len + 4) & (~3));
		assert(dst != NULL);

		/* Save start ptr for debug print */
		cmdline_final = dst;
		if (have_cmdline) {
			src = cmdline;
			while ((*dst++ = *src++));
		}
		if (target_is_emmc_boot()) {
			src = emmc_cmdline;
			if (have_cmdline) --dst;
			have_cmdline = 1;
			while ((*dst++ = *src++));
		}

		src = usb_sn_cmdline;
		if (have_cmdline) --dst;
		have_cmdline = 1;
		while ((*dst++ = *src++));
		src = sn_buf;
		if (have_cmdline) --dst;
		have_cmdline = 1;
		while ((*dst++ = *src++));

		if(target_use_signed_kernel() && auth_kernel_img) {
			src = auth_kernel;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}

		switch(target_baseband())
		{
			case BASEBAND_APQ:
				src = baseband_apq;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_MSM:
				src = baseband_msm;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_CSFB:
				src = baseband_csfb;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_SVLTE2A:
				src = baseband_svlte2a;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_MDM:
				src = baseband_mdm;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_SGLTE:
				src = baseband_sglte;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;
		}
		switch(boot_mode)
		{
		case BOOT_MODE_FASTMMI_PCBA:
			src = boot_up_mode_fastmmi_pcba;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
			break;
		case BOOT_MODE_FASTMMI_FULL:
			src = boot_up_mode_fastmmi_full;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
			break;
		case BOOT_MODE_FTM:
			src = boot_up_mode_ftm;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
			break;
		case BOOT_MODE_USB_CHG:
			src = boot_up_mode_charger;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
			break;
		case BOOT_MODE_RECOVERY:
			src = boot_up_mode_recovery;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
			break;
		case BOOT_MODE_BOOTLOADER:
			break;
			
		case BOOT_MODE_NORMAL:
		default:
			src = boot_up_mode_normal;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}
	}
	return cmdline_final;
}

unsigned *atag_core(unsigned *ptr)
{
	/* CORE */
	*ptr++ = 2;
	*ptr++ = 0x54410001;

	return ptr;

}

unsigned *atag_ramdisk(unsigned *ptr, void *ramdisk,
							   unsigned ramdisk_size)
{
	if (ramdisk_size) {
		*ptr++ = 4;
		*ptr++ = 0x54420005;
		*ptr++ = (unsigned)ramdisk;
		*ptr++ = ramdisk_size;
	}

	return ptr;
}

unsigned *atag_ptable(unsigned **ptr_addr)
{
	int i;
	struct ptable *ptable;

	if ((ptable = flash_get_ptable()) && (ptable->count != 0)) {
        	*(*ptr_addr)++ = 2 + (ptable->count * (sizeof(struct atag_ptbl_entry) /
					  		sizeof(unsigned)));
		*(*ptr_addr)++ = 0x4d534d70;
		for (i = 0; i < ptable->count; ++i)
			ptentry_to_tag(ptr_addr, ptable_get(ptable, i));
	}

	return (*ptr_addr);
}

unsigned *atag_cmdline(unsigned *ptr, const char *cmdline)
{
	int cmdline_length = 0;
	int n;
	unsigned char *cmdline_final = NULL;
	char *dest;

	cmdline_final = update_cmdline(cmdline);
	if (cmdline_final){
		dprintf(INFO, "cmdline: %s\n", cmdline_final);
	}

	cmdline_length =strlen(cmdline_final);
	n = (cmdline_length + 4) & (~3);

	*ptr++ = (n / 4) + 2;
	*ptr++ = 0x54410009;
	dest = (char *) ptr;
	while (*dest++ = *cmdline_final++);
	ptr += (n / 4);

	return ptr;
}

unsigned *atag_end(unsigned *ptr)
{
	/* END */
	*ptr++ = 0;
	*ptr++ = 0;

	return ptr;
}

void generate_atags(unsigned *ptr, const char *cmdline,
                    void *ramdisk, unsigned ramdisk_size)
{

	ptr = atag_core(ptr);
	ptr = atag_ramdisk(ptr, ramdisk, ramdisk_size);
	ptr = target_atag_mem(ptr);

	/* Skip NAND partition ATAGS for eMMC boot */
	if (!target_is_emmc_boot()){
		ptr = atag_ptable(&ptr);
	}

	ptr = atag_cmdline(ptr, cmdline);
	ptr = atag_end(ptr);
}

void boot_linux(void *kernel, unsigned *tags,
		const char *cmdline, unsigned machtype,
		void *ramdisk, unsigned ramdisk_size)
{
	int ret = 0;
	void (*entry)(unsigned, unsigned, unsigned*) = kernel;

#if DEVICE_TREE
	/* Update the Device Tree */
	ret = update_device_tree(tags, cmdline, ramdisk, ramdisk_size);
	if(ret)
	{
		dprintf(CRITICAL, "ERROR: Updating Device Tree Failed \n");
		ASSERT(0);
	}
#else
	/* Generating the Atags */
	generate_atags(tags, cmdline, ramdisk, ramdisk_size);
#endif

	dprintf(INFO, "booting linux @ %p, ramdisk @ %p (%d)\n",
		kernel, ramdisk, ramdisk_size);
	if (cmdline)
		dprintf(INFO, "cmdline: %s\n", cmdline);

	enter_critical_section();
	/* do any platform specific cleanup before kernel entry */
	platform_uninit();
	arch_disable_cache(UCACHE);
	/* NOTE:
	 * The value of "entry" is getting corrupted at this point.
	 * The value is in R4 and gets pushed to stack on entry into
	 * disable_cache(), however, on return it is not the same.
	 * Not entirely sure why this dsb() seems to take of this.
	 * The stack pop operation on return from disable_cache()
	 * should restore R4 properly, but that is not happening.
	 * Will need to revisit to find the root cause.
	 */
	dsb();
	arch_disable_mmu();
	entry(0, machtype, tags);
}

unsigned page_size = 0;
unsigned page_mask = 0;

#define ROUND_TO_PAGE(x,y) (((x) + (y)) & (~(y)))

static unsigned char buf[4096]; //Equal to max-supported pagesize
static unsigned char dt_buf[4096];
int boot_linux_from_mmc(void)
{
	struct boot_img_hdr *hdr = (void*) buf;
	struct boot_img_hdr *uhdr;
	unsigned offset = 0;
	unsigned long long ptn = 0;
	unsigned n = 0;
	const char *cmdline;
	int index = INVALID_PTN;

	unsigned char *image_addr = 0;
	unsigned kernel_actual;
	unsigned ramdisk_actual;
	unsigned imagesize_actual;
	unsigned second_actual = 0;
	unsigned dt_actual = 0;
	boot_mode_type boot_mode;
	struct cpr_status_info info;

#if DEVICE_TREE
	struct dt_table *table;
	struct dt_entry *dt_entry_ptr;
	unsigned dt_table_offset;
#endif

	uhdr = (struct boot_img_hdr *)EMMC_BOOT_IMG_HEADER_ADDR;
	if (!memcmp(uhdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
		dprintf(INFO, "Unified boot method!\n");
		hdr = uhdr;
		goto unified_boot;
	}
	if (!boot_into_recovery) {
		index = partition_get_index("boot");
		ptn = partition_get_offset(index);
		if(ptn == 0) {
			dprintf(CRITICAL, "ERROR: No boot partition found\n");
                    return -1;
		}
	}
	else {
		index = partition_get_index("recovery");
		ptn = partition_get_offset(index);
		if(ptn == 0) {
			dprintf(CRITICAL, "ERROR: No recovery partition found\n");
                    return -1;
		}
	}

	if (mmc_read(ptn + offset, (unsigned int *) buf, page_size)) {
		dprintf(CRITICAL, "ERROR: Cannot read boot image header\n");
                return -1;
	}

	if (memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
		dprintf(CRITICAL, "ERROR: Invalid boot image header\n");
                return -1;
	}

	if (hdr->page_size && (hdr->page_size != page_size)) {
		page_size = hdr->page_size;
		page_mask = page_size - 1;
	}

	/* Authenticate Kernel */
	if(target_use_signed_kernel() && (!device.is_unlocked) && (!device.is_tampered))
	{
		image_addr = (unsigned char *)target_get_scratch_address();
		kernel_actual = ROUND_TO_PAGE(hdr->kernel_size, page_mask);
		ramdisk_actual = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
		#if DEVICE_TREE
		second_actual = ROUND_TO_PAGE(hdr->second_size, page_mask);
		dt_actual = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
		#endif
		imagesize_actual = (page_size + kernel_actual + ramdisk_actual + second_actual +
							dt_actual);

		offset = 0;

		/* Assuming device rooted at this time */
		device.is_tampered = 1;

		/* Read image without signature */
		if (mmc_read(ptn + offset, (void *)image_addr, imagesize_actual))
		{
			dprintf(CRITICAL, "ERROR: Cannot read boot image\n");
				return -1;
		}

		offset = imagesize_actual;
		boot_mode = get_boot_mode();
		/* Read signature */
		if(mmc_read(ptn + offset, (void *)(image_addr + offset), page_size))
		{
			dprintf(CRITICAL, "ERROR: Cannot read boot image signature\n");
		}
		else if(boot_mode == BOOT_MODE_NORMAL || boot_mode == BOOT_MODE_USB_CHG)
		{
			auth_kernel_img = image_verify((unsigned char *)image_addr,
					(unsigned char *)(image_addr + imagesize_actual),
					imagesize_actual,
					CRYPTO_AUTH_ALG_SHA256);

			if(auth_kernel_img)
			{
				/* Authorized kernel */
				device.is_tampered = 0;
			}
		}

		/* Move kernel, ramdisk and device tree to correct address */
		memmove((void*) hdr->kernel_addr, (char *)(image_addr + page_size), hdr->kernel_size);
		memmove((void*) hdr->ramdisk_addr, (char *)(image_addr + page_size + kernel_actual), hdr->ramdisk_size);

		#if DEVICE_TREE
		if(hdr->dt_size) {
			table = (struct dt_table*) dt_buf;
			dt_table_offset = (image_addr + page_size + kernel_actual + ramdisk_actual + second_actual);

			memmove((void *) dt_buf, (char *)dt_table_offset, page_size);

			/* Restriction that the device tree entry table should be less than a page*/
			ASSERT(((table->num_entries * sizeof(struct dt_entry))+ DEV_TREE_HEADER_SIZE) < hdr->page_size);

			/* Validate the device tree table header */
			if((table->magic != DEV_TREE_MAGIC) && (table->version != DEV_TREE_VERSION)) {
				dprintf(CRITICAL, "ERROR: Cannot validate Device Tree Table \n");
				return -1;
			}

			/* Find index of device tree within device tree table */
			if((dt_entry_ptr = get_device_tree_ptr(table)) == NULL){
				dprintf(CRITICAL, "ERROR: Device Tree Blob cannot be found\n");
				return -1;
			}

			/* Read device device tree in the "tags_add */
			memmove((void *)hdr->tags_addr, (char *)dt_table_offset + dt_entry_ptr->offset, dt_entry_ptr->size);
		}
		#endif
		/* Make sure everything from scratch address is read before next step!*/
		if(device.is_tampered)
		{
			write_device_info_mmc(&device);
		#ifdef TZ_TAMPER_FUSE
			set_tamper_fuse_cmd();
		#endif
		}
	#if USE_PCOM_SECBOOT
		set_tamper_flag(device.is_tampered);
	#endif
	}
	else
	{
		offset += page_size;

		n = ROUND_TO_PAGE(hdr->kernel_size, page_mask);
		if (mmc_read(ptn + offset, (void *)hdr->kernel_addr, n)) {
			dprintf(CRITICAL, "ERROR: Cannot read kernel image\n");
					return -1;
		}
		offset += n;

		n = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
		if(n != 0)
		{
			if (mmc_read(ptn + offset, (void *)hdr->ramdisk_addr, n)) {
				dprintf(CRITICAL, "ERROR: Cannot read ramdisk image\n");
				return -1;
			}
		}
		offset += n;

		if(hdr->second_size != 0) {
			n = ROUND_TO_PAGE(hdr->second_size, page_mask);
			offset += n;
		}

		#if DEVICE_TREE
		if(hdr->dt_size != 0) {

			/* Read the device tree table into buffer */
			if(mmc_read(ptn + offset,(unsigned int *) dt_buf, page_size)) {
				dprintf(CRITICAL, "ERROR: Cannot read the Device Tree Table\n");
				return -1;
			}
			table = (struct dt_table*) dt_buf;

			/* Restriction that the device tree entry table should be less than a page*/
			ASSERT(((table->num_entries * sizeof(struct dt_entry))+ DEV_TREE_HEADER_SIZE) < hdr->page_size);

			/* Validate the device tree table header */
			if((table->magic != DEV_TREE_MAGIC) && (table->version != DEV_TREE_VERSION)) {
				dprintf(CRITICAL, "ERROR: Cannot validate Device Tree Table \n");
				return -1;
			}

			/* Calculate the offset of device tree within device tree table */
			if((dt_entry_ptr = get_device_tree_ptr(table)) == NULL){
				dprintf(CRITICAL, "ERROR: Getting device tree address failed\n");
				return -1;
			}

			/* Read device device tree in the "tags_add */
			hdr->tags_addr = 0x8400000;
			if(mmc_read(ptn + offset + dt_entry_ptr->offset,
						 (void *)hdr->tags_addr, dt_entry_ptr->size)) {
				dprintf(CRITICAL, "ERROR: Cannot read device tree\n");
				return -1;
			}
		}
		#endif
	}

unified_boot:
	dprintf(INFO, "\nkernel  @ %x (%d bytes)\n", hdr->kernel_addr,
		hdr->kernel_size);
	dprintf(INFO, "ramdisk @ %x (%d bytes)\n", hdr->ramdisk_addr,
		hdr->ramdisk_size);

	if(hdr->cmdline[0]) {
		cmdline = (char*) hdr->cmdline;
	} else {
		cmdline = DEFAULT_CMDLINE;
	}

	if (get_cpr_config(&info)) {
		dprintf(CRITICAL, "ERROR: Get CPR status failed, continue anyway\n");
	}

	dprintf(CRITICAL, "CPR status=%d\n", info.status);

	if ((info.flag[0] == CPR_FLAG0) && (info.flag[1] == CPR_FLAG1) &&
			((info.status == CPR_DISABLED) || (info.status == CPR_ENABLED))) {
		strncat(cmdline, info.status == CPR_ENABLED ? " msm_cpr.enable=1" : " msm_cpr.enable=0",
				BOOT_ARGS_SIZE);
	}

	dprintf(INFO, "cmdline = '%s'\n", cmdline);

	dprintf(INFO, "\nBooting Linux\n");
	boot_linux((void *)hdr->kernel_addr, (unsigned *) hdr->tags_addr,
		   (const char *)cmdline, board_machtype(),
		   (void *)hdr->ramdisk_addr, hdr->ramdisk_size);

	return 0;
}

int boot_linux_from_flash(void)
{
	struct boot_img_hdr *hdr = (void*) buf;
	unsigned n;
	struct ptentry *ptn;
	struct ptable *ptable;
	unsigned offset = 0;
	const char *cmdline;

	unsigned char *image_addr = 0;
	unsigned kernel_actual;
	unsigned ramdisk_actual;
	unsigned imagesize_actual;

	if (target_is_emmc_boot()) {
		hdr = (struct boot_img_hdr *)EMMC_BOOT_IMG_HEADER_ADDR;
		if (memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
			dprintf(CRITICAL, "ERROR: Invalid boot image header\n");
			return -1;
		}
		goto continue_boot;
	}

	ptable = flash_get_ptable();
	if (ptable == NULL) {
		dprintf(CRITICAL, "ERROR: Partition table not found\n");
		return -1;
	}

	if(!boot_into_recovery)
	{
	        ptn = ptable_find(ptable, "boot");
	        if (ptn == NULL) {
		        dprintf(CRITICAL, "ERROR: No boot partition found\n");
		        return -1;
	        }
	}
	else
	{
	        ptn = ptable_find(ptable, "recovery");
	        if (ptn == NULL) {
		        dprintf(CRITICAL, "ERROR: No recovery partition found\n");
		        return -1;
	        }
	}

	if (flash_read(ptn, offset, buf, page_size)) {
		dprintf(CRITICAL, "ERROR: Cannot read boot image header\n");
		return -1;
	}

	if (memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
		dprintf(CRITICAL, "ERROR: Invalid boot image header\n");
		return -1;
	}

	if (hdr->page_size != page_size) {
		dprintf(CRITICAL, "ERROR: Invalid boot image pagesize. Device pagesize: %d, Image pagesize: %d\n",page_size,hdr->page_size);
		return -1;
	}

	/* Authenticate Kernel */
	if(target_use_signed_kernel() && (!device.is_unlocked) && (!device.is_tampered))
	{
		image_addr = (unsigned char *)target_get_scratch_address();
		kernel_actual = ROUND_TO_PAGE(hdr->kernel_size, page_mask);
		ramdisk_actual = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
		imagesize_actual = (page_size + kernel_actual + ramdisk_actual);

		offset = 0;

		/* Assuming device rooted at this time */
		device.is_tampered = 1;

		/* Read image without signature */
		if (flash_read(ptn, offset, (void *)image_addr, imagesize_actual))
		{
			dprintf(CRITICAL, "ERROR: Cannot read boot image\n");
				return -1;
		}

		offset = imagesize_actual;
		/* Read signature */
		if (flash_read(ptn, offset, (void *)(image_addr + offset), page_size))
		{
			dprintf(CRITICAL, "ERROR: Cannot read boot image signature\n");
		}
		else
		{

			/* Verify signature */
			auth_kernel_img = image_verify((unsigned char *)image_addr,
						(unsigned char *)(image_addr + imagesize_actual),
						imagesize_actual,
						CRYPTO_AUTH_ALG_SHA256);

			if(auth_kernel_img)
			{
				/* Authorized kernel */
				device.is_tampered = 0;
			}
		}

		/* Move kernel and ramdisk to correct address */
		memmove((void*) hdr->kernel_addr, (char *)(image_addr + page_size), hdr->kernel_size);
		memmove((void*) hdr->ramdisk_addr, (char *)(image_addr + page_size + kernel_actual), hdr->ramdisk_size);

		/* Make sure everything from scratch address is read before next step!*/
		if(device.is_tampered)
		{
			write_device_info_flash(&device);
		}
#if USE_PCOM_SECBOOT
		set_tamper_flag(device.is_tampered);
#endif
	}
	else
	{
		offset = page_size;

		n = ROUND_TO_PAGE(hdr->kernel_size, page_mask);
		if (flash_read(ptn, offset, (void *)hdr->kernel_addr, n)) {
			dprintf(CRITICAL, "ERROR: Cannot read kernel image\n");
			return -1;
		}
		offset += n;

		n = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
		if (flash_read(ptn, offset, (void *)hdr->ramdisk_addr, n)) {
			dprintf(CRITICAL, "ERROR: Cannot read ramdisk image\n");
			return -1;
		}
		offset += n;
	}
continue_boot:
	dprintf(INFO, "\nkernel  @ %x (%d bytes)\n", hdr->kernel_addr,
		hdr->kernel_size);
	dprintf(INFO, "ramdisk @ %x (%d bytes)\n", hdr->ramdisk_addr,
		hdr->ramdisk_size);

	if(hdr->cmdline[0]) {
		cmdline = (char*) hdr->cmdline;
	} else {
		cmdline = DEFAULT_CMDLINE;
	}
	dprintf(INFO, "cmdline = '%s'\n", cmdline);

	/* TODO: create/pass atags to kernel */

	dprintf(INFO, "\nBooting Linux\n");
	boot_linux((void *)hdr->kernel_addr, (void *)hdr->tags_addr,
		   (const char *)cmdline, board_machtype(),
		   (void *)hdr->ramdisk_addr, hdr->ramdisk_size);

	return 0;
}

unsigned char info_buf[4096];
void write_device_info_mmc(device_info *dev)
{
	struct device_info *info = (void*) info_buf;
	unsigned long long ptn = 0;
	unsigned long long size;
	int index = INVALID_PTN;

	index = partition_get_index("aboot");
	ptn = partition_get_offset(index);
	if(ptn == 0)
	{
		return;
	}

	size = partition_get_size(index);

	memcpy(info, dev, sizeof(device_info));

	if(mmc_write((ptn + size - 512), 512, (void *)info_buf))
	{
		dprintf(CRITICAL, "ERROR: Cannot write device info\n");
		return;
	}
}

void read_device_info_mmc(device_info *dev)
{
	struct device_info *info = (void*) info_buf;
	unsigned long long ptn = 0;
	unsigned long long size;
	int index = INVALID_PTN;

	index = partition_get_index("aboot");
	ptn = partition_get_offset(index);
	if(ptn == 0)
	{
		return;
	}

	size = partition_get_size(index);

	if(mmc_read((ptn + size - 512), (void *)info_buf, 512))
	{
		dprintf(CRITICAL, "ERROR: Cannot read device info\n");
		return;
	}

	if (memcmp(info->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE))
	{
		memcpy(info->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE);
		info->is_unlocked = 0;
		info->is_tampered = 0;

		write_device_info_mmc(info);
	}
	memcpy(dev, info, sizeof(device_info));
}

void write_device_info_flash(device_info *dev)
{
	struct device_info *info = (void *) info_buf;
	struct ptentry *ptn;
	struct ptable *ptable;

	ptable = flash_get_ptable();
	if (ptable == NULL)
	{
		dprintf(CRITICAL, "ERROR: Partition table not found\n");
		return;
	}

	ptn = ptable_find(ptable, "devinfo");
	if (ptn == NULL)
	{
		dprintf(CRITICAL, "ERROR: No boot partition found\n");
			return;
	}

	memcpy(info, dev, sizeof(device_info));

	if (flash_write(ptn, 0, (void *)info_buf, page_size))
	{
		dprintf(CRITICAL, "ERROR: Cannot write device info\n");
			return;
	}
}

void read_device_info_flash(device_info *dev)
{
	struct device_info *info = (void*) info_buf;
	struct ptentry *ptn;
	struct ptable *ptable;

	ptable = flash_get_ptable();
	if (ptable == NULL)
	{
		dprintf(CRITICAL, "ERROR: Partition table not found\n");
		return;
	}

	ptn = ptable_find(ptable, "devinfo");
	if (ptn == NULL)
	{
		dprintf(CRITICAL, "ERROR: No boot partition found\n");
			return;
	}

	if (flash_read(ptn, 0, (void *)info_buf, page_size))
	{
		dprintf(CRITICAL, "ERROR: Cannot write device info\n");
			return;
	}

	if (memcmp(info->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE))
	{
		memcpy(info->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE);
		info->is_unlocked = 0;
		info->is_tampered = 0;
		write_device_info_flash(info);
	}
	memcpy(dev, info, sizeof(device_info));
}

void write_device_info(device_info *dev)
{
	if(target_is_emmc_boot())
	{
		write_device_info_mmc(dev);
	}
	else
	{
		write_device_info_flash(dev);
	}
}

void read_device_info(device_info *dev)
{
	if(target_is_emmc_boot())
	{
		read_device_info_mmc(dev);
	}
	else
	{
		read_device_info_flash(dev);
	}
}

void reset_device_info()
{
	dprintf(ALWAYS, "reset_device_info called.");
	device.is_tampered = 0;
	write_device_info(&device);
}

void set_device_root()
{
	dprintf(ALWAYS, "set_device_root called.");
	device.is_tampered = 1;
	write_device_info(&device);
}

void cmd_boot(const char *arg, void *data, unsigned sz)
{
	unsigned kernel_actual;
	unsigned ramdisk_actual;
	static struct boot_img_hdr hdr;
	char *ptr = ((char*) data);

	if (sz < sizeof(hdr)) {
		fastboot_fail("invalid bootimage header");
		return;
	}

	memcpy(&hdr, data, sizeof(hdr));

	/* ensure commandline is terminated */
	hdr.cmdline[BOOT_ARGS_SIZE-1] = 0;

	if(target_is_emmc_boot() && hdr.page_size) {
		page_size = hdr.page_size;
		page_mask = page_size - 1;
	}

	kernel_actual = ROUND_TO_PAGE(hdr.kernel_size, page_mask);
	ramdisk_actual = ROUND_TO_PAGE(hdr.ramdisk_size, page_mask);

	/* sz should have atleast raw boot image */
	if (page_size + kernel_actual + ramdisk_actual > sz) {
		fastboot_fail("incomplete bootimage");
		return;
	}

	memmove((void*) hdr.kernel_addr, ptr + page_size, hdr.kernel_size);
	memmove((void*) hdr.ramdisk_addr, ptr + page_size + kernel_actual, hdr.ramdisk_size);

	fastboot_okay("");
	udc_stop();

	boot_linux((void*) hdr.kernel_addr, (void*) hdr.tags_addr,
		   (const char*) hdr.cmdline, board_machtype(),
		   (void*) hdr.ramdisk_addr, hdr.ramdisk_size);
}

void cmd_erase(const char *arg, void *data, unsigned sz)
{
	struct ptentry *ptn;
	struct ptable *ptable;

	ptable = flash_get_ptable();
	if (ptable == NULL) {
		fastboot_fail("partition table doesn't exist");
		return;
	}

	ptn = ptable_find(ptable, arg);
	if (ptn == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	if (flash_erase(ptn)) {
		fastboot_fail("failed to erase partition");
		return;
	}
	fastboot_okay("");
}


#define DEFAULT_ERASE_SECTORS	8
static unsigned int g_out[512 * DEFAULT_ERASE_SECTORS] = {0};
void cmd_erase_mmc(const char *arg, void *data, unsigned sz)
{
	unsigned long long ptn = 0;
	int index = INVALID_PTN;

	index = partition_get_index(arg);
	ptn = partition_get_offset(index);

	if(ptn == 0) {
		fastboot_fail("Partition table doesn't exist\n");
		return;
	}
	/* Simple inefficient version of erase. Just writing
       0 in first block */
	if (mmc_write(ptn , 512 * DEFAULT_ERASE_SECTORS, (unsigned int *)g_out)) {
		fastboot_fail("failed to erase partition");
		return;
	}
	fastboot_okay("");
}


void cmd_flash_mmc_img(const char *arg, void *data, unsigned sz)
{
	unsigned long long ptn = 0;
	unsigned long long size = 0;
	int index = INVALID_PTN;

	if (!strcmp(arg, "partition"))
	{
		dprintf(INFO, "Attempt to write partition image.\n");
		if (write_partition(sz, (unsigned char *) data)) {
			fastboot_fail("failed to write partition");
			return;
		}
	}
	else
	{
		index = partition_get_index(arg);
		ptn = partition_get_offset(index);
		if(ptn == 0) {
			fastboot_fail("partition table doesn't exist");
			return;
		}

		if (!strcmp(arg, "boot") || !strcmp(arg, "recovery")) {
			if (memcmp((void *)data, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
				fastboot_fail("image is not a boot image");
				return;
			}
		}

		size = partition_get_size(index);
		if (ROUND_TO_PAGE(sz,511) > size) {
			fastboot_fail("size too large");
			return;
		}
		else if (mmc_write(ptn , sz, (unsigned int *)data)) {
			fastboot_fail("flash write failure");
			return;
		}
	}
	fastboot_okay("");
	return;
}

void cmd_flash_mmc_sparse_img(const char *arg, void *data, unsigned sz)
{
	unsigned int chunk;
	unsigned int chunk_data_sz;
	sparse_header_t *sparse_header;
	chunk_header_t *chunk_header;
	uint32_t total_blocks = 0;
	unsigned long long ptn = 0;
	unsigned long long size = 0;
	int index = INVALID_PTN;

	index = partition_get_index(arg);
	ptn = partition_get_offset(index);
	if(ptn == 0) {
		fastboot_fail("partition table doesn't exist");
		return;
	}

	size = partition_get_size(index);
	if (ROUND_TO_PAGE(sz,511) > size) {
		fastboot_fail("size too large");
		return;
	}

	/* Read and skip over sparse image header */
	sparse_header = (sparse_header_t *) data;
	data += sparse_header->file_hdr_sz;
	if(sparse_header->file_hdr_sz > sizeof(sparse_header_t))
	{
		/* Skip the remaining bytes in a header that is longer than
		 * we expected.
		 */
		data += (sparse_header->file_hdr_sz - sizeof(sparse_header_t));
	}

	dprintf (SPEW, "=== Sparse Image Header ===\n");
	dprintf (SPEW, "magic: 0x%x\n", sparse_header->magic);
	dprintf (SPEW, "major_version: 0x%x\n", sparse_header->major_version);
	dprintf (SPEW, "minor_version: 0x%x\n", sparse_header->minor_version);
	dprintf (SPEW, "file_hdr_sz: %d\n", sparse_header->file_hdr_sz);
	dprintf (SPEW, "chunk_hdr_sz: %d\n", sparse_header->chunk_hdr_sz);
	dprintf (SPEW, "blk_sz: %d\n", sparse_header->blk_sz);
	dprintf (SPEW, "total_blks: %d\n", sparse_header->total_blks);
	dprintf (SPEW, "total_chunks: %d\n", sparse_header->total_chunks);

	/* Start processing chunks */
	for (chunk=0; chunk<sparse_header->total_chunks; chunk++)
	{
		/* Read and skip over chunk header */
		chunk_header = (chunk_header_t *) data;
		data += sizeof(chunk_header_t);

		dprintf (SPEW, "=== Chunk Header ===\n");
		dprintf (SPEW, "chunk_type: 0x%x\n", chunk_header->chunk_type);
		dprintf (SPEW, "chunk_data_sz: 0x%x\n", chunk_header->chunk_sz);
		dprintf (SPEW, "total_size: 0x%x\n", chunk_header->total_sz);

		if(sparse_header->chunk_hdr_sz > sizeof(chunk_header_t))
		{
			/* Skip the remaining bytes in a header that is longer than
			 * we expected.
			 */
			data += (sparse_header->chunk_hdr_sz - sizeof(chunk_header_t));
		}

		chunk_data_sz = sparse_header->blk_sz * chunk_header->chunk_sz;
		switch (chunk_header->chunk_type)
		{
			case CHUNK_TYPE_RAW:
			if(chunk_header->total_sz != (sparse_header->chunk_hdr_sz +
											chunk_data_sz))
			{
				fastboot_fail("Bogus chunk size for chunk type Raw");
				return;
			}

			if(mmc_write(ptn + ((uint64_t)total_blocks*sparse_header->blk_sz),
						chunk_data_sz,
						(unsigned int*)data))
			{
				fastboot_fail("flash write failure");
				return;
			}
			total_blocks += chunk_header->chunk_sz;
			data += chunk_data_sz;
			break;

			case CHUNK_TYPE_DONT_CARE:
			total_blocks += chunk_header->chunk_sz;
			break;

			case CHUNK_TYPE_CRC:
			if(chunk_header->total_sz != sparse_header->chunk_hdr_sz)
			{
				fastboot_fail("Bogus chunk size for chunk type Dont Care");
				return;
			}
			total_blocks += chunk_header->chunk_sz;
			data += chunk_data_sz;
			break;

			default:
			fastboot_fail("Unknown chunk type");
			return;
		}
	}

	dprintf(INFO, "Wrote %d blocks, expected to write %d blocks\n",
					total_blocks, sparse_header->total_blks);

	if(total_blocks != sparse_header->total_blks)
	{
		fastboot_fail("sparse image write failure");
	}

	fastboot_okay("");
	return;
}

void cmd_flash_mmc(const char *arg, void *data, unsigned sz)
{
	sparse_header_t *sparse_header;
	/* 8 Byte Magic + 2048 Byte xml + Encrypted Data */
	unsigned int *magic_number = (unsigned int *) data;
	int ret=0;

	if (magic_number[0] == DECRYPT_MAGIC_0 &&
		magic_number[1] == DECRYPT_MAGIC_1)
	{
#ifdef SSD_ENABLE
		ret = decrypt_scm((uint32 **) &data, &sz);
#endif
		if (ret != 0) {
			dprintf(CRITICAL, "ERROR: Invalid secure image\n");
			return;
		}
	}
	else if (magic_number[0] == ENCRYPT_MAGIC_0 &&
			 magic_number[1] == ENCRYPT_MAGIC_1)
	{
#ifdef SSD_ENABLE
		ret = encrypt_scm((uint32 **) &data, &sz);
#endif
		if (ret != 0) {
			dprintf(CRITICAL, "ERROR: Encryption Failure\n");
			return;
		}
	}

	sparse_header = (sparse_header_t *) data;
	if (sparse_header->magic != SPARSE_HEADER_MAGIC)
		cmd_flash_mmc_img(arg, data, sz);
	else
		cmd_flash_mmc_sparse_img(arg, data, sz);
	return;
}

void cmd_flash(const char *arg, void *data, unsigned sz)
{
	struct ptentry *ptn;
	struct ptable *ptable;
	unsigned extra = 0;

	ptable = flash_get_ptable();
	if (ptable == NULL) {
		fastboot_fail("partition table doesn't exist");
		return;
	}

	ptn = ptable_find(ptable, arg);
	if (ptn == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	if (!strcmp(ptn->name, "boot") || !strcmp(ptn->name, "recovery")) {
		if (memcmp((void *)data, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
			fastboot_fail("image is not a boot image");
			return;
		}
	}

	if (!strcmp(ptn->name, "system")
		|| !strcmp(ptn->name, "userdata")
		|| !strcmp(ptn->name, "persist")
		|| !strcmp(ptn->name, "recoveryfs")) {
		if (flash_ecc_bch_enabled())
			/* Spare data bytes for 8 bit ECC increased by 4 */
			extra = ((page_size >> 9) * 20);
		else
			extra = ((page_size >> 9) * 16);
	} else
		sz = ROUND_TO_PAGE(sz, page_mask);

	dprintf(INFO, "writing %d bytes to '%s'\n", sz, ptn->name);
	if (flash_write(ptn, extra, data, sz)) {
		fastboot_fail("flash write failure");
		return;
	}
	dprintf(INFO, "partition '%s' updated\n", ptn->name);
	fastboot_okay("");
}

void disable_poweroff_charging(void);

void cmd_continue(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	udc_stop();
	disable_poweroff_charging();
	if (target_is_emmc_boot())
	{
		boot_linux_from_mmc();
	}
	else
	{
		boot_linux_from_flash();
	}
}

void cmd_reboot(const char *arg, void *data, unsigned sz)
{
	dprintf(INFO, "rebooting the device\n");
	fastboot_okay("");
	reboot_device(0);
}

void cmd_reboot_bootloader(const char *arg, void *data, unsigned sz)
{
	dprintf(INFO, "rebooting the device\n");
	fastboot_okay("");
	reboot_device(FASTBOOT_MODE);
}

void cmd_oem_unlock(const char *arg, void *data, unsigned sz)
{
	if(!device.is_unlocked)
	{
		device.is_unlocked = 1;
		write_device_info(&device);
	}
	fastboot_okay("");
}

void cmd_oem_devinfo(const char *arg, void *data, unsigned sz)
{
	char response[64];
	snprintf(response, 64, "\tDevice tampered: %s", (device.is_tampered ? "true" : "false"));
	fastboot_info(response);
	snprintf(response, 64, "\tDevice unlocked: %s", (device.is_unlocked ? "true" : "false"));
	fastboot_info(response);
	fastboot_okay("");
}

/* XXX: Note offset & sz should be page aligned
 * opt: 0 stand for write, otherwise read
 */
int handle_misc_data(enum misc_region_type type, int opt, void* data, uint64_t offset, uint64_t sz)
{
	int ptn;
	uint64_t ptn_offset; // The global offset compare to the beginning of the mmc
	uint64_t ptn_size;
	uint64_t rgn_offset; // offset of the region compare to the beginning of misc partition

	if ((page_size == 0) || (offset & (page_size -1)) || (sz & (page_size-1))) {
		dprintf(CRITICAL, "offset & sz should be page aligned\n");
		return -1;
	}

	ptn = partition_get_index("misc");
	ptn_offset = partition_get_offset(ptn);
	ptn_size = partition_get_size(ptn);

	/* Get the offset of the region */
	rgn_offset = misc_regions[type].offset;

	/* The config data */
	if (ptn_size < rgn_offset + offset + sz) {
		dprintf(CRITICAL, "offset & sz exceeded the misc region");
		return -1;
	}

	/* get data from mmc */
	if (opt != 0) {
		if (mmc_read(ptn_offset + rgn_offset + offset, data, sz)) {
			dprintf(CRITICAL, "Read from mmc failed\n");
			return -1;
		}
	} else {
		if (mmc_write(ptn_offset + rgn_offset + offset, sz, data)) {
			dprintf(CRITICAL, "Write to mmc failed\n");
			return -1;
		}
	}

	return 0;
}

/* Get the cpr configration */
int set_cpr_config(struct cpr_status_info *info)
{
	if (page_size == 0) {
		dprintf(CRITICAL, "page_size not initialized yet\n");
		return -1;
	}

	memcpy(cpr_buf, info, sizeof(struct cpr_status_info));

	/* Write CPR config to mmc */
	if (handle_misc_data(TYPE_CPR, 0/*Write*/, cpr_buf, 0, page_size)) {
		dprintf(CRITICAL, "get misc data failed\n");
		return -1;
	}

	return 0;
}

/* Get the cpr configration */
int get_cpr_config(struct cpr_status_info *info)
{
	if (page_size == 0) {
		dprintf(CRITICAL, "page_size not initialized yet\n");
		return -1;
	}
	if (handle_misc_data(TYPE_CPR, 1/*Read*/, cpr_buf, 0, page_size)) {
		dprintf(CRITICAL, "get misc data failed\n");
		return -1;
	}

	memcpy(info, cpr_buf, sizeof(struct cpr_status_info));
	return 0;
}

/* fastboot oem cpr disable: Disable CPR in kernel
 * fastboot oem cpr enable:  Enable CPR in kernel
 * fastboot oem cpr status:  Display current CPR setting
 */
void cmd_oem_cpr(const char *arg, void *data, unsigned sz)
{
	char response[64];
	int cmd = GET_CPR_STATUS; // disable:0 enable:1 status:2
	struct cpr_status_info info;

	snprintf(response, sizeof(response), "processing cpr command:%s", arg);
	fastboot_info(response);

	if (strstr(arg, "enable")) {
		cmd = ENABLE_CPR;
		dprintf(INFO, "enable CPR\n");
	} else if (strstr(arg, "disable")) {
		cmd = DISABLE_CPR;
		dprintf(INFO, "disable CPR\n");
	} else if (strstr(arg, "status")) {
		cmd = GET_CPR_STATUS;
		dprintf(INFO, "show CPR status\n");
	} else {
		fastboot_info("Please use the follow command");
		fastboot_info("fastboot oem cpr disable: Disable CPR in kernel");
		fastboot_info("fastboot oem cpr enable:  Enable CPR in kernel");
		fastboot_info("fastboot oem cpr status:  Display current CPR setting");
		goto out;
	}

	if (target_is_emmc_boot()) {
		if (cmd != GET_CPR_STATUS) {
			info.status = (cmd == ENABLE_CPR ? CPR_ENABLED : CPR_DISABLED);
			info.flag[0] = CPR_FLAG0;
			info.flag[1] = CPR_FLAG1; /*CPRE*/

			if (set_cpr_config(&info)) {
				fastboot_info("Set CPR config failed");
				goto out;
			}
			snprintf(response, sizeof(response), "CPR cmd(%d) write success!\n", cmd);
			fastboot_info(response);
			goto out;
		} else {
			if (get_cpr_config(&info)) {
				fastboot_info("get cpr config failed");
				goto out;
			}

			if ((info.flag[0] == CPR_FLAG0) && (info.flag[1] == CPR_FLAG1)
					&& ((info.status == CPR_DISABLED) || (info.status == CPR_ENABLED))) {
				snprintf(response, sizeof(response), "CPR status: %s\n",
						info.status == CPR_DISABLED ? "disabled" : "enabled");
				fastboot_info(response);
			} else {
				fastboot_info("bad CPR flag");
			}

		}

	} else {
		snprintf(response, sizeof(response), "ERROR: Partition type not supported\n");
		fastboot_info(response);
		goto out;
	}
out:
	fastboot_okay("");
}

void cmd_oem_log(const char *arg, void *data, unsigned sz)
{
#ifdef WITH_DEBUG_GLOBAL_RAM
	extern char print_buf[PRINT_BUFF_SIZE];
	// The max response size if 64 bytes. Should minus 4 byte "INFO" and '\0'
	char response[64 - 4 - 1];
	char* buf = print_buf;
	int size = (int)(buf + 4);
	int index = (int)(buf + 8);
	int i = 0;

	snprintf(response, sizeof(response), "\tbuf: %p\t size: %d\tindex: %d", print_buf, size, index);
	fastboot_info(response);
	buf = print_buf + 12;

	while ((buf < (char*)print_buf + PRINT_BUFF_SIZE) && (*buf != 0)){
		memset(response, 0, sizeof(response));
		// Just output and exit if we have reached the end of the buffer.
		while((i < sizeof(response)) && (*buf != '\0') && (buf < (char*)print_buf + PRINT_BUFF_SIZE)) {
			if (*buf == '\n') {
				//just jump it. For fastboot_info will output a new line
				buf++;
				break;
			}
			response[i++] = *buf++;
		}
		i = 0;
		fastboot_info(response);
	}
	fastboot_okay("");

#endif

}

void splash_screen ()
{
	struct ptentry *ptn;
	struct ptable *ptable;
	struct fbcon_config *fb_display = NULL;

	if (!target_is_emmc_boot())
	{
		ptable = flash_get_ptable();
		if (ptable == NULL) {
			dprintf(CRITICAL, "ERROR: Partition table not found\n");
			return;
		}

		ptn = ptable_find(ptable, "splash");
		if (ptn == NULL) {
			dprintf(CRITICAL, "ERROR: No splash partition found\n");
		} else {
			fb_display = fbcon_display();
			if (fb_display) {
				if (flash_read(ptn, 0, fb_display->base,
					(fb_display->width * fb_display->height * fb_display->bpp/8))) {
					fbcon_clear();
					dprintf(CRITICAL, "ERROR: Cannot read splash image\n");
				}
			}
		}
	}
}
//ML add
#ifdef PLATFORM_MSM7X27A
static void set_usb_serial_num()
{
	boot_info_for_apps binfo ;
	unsigned smem_status;
	smem_status = smem_read_alloc_entry(SMEM_BOOT_INFO_FOR_APPS,
										&binfo,  sizeof(boot_info_for_apps));
	if (smem_status)
	{
		dprintf(CRITICAL, "ERROR: unable to read shared memory for boot info\n");
		return;
	}
	  //check if is valid
	  if(binfo.usb_info.magicNum ==USB_ID_MAGIC_NUM ){
			if(binfo.usb_info.serialNumberLen>8 || binfo.usb_info.serialNumberLen==0){
			dprintf(CRITICAL, "ERROR: wrong serial num length \n");
			return;
		}

		memset(sn_buf,0,sizeof(sn_buf));
		memcpy(sn_buf,&(binfo.usb_info.serialNumber),binfo.usb_info.serialNumberLen);
	}
}
#endif

void aboot_init(const struct app_descriptor *app)
{
	unsigned reboot_mode = 0;
	unsigned usb_init = 0;
	unsigned sz = 0;

	/* The following varibale should be defined statically because LK may refer to them out of the function */
	static char userdata_sz_hex[sizeof(long long)*2+3];//The length of an long long integer in hex mode
	static char cache_sz_hex[sizeof(long long)*2+3];//The length of an long long integer in hex mode

	int index;
	unsigned long long ptn_size = 0;

	/* Setup page size information for nand/emmc reads */
	if (target_is_emmc_boot())
	{
		page_size = 2048;
		page_mask = page_size - 1;
	}
	else
	{
		page_size = flash_page_size();
		page_mask = page_size - 1;
	}

	if(target_use_signed_kernel())
	{
		read_device_info(&device);

	}

	target_serialno((unsigned char *) sn_buf);
	dprintf(SPEW,"serial number: %s\n",sn_buf);
	surf_udc_device.serialno = sn_buf;

	/* Check if we should do something other than booting up */
	if (keys_get_state(KEY_HOME) != 0)
		boot_into_recovery = 1;
	if (keys_get_state(KEY_VOLUMEUP) != 0)
		boot_into_recovery = 1;
	if(!boot_into_recovery)
	{
		if (keys_get_state(KEY_BACK) != 0)
			goto fastboot;
		if (keys_get_state(KEY_VOLUMEDOWN) != 0)
			goto fastboot;
	}

	#if NO_KEYPAD_DRIVER
	if (fastboot_trigger())
		goto fastboot;
	#endif

	reboot_mode = check_reboot_mode();
	if (reboot_mode == RECOVERY_MODE) {
		boot_into_recovery = 1;
	} else if(reboot_mode == FASTBOOT_MODE) {
		goto fastboot;
	}

	if (target_is_emmc_boot())
	{
		if(emmc_recovery_init())
			dprintf(ALWAYS,"error in emmc_recovery_init\n");
		if(target_use_signed_kernel())
		{
			if((device.is_unlocked) || (device.is_tampered))
			{
			#ifdef TZ_TAMPER_FUSE
				set_tamper_fuse_cmd();
			#endif
			#if USE_PCOM_SECBOOT
				set_tamper_flag(device.is_tampered);
			#endif
			}
		}

		save_debug_message();
		boot_linux_from_mmc();
	}
	else
	{
		recovery_init();
#if USE_PCOM_SECBOOT
	if((device.is_unlocked) || (device.is_tampered))
		set_tamper_flag(device.is_tampered);
#endif
		save_debug_message();
		boot_linux_from_flash();
	}
	dprintf(CRITICAL, "ERROR: Could not do normal boot. Reverting "
		"to fastboot mode.\n");

fastboot:

	target_fastboot_init();

	#ifdef PLATFORM_MSM7X27A
	//change the sn_buf
	   set_usb_serial_num();
	 #endif

	if(!usb_init)
		udc_init(&surf_udc_device);

	fastboot_register("boot", cmd_boot);

	if (target_is_emmc_boot())
	{
		fastboot_register("flash:", cmd_flash_mmc);
		fastboot_register("erase:", cmd_erase_mmc);
		save_debug_message();
	}
	else
	{
		fastboot_register("flash:", cmd_flash);
		fastboot_register("erase:", cmd_erase);
		save_debug_message();
	}

	fastboot_register("continue", cmd_continue);
	fastboot_register("reboot", cmd_reboot);
	fastboot_register("reboot-bootloader", cmd_reboot_bootloader);
	fastboot_register("oem unlock", cmd_oem_unlock);
	fastboot_register("oem device-info", cmd_oem_devinfo);
	fastboot_register("oem log", cmd_oem_log);
	fastboot_register("oem cpr", cmd_oem_cpr);
	fastboot_publish("product", TARGET(BOARD));
	fastboot_publish("kernel", "lk");
	fastboot_publish("serialno", sn_buf);

	/* The new version of fastboot protocol needs the following information to generate
	 *  an empty ext4 image when using 'fastboot -w'.
	 */
#if _EMMC_BOOT
	index = partition_get_index("userdata");
	ptn_size = partition_get_size(index);
	snprintf(userdata_sz_hex, sizeof(userdata_sz_hex), "0x%llx", ptn_size);
	dprintf(SPEW, "partition-size:userdata: %s\n", userdata_sz_hex);

	fastboot_publish("partition-type:userdata", "ext4");
	fastboot_publish("partition-size:userdata", userdata_sz_hex);

	index = partition_get_index("cache");
	ptn_size = partition_get_size(index);
	snprintf(cache_sz_hex, sizeof(cache_sz_hex), "0x%llx", ptn_size);
	dprintf(SPEW, "partition-size:cache: %s\n", cache_sz_hex);

	fastboot_publish("partition-type:cache", "ext4");
	fastboot_publish("partition-size:cache", cache_sz_hex);
#endif

	partition_dump();
	sz = target_get_max_flash_size();
	fastboot_init(target_get_scratch_address(), sz);
	udc_start();
}

APP_START(aboot)
	.init = aboot_init,
APP_END

#if DEVICE_TREE
struct dt_entry * get_device_tree_ptr(struct dt_table *table)
{
	unsigned i;
	struct dt_entry *dt_entry_ptr;

	dt_entry_ptr = (char *)table + DEV_TREE_HEADER_SIZE ;

	for(i = 0; i < table->num_entries; i++)
	{
		if((dt_entry_ptr->platform_id == board_platform_id()) &&
		   (dt_entry_ptr->variant_id == board_hardware_id()) &&
		   (dt_entry_ptr->soc_rev == 0)){
				return dt_entry_ptr;
		}
		dt_entry_ptr++;
	}
	return NULL;
}

int update_device_tree(const void * fdt, char *cmdline,
					   void *ramdisk, unsigned ramdisk_size)
{
	int ret = 0;
	int offset;
	uint32_t *memory_reg;
	unsigned char *final_cmdline;
	uint32_t len;

	/* Check the device tree header */
	ret = fdt_check_header(fdt);
	if(ret)
	{
		dprintf(CRITICAL, "Invalid device tree header \n");
		return ret;
	}

	/* Get offset of the memory node */
	offset = fdt_path_offset(fdt,"/memory");

	memory_reg = target_dev_tree_mem(&len);

	/* Adding the memory values to the reg property */
	ret = fdt_setprop(fdt, offset, "reg", memory_reg, sizeof(uint32_t) * len * 2);
	if(ret)
	{
		dprintf(CRITICAL, "ERROR: Cannot update memory node\n");
		return ret;
	}

	/* Get offset of the chosen node */
	offset = fdt_path_offset(fdt, "/chosen");

	/* Adding the cmdline to the chosen node */
	final_cmdline = update_cmdline(cmdline);
	ret = fdt_setprop_string(fdt, offset, "bootargs", final_cmdline);
	if(ret)
	{
		dprintf(CRITICAL, "ERROR: Cannot update chosen node [bootargs]\n");
		return ret;
	}

	/* Adding the initrd-start to the chosen node */
	ret = fdt_setprop_cell(fdt, offset, "linux,initrd-start", ramdisk);
	if(ret)
	{
		dprintf(CRITICAL, "ERROR: Cannot update chosen node [linux,initrd-start]\n");
		return ret;
	}

	/* Adding the initrd-end to the chosen node */
	ret = fdt_setprop_cell(fdt, offset, "linux,initrd-end", (ramdisk + ramdisk_size));
	if(ret)
	{
		dprintf(CRITICAL, "ERROR: Cannot update chosen node [linux,initrd-end]\n");
		return ret;
	}

	fdt_pack(fdt);

	return ret;
}
#endif
