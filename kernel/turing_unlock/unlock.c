// SPDX-License-Identifier: GPL-2.0-only
/* TU102 pre-driver V67 unlock with a complete ACR mutex handoff. */
#include <linux/aperture.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

#include "boot.h"
#include "diagnostic.h"
#include "firmware.h"
#include "gfw_eval.h"
#include "layout.h"
#include "prep.h"
#include "unlock.h"
#include "vbios.h"

#define GSP_BASE 0x00110000u
#define GSP_FBIF 0x00110600u

#define SEC_BASE 0x00840000u
#define SEC_FBIF 0x00840600u
#define SEC_FALCON2 0x00841000u

#define TU_FEAT_PLM 0x00409650u
#define TU_FEAT_SS0 0x00409664u
#define TU_FEAT_SS1 0x0040966cu
#define TU_FEAT_READOUT 0x00409668u
#define TU_SS0_UNLOCK 0x88888888u
#define TU_SS1_UNLOCK 0x8u

#define BOOTER_FILE "turing-unlock/610.43.03/booter_load_tu102.bin"
#define V67_FILE "turing-unlock/610.43.03/v67-tu102-610.43.03.bin"
#define GSP_FILE "turing-unlock/610.43.03/gsp_tu10x.bin"
#define BOOT_FILE "turing-unlock/610.43.03/gsp_boot_tu102.bin"
#define BOOT_DESC_FILE "turing-unlock/610.43.03/gsp_boot_desc_tu102.bin"
#define SEC2_BL_FILE "turing-unlock/610.43.03/sec2_bl_tu102.bin"

#define TU_V67_SIZE 0xfa00u
#define TU_SEC2_BL_SIZE 768u

struct tu_work {
	struct pci_dev *pdev;
	void __iomem *bar;
	resource_size_t bar_len;
	const struct firmware *booter_fw, *v67_fw, *gsp_fw, *boot_fw, *desc_fw, *bl_fw;
	struct turing_unlock_span image_span;
	struct turing_unlock_booter_hdr hdr;
	struct turing_unlock_boot_desc boot_desc;
	struct turing_unlock_radix_plan radix;
	struct turing_unlock_layout_in layout_in;
	struct turing_unlock_layout_out layout;
	struct page_list { void *cpu; dma_addr_t dma; } *pages;
	u32 page_count, pages_allocated;
	void *booter_cpu, *v67_cpu, *meta_cpu, *boot_cpu;
	dma_addr_t booter_dma, v67_dma, meta_dma, boot_dma;
	void *fwsec_code_cpu, *fwsec_data_cpu;
	dma_addr_t fwsec_code_dma, fwsec_data_dma;
	u32 fwsec_code_size, fwsec_data_size;
	u64 *streaming_ptr, streaming_before, coherent_before;
	u16 original_cmd;
	bool cmd_valid, enabled, master;
	bool streaming_set, coherent_set;
	bool gsp_started, gsp_quiesced, sec_started, sec_quiesced;
	bool retained;
	u32 last_off, last_val;
};

static u32 rd(struct tu_work *w, u32 off)
{
	u32 v = readl(w->bar + off);

	w->last_off = off;
	w->last_val = v;
	return v;
}

static void wr(struct tu_work *w, u32 off, u32 val)
{
	w->last_off = off;
	w->last_val = val;
	writel(val, w->bar + off);
}

static bool bad(u32 v)
{
	return v == ~0u || (v & 0xffff0000u) == 0xbadf0000u;
}

static int poll(struct tu_work *w, u32 off, u32 mask, u32 expect, unsigned int us, bool fatal)
{
	ktime_t deadline = ktime_add_us(ktime_get(), us);

	for (;;) {
		u32 v = rd(w, off);

		if (bad(v))
			return GU_DIAG(-EIO);
		if ((v & mask) == expect)
			return 0;
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			if (!fatal) {
				GU_NOTE("poll tolerated timeout: reg=%#x last=%#x want=%#x&%#x\n",
					off, v, expect, mask);
				return -ETIMEDOUT;
			}
			return GU_DIAG(-ETIMEDOUT);
		}
		usleep_range(50, 100);
	}
}

static int wait_gfw(struct tu_work *w)
{
	ktime_t deadline = ktime_add_us(ktime_get(), GU_GFW_TIMEOUT_US);

	for (;;) {
		struct turing_unlock_gfw_sample sample = { .config_ok = true };
		int ret;

		sample.plm = rd(w, GU_GFW_PLM_OFFSET);
		sample.cpuctl = rd(w, GU_GFW_CPUCTL_OFFSET);
		if (!bad(sample.plm) && (sample.plm & GU_GFW_PLM_LEVEL0))
			sample.boot = rd(w, GU_GFW_BOOT_OFFSET);
		if (bad(sample.plm) || bad(sample.boot) || bad(sample.cpuctl))
			ret = -EAGAIN;
		else
			ret = turing_unlock_gfw_assess(&sample);
		if (!ret) {
			GU_NOTE("GFW boot complete: plm=%#x boot=%#x cpuctl=%#x\n",
				sample.plm, sample.boot, sample.cpuctl);
			return 0;
		}
		if (ret != -EAGAIN)
			return GU_DIAG(ret);
		if (ktime_compare(ktime_get(), deadline) >= 0)
			return GU_DIAG(-ETIMEDOUT);
		usleep_range(1000, 2000);
	}
}

