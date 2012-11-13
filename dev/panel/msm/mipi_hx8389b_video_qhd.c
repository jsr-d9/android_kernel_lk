/* Copyright (c) 2012, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdint.h>
#include <msm_panel.h>
#include <mipi_dsi.h>
#include <sys/types.h>
#include <err.h>
#include <reg.h>
#include <debug.h>
#include <platform/iomap.h>
#include <target/display.h>

/* MIPI HX3839B panel commands */
static char exit_sleep[4] = {0x11, 0x00, 0x05, 0x80};
static char display_on[4] = {0x29, 0x00, 0x05, 0x80};

static char video0[8] = {
        0x04, 0x00, 0x39, 0xc0,
        0xB9, 0xFF, 0x83, 0x89,
};
static char video1[24] = {
        0x14, 0x00, 0x39, 0xc0,
        0xb1, 0x00, 0x00, 0x04,
        0xe8, 0x50, 0x10, 0x11,
        0xb0, 0xf0, 0x2b, 0x33,
        0x1a, 0x1a, 0x43, 0x01,
        0x58, 0xf2, 0x00, 0xe6,
};
static char video2[12] = {
        0x08, 0x00, 0x39, 0xc0,
        0xb2, 0x00, 0x00, 0x78,
        0x0c, 0x07, 0x00, 0x30
};
static char video3[28] = {
        0x18, 0x00, 0x39, 0xc0,
        0xb4, 0x80, 0x08, 0x00,
        0x32, 0x10, 0x04, 0x32,
        0x10, 0x00, 0x32, 0x10,
        0x00, 0x37, 0x0a, 0x40,
        0x08, 0x37, 0x0a, 0x40,
        0x14, 0x46, 0x50, 0x0a,
};
static char video4[64] = {
        0x39, 0x00, 0x39, 0xc0,
        0xd5, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00,
        0x00, 0x60, 0x00, 0x88,
        0x88, 0x88, 0x88, 0x88,
        0x23, 0x88, 0x01, 0x88,
        0x67, 0x88, 0x45, 0x01,
        0x23, 0x88, 0x88, 0x88,
        0x88, 0x88, 0x88, 0x88,
        0x88, 0x88, 0x88, 0x54,
        0x88, 0x76, 0x88, 0x10,
        0x88, 0x32, 0x32, 0x10,
        0x88, 0x88, 0x88, 0x88,
        0x88, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0xff, 0xff, 0xff,
};
static char video5[8] = {
        0x03, 0x00, 0x39, 0xc0,
        0xcb, 0x07, 0x07, 0xff,
};
static char video6[12] = {
        0x05, 0x00, 0x39, 0xc0,
        0xbb, 0x00, 0x00, 0xff,
        0x80, 0xff, 0xff, 0xff,
};

static char video7[8] = {
        0x03, 0x00, 0x39, 0xc0,
        0xde, 0x05, 0x58, 0xff,
};

static char video8[12] = {
        0x05, 0x00, 0x39, 0xc0,
        0xb6, 0x00, 0xa4, 0x00,
        0xa4, 0xff, 0xff, 0xff,
};

static char video9[40] = {
        0x23, 0x00, 0x39, 0xc0,
        0xe0, 0x05, 0x07, 0x16,
        0x2d, 0x2b, 0x3f, 0x39,
        0x4c, 0x06, 0x12, 0x18,
        0x19, 0x1a, 0x17, 0x18,
        0x10, 0x16, 0x05, 0x07,
        0x16, 0x2d, 0x2b, 0x3f,
        0x39, 0x4c, 0x06, 0x12,
        0x18, 0x19, 0x1a, 0x17,
        0x18, 0x10, 0x16, 0xff,
};

static char video10[132] = {
        0x80, 0x00, 0x39, 0xc0,
        0xc1, 0x01, 0x03, 0x05,
        0x0d, 0x16, 0x1C, 0x27,
        0x31, 0x38, 0x43, 0x4b,
        0x56, 0x60, 0x6a, 0x74,
        0x7D, 0x87, 0x8f, 0x97,
        0x9e, 0xa8, 0xb1, 0xba,
        0xc2, 0xca, 0xd1, 0xd9,
        0xe0, 0xe4, 0xea, 0xf1,
        0xf5, 0xfb, 0xff, 0x00,
        0x15, 0x2a, 0xec, 0x0d,
        0x49, 0x4a, 0x45, 0x00,
        0x00, 0x03, 0x0a, 0x12,
        0x19, 0x1f, 0x2a, 0x32,
        0x39, 0x42, 0x4a, 0x54,
        0x5D, 0x66, 0x6f, 0x78,
        0x80, 0x88, 0x8f, 0x96,
        0x9f, 0xa7, 0xb0, 0xb8,
        0xc1, 0xc9, 0xd1, 0xda,
        0xe2, 0xe7, 0xef, 0xf7,
        0xfd, 0x52, 0xdb, 0xa9,
        0x54, 0x57, 0x16, 0x64,
        0x56, 0x80, 0x01, 0x01,
        0x07, 0x0f, 0x17, 0x1b,
        0x26, 0x2c, 0x34, 0x3b,
        0x43, 0x4b, 0x54, 0x5d,
        0x66, 0x6f, 0x78, 0x80,
        0x88, 0x90, 0x96, 0x9a,
        0xa3, 0xab, 0xb4, 0xbd,
        0xc6, 0xce, 0xd7, 0xe1,
        0xe7, 0xf2, 0xfe, 0x00,
        0x23, 0x08, 0xbe, 0x7a,
        0xe7, 0xe2, 0x3e, 0x80,
};

