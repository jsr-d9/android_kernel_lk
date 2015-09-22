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
 *     * Neither the name of Code Aurora Forum, Inc. nor the names of its
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
#include <debug.h>
#include <msm_panel.h>
#include <target/display.h>
#include <target/board.h>

static struct msm_fb_panel_data panel;
static uint8_t display_enabled;

extern int msm_display_init(struct msm_fb_panel_data *pdata);
extern int msm_display_off();
extern int mipi_renesas_panel_dsi_config(int);
extern int mipi_nt35510_panel_dsi_config(int);
extern int mipi_hx8389b_panel_dsi_config(int);
#if DISPLAY_MIPI_CMD_PANEL_NOVATEK_SHARP_QHD
extern int pcom_init_backlight(void);
extern int pcom_set_backlight(int);
extern int mipi_novatek_sharp_panel_dsi_config(int);
#endif

static int msm7627a_mdp_clock_init(int enable)
{
	int ret = 0;
	unsigned rate = 0;

	rate = panel.panel_info.clk_rate;

	if (enable) {
                /* enable MDP clock in MP to boost display up */
                if (board_machtype() != MSM8X25_QRD5
                        && board_machtype() != MSM8X25Q_SKUD)
                        mdp_clock_init(rate);
	} else {
		mdp_clock_disable();
        }
	return ret;
}

void display_init(void)
{
	unsigned mach_type;
	mach_type = board_machtype();

	dprintf(SPEW, "display_init\n");

	switch (mach_type) {
	case MSM7X27A_SURF:
	case MSM8X25_SURF:
	case MSM7X27A_FFA:
#if MIPI_VIDEO_MODE
		mipi_renesas_video_fwvga_init(&(panel.panel_info));
#else
		mipi_renesas_cmd_fwvga_init(&(panel.panel_info));
#endif
		panel.clk_func = msm7627a_mdp_clock_init;
		panel.power_func = mipi_renesas_panel_dsi_config;
		panel.fb.base = MIPI_FB_ADDR;
		panel.fb.width =  panel.panel_info.xres;
		panel.fb.height =  panel.panel_info.yres;
		panel.fb.stride =  panel.panel_info.xres;
		panel.fb.bpp =  panel.panel_info.bpp;
		panel.fb.format = FB_FORMAT_RGB888;
		panel.mdp_rev = MDP_REV_303;
		break;
	case MSM7X25A_SURF:
	case MSM7X25A_FFA:
#if MIPI_VIDEO_MODE
		mipi_renesas_video_hvga_init(&(panel.panel_info));
#else
		mipi_renesas_cmd_hvga_init(&(panel.panel_info));
#endif
		panel.clk_func = msm7627a_mdp_clock_init;
		panel.power_func = mipi_renesas_panel_dsi_config;
		panel.fb.base = MIPI_FB_ADDR;
		panel.fb.width =  panel.panel_info.xres;
		panel.fb.height =  panel.panel_info.yres;
		panel.fb.stride =  panel.panel_info.xres;
		panel.fb.bpp =  panel.panel_info.bpp;
		panel.fb.format = FB_FORMAT_RGB888;
		panel.mdp_rev = MDP_REV_303;
		break;
	case MSM7X27A_EVB:
        case MSM7X27A_QRD5A:
	case MSM8X25_EVB:
	case MSM8X25_QRD5:
#if MIPI_VIDEO_MODE
		mipi_nt35510_video_wvga_init(&(panel.panel_info));
#else
	#if DISPLAY_MIPI_CMD_PANEL_NOVATEK_SHARP_QHD	
		mipi_novatek_sharp_cmd_qhd_init(&(panel.panel_info));
	#else
		mipi_nt35510_cmd_wvga_init(&(panel.panel_info));
	#endif
#endif
		panel.clk_func = msm7627a_mdp_clock_init;
	#if DISPLAY_MIPI_CMD_PANEL_NOVATEK_SHARP_QHD		
		panel.power_func = mipi_novatek_sharp_panel_dsi_config;
	#else
		panel.power_func = mipi_nt35510_panel_dsi_config;
	#endif
		panel.fb.base = MIPI_FB_ADDR;
		panel.fb.width =  panel.panel_info.xres;
		panel.fb.height =  panel.panel_info.yres;
		panel.fb.stride =  panel.panel_info.xres;
		panel.fb.bpp =  panel.panel_info.bpp;
		panel.fb.format = FB_FORMAT_RGB888;
		panel.mdp_rev = MDP_REV_303;
		if (mach_type == MSM8X25_QRD5 || mach_type == MSM7X27A_QRD5A)
			panel.rotate = 1;
		break;
	case MSM8X25Q_SKUD:
               mipi_hx8389b_video_qhd_init(&(panel.panel_info));
               panel.clk_func = msm7627a_mdp_clock_init;
               panel.power_func = mipi_hx8389b_panel_dsi_config;
               panel.fb.base = MIPI_FB_ADDR;
               panel.fb.width =  panel.panel_info.xres;
               panel.fb.height =  panel.panel_info.yres;
               panel.fb.stride =  panel.panel_info.xres;
               panel.fb.bpp =  panel.panel_info.bpp;
               panel.fb.format = FB_FORMAT_RGB888;
               panel.mdp_rev = MDP_REV_303;
               break;
	default:
		return;
	};

	if (msm_display_init(&panel)) {
		dprintf(CRITICAL, "Display init failed!\n");
		return;
	}

	#if DISPLAY_MIPI_CMD_PANEL_NOVATEK_SHARP_QHD
	    	//turn on backlight
		dprintf(INFO, "target_init  Turn on LCD Backlight using Pcom PM8029\n");
              pcom_init_backlight();
	    	mdelay(5);  				  
              pcom_set_backlight(180);
		//mdelay(30); 			  			//modified for innos logo issue
	#endif
	
	display_enabled = 1;
}

void display_shutdown(void)
{
	dprintf(SPEW, "display_shutdown()\n");
	if (display_enabled)
		msm_display_off();
}