/* kflcnReset_TU102: scrub status is in DMACTL, and there is no BCR switch. */
static int flcn_reset(struct tu_work *w, u32 base)
{
	u32 engine;
	unsigned int i;
	int ret;

	engine = rd(w, base + 0x3c0);
	if (bad(engine))
		return GU_DIAG(-ENODEV);
	wr(w, base + 0x3c0, engine | 1);
	for (i = 0; i < 10; i++)
		readl(w->bar + base + 0x3c0);
	wr(w, base + 0x3c0, engine & ~1u);
	for (i = 0; i < 10; i++)
		readl(w->bar + base + 0x3c0);
	ret = poll(w, base + 0x10c, 0x6, 0, 1000000, true);
	if (ret)
		return GU_DIAG(ret);
	wr(w, base + 0x84, rd(w, 0));
	GU_NOTE("flcn reset done: base=%#x\n", base);
	return 0;
}

static int sec_reset(struct tu_work *w)
{
	return flcn_reset(w, SEC_BASE);
}

static void flcn_pio_imem(struct tu_work *w, u32 base, u32 dst, const void *src,
			  u32 size, bool secure, u32 tag)
{
	const u32 *words = src;
	u32 i, n = size >> 2;
	u32 c = ((dst >> 2) & 0x3fu) | (((dst >> 8) & 0xffu) << 8) | (1u << 24);

	if (secure)
		c |= 1u << 28;
	wr(w, base + 0x180, c); /* IMEMC(0): offs/blk/AINCW(+SECURE) */
	tag >>= 8;
	for (i = 0; i < n; i++) {
		if (!(i & 63))
			wr(w, base + 0x188, tag++); /* IMEMT(0), per 256B block */
		wr(w, base + 0x184, words[i]);      /* IMEMD(0) */
	}
}

static void flcn_pio_dmem(struct tu_work *w, u32 base, u32 dst, const void *src,
			  u32 size)
{
	const u32 *words = src;
	u32 i, n = size >> 2;
	u32 c = ((dst >> 2) & 0x3fu) | (((dst >> 8) & 0xffu) << 8) | (1u << 24);

	wr(w, base + 0x1c0, c); /* DMEMC(0): offs/blk/AINCW */
	for (i = 0; i < n; i++)
		wr(w, base + 0x1c4, words[i]); /* DMEMD(0) */
}

static void sec_pio_imem(struct tu_work *w, u32 dst, const void *src, u32 size,
			 bool secure, u32 tag)
{
	flcn_pio_imem(w, SEC_BASE, dst, src, size, secure, tag);
}

static void sec_pio_dmem(struct tu_work *w, u32 dst, const void *src, u32 size)
{
	flcn_pio_dmem(w, SEC_BASE, dst, src, size);
}

#define TU_DMA_CMD_DMEM (6u << 8)
#define TU_DMA_CMD_IMEM_NS (TU_DMA_CMD_DMEM | (1u << 4))
#define TU_DMA_CMD_IMEM_SEC (TU_DMA_CMD_DMEM | (1u << 4) | (1u << 2))

static int alloc_dma(struct tu_work *w, const char *name, size_t size, void **cpu, dma_addr_t *dma)
{
	*cpu = dma_alloc_coherent(&w->pdev->dev, size, dma, GFP_KERNEL);
	if (!*cpu) {
		GU_ERROR("dma alloc failed: %s %zu bytes\n", name, size);
		return GU_DIAG(-ENOMEM);
	}
	return 0;
}

struct tu_bl_dmem_desc {
	u32 reserved[4];
	u32 signature[4];
	u32 ctx_dma;
	u32 code_dma_lo;
	u32 code_dma_hi;
	u32 ns_code_off;
	u32 ns_code_size;
	u32 sec_code_off;
	u32 sec_code_size;
	u32 code_entry_point;
	u32 data_dma_lo;
	u32 data_dma_hi;
	u32 data_size;
	u32 argc;
	u32 argv;
};

