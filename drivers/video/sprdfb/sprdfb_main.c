/*
 * Copyright (C) 2012 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/reboot.h>
#include <asm/uaccess.h>
#include "sprdfb.h"
#include "sprdfb_panel.h"
#include <mach/board.h>
#ifdef CONFIG_FB_MMAP_CACHED
#include <asm/pgtable.h>
#include <linux/mm.h>
#endif
#ifdef CONFIG_LCD_ESD_RECOVERY
#include "esd_detect.h"
#endif
#if defined(CONFIG_SEC_DEBUG)
#include <mach/sec_debug.h>
#endif
#include <video/sprd_fb.h>
extern void lcd_backlight_off(int num);
enum {
	SPRD_IN_DATA_TYPE_ABGR888 = 0,
	SPRD_IN_DATA_TYPE_BGR565,
/*
	SPRD_IN_DATA_TYPE_RGB666,
	SPRD_IN_DATA_TYPE_RGB555,
	SPRD_IN_DATA_TYPE_PACKET,
*/ /*not support*/
	SPRD_IN_DATA_TYPE_LIMIT
};
#define SPRDFB_IN_DATA_TYPE SPRD_IN_DATA_TYPE_ABGR888
#define SPRDFB_FRAMES_TO_SKIP	(1)
#define SPRDFB_DEFAULT_FPS	(60)
#define SPRDFB_ESD_TIME_OUT_CMD	(2000)
#define SPRDFB_ESD_TIME_OUT_VIDEO (500)
extern bool sprdfb_panel_get(struct sprdfb_device *dev);
extern int sprdfb_panel_probe(struct sprdfb_device *dev);
extern void sprdfb_panel_remove(struct sprdfb_device *dev);
extern struct display_ctrl sprdfb_dispc_ctrl ;
#ifdef CONFIG_FB_SC8825
extern struct display_ctrl sprdfb_lcdc_ctrl;
#endif
#ifdef CONFIG_FB_LCD_HX8369B_MIPI_DTC
static struct sprdfb_device *glo_dev = NULL;
#endif
static int lcd_is_dummy = 0;
extern struct panel_spec lcd_panel_dummy;
int recovery_mode;

static unsigned PP[16];
static int frame_count = 0;
static int sprdfb_check_var(struct fb_var_screeninfo *var, struct fb_info *fb);
static int sprdfb_pan_display(struct fb_var_screeninfo *var, struct fb_info *fb);
static int sprdfb_ioctl(struct fb_info *info, unsigned int cmd, \
							unsigned long arg);
#ifdef CONFIG_FB_MMAP_CACHED
static int sprdfb_mmap(struct fb_info *info, struct vm_area_struct *vma);
#endif
static struct fb_ops sprdfb_ops = {
	.owner = THIS_MODULE,
	.fb_check_var = sprdfb_check_var,
	.fb_pan_display = sprdfb_pan_display,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
	.fb_ioctl = sprdfb_ioctl,
#ifdef CONFIG_FB_MMAP_CACHED
	.fb_mmap = sprdfb_mmap,
#endif
};
#ifdef CONFIG_LCD_ESD_RECOVERY
	struct delayed_work enable_esd_work;
	struct sprdfb_device *dev_global = NULL;
