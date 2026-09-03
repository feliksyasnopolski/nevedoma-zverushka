// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/memremap.h>
#include <linux/module.h>

#define TB330FU_LK_FRAMEBUFFER_PHYSICAL_BASE 0x7bce0000ULL
#define TB330FU_LK_FRAMEBUFFER_LENGTH 0x008ca000ULL

/*
 * LK leaves a stale boot framebuffer in the MediaTek reserved framebuffer
 * region. The inherited display path can expose those pixels during DRM
 * compositor handoff gaps. Clearing the confirmed first framebuffer slice
 * once before status-screen starts prevents the LK splash from reappearing.
 */
static int clear_confirmed_lk_framebuffer(void)
{
	void *framebuffer_mapping;

	framebuffer_mapping = memremap(
		(phys_addr_t)TB330FU_LK_FRAMEBUFFER_PHYSICAL_BASE,
		TB330FU_LK_FRAMEBUFFER_LENGTH, MEMREMAP_WB);
	if (!framebuffer_mapping) {
		pr_err("tb330fu_lk_fb_clear: memremap(MEMREMAP_WB) failed "
		       "physical=0x%llx length=0x%llx\n",
		       (unsigned long long)TB330FU_LK_FRAMEBUFFER_PHYSICAL_BASE,
		       (unsigned long long)TB330FU_LK_FRAMEBUFFER_LENGTH);
		return -ENOMEM;
	}

	memset(framebuffer_mapping, 0, TB330FU_LK_FRAMEBUFFER_LENGTH);
	wmb();
	memunmap(framebuffer_mapping);
	pr_info("tb330fu_lk_fb_clear: cleared physical=0x%llx length=0x%llx "
		"mapping=memremap(MEMREMAP_WB)\n",
		(unsigned long long)TB330FU_LK_FRAMEBUFFER_PHYSICAL_BASE,
		(unsigned long long)TB330FU_LK_FRAMEBUFFER_LENGTH);
	return 0;
}

static int __init tb330fu_lk_fb_clear_init(void)
{
	return clear_confirmed_lk_framebuffer();
}

static void __exit tb330fu_lk_fb_clear_exit(void)
{
	pr_info("tb330fu_lk_fb_clear: unloaded; no persistent mapping or control surface\n");
}

module_init(tb330fu_lk_fb_clear_init);
module_exit(tb330fu_lk_fb_clear_exit);

MODULE_DESCRIPTION("TB330FU one-shot LK framebuffer clear");
MODULE_LICENSE("GPL");