static int gsp_fwsec_frts(struct tu_work *w)
{
	size_t mapped;
	u8 *rom = NULL;
	struct turing_unlock_fwsec fw;
	struct tu_bl_dmem_desc bldesc = { 0 };
	u32 trans, hwcfg, imem_blks, dst_blk, cpuctl, scratch, wpr_lo, wpr_hi;
	u64 frts = w->layout.frts_offset;
	int ret;

	wpr_hi = rd(w, 0x1fa828);
	wpr_lo = rd(w, 0x1fa824);
	if ((wpr_hi >> 4) && (wpr_lo >> 4) == (u32)(frts >> 12)) {
		GU_NOTE("FWSEC: WPR2 already up at %#x:%#x\n", wpr_hi, wpr_lo);
		return 0;
	}
	if (w->bar_len < 0x300000 + 0x1a)
		return GU_DIAG(-ENODEV);
	mapped = min_t(size_t, 1024 * 1024, w->bar_len - 0x300000);
	rom = kvmalloc(mapped, GFP_KERNEL);
	if (!rom)
		return GU_DIAG(-ENOMEM);
	memcpy_fromio(rom, w->bar + 0x300000, mapped);
	ret = turing_unlock_vbios_fwsec(rom, mapped, &fw);
	if (ret)
		goto out;
	if (fw.desc_version != 2) {
		ret = GU_DIAG(-EOPNOTSUPP);
		goto out;
	}
	/* Patch FRTS command into VBIOS image buffer before copying to DMA */
	ret = turing_unlock_fwsec_patch_frts(rom + fw.image_off, &fw, frts);
	if (ret)
		goto out;
	w->fwsec_code_size = (fw.imem_size + 255u) & ~255u;
	w->fwsec_data_size = (fw.dmem_size + 255u) & ~255u;
	ret = alloc_dma(w, "fwsec-code", w->fwsec_code_size, &w->fwsec_code_cpu, &w->fwsec_code_dma);
	if (ret)
		goto out;
	ret = alloc_dma(w, "fwsec-data", w->fwsec_data_size, &w->fwsec_data_cpu, &w->fwsec_data_dma);
	if (ret)
		goto out;
	memset(w->fwsec_code_cpu, 0, w->fwsec_code_size);
	memcpy(w->fwsec_code_cpu + fw.imem_pa, rom + fw.image_off, fw.imem_size);
	memset(w->fwsec_data_cpu, 0, w->fwsec_data_size);
	memcpy(w->fwsec_data_cpu + fw.dmem_pa, rom + fw.image_off + fw.data_off, fw.dmem_size);
	dma_wmb();

	bldesc.ctx_dma = 4;
	bldesc.code_dma_lo = lower_32_bits(w->fwsec_code_dma);
	bldesc.code_dma_hi = upper_32_bits(w->fwsec_code_dma);
	bldesc.ns_code_off = fw.imem_pa;
	bldesc.ns_code_size = fw.imem_ns_size;
	bldesc.sec_code_off = fw.imem_sec_pa;
	bldesc.sec_code_size = fw.imem_sec_size;
	bldesc.code_entry_point = 0;
	bldesc.data_dma_lo = lower_32_bits(w->fwsec_data_dma);
	bldesc.data_dma_hi = upper_32_bits(w->fwsec_data_dma);
	bldesc.data_size = fw.dmem_size;

	ret = flcn_reset(w, GSP_BASE);
	if (ret)
		goto out;
	/* kflcnDisableCtxReq */
	trans = rd(w, GSP_FBIF + 0x24);
	if (bad(trans)) {
		ret = GU_DIAG(-EIO);
		goto out;
	}
	wr(w, GSP_FBIF + 0x24, trans | (1u << 7));
	wr(w, GSP_BASE + 0x10c, 0);
	/* s_setupLoaderAperture: dmaIdx=4 -> coherent sysmem, physical */
	trans = rd(w, GSP_FBIF + 4 * 4);
	if (bad(trans)) {
		ret = GU_DIAG(-EIO);
		goto out;
	}
	wr(w, GSP_FBIF + 4 * 4, (trans & ~7u) | 5u);

	flcn_pio_dmem(w, GSP_BASE, 0, &bldesc, sizeof(bldesc));
	hwcfg = rd(w, GSP_BASE + 0x108);
	imem_blks = hwcfg & 0x1ffu;
	dst_blk = imem_blks - 2u; /* blCodeSize = 0x200 = 2 blocks */
	GU_NOTE("FWSEC loader: hwcfg=%#x imem_blks=%u dst_blk=%u frts=%#llx\n",
		hwcfg, imem_blks, dst_blk, (unsigned long long)frts);
	flcn_pio_imem(w, GSP_BASE, dst_blk << 8, w->bl_fw->data, 0x200u,
		      false, 0xfd00u);
	wr(w, GSP_BASE + 0x104, 0xfd00u); /* BOOTVEC = 0xfd00 */
	wr(w, GSP_BASE + 0x40, 0);
	wr(w, GSP_BASE + 0x44, 0);
	cpuctl = rd(w, GSP_BASE + 0x100);
	if (bad(cpuctl)) {
		ret = GU_DIAG(-EIO);
		goto out;
	}
	w->gsp_started = true;
	if (cpuctl & (1u << 6))
		wr(w, GSP_BASE + 0x130, 1u << 1);
	else
		wr(w, GSP_BASE + 0x100, 1u << 1);
	ret = poll(w, GSP_BASE + 0x100, 1u << 4, 1u << 4, 5000000, true);
	scratch = rd(w, 0x001400 + 0x0e * 4);
	wpr_lo = rd(w, 0x1fa824);
	wpr_hi = rd(w, 0x1fa828);
	GU_NOTE("FWSEC done: ret=%d mb0=%#x mb1=%#x scratch0e=%#x wpr2=%#x:%#x\n",
		ret, rd(w, GSP_BASE + 0x40), rd(w, GSP_BASE + 0x44),
		scratch, wpr_hi, wpr_lo);
	if (ret)
		goto out;
	if ((scratch >> 16) || !(wpr_hi >> 4) || (wpr_lo >> 4) != (u32)(frts >> 12)) {
		GU_ERROR("FWSEC FRTS check failed: scratch=%#x wpr2=%#x:%#x want_lo=%#x\n",
			 scratch, wpr_hi, wpr_lo, (u32)(frts >> 12));
		ret = GU_DIAG(-EIO);
	}
out:
	kvfree(rom);
	return ret;
}