#endif
#ifdef CONFIG_FB_MMAP_CACHED
static int sprdfb_mmap(struct fb_info *info,struct vm_area_struct *vma)
{
	struct sprdfb_device *dev = NULL;
	if (NULL == info) {
			printk(KERN_ERR "sprdfb: sprdfb_ioctl error.\n");
			return -1;
	}
	dev = info->par;
	printk("sprdfb: sprdfb_mmap,vma=0x%x\n", vma);
	vma->vm_page_prot = pgprot_cached(vma->vm_page_prot);
	dev->ctrl->set_vma(vma);
	return vm_iomap_memory(vma, info->fix.smem_start, info->fix.smem_len);
}
#endif
static int setup_fb_mem(struct sprdfb_device *dev, struct platform_device *pdev)
{
	uint32_t len, addr;
#ifdef CCONFIG_FB_LOW_RES_SIMU
	if ((0!= dev->panel->display_width) && \
					(0 != dev->panel->display_height)) {

		len = dev->panel->display_width * \
			dev->panel->display_height * \
			(dev->bpp / 8) * (dev->framebuffer_nr);
	} else
#endif
	len = dev->panel->width * dev->panel->height * (dev->bpp / 8) * \
							(dev->framebuffer_nr);
#ifndef	CONFIG_FB_LCD_RESERVE_MEM
	addr = __get_free_pages(GFP_ATOMIC | __GFP_ZERO, get_order(len));
	if (!addr) {
		printk(KERN_ERR "sprdfb:[%s] framebuffer allocation failed.\n", \
								__FUNCTION__);
		return -ENOMEM;
	}
	pr_debug(KERN_INFO "sprdfb: got %d bytes mem at 0x%lx\n", len, addr);
	dev->fb->fix.smem_start = __pa(addr);
	dev->fb->fix.smem_len = len;
	dev->fb->screen_base = (char*)addr;
#else
	dev->fb->fix.smem_start = SPRD_FB_MEM_BASE;
	printk("sprdfb:setup_fb_mem--smem_start:%lx,len:%d,reserved len:%d\n", \
				dev->fb->fix.smem_start, len, SPRD_FB_MEM_SIZE);
	addr =  (uint32_t)ioremap(SPRD_FB_MEM_BASE, len);
	if (!addr) {
		printk(KERN_ERR "sprdfb:[%s]Unable to map framebuffer base:0x%08x\n", \
							__FUNCTION__, addr);
		return -ENOMEM;
	}
	dev->fb->fix.smem_len = len;
	dev->fb->screen_base = (char*)addr;
#endif
	return 0;
}
static void setup_fb_info(struct sprdfb_device *dev)
{
	struct fb_info *fb = dev->fb;
	struct panel_spec *panel = dev->panel;
	int r;
	fb->fbops = &sprdfb_ops;
	fb->flags = FBINFO_DEFAULT;
	/* finish setting up the fb_info struct */
	strncpy(fb->fix.id, "sprdfb", 16);
	fb->fix.ypanstep = 1;
	fb->fix.type = FB_TYPE_PACKED_PIXELS;
	fb->fix.visual = FB_VISUAL_TRUECOLOR;
#ifdef CONFIG_FB_LOW_RES_SIMU
	if ((0 != panel->display_width) && (0 != panel->display_height)) {
		fb->fix.line_length = panel->display_width * dev->bpp / 8;
		fb->var.xres = panel->display_width;
		fb->var.yres = panel->display_height;
		fb->var.width = panel->display_width;
		fb->var.height = panel->display_height;
		fb->var.xres_virtual = panel->display_width;
		fb->var.yres_virtual = panel->display_height * \
							dev->framebuffer_nr;
	} else
#endif
	{
		fb->fix.line_length = panel->width * dev->bpp / 8;
		fb->var.xres = panel->width;
		fb->var.yres = panel->height;
		fb->var.width = panel->width;
		fb->var.height = panel->height;
		fb->var.xres_virtual = panel->width;
		fb->var.yres_virtual = panel->height * (dev->framebuffer_nr);
	}
	fb->var.bits_per_pixel = dev->bpp;
	if (0 != dev->panel->fps)
		fb->var.pixclock = ((1000000000 /panel->width) * 1000) \
					/ (dev->panel->fps * panel->height);
	else
		fb->var.pixclock = ((1000000000 /panel->width) * 1000) \
					/ (SPRDFB_DEFAULT_FPS * panel->height);

	fb->var.accel_flags = 0;
	fb->var.yoffset = 0;
	/* only support two pixel format */
	if (dev->bpp == 32) { /* ABGR */
		fb->var.red.offset     = 24;
		fb->var.red.length     = 8;
		fb->var.red.msb_right  = 0;
		fb->var.green.offset   = 16;
		fb->var.green.length   = 8;
		fb->var.green.msb_right = 0;
		fb->var.blue.offset    = 8;
		fb->var.blue.length    = 8;
		fb->var.blue.msb_right = 0;
	} else { /*BGR*/
		fb->var.red.offset     = 11;
		fb->var.red.length     = 5;
		fb->var.red.msb_right  = 0;
		fb->var.green.offset   = 5;
		fb->var.green.length   = 6;
		fb->var.green.msb_right = 0;
		fb->var.blue.offset    = 0;
		fb->var.blue.length    = 5;
		fb->var.blue.msb_right = 0;
	}
	r = fb_alloc_cmap(&fb->cmap, 16, 0);
	fb->pseudo_palette = PP;
	PP[0] = 0;

	for (r = 1; r < 16; r++)
		PP[r] = 0xffffffff;

#if defined(CONFIG_SEC_DEBUG)
	/*{{ Mark for GetLog*/
	sec_getlog_supply_fbinfo(phys_to_virt(fb->fix.smem_start) , \
						fb->var.xres, fb->var.yres, \
						fb->var.bits_per_pixel, \
						dev->framebuffer_nr);
#endif
}