static char video11[8] = {
        0x02, 0x00, 0x39, 0xc0,
        0x21, 0x00, 0xff, 0xff,
};

static struct mipi_dsi_cmd hx8389b_panel_video_mode_cmds[] = {
	{sizeof(video0), video0},
	{sizeof(video1), video1},
	{sizeof(video2), video2},
	{sizeof(video3), video3},
	{sizeof(video4), video4},
	{sizeof(video5), video5},
	{sizeof(video6), video6},
	{sizeof(video7), video7},
	{sizeof(video8), video8},
	{sizeof(video9), video9},
	{sizeof(video10), video10},
	{sizeof(video11), video11},
	{sizeof(exit_sleep), exit_sleep},
	{sizeof(display_on), display_on},
};

int mipi_hx8389b_video_qhd_config(void *pdata)
{
	int ret = NO_ERROR;
	/* 3 Lanes -- Enables Data Lane0, 1, 2 */
	unsigned char lane_en = 3;
	unsigned long low_pwr_stop_mode = 1;

	/* Needed or else will have blank line at top of display */
	unsigned char eof_bllp_pwr = 0x9;

	unsigned char interleav = 0;
	struct lcdc_panel_info *lcdc = NULL;
	struct msm_panel_info *pinfo = (struct msm_panel_info *) pdata;

	if (pinfo == NULL)
		return ERR_INVALID_ARGS;

	lcdc =  &(pinfo->lcdc);
	if (lcdc == NULL)
		return ERR_INVALID_ARGS;

	ret = mipi_dsi_video_mode_config((pinfo->xres),
			(pinfo->yres),
			(pinfo->xres),
			(pinfo->yres),
			(lcdc->h_front_porch),
			(lcdc->h_back_porch),
			(lcdc->v_front_porch),
			(lcdc->v_back_porch),
			(lcdc->h_pulse_width),
			(lcdc->v_pulse_width),
			pinfo->mipi.dst_format,
			pinfo->mipi.traffic_mode,
			lane_en,
			low_pwr_stop_mode,
			eof_bllp_pwr,
			interleav);
	return ret;
}

int mipi_hx8389b_video_qhd_on()
{
	int ret = NO_ERROR;
	return ret;
}

int mipi_hx8389b_video_qhd_off()
{
	int ret = NO_ERROR;
	return ret;
}

static struct mipi_dsi_phy_ctrl dsi_video_mode_phy_db = {
	/* DSI_BIT_CLK at 500MHz, 2 lane, RGB888 */
	{0x03, 0x01, 0x01, 0x00},	/* regulator */
	/* timing   */
	{0xb9, 0x8e, 0x1f, 0x00, 0x98, 0x9c, 0x22,
	0x90, 0x18, 0x03, 0x04},
	{0x7f, 0x00, 0x00, 0x00},	/* phy ctrl */
	{0xbb, 0x02, 0x06, 0x00},	/* strength */
	/* pll control */
	{0x00, 0xbb, 0x31, 0xd2, 0x00, 0x40, 0x37, 0x62,
	0x01, 0x0f, 0x07,	/*  --> Two lane configuration */
	0x05, 0x14, 0x03, 0x0, 0x0, 0x0, 0x20, 0x0, 0x02, 0x0},
};

void mipi_hx8389b_video_qhd_init(struct msm_panel_info *pinfo)
{
	if (!pinfo)
		return;

	pinfo->xres = 540;
	pinfo->yres = 960;
	pinfo->lcdc.h_back_porch = 55;
	pinfo->lcdc.h_front_porch = 105;
	pinfo->lcdc.h_pulse_width = 8;
	pinfo->lcdc.v_back_porch = 15;
	pinfo->lcdc.v_front_porch = 20;
	pinfo->lcdc.v_pulse_width = 1;
	pinfo->mipi.num_of_lanes = 2;

	pinfo->type = MIPI_VIDEO_PANEL;
	pinfo->wait_cycle = 0;
	pinfo->bpp = 24;
	pinfo->clk_rate = 499000000;

	pinfo->mipi.mode = DSI_VIDEO_MODE;
	pinfo->mipi.traffic_mode = 2;
	pinfo->mipi.dst_format = DSI_VIDEO_DST_FORMAT_RGB888;
	pinfo->mipi.dsi_phy_db = &dsi_video_mode_phy_db;
	pinfo->mipi.tx_eot_append = TRUE;

	pinfo->mipi.lane_swap = 1;
	pinfo->mipi.panel_cmds = hx8389b_panel_video_mode_cmds;
	pinfo->mipi.num_of_panel_cmds = \
			ARRAY_SIZE(hx8389b_panel_video_mode_cmds);

	pinfo->on = mipi_hx8389b_video_qhd_on;
	pinfo->off = mipi_hx8389b_video_qhd_off;
	pinfo->config = mipi_hx8389b_video_qhd_config;
	pinfo->rotate = NULL;

	return;
}