static int sec_dma(struct tu_work *w, u32 dest, u32 mem_off, dma_addr_t src, u32 size, u32 cmd)
{
	u32 done = 0;
	int ret;

	if (!size || ((size | dest | mem_off | src) & 255) ||
	    (u64)dest + size > (1ull << 24) || (u64)mem_off + size > (1ull << 32) ||
	    src >= (1ull << 49))
		return GU_DIAG(-EINVAL);
	ret = poll(w, SEC_BASE + 0x118, 1, 0, 1000000, true);
	if (ret)
		return GU_DIAG(ret);
	wr(w, SEC_BASE + 0x110, lower_32_bits(src >> 8));
	wr(w, SEC_BASE + 0x128, upper_32_bits(src >> 8) & 0x1ff);
	while (done < size) {
		ret = poll(w, SEC_BASE + 0x118, 1, 0, 1000000, true);
		if (ret)
			return GU_DIAG(ret);
		wr(w, SEC_BASE + 0x114, dest);
		wr(w, SEC_BASE + 0x11c, mem_off);
		wr(w, SEC_BASE + 0x118, cmd);
		done += 256;
		dest += 256;
		mem_off += 256;
	}
	return GU_DIAG(poll(w, SEC_BASE + 0x118, 2, 2, 1000000, true));
}

static int sec_hs_fire(struct tu_work *w, dma_addr_t meta)
{
	const struct turing_unlock_booter_hdr *h = &w->hdr;
	u32 trans, cpuctl = 0, plm = 0, mb0, mb1, saw_running = 0;
	ktime_t deadline;
	int ret;

	ret = sec_reset(w);
	if (ret)
		return GU_DIAG(ret);
	/* kflcnWaitForResetToFinish: IMEM/DMEM scrubbing done (bits read 0). */
	ret = poll(w, SEC_BASE + 0x10c, 0x6, 0, 1000000, true);
	if (ret)
		return GU_DIAG(ret);
	/* kflcnDisableCtxReq: FBIF_CTL.ALLOW_PHYS_NO_CTX + DMACTL := 0. */
	trans = rd(w, SEC_FBIF + 0x24);
	if (bad(trans))
		return GU_DIAG(-EIO);
	wr(w, SEC_FBIF + 0x24, trans | (1u << 7));
	wr(w, SEC_BASE + 0x10c, 0);
	/* TRANSCFG(0) = coherent sysmem, physical (staging aperture). Writable
	 * in the post-FLR no-RM state this module requires. */
	trans = rd(w, SEC_FBIF);
	if (bad(trans))
		return GU_DIAG(-ENODEV);
	wr(w, SEC_FBIF, (trans & ~7u) | 5u);
	trans = rd(w, SEC_FBIF);
	if (bad(trans) || (trans & 7u) != 5u)
		return GU_DIAG(-EIO);
	dma_wmb();

	/* s_prepareHsFalconDirect: PIO staging of the three regions. */
	sec_pio_imem(w, 0, w->booter_cpu + h->os_code_off, h->os_code_size,
		     false, h->os_code_off);
	sec_pio_imem(w, (h->os_code_size + 255u) & ~255u,
		     w->booter_cpu + h->app_code_off, h->app_code_size,
		     true, h->app_code_off);
	sec_pio_dmem(w, 0, w->booter_cpu + h->os_data_off, h->os_data_size);

	/* Verify staging by DMAing falcon memory back (DMATRFCMD.WRITE=1).
	 * Secure IMEM pages read back as zeros by design; check the NS stub
	 * and the patched signature in DMEM. */
	{
		void *rb;
		dma_addr_t rb_dma;

		rb = dma_alloc_coherent(&w->pdev->dev, 256, &rb_dma, GFP_KERNEL);
		if (rb) {
			u32 *s = rb;
			bool ok_ns, ok_dmem;

			memset(rb, 0xa5, 256);
			ret = sec_dma(w, 0, 0, rb_dma, 256,
				      TU_DMA_CMD_IMEM_NS | (1u << 5));
			if (ret) {
				dma_free_coherent(&w->pdev->dev, 256, rb, rb_dma);
				return GU_DIAG(ret);
			}
			ok_ns = !memcmp(rb, w->booter_cpu, 16);
			GU_NOTE("imem ns readback: %08x %08x %08x %08x match=%u\n",
				s[0], s[1], s[2], s[3], ok_ns);
			memset(rb, 0xa5, 256);
			ret = sec_dma(w, h->patch_loc - h->os_data_off, 0, rb_dma,
				      256, TU_DMA_CMD_DMEM | (1u << 5));
			if (ret) {
				dma_free_coherent(&w->pdev->dev, 256, rb, rb_dma);
				return GU_DIAG(ret);
			}
			ok_dmem = !memcmp(rb, w->booter_cpu + h->patch_loc, 16);
			GU_NOTE("dmem sig readback: %08x %08x %08x %08x match=%u\n",
				s[0], s[1], s[2], s[3], ok_dmem);
			dma_free_coherent(&w->pdev->dev, 256, rb, rb_dma);
			if (!ok_ns || !ok_dmem)
				GU_NOTE("staging readback unverified (DMA aperture locked?)\n");
		}
	}

	wr(w, SEC_BASE + 0x104, 0); /* BOOTVEC = NS stub (readback is refused) */
	wr(w, SEC_BASE + 0x40, lower_32_bits(meta));
	wr(w, SEC_BASE + 0x44, upper_32_bits(meta));
	cpuctl = rd(w, SEC_BASE + 0x100);
	if (bad(cpuctl))
		return GU_DIAG(-EIO);
	GU_NOTE("sec fire (direct-via-dma): cpuctl=%#x meta=%#llx\n",
		cpuctl, (unsigned long long)meta);
	w->sec_started = true;
	if (cpuctl & (1u << 6))
		wr(w, SEC_BASE + 0x130, 1u << 1);
	else
		wr(w, SEC_BASE + 0x100, 1u << 1);

	/* The payload must finish its mutex release and normal main handoff. */
	deadline = ktime_add_us(ktime_get(), 5000000);
	for (;;) {
		cpuctl = rd(w, SEC_BASE + 0x100);
		if (bad(cpuctl))
			return GU_DIAG(-EIO);
		if (cpuctl & (1u << 4))
			break;
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			GU_ERROR("sec fire timeout: cpuctl=%#x dmactl=%#x plm=%#x mb=%#x:%#x ever_ran=%u\n",
				 cpuctl, rd(w, SEC_BASE + 0x10c), rd(w, TU_FEAT_PLM),
				 rd(w, SEC_BASE + 0x44), rd(w, SEC_BASE + 0x40), saw_running);
			return GU_DIAG(-ETIMEDOUT);
		}
		if (cpuctl != 0x20)
			saw_running = 1;
		udelay(2);
	}
	plm = rd(w, TU_FEAT_PLM);
	mb0 = rd(w, SEC_BASE + 0x40);
	mb1 = rd(w, SEC_BASE + 0x44);
	GU_NOTE("sec done: halted=%u plm=%#x mailbox=%#x:%#x\n",
		!!(cpuctl & (1u << 4)), plm, mb1, mb0);
	if (plm != ~0u || mb0) {
		GU_ERROR("sec fire failed: halted=%u mb0=%#x plm=%#x\n",
			 !!(cpuctl & (1u << 4)), mb0, plm);
		return -EIO;
	}
	return 0;
}