static int __init mode_get(char *str)
{
	if (strcmp(str,"2") == 0)
		recovery_mode = 1;
	else
		recovery_mode = 0;
	return 1;
}
__setup("bootmode=", mode_get);

static void fb_free_resources(struct sprdfb_device *dev)
{
	if (dev == NULL)
		return;

	if (&dev->fb->cmap != NULL)
		fb_dealloc_cmap(&dev->fb->cmap);

	if (dev->fb->screen_base)
		free_pages((unsigned long)dev->fb->screen_base, \
				get_order(dev->fb->fix.smem_len));

	unregister_framebuffer(dev->fb);
	framebuffer_release(dev->fb);
}

static int sprdfb_pan_display(struct fb_var_screeninfo *var, struct fb_info *fb)
{
	int32_t ret;
	struct sprdfb_device *dev = fb->par;
	pr_debug("sprdfb: [%s]\n", __FUNCTION__);

	if (lcd_is_dummy)
	return 0;

	if (frame_count < SPRDFB_FRAMES_TO_SKIP) {
		frame_count++;
		return 0;
	}
	if (0 == dev->enable) {
		printk(KERN_ERR "sprdfb:[%s]: Invalid Device status %d", \
						__FUNCTION__, dev->enable);
		return -1;
	}
	ret = dev->ctrl->refresh(dev);
	if (ret) {
		printk(KERN_ERR "sprdfb: failed to refresh !!!!\n");
		return -1;
	}
	return 0;
}

static int sprdfb_ioctl(struct fb_info *info, unsigned int cmd, \
						unsigned long arg)
{
	int result = 0;
	struct sprdfb_device *dev = NULL;
#ifdef CONFIG_FB_LCD_OVERLAY_SUPPORT
	overlay_info local_overlay_info;
	overlay_display local_overlay_display;
	void __user *argp = (void __user *)arg;
#endif
	if (NULL == info) {
		printk(KERN_ERR "sprdfb:[%s] error.(Invalid Parameter)", \
								__FUNCTION__);
		return -1;
	}
	if (lcd_is_dummy)
		return 0;

	dev = info->par;

	switch (cmd) {
#ifdef CONFIG_FB_LCD_OVERLAY_SUPPORT
	case SPRD_FB_SET_OVERLAY:
		pr_debug(KERN_INFO "sprdfb: [%s]: SPRD_FB_SET_OVERLAY\n", \
								__FUNCTION__);
		memset(&local_overlay_info,0,sizeof(local_overlay_info));
		if (copy_from_user(&local_overlay_info, argp, \
						sizeof(local_overlay_info))) {
			printk("sprdfb: SET_OVERLAY copy failed!\n");
			return -EFAULT;
		}
		if (NULL != dev->ctrl->enable_overlay)
			result = dev->ctrl->enable_overlay(dev, \
							&local_overlay_info, 1);

		break;
	case SPRD_FB_DISPLAY_OVERLAY:
		pr_debug(KERN_INFO "sprdfb: [%s]: SPRD_FB_DISPLAY_OVERLAY\n", \
								__FUNCTION__);

		memset(&local_overlay_display,0,sizeof(local_overlay_display));
		if (copy_from_user(&local_overlay_display, argp, \
					sizeof(local_overlay_display))) {
			printk("sprdfb: DISPLAY_OVERLAY copy failed!\n");
			return -EFAULT;
		}
		if (NULL != dev->ctrl->display_overlay)
			result = dev->ctrl->display_overlay(dev, \
							&local_overlay_display);
		break;
#endif
#ifdef CONFIG_FB_VSYNC_SUPPORT
	case FBIO_WAITFORVSYNC:
		pr_debug(KERN_INFO "sprdfb: [%s]: FBIO_WAITFORVSYNC\n", \
								__FUNCTION__);
		if (NULL != dev->ctrl->wait_for_vsync)
			result = dev->ctrl->wait_for_vsync(dev);
		break;
#endif
#ifdef CONFIG_FB_DYNAMIC_FPS_SUPPORT
    case SPRD_FB_CHANGE_FPS:
		printk(KERN_INFO "sprdfb: [%s]: SPRD_FB_CHANGE_FPS\n", \
								__FUNCTION__);
		if (NULL != dev->ctrl->change_fps)
			result = dev->ctrl->change_fps(dev, (int)arg);
		break;
#endif
	case SPRD_FB_IS_REFRESH_DONE:
		pr_debug(KERN_INFO "sprdfb: [%s]: SPRD_FB_IS_REFRESH_DONE\n", \
								__FUNCTION__);
		if (NULL != dev->ctrl->is_refresh_done)
			result = dev->ctrl->is_refresh_done(dev);
		break;
	default:
		printk(KERN_INFO "sprdfb: [%s]: unknown cmd(%d)\n", \
							__FUNCTION__, cmd);
		break;
	}
	pr_debug(KERN_INFO "sprdfb: [%s]: return %d\n", \
							__FUNCTION__, result);
	return result;
}