static int build_radix(struct tu_work *w)
{
	const struct turing_unlock_radix_plan *r = &w->radix;
	u64 *addrs;
	u32 i, level, first = 0;
	int ret = 0;

	w->page_count = r->table_pages + r->pages[3];
	w->pages = kvcalloc(w->page_count, sizeof(*w->pages), GFP_KERNEL);
	addrs = kvcalloc(w->page_count, sizeof(*addrs), GFP_KERNEL);
	if (!w->pages || !addrs) {
		kvfree(addrs);
		return GU_DIAG(-ENOMEM);
	}
	for (i = 0; i < w->page_count; i++) {
		w->pages[i].cpu = dma_alloc_coherent(&w->pdev->dev, GU_RADIX_PAGE,
						     &w->pages[i].dma, GFP_KERNEL);
		if (!w->pages[i].cpu) {
			ret = GU_DIAG(-ENOMEM);
			goto out;
		}
		w->pages_allocated++;
		addrs[i] = w->pages[i].dma;
		memset(w->pages[i].cpu, 0, GU_RADIX_PAGE);
		if (i >= r->table_pages) {
			u64 off = (u64)(i - r->table_pages) * GU_RADIX_PAGE;
			size_t n = min_t(u64, GU_RADIX_PAGE, w->image_span.size - off);

			memcpy(w->pages[i].cpu, w->gsp_fw->data + w->image_span.offset + off, n);
		}
	}
	for (level = 0; level < 3; level++) {
		u32 child = first + r->pages[level];

		for (i = 0; i < r->pages[level]; i++) {
			u32 at = i * (GU_RADIX_PAGE / 8);
			u32 n = min_t(u64, GU_RADIX_PAGE / 8, r->pages[level + 1] - at);

			ret = turing_unlock_radix_page(w->pages[first + i].cpu, addrs + child + at, n);
			if (ret)
				goto out;
		}
		first = child;
	}
out:
	kvfree(addrs);
	return ret;
}

static bool firmware_quiesced(const struct tu_work *w)
{
	return (!w->gsp_started || w->gsp_quiesced) &&
		(!w->sec_started || w->sec_quiesced);
}

static int tu_flr(struct tu_work *w)
{
	u16 pending;
	int ret;

	if (w->master) {
		pci_clear_master(w->pdev);
		w->master = false;
	}
	ret = pci_save_state(w->pdev);
	if (ret)
		return GU_DIAG(ret);
	ret = aperture_remove_conflicting_pci_devices(w->pdev, "turing_unlock");
	if (ret)
		return GU_DIAG(ret);
	ret = pcie_flr(w->pdev);
	if (ret)
		return GU_DIAG(ret);
	pci_restore_state(w->pdev);
	ret = pcie_capability_read_word(w->pdev, PCI_EXP_DEVSTA, &pending);
	if (ret)
		return GU_DIAG(ret);
	if (pending == 0xffff || (pending & PCI_EXP_DEVSTA_TRPND))
		return GU_DIAG(-EBUSY);
	w->gsp_quiesced = w->gsp_started;
	w->sec_quiesced = w->sec_started;
	return 0;
}

int turing_unlock_card(struct pci_dev *pdev)
{
	struct tu_work *w;
	u16 command;
	u32 plm, ss0, ss1, readout, wpr_lo, wpr_hi, boot_state;
	int ret, cleanup;

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->pdev = pci_dev_get(pdev);
	if (!pdev->dev.dma_mask) {
		ret = GU_DIAG(-EINVAL);
		goto out;
	}
	w->streaming_ptr = pdev->dev.dma_mask;
	w->streaming_before = *w->streaming_ptr;
	w->coherent_before = pdev->dev.coherent_dma_mask;

	ret = turing_unlock_check_devres(&pdev->dev, "entry");
	if (ret)
		goto out;
	ret = turing_unlock_request_scoped(&pdev->dev, &w->booter_fw, BOOTER_FILE);
	if (ret)
		goto out;
	ret = turing_unlock_request_scoped(&pdev->dev, &w->v67_fw, V67_FILE);
	if (ret)
		goto out;
	if (w->v67_fw->size != TU_V67_SIZE) {
		ret = GU_DIAG(-EBADMSG);
		goto out;
	}
	ret = turing_unlock_request_scoped(&pdev->dev, &w->gsp_fw, GSP_FILE);
	if (ret)
		goto out;
	ret = turing_unlock_request_scoped(&pdev->dev, &w->boot_fw, BOOT_FILE);
	if (ret)
		goto out;
	ret = turing_unlock_request_scoped(&pdev->dev, &w->desc_fw, BOOT_DESC_FILE);
	if (ret)
		goto out;
	ret = turing_unlock_request_scoped(&pdev->dev, &w->bl_fw, SEC2_BL_FILE);
	if (ret)
		goto out;
	if (w->bl_fw->size != TU_SEC2_BL_SIZE) {
		ret = GU_DIAG(-EBADMSG);
		goto out;
	}
	w->streaming_set = true;
	ret = dma_set_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto out;
	w->coherent_set = true;
	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto out;
	ret = turing_unlock_booter_parse(w->booter_fw, &w->hdr);
	if (ret)
		goto out;
	ret = turing_unlock_elf_section(w->gsp_fw->data, w->gsp_fw->size, ".fwimage",
				     &w->image_span);
	if (ret)
		goto out;
	if (!w->image_span.size || w->image_span.size > (128ull << 20)) {
		ret = GU_DIAG(-EINVAL);
		goto out;
	}
	ret = turing_unlock_boot_desc_v4(w->desc_fw->data, w->desc_fw->size,
					   w->boot_fw->size, &w->boot_desc);
	if (ret)
		goto out;
	ret = turing_unlock_radix_plan(w->image_span.size, &w->radix);
	if (ret)
		goto out;

	if (pci_resource_len(pdev, 0) < SEC_FALCON2 + 0x1000) {
		ret = GU_DIAG(-ENODEV);
		goto out;
	}
	ret = pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (ret || command == 0xffff) {
		ret = GU_DIAG(ret ? ret : -ENODEV);
		goto out;
	}
	w->original_cmd = command;
	w->cmd_valid = true;
	if (command & PCI_COMMAND_MASTER) {
		pci_clear_master(pdev);
		ret = pci_read_config_word(pdev, PCI_COMMAND, &command);
		if (ret || command == 0xffff || (command & PCI_COMMAND_MASTER)) {
			ret = GU_DIAG(ret ? ret : -EBUSY);
			goto out;
		}
		w->original_cmd = command;
	}
	if (!(command & PCI_COMMAND_MEMORY)) {
		ret = pci_enable_device_mem(pdev);
		if (ret)
			goto out;
		w->enabled = true;
	}
	w->bar_len = pci_resource_len(pdev, 0);
	w->bar = ioremap(pci_resource_start(pdev, 0), w->bar_len);
	if (!w->bar) {
		ret = GU_DIAG(-ENOMEM);
		goto out;
	}

	plm = rd(w, TU_FEAT_PLM);
	ss0 = rd(w, TU_FEAT_SS0);
	ss1 = rd(w, TU_FEAT_SS1);
	wpr_lo = rd(w, 0x1fa824);
	wpr_hi = rd(w, 0x1fa828);
	boot_state = rd(w, 0x1180f8);
	GU_NOTE("entry: plm=%#x ss0=%#x ss1=%#x wpr2=%#x:%#x boot=%#x\n",
		plm, ss0, ss1, wpr_hi, wpr_lo, boot_state);
	/* A previous FWSEC or completed boot requires a clean first-fire state. */
	if ((wpr_hi >> 4) || (boot_state & 0xfff00000u)) {
		GU_NOTE("reset required: wpr2=%#x:%#x boot=%#x, doing PCIe FLR first\n",
			wpr_hi, wpr_lo, boot_state);
		ret = tu_flr(w);
		if (ret)
			goto out;
		msleep(50);
	}

	/* Layout from live NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE (0x100ce0) and
	 * NV_PDISP_VGA_WORKSPACE_BASE (0x625f04), matching kgspPopulateWprMeta_TU102.
	 * After a PCIe FLR the GPU re-runs devinit asynchronously and
	 * LOCAL_MEMORY_RANGE reads 0 until that finishes (~100ms), so poll. */
	{
		u32 range_reg = 0, vga_reg, mag, scale;
		u64 fb_bytes;
		ktime_t wait_end = ktime_add_us(ktime_get(), 3000000);

		for (;;) {
			range_reg = rd(w, 0x100ce0);
			if (turing_unlock_fb_range_ready(range_reg))
				break;
			if (ktime_compare(ktime_get(), wait_end) >= 0) {
				ret = GU_DIAG(-ETIMEDOUT);
				goto out;
			}
			msleep(20);
		}
		ret = wait_gfw(w);
		if (ret)
			goto out;
		vga_reg = rd(w, 0x625f04);
		if (bad(vga_reg)) {
			ret = GU_DIAG(-EIO);
			goto out;
		}
		mag = (range_reg >> 4) & 0x3fu;
		scale = range_reg & 0xfu;
		fb_bytes = (u64)mag << (scale + 20);

		if (range_reg & (1u << 30))
			fb_bytes = fb_bytes / 16 * 15;
		GU_NOTE("fb layout regs: range=%#x (fb=%#llx) vga=%#x\n",
			range_reg, (unsigned long long)fb_bytes, vga_reg);
		w->layout_in.usable_fb = fb_bytes;
		w->layout_in.vga_base = fb_bytes - (1ull << 20);
		if (vga_reg & (1u << 3)) {
			w->layout_in.vga_base = (u64)(vga_reg >> 8) << 16;
			if (w->layout_in.vga_base < fb_bytes - (1ull << 20))
				w->layout_in.vga_base = fb_bytes - 0x20000ull;
		}
		w->layout_in.mmu_lock_valid = false;
		w->layout_in.frts_size = 1ull << 20;
		w->layout_in.boot_size = w->boot_desc.image_size;
		w->layout_in.image_size = w->image_span.size;
		w->layout_in.nonwpr_size = 1ull << 20;
		w->layout_in.heap_request = 88ull << 20;
		ret = turing_unlock_layout(&w->layout_in, &w->layout);
		if (ret)
			goto out;
		GU_NOTE("layout ok: nonwpr=%#llx wpr=%#llx frts=%#llx end=%#llx\n",
			(unsigned long long)w->layout.nonwpr_start,
			(unsigned long long)w->layout.wpr_start,
			(unsigned long long)w->layout.frts_offset,
			(unsigned long long)w->layout.wpr_end);
	}

	pci_set_master(pdev);
	w->master = true;
	ret = pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (ret || command == 0xffff || !(command & PCI_COMMAND_MASTER)) {
		ret = GU_DIAG(ret ? ret : -EIO);
		goto out;
	}
	ret = gsp_fwsec_frts(w);
	if (ret)
		goto out;
	/* kflcnResetIntoRiscv_TU102 == kflcnReset_TU102 (no BCR switch on Turing) */
	ret = flcn_reset(w, GSP_BASE);
	if (ret)
		goto out;
	w->gsp_quiesced = w->gsp_started;

	ret = alloc_dma(w, "booter", TU_BOOTER_IMAGE_SIZE, &w->booter_cpu, &w->booter_dma);
	if (ret)
		goto out;
	memcpy(w->booter_cpu, w->booter_fw->data + 72, TU_BOOTER_IMAGE_SIZE);
	/* single 16-byte production signature, fuse version always selects slot 0 */
	ret = turing_unlock_booter_patch(w->booter_cpu, TU_BOOTER_IMAGE_SIZE, &w->hdr,
				      w->booter_fw->data + 72 + TU_BOOTER_IMAGE_SIZE,
				      TU_BOOTER_SIG_TOTAL, 0);
	if (ret)
		goto out;
	dma_wmb();
	ret = alloc_dma(w, "v67", TU_V67_SIZE, &w->v67_cpu, &w->v67_dma);
	if (ret)
		goto out;
	memcpy(w->v67_cpu, w->v67_fw->data, TU_V67_SIZE);
	ret = alloc_dma(w, "meta", GU_WPR_META_SIZE, &w->meta_cpu, &w->meta_dma);
	if (ret)
		goto out;
	ret = alloc_dma(w, "boot", w->boot_desc.image_size, &w->boot_cpu, &w->boot_dma);
	if (ret)
		goto out;
	memcpy(w->boot_cpu, w->boot_fw->data, w->boot_desc.image_size);
	ret = build_radix(w);
	if (ret)
		goto out;

	{
		struct turing_unlock_wpr_args args = {
			.fb_size = w->layout_in.usable_fb,
			.vga_offset = w->layout_in.vga_base,
			.frts_size = w->layout_in.frts_size,
			.signature_dma = w->v67_dma,
			.signature_size = TU_V67_SIZE,
			.radix_dma = w->pages[0].dma,
			.image_size = w->image_span.size,
			.boot_dma = w->boot_dma,
			.boot = w->boot_desc,
		};

		ret = turing_unlock_wpr_meta_pack(w->meta_cpu, &w->layout, &args);
		if (ret)
			goto out;
		dma_wmb();
	}

	ret = sec_hs_fire(w, w->meta_dma);
	if (ret)
		goto out;
	w->sec_quiesced = w->sec_started;

	plm = rd(w, TU_FEAT_PLM);
	GU_NOTE("post-fire plm=%#x\n", plm);
	if (plm != ~0u) {
		ret = GU_DIAG(-EIO);
		goto out;
	}
	wr(w, TU_FEAT_SS0, TU_SS0_UNLOCK);
	wr(w, TU_FEAT_SS1, TU_SS1_UNLOCK);
	ss0 = rd(w, TU_FEAT_SS0);
	ss1 = rd(w, TU_FEAT_SS1);
	readout = rd(w, TU_FEAT_READOUT);
	GU_NOTE("unlock result: ss0=%#x ss1=%#x readout=%#x (want ss0=%#x ss1=%#x readout=0)\n",
		ss0, ss1, readout, TU_SS0_UNLOCK, TU_SS1_UNLOCK);
	if (ss0 != TU_SS0_UNLOCK || ss1 != TU_SS1_UNLOCK || readout)
		ret = GU_DIAG(-EIO);
	else
		GU_NOTE("TU102 FEAT UNLOCKED\n");
out:
	if (ret && w->bar && (w->gsp_started || w->sec_started)) {
		cleanup = tu_flr(w);
		if (cleanup)
			GU_ERROR("error-path FLR failed: %d\n", cleanup);
	}
	if (!firmware_quiesced(w)) {
		w->retained = true;
		pci_err(pdev, "turing_unlock: firmware did not stop; retaining DMA resources until power off\n");
		__module_get(THIS_MODULE);
		if (!ret)
			ret = -EBUSY;
		return ret;
	}
	if (w->bar) {
		iounmap(w->bar);
		w->bar = NULL;
	}
	if (w->master) {
		pci_clear_master(pdev);
		w->master = false;
	}
	if (w->streaming_set) {
		if (pdev->dev.dma_mask != w->streaming_ptr) {
			cleanup = -EIO;
		} else {
			cleanup = dma_set_mask(&pdev->dev, w->streaming_before);
			if (!cleanup && *w->streaming_ptr != w->streaming_before)
				cleanup = -EIO;
		}
		if (cleanup && !ret)
			ret = GU_DIAG(cleanup);
	}
	if (w->coherent_set) {
		cleanup = dma_set_coherent_mask(&pdev->dev, w->coherent_before);
		if (!cleanup && pdev->dev.coherent_dma_mask != w->coherent_before)
			cleanup = -EIO;
		if (cleanup && !ret)
			ret = GU_DIAG(cleanup);
	}
	if (w->pages) {
		u32 i;

		for (i = 0; i < w->pages_allocated; i++)
			dma_free_coherent(&pdev->dev, GU_RADIX_PAGE,
					  w->pages[i].cpu, w->pages[i].dma);
		kvfree(w->pages);
	}
	if (w->booter_cpu)
		dma_free_coherent(&pdev->dev, TU_BOOTER_IMAGE_SIZE, w->booter_cpu, w->booter_dma);
	if (w->v67_cpu)
		dma_free_coherent(&pdev->dev, TU_V67_SIZE, w->v67_cpu, w->v67_dma);
	if (w->meta_cpu)
		dma_free_coherent(&pdev->dev, GU_WPR_META_SIZE, w->meta_cpu, w->meta_dma);
	if (w->boot_cpu)
		dma_free_coherent(&pdev->dev, w->boot_desc.image_size, w->boot_cpu, w->boot_dma);
	if (w->fwsec_code_cpu)
		dma_free_coherent(&pdev->dev, w->fwsec_code_size, w->fwsec_code_cpu, w->fwsec_code_dma);
	if (w->fwsec_data_cpu)
		dma_free_coherent(&pdev->dev, w->fwsec_data_size, w->fwsec_data_cpu, w->fwsec_data_dma);
	if (w->booter_fw)
		release_firmware(w->booter_fw);
	if (w->v67_fw)
		release_firmware(w->v67_fw);
	if (w->gsp_fw)
		release_firmware(w->gsp_fw);
	if (w->boot_fw)
		release_firmware(w->boot_fw);
	if (w->desc_fw)
		release_firmware(w->desc_fw);
	if (w->bl_fw)
		release_firmware(w->bl_fw);
	if (w->enabled) {
		pci_disable_device(pdev);
		w->enabled = false;
	}
	if (w->cmd_valid) {
		u16 want;

		cleanup = pci_read_config_word(pdev, PCI_COMMAND, &command);
		if (!cleanup && command != 0xffff) {
			want = turing_unlock_command_restore(w->original_cmd, command);
			if (want != command)
				cleanup = pci_write_config_word(pdev, PCI_COMMAND, want);
			if (!cleanup)
				cleanup = pci_read_config_word(pdev, PCI_COMMAND, &command);
		}
		if (cleanup || command == 0xffff ||
		    (command & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) !=
		    (w->original_cmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))) {
			if (!ret)
				ret = GU_DIAG(cleanup ? cleanup : -EIO);
		}
	}
	cleanup = turing_unlock_check_devres(&pdev->dev, "handoff");
	if (!ret)
		ret = cleanup;
	pci_dev_put(w->pdev);
	kfree(w);
	return ret;
}

MODULE_FIRMWARE(BOOTER_FILE);
MODULE_FIRMWARE(V67_FILE);
MODULE_FIRMWARE(GSP_FILE);
MODULE_FIRMWARE(BOOT_FILE);
MODULE_FIRMWARE(BOOT_DESC_FILE);
MODULE_FIRMWARE(SEC2_BL_FILE);