static int sprdfb_check_var(struct fb_var_screeninfo *var, struct fb_info *fb)
{
	if ((var->xres != fb->var.xres) ||
		(var->yres != fb->var.yres) ||
		(var->xres_virtual != fb->var.xres_virtual) ||
		(var->yres_virtual != fb->var.yres_virtual) ||
		(var->xoffset != fb->var.xoffset) ||
#ifndef BIT_PER_PIXEL_SURPPORT
		(var->bits_per_pixel != fb->var.bits_per_pixel) ||
#endif
		(var->grayscale != fb->var.grayscale))
		return -EINVAL;

	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void sprdfb_early_suspend (struct early_suspend* es)
{
	struct sprdfb_device *dev = container_of(es, struct sprdfb_device, \
								early_suspend);
	struct fb_info *fb = dev->fb;
	printk("sprdfb: [%s]\n",__FUNCTION__);

	if (lcd_is_dummy)
	return;

#ifdef CONFIG_LCD_ESD_RECOVERY
	if (dev->panel->esd_info->mode != ESD_DET_NOT_REQUIRED)
		esd_det_disable(dev->panel->esd_info);
#endif
	fb_set_suspend(fb, FBINFO_STATE_SUSPENDED);
	if (!lock_fb_info(fb))
		return ;

	dev->ctrl->suspend(dev);
	unlock_fb_info(fb);
}

static void sprdfb_late_resume (struct early_suspend* es)
{
	struct sprdfb_device *dev = container_of(es, struct sprdfb_device, \
								early_suspend);
	struct fb_info *fb = dev->fb;
	pr_debug("sprdfb: [%s]\n",__FUNCTION__);

	if (lcd_is_dummy)
		return ;

	if (!lock_fb_info(fb))
		return ;

	dev->ctrl->resume(dev);
	unlock_fb_info(fb);
	fb_set_suspend(fb, FBINFO_STATE_RUNNING);
#ifdef CONFIG_LCD_ESD_RECOVERY
	if (dev->panel->esd_info->mode != ESD_DET_NOT_REQUIRED)
		esd_det_enable(dev->panel->esd_info);
#endif
}
#else
static int sprdfb_suspend(struct platform_device *pdev,pm_message_t state)
{
	struct sprdfb_device *dev = platform_get_drvdata(pdev);
	printk("sprdfb: [%s]\n",__FUNCTION__);

	if (lcd_is_dummy)
		return;

	dev->ctrl->suspend(dev);
	return 0;
}
static int sprdfb_resume(struct platform_device *pdev)
{
	struct sprdfb_device *dev = platform_get_drvdata(pdev);
	printk("sprdfb: [%s]\n",__FUNCTION__);

	if (lcd_is_dummy)
		return;

	dev->ctrl->resume(dev);
	return 0;
}
#endif
#ifdef CONFIG_FB_ESD_SUPPORT
static void ESD_work_func(struct work_struct *work)
{
	struct sprdfb_device *dev = container_of(work, struct sprdfb_device, \
								ESD_work.work);
	pr_debug("sprdfb: [%s] enter!\n", __FUNCTION__);
	/*
	do real ESD check
	mdelay(1000);
	*/
	if (NULL != dev->ctrl->ESD_check)
		dev->ctrl->ESD_check(dev);

	if (0 != dev->enable) {
		pr_debug("sprdfb: reschedule ESD workqueue!\n");
		schedule_delayed_work(&dev->ESD_work, \
					msecs_to_jiffies(dev->ESD_timeout_val));
		dev->ESD_work_start = true;
	} else
		printk("sprdfb:DON't reschedule ESD workqueue since device not avialbe!!\n");

	pr_debug("sprdfb: [%s] leave!\n", __FUNCTION__);
}
#endif

#ifdef CONFIG_LCD_ESD_RECOVERY
static int ESD_is_active(struct sprdfb_device *dev)
{
    return dev->enable;
}

static int  ESD_recover(struct sprdfb_device *dev)
{
    return dev->ctrl->ESD_reset(dev);
}

static void esd_enable_func(struct work_struct *work)
{
    esd_det_init(dev_global->panel->esd_info);
    esd_det_enable(dev_global->panel->esd_info);
}
#endif
#ifdef CONFIG_FB_LCD_HX8369B_MIPI_DTC
static int lcd_pcwd_notify_sys(struct notifier_block *this, unsigned long code,
								void *unused)
{
	struct sprdfb_device *dev = glo_dev;
	printk("sprdfb: [%s]\n",__FUNCTION__);
	if(dev==NULL)
	{
		printk("sprdfb: dev is NULL!\n");
		return 0;
	}
	if(code!=SYS_DOWN && code!=SYS_HALT && code!=SYS_POWER_OFF)
	{
		printk("sprdfb: error power-off messsage\n");
		return 0;
	}
	
	lcd_backlight_off(0);
	panel_pulldown_rstn();
	return 0;
}
static struct notifier_block lcd_pcwd_notifier = {
	.notifier_call =	lcd_pcwd_notify_sys,
};
#endif
static int sprdfb_probe(struct platform_device *pdev)
{
	struct fb_info *fb = NULL;
	struct sprdfb_device *dev = NULL;
	int ret = 0;

	pr_debug(KERN_INFO "sprdfb:[%s], id = %d\n", \
							__FUNCTION__, pdev->id);
	fb = framebuffer_alloc(sizeof(struct sprdfb_device), &pdev->dev);
	if (!fb) {
		printk(KERN_ERR "sprdfb:[%s] allocate buffer fail.\n", \
								__FUNCTION__);
		ret = -ENOMEM;
		goto err0;
	}
	dev = fb->par;
	dev->fb = fb;
	dev->dev_id = pdev->id;
#ifdef CONFIG_FB_LCD_HX8369B_MIPI_DTC
	glo_dev=dev;
#endif
	
	/*
	Since it do not support TRIPLE Framebuffer during Recovery; 
	So,we made here to take Two Framebuffer during recovery, 
	and in other case it will take three (Due to Triple Framebuffer 
	in recovery mode,we got Black screen instead of Android image)
	*/
	if (recovery_mode == 1)
		dev->framebuffer_nr = 2;
	else
		dev->framebuffer_nr = 3;

	if ((SPRDFB_MAINLCD_ID != dev->dev_id) && \
					(SPRDFB_SUBLCD_ID != dev->dev_id)) {
		printk(KERN_ERR "sprdfb:[%s] fail. (unsupported device id)\n", \
								__FUNCTION__);
		goto err0;
	}

	switch (SPRDFB_IN_DATA_TYPE) {
	case SPRD_IN_DATA_TYPE_ABGR888:
		dev->bpp = 32;
		break;
	case SPRD_IN_DATA_TYPE_BGR565:
		dev->bpp = 16;
		break;
	default:
		dev->bpp = 32;
		break;
	}

	if (SPRDFB_MAINLCD_ID == dev->dev_id)
		dev->ctrl = &sprdfb_dispc_ctrl;
#ifdef CONFIG_FB_SC8825
	else
		dev->ctrl = &sprdfb_lcdc_ctrl;
#endif

	dev->logo_buffer_addr_v = 0;
	if (sprdfb_panel_get(dev)) {
		dev->panel_ready = true;
		lcd_is_dummy = 0;

		dev->ctrl->logo_proc(dev);
	} else {
		dev->panel_ready = false;

		lcd_is_dummy = 1;

		dev->panel = &lcd_panel_dummy;
	}
	dev->ctrl->early_init(dev);
#if 0
	if (!dev->panel_ready) {
		if (!sprdfb_panel_probe(dev)) {
			ret = -EIO;
			goto cleanup;
		}
	}
#endif
	ret = setup_fb_mem(dev, pdev);
	if (ret)
		goto cleanup;

	setup_fb_info(dev);

	if (!lcd_is_dummy)
		dev->ctrl->init(dev);

	/* register framebuffer device */
	ret = register_framebuffer(fb);
	if (ret) {
		printk(KERN_ERR "sprdfb:[%s] register framebuffer fail.\n", \
								__FUNCTION__);
		goto cleanup;
	}
	platform_set_drvdata(pdev, dev);

#ifdef CONFIG_HAS_EARLYSUSPEND
	dev->early_suspend.suspend = sprdfb_early_suspend;
	dev->early_suspend.resume  = sprdfb_late_resume;
	dev->early_suspend.level   = EARLY_SUSPEND_LEVEL_DISABLE_FB;
	register_early_suspend(&dev->early_suspend);
#endif
#ifdef CONFIG_FB_ESD_SUPPORT
	pr_debug("sprdfb: Init ESD work queue!\n");

	INIT_DELAYED_WORK(&dev->ESD_work, ESD_work_func);

	if (SPRDFB_PANEL_IF_DPI == dev->panel_if_type)
		dev->ESD_timeout_val = SPRDFB_ESD_TIME_OUT_VIDEO;
	else
		dev->ESD_timeout_val = SPRDFB_ESD_TIME_OUT_CMD;

	dev->ESD_work_start = false;
	dev->check_esd_time = 0;
	dev->reset_dsi_time = 0;
	dev->panel_reset_time = 0;
#endif
#ifdef CONFIG_LCD_ESD_RECOVERY
	if (!lcd_is_dummy && dev->panel->esd_info->mode != ESD_DET_NOT_REQUIRED) {
		printk("enable esd %s, mode %d\n", dev->panel->esd_info->name, \
		dev->panel->esd_info->mode);
		dev->panel->esd_info->pdata = dev;
		dev->panel->esd_info->is_active = (bool (*)(void *))ESD_is_active;
		dev->panel->esd_info->recover = (int (*)(void *))ESD_recover;

		dev_global = dev;

		INIT_DELAYED_WORK(&enable_esd_work, esd_enable_func);
		schedule_delayed_work(&enable_esd_work, msecs_to_jiffies(5000));
	}
#endif
#ifdef CONFIG_FB_LCD_HX8369B_MIPI_DTC
	ret = register_reboot_notifier(&lcd_pcwd_notifier);
	if (ret != 0) {
		printk("sprdfb: cannot register reboot notifier (err=%d)\n", ret);
		goto cleanup;
	}
#endif
	return 0;
cleanup:
	sprdfb_panel_remove(dev);
	dev->ctrl->uninit(dev);
	fb_free_resources(dev);
err0:
	dev_err(&pdev->dev, "failed to probe sprdfb\n");
	return ret;
}
static int sprdfb_remove(struct platform_device *pdev)
{
	struct sprdfb_device *dev = platform_get_drvdata(pdev);
	printk("sprdfb: [%s]\n",__FUNCTION__);
	sprdfb_panel_remove(dev);
	dev->ctrl->uninit(dev);
	fb_free_resources(dev);
	return 0;
}
static struct platform_driver sprdfb_driver = {
	.probe = sprdfb_probe,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend = sprdfb_suspend,
	.resume = sprdfb_resume,
#endif
	.remove = sprdfb_remove,
	.driver = {
		.name = "sprd_fb",
		.owner = THIS_MODULE,
	},
};
static int __init sprdfb_init(void)
{
	return platform_driver_register(&sprdfb_driver);
}
static void __exit sprdfb_exit(void)
{
	return platform_driver_unregister(&sprdfb_driver);
}
module_init(sprdfb_init);
module_exit(sprdfb_exit);