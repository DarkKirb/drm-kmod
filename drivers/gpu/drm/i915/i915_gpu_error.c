/*
 * Copyright (c) 2008 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *    Keith Packard <keithp@keithp.com>
 *    Mika Kuoppala <mika.kuoppala@intel.com>
 *
 */

#ifdef __linux__
#include <generated/utsrelease.h>
#else
#define UTS_RELEASE "FreeBSD 11 prerelease"
#endif
#include <linux/stop_machine.h>
#include <linux/zlib.h>
#include "i915_drv.h"

static const char *engine_str(int engine)
{
	switch (engine) {
	case RCS: return "render";
	case VCS: return "bsd";
	case BCS: return "blt";
	case VECS: return "vebox";
	case VCS2: return "bsd2";
	default: return "";
	}
}

static const char *tiling_flag(int tiling)
{
	switch (tiling) {
	default:
	case I915_TILING_NONE: return "";
	case I915_TILING_X: return " X";
	case I915_TILING_Y: return " Y";
	}
}

static const char *dirty_flag(int dirty)
{
	return dirty ? " dirty" : "";
}

static const char *purgeable_flag(int purgeable)
{
	return purgeable ? " purgeable" : "";
}

static bool __i915_error_ok(struct drm_i915_error_state_buf *e)
{

	if (!e->err && WARN(e->bytes > (e->size - 1), "overflow")) {
		e->err = -ENOSPC;
		return false;
	}

	if (e->bytes == e->size - 1 || e->err)
		return false;

	return true;
}

static bool __i915_error_seek(struct drm_i915_error_state_buf *e,
			      unsigned len)
{
	if (e->pos + len <= e->start) {
		e->pos += len;
		return false;
	}

	/* First vsnprintf needs to fit in its entirety for memmove */
	if (len >= e->size) {
		e->err = -EIO;
		return false;
	}

	return true;
}

static void __i915_error_advance(struct drm_i915_error_state_buf *e,
				 unsigned len)
{
	/* If this is first printf in this window, adjust it so that
	 * start position matches start of the buffer
	 */

	if (e->pos < e->start) {
		const size_t off = e->start - e->pos;

		/* Should not happen but be paranoid */
		if (off > len || e->bytes) {
			e->err = -EIO;
			return;
		}

		memmove(e->buf, e->buf + off, len - off);
		e->bytes = len - off;
		e->pos = e->start;
		return;
	}

	e->bytes += len;
	e->pos += len;
}

__printf(2, 0)
static void i915_error_vprintf(struct drm_i915_error_state_buf *e,
			       const char *f, va_list args)
{
	unsigned len;

	if (!__i915_error_ok(e))
		return;

	/* Seek the first printf which is hits start position */
	if (e->pos < e->start) {
		va_list tmp;

		va_copy(tmp, args);
		len = vsnprintf(NULL, 0, f, tmp);
		va_end(tmp);

		if (!__i915_error_seek(e, len))
			return;
	}

	len = vsnprintf(e->buf + e->bytes, e->size - e->bytes, f, args);
	if (len >= e->size - e->bytes)
		len = e->size - e->bytes - 1;

	__i915_error_advance(e, len);
}

static void i915_error_puts(struct drm_i915_error_state_buf *e,
			    const char *str)
{
	unsigned len;

	if (!__i915_error_ok(e))
		return;

	len = strlen(str);

	/* Seek the first printf which is hits start position */
	if (e->pos < e->start) {
		if (!__i915_error_seek(e, len))
			return;
	}

	if (len >= e->size - e->bytes)
		len = e->size - e->bytes - 1;
	memcpy(e->buf + e->bytes, str, len);

	__i915_error_advance(e, len);
}

#define err_printf(e, ...) i915_error_printf(e, __VA_ARGS__)
#define err_puts(e, s) i915_error_puts(e, s)

#ifdef CONFIG_DRM_I915_COMPRESS_ERROR

struct compress {
	struct z_stream_s zstream;
	void *tmp;
};

static bool compress_init(struct compress *c)
{
	struct z_stream_s *zstream = memset(&c->zstream, 0, sizeof(c->zstream));

	zstream->workspace =
		kmalloc(zlib_deflate_workspacesize(MAX_WBITS, MAX_MEM_LEVEL),
			GFP_ATOMIC | __GFP_NOWARN);
	if (!zstream->workspace)
		return false;

	if (zlib_deflateInit(zstream, Z_DEFAULT_COMPRESSION) != Z_OK) {
		kfree(zstream->workspace);
		return false;
	}

	c->tmp = NULL;
	if (i915_has_memcpy_from_wc())
		c->tmp = (void *)__get_free_page(GFP_ATOMIC | __GFP_NOWARN);

	return true;
}

static int compress_page(struct compress *c,
			 void *src,
			 struct drm_i915_error_object *dst)
{
	struct z_stream_s *zstream = &c->zstream;

	zstream->next_in = src;
	if (c->tmp && i915_memcpy_from_wc(c->tmp, src, PAGE_SIZE))
		zstream->next_in = c->tmp;
	zstream->avail_in = PAGE_SIZE;

	do {
		if (zstream->avail_out == 0) {
			unsigned long page;

			page = __get_free_page(GFP_ATOMIC | __GFP_NOWARN);
			if (!page)
				return -ENOMEM;

			dst->pages[dst->page_count++] = (void *)page;

			zstream->next_out = (void *)page;
			zstream->avail_out = PAGE_SIZE;
		}

		if (zlib_deflate(zstream, Z_SYNC_FLUSH) != Z_OK)
			return -EIO;
	} while (zstream->avail_in);

	/* Fallback to uncompressed if we increase size? */
	if (0 && zstream->total_out > zstream->total_in)
		return -E2BIG;

	return 0;
}

static void compress_fini(struct compress *c,
			  struct drm_i915_error_object *dst)
{
	struct z_stream_s *zstream = &c->zstream;

	if (dst) {
		zlib_deflate(zstream, Z_FINISH);
		dst->unused = zstream->avail_out;
	}

	zlib_deflateEnd(zstream);
	kfree(zstream->workspace);

	if (c->tmp)
		free_page((unsigned long)c->tmp);
}

static void err_compression_marker(struct drm_i915_error_state_buf *m)
{
	err_puts(m, ":");
}

#else

struct compress {
};

static bool compress_init(struct compress *c)
{
	return true;
}

static int compress_page(struct compress *c,
			 void *src,
			 struct drm_i915_error_object *dst)
{
	unsigned long page;
	void *ptr;

	page = __get_free_page(GFP_ATOMIC | __GFP_NOWARN);
	if (!page)
		return -ENOMEM;

	ptr = (void *)page;
	if (!i915_memcpy_from_wc(ptr, src, PAGE_SIZE))
		memcpy(ptr, src, PAGE_SIZE);
	dst->pages[dst->page_count++] = ptr;

	return 0;
}

static void compress_fini(struct compress *c,
			  struct drm_i915_error_object *dst)
{
}

static void err_compression_marker(struct drm_i915_error_state_buf *m)
{
	err_puts(m, "~");
}

#endif

static void print_error_buffers(struct drm_i915_error_state_buf *m,
				const char *name,
				struct drm_i915_error_buffer *err,
				int count)
{
	int i;

	err_printf(m, "%s [%d]:\n", name, count);

	while (count--) {
		err_printf(m, "    %08x_%08x %8u %02x %02x [ ",
			   upper_32_bits(err->gtt_offset),
			   lower_32_bits(err->gtt_offset),
			   err->size,
			   err->read_domains,
			   err->write_domain);
		for (i = 0; i < I915_NUM_ENGINES; i++)
			err_printf(m, "%02x ", err->rseqno[i]);

		err_printf(m, "] %02x", err->wseqno);
		err_puts(m, tiling_flag(err->tiling));
		err_puts(m, dirty_flag(err->dirty));
		err_puts(m, purgeable_flag(err->purgeable));
		err_puts(m, err->userptr ? " userptr" : "");
		err_puts(m, err->engine != -1 ? " " : "");
		err_puts(m, engine_str(err->engine));
		err_puts(m, i915_cache_level_str(m->i915, err->cache_level));

		if (err->name)
			err_printf(m, " (name: %d)", err->name);
		if (err->fence_reg != I915_FENCE_REG_NONE)
			err_printf(m, " (fence: %d)", err->fence_reg);

		err_puts(m, "\n");
		err++;
	}
}

static void error_print_instdone(struct drm_i915_error_state_buf *m,
				 struct drm_i915_error_engine *ee)
{
	int slice;
	int subslice;

	err_printf(m, "  INSTDONE: 0x%08x\n",
		   ee->instdone.instdone);

	if (ee->engine_id != RCS || INTEL_GEN(m->i915) <= 3)
		return;

	err_printf(m, "  SC_INSTDONE: 0x%08x\n",
		   ee->instdone.slice_common);

	if (INTEL_GEN(m->i915) <= 6)
		return;

	for_each_instdone_slice_subslice(m->i915, slice, subslice)
		err_printf(m, "  SAMPLER_INSTDONE[%d][%d]: 0x%08x\n",
			   slice, subslice,
			   ee->instdone.sampler[slice][subslice]);

	for_each_instdone_slice_subslice(m->i915, slice, subslice)
		err_printf(m, "  ROW_INSTDONE[%d][%d]: 0x%08x\n",
			   slice, subslice,
			   ee->instdone.row[slice][subslice]);
}

static void error_print_request(struct drm_i915_error_state_buf *m,
				const char *prefix,
				struct drm_i915_error_request *erq)
{
	if (!erq->seqno)
		return;

	err_printf(m, "%s pid %d, ban score %d, seqno %8x:%08x, emitted %dms ago, head %08x, tail %08x\n",
		   prefix, erq->pid, erq->ban_score,
		   erq->context, erq->seqno,
		   jiffies_to_msecs(jiffies - erq->jiffies),
		   erq->head, erq->tail);
}

static void error_print_context(struct drm_i915_error_state_buf *m,
				const char *header,
				struct drm_i915_error_context *ctx)
{
	err_printf(m, "%s%s[%d] user_handle %d hw_id %d, ban score %d guilty %d active %d\n",
		   header, ctx->comm, ctx->pid, ctx->handle, ctx->hw_id,
		   ctx->ban_score, ctx->guilty, ctx->active);
}

static void error_print_engine(struct drm_i915_error_state_buf *m,
			       struct drm_i915_error_engine *ee)
{
	err_printf(m, "%s command stream:\n", engine_str(ee->engine_id));
	err_printf(m, "  START: 0x%08x\n", ee->start);
	err_printf(m, "  HEAD:  0x%08x [0x%08x]\n", ee->head, ee->rq_head);
	err_printf(m, "  TAIL:  0x%08x [0x%08x, 0x%08x]\n",
		   ee->tail, ee->rq_post, ee->rq_tail);
	err_printf(m, "  CTL:   0x%08x\n", ee->ctl);
	err_printf(m, "  MODE:  0x%08x\n", ee->mode);
	err_printf(m, "  HWS:   0x%08x\n", ee->hws);
	err_printf(m, "  ACTHD: 0x%08x %08x\n",
		   (u32)(ee->acthd>>32), (u32)ee->acthd);
	err_printf(m, "  IPEIR: 0x%08x\n", ee->ipeir);
	err_printf(m, "  IPEHR: 0x%08x\n", ee->ipehr);

	error_print_instdone(m, ee);

	if (ee->batchbuffer) {
		u64 start = ee->batchbuffer->gtt_offset;
		u64 end = start + ee->batchbuffer->gtt_size;

		err_printf(m, "  batch: [0x%08x_%08x, 0x%08x_%08x]\n",
			   upper_32_bits(start), lower_32_bits(start),
			   upper_32_bits(end), lower_32_bits(end));
	}
	if (INTEL_GEN(m->i915) >= 4) {
		err_printf(m, "  BBADDR: 0x%08x_%08x\n",
			   (u32)(ee->bbaddr>>32), (u32)ee->bbaddr);
		err_printf(m, "  BB_STATE: 0x%08x\n", ee->bbstate);
		err_printf(m, "  INSTPS: 0x%08x\n", ee->instps);
	}
	err_printf(m, "  INSTPM: 0x%08x\n", ee->instpm);
	err_printf(m, "  FADDR: 0x%08x %08x\n", upper_32_bits(ee->faddr),
		   lower_32_bits(ee->faddr));
	if (INTEL_GEN(m->i915) >= 6) {
		err_printf(m, "  RC PSMI: 0x%08x\n", ee->rc_psmi);
		err_printf(m, "  FAULT_REG: 0x%08x\n", ee->fault_reg);
		err_printf(m, "  SYNC_0: 0x%08x\n",
			   ee->semaphore_mboxes[0]);
		err_printf(m, "  SYNC_1: 0x%08x\n",
			   ee->semaphore_mboxes[1]);
		if (HAS_VEBOX(m->i915))
			err_printf(m, "  SYNC_2: 0x%08x\n",
				   ee->semaphore_mboxes[2]);
	}
	if (USES_PPGTT(m->i915)) {
		err_printf(m, "  GFX_MODE: 0x%08x\n", ee->vm_info.gfx_mode);

		if (INTEL_GEN(m->i915) >= 8) {
			int i;
			for (i = 0; i < 4; i++)
				err_printf(m, "  PDP%d: 0x%016llx\n",
					   i, ee->vm_info.pdp[i]);
		} else {
			err_printf(m, "  PP_DIR_BASE: 0x%08x\n",
				   ee->vm_info.pp_dir_base);
		}
	}
	err_printf(m, "  seqno: 0x%08x\n", ee->seqno);
	err_printf(m, "  last_seqno: 0x%08x\n", ee->last_seqno);
	err_printf(m, "  waiting: %s\n", yesno(ee->waiting));
	err_printf(m, "  ring->head: 0x%08x\n", ee->cpu_ring_head);
	err_printf(m, "  ring->tail: 0x%08x\n", ee->cpu_ring_tail);
	err_printf(m, "  hangcheck stall: %s\n", yesno(ee->hangcheck_stalled));
	err_printf(m, "  hangcheck action: %s\n",
		   hangcheck_action_to_str(ee->hangcheck_action));
	err_printf(m, "  hangcheck action timestamp: %lu, %u ms ago\n",
		   ee->hangcheck_timestamp,
		   jiffies_to_msecs(jiffies - ee->hangcheck_timestamp));

	error_print_request(m, "  ELSP[0]: ", &ee->execlist[0]);
	error_print_request(m, "  ELSP[1]: ", &ee->execlist[1]);
	error_print_context(m, "  Active context: ", &ee->context);
}

void i915_error_printf(struct drm_i915_error_state_buf *e, const char *f, ...)
{
	va_list args;

	va_start(args, f);
	i915_error_vprintf(e, f, args);
	va_end(args);
}

static int
ascii85_encode_len(int len)
{
	return DIV_ROUND_UP(len, 4);
}

static bool
ascii85_encode(u32 in, char *out)
{
	int i;

	if (in == 0)
		return false;

	out[5] = '\0';
	for (i = 5; i--; ) {
		out[i] = '!' + in % 85;
		in /= 85;
	}

	return true;
}

static void print_error_obj(struct drm_i915_error_state_buf *m,
			    struct intel_engine_cs *engine,
			    const char *name,
			    struct drm_i915_error_object *obj)
{
	char out[6];
	int page;

	if (!obj)
		return;

	if (name) {
		err_printf(m, "%s --- %s = 0x%08x %08x\n",
			   engine ? engine->name : "global", name,
			   upper_32_bits(obj->gtt_offset),
			   lower_32_bits(obj->gtt_offset));
	}

	err_compression_marker(m);
	for (page = 0; page < obj->page_count; page++) {
		int i, len;

		len = PAGE_SIZE;
		if (page == obj->page_count - 1)
			len -= obj->unused;
		len = ascii85_encode_len(len);

		for (i = 0; i < len; i++) {
			if (ascii85_encode(obj->pages[page][i], out))
				err_puts(m, out);
			else
				err_puts(m, "z");
		}
	}
	err_puts(m, "\n");
}

static void err_print_capabilities(struct drm_i915_error_state_buf *m,
				   const struct intel_device_info *info)
{
#define PRINT_FLAG(x)  err_printf(m, #x ": %s\n", yesno(info->x))
	DEV_INFO_FOR_EACH_FLAG(PRINT_FLAG);
#undef PRINT_FLAG
}

static __always_inline void err_print_param(struct drm_i915_error_state_buf *m,
					    const char *name,
					    const char *type,
					    const void *x)
{
	if (!__builtin_strcmp(type, "bool"))
		err_printf(m, "i915.%s=%s\n", name, yesno(*(const bool *)x));
	else if (!__builtin_strcmp(type, "int"))
		err_printf(m, "i915.%s=%d\n", name, *(const int *)x);
	else if (!__builtin_strcmp(type, "unsigned int"))
		err_printf(m, "i915.%s=%u\n", name, *(const unsigned int *)x);
	else
		BUILD_BUG();
}

static void err_print_params(struct drm_i915_error_state_buf *m,
			     const struct i915_params *p)
{
#define PRINT(T, x) err_print_param(m, #x, #T, &p->x);
	I915_PARAMS_FOR_EACH(PRINT);
#undef PRINT
}

int i915_error_state_to_str(struct drm_i915_error_state_buf *m,
			    const struct i915_error_state_file_priv *error_priv)
{
	struct drm_i915_private *dev_priv = error_priv->i915;
	struct pci_dev *pdev = dev_priv->drm.pdev;
	struct drm_i915_error_state *error = error_priv->error;
	struct drm_i915_error_object *obj;
	int i, j;

	if (!error) {
		err_printf(m, "no error state collected\n");
		goto out;
	}

	err_printf(m, "%s\n", error->error_msg);
	err_printf(m, "Kernel: " UTS_RELEASE "\n");
	err_printf(m, "Time: %ld s %ld us\n",
		   error->time.tv_sec, error->time.tv_usec);
	err_printf(m, "Boottime: %ld s %ld us\n",
		   error->boottime.tv_sec, error->boottime.tv_usec);
	err_printf(m, "Uptime: %ld s %ld us\n",
		   error->uptime.tv_sec, error->uptime.tv_usec);

	for (i = 0; i < ARRAY_SIZE(error->engine); i++) {
		if (error->engine[i].hangcheck_stalled &&
		    error->engine[i].context.pid) {
			err_printf(m, "Active process (on ring %s): %s [%d], score %d\n",
				   engine_str(i),
				   error->engine[i].context.comm,
				   error->engine[i].context.pid,
				   error->engine[i].context.ban_score);
		}
	}
	err_printf(m, "Reset count: %u\n", error->reset_count);
	err_printf(m, "Suspend count: %u\n", error->suspend_count);
	err_printf(m, "Platform: %s\n", intel_platform_name(error->device_info.platform));
	err_printf(m, "PCI ID: 0x%04x\n", pdev->device);
	err_printf(m, "PCI Revision: 0x%02x\n", pdev->revision);
	err_printf(m, "PCI Subsystem: %04x:%04x\n",
		   pdev->subsystem_vendor,
		   pdev->subsystem_device);

	err_printf(m, "IOMMU enabled?: %d\n", error->iommu);

	if (HAS_CSR(dev_priv)) {
		struct intel_csr *csr = &dev_priv->csr;

		err_printf(m, "DMC loaded: %s\n",
			   yesno(csr->dmc_payload != NULL));
		err_printf(m, "DMC fw version: %d.%d\n",
			   CSR_VERSION_MAJOR(csr->version),
			   CSR_VERSION_MINOR(csr->version));
	}

	err_printf(m, "EIR: 0x%08x\n", error->eir);
	err_printf(m, "IER: 0x%08x\n", error->ier);
	if (INTEL_GEN(dev_priv) >= 8) {
		for (i = 0; i < 4; i++)
			err_printf(m, "GTIER gt %d: 0x%08x\n", i,
				   error->gtier[i]);
	} else if (HAS_PCH_SPLIT(dev_priv) || IS_VALLEYVIEW(dev_priv))
		err_printf(m, "GTIER: 0x%08x\n", error->gtier[0]);
	err_printf(m, "PGTBL_ER: 0x%08x\n", error->pgtbl_er);
	err_printf(m, "FORCEWAKE: 0x%08x\n", error->forcewake);
	err_printf(m, "DERRMR: 0x%08x\n", error->derrmr);
	err_printf(m, "CCID: 0x%08x\n", error->ccid);
	err_printf(m, "Missed interrupts: 0x%08lx\n", dev_priv->gpu_error.missed_irq_rings);

	for (i = 0; i < dev_priv->num_fence_regs; i++)
		err_printf(m, "  fence[%d] = %08llx\n", i, error->fence[i]);

	if (INTEL_GEN(dev_priv) >= 6) {
		err_printf(m, "ERROR: 0x%08x\n", error->error);

		if (INTEL_GEN(dev_priv) >= 8)
			err_printf(m, "FAULT_TLB_DATA: 0x%08x 0x%08x\n",
				   error->fault_data1, error->fault_data0);

		err_printf(m, "DONE_REG: 0x%08x\n", error->done_reg);
	}

	if (IS_GEN7(dev_priv))
		err_printf(m, "ERR_INT: 0x%08x\n", error->err_int);

	for (i = 0; i < ARRAY_SIZE(error->engine); i++) {
		if (error->engine[i].engine_id != -1)
			error_print_engine(m, &error->engine[i]);
	}

	for (i = 0; i < ARRAY_SIZE(error->active_vm); i++) {
		char buf[128];
		int len, first = 1;

		if (!error->active_vm[i])
			break;

		len = scnprintf(buf, sizeof(buf), "Active (");
		for (j = 0; j < ARRAY_SIZE(error->engine); j++) {
			if (error->engine[j].vm != error->active_vm[i])
				continue;

			len += scnprintf(buf + len, sizeof(buf), "%s%s",
					 first ? "" : ", ",
					 dev_priv->engine[j]->name);
			first = 0;
		}
		scnprintf(buf + len, sizeof(buf), ")");
		print_error_buffers(m, buf,
				    error->active_bo[i],
				    error->active_bo_count[i]);
	}

	print_error_buffers(m, "Pinned (global)",
			    error->pinned_bo,
			    error->pinned_bo_count);

	for (i = 0; i < ARRAY_SIZE(error->engine); i++) {
		struct drm_i915_error_engine *ee = &error->engine[i];

		obj = ee->batchbuffer;
		if (obj) {
			err_puts(m, dev_priv->engine[i]->name);
			if (ee->context.pid)
				err_printf(m, " (submitted by %s [%d], ctx %d [%d], score %d)",
					   ee->context.comm,
					   ee->context.pid,
					   ee->context.handle,
					   ee->context.hw_id,
					   ee->context.ban_score);
			err_printf(m, " --- gtt_offset = 0x%08x %08x\n",
				   upper_32_bits(obj->gtt_offset),
				   lower_32_bits(obj->gtt_offset));
			print_error_obj(m, dev_priv->engine[i], NULL, obj);
		}

		if (ee->num_requests) {
			err_printf(m, "%s --- %d requests\n",
				   dev_priv->engine[i]->name,
				   ee->num_requests);
			for (j = 0; j < ee->num_requests; j++)
				error_print_request(m, " ", &ee->requests[j]);
		}

		if (IS_ERR(ee->waiters)) {
			err_printf(m, "%s --- ? waiters [unable to acquire spinlock]\n",
				   dev_priv->engine[i]->name);
		} else if (ee->num_waiters) {
			err_printf(m, "%s --- %d waiters\n",
				   dev_priv->engine[i]->name,
				   ee->num_waiters);
			for (j = 0; j < ee->num_waiters; j++) {
				err_printf(m, " seqno 0x%08x for %s [%d]\n",
					   ee->waiters[j].seqno,
					   ee->waiters[j].comm,
					   ee->waiters[j].pid);
			}
		}

		print_error_obj(m, dev_priv->engine[i],
				"ringbuffer", ee->ringbuffer);

		print_error_obj(m, dev_priv->engine[i],
				"HW Status", ee->hws_page);

		print_error_obj(m, dev_priv->engine[i],
				"HW context", ee->ctx);

		print_error_obj(m, dev_priv->engine[i],
				"WA context", ee->wa_ctx);

		print_error_obj(m, dev_priv->engine[i],
				"WA batchbuffer", ee->wa_batchbuffer);
	}

	print_error_obj(m, NULL, "Semaphores", error->semaphore);

	print_error_obj(m, NULL, "GuC log buffer", error->guc_log);

	if (error->overlay)
		intel_overlay_print_error_state(m, error->overlay);

	if (error->display)
		intel_display_print_error_state(m, dev_priv, error->display);

	err_print_capabilities(m, &error->device_info);
	err_print_params(m, &error->params);

out:
	if (m->bytes == 0 && m->err)
		return m->err;

	return 0;
}

int i915_error_state_buf_init(struct drm_i915_error_state_buf *ebuf,
			      struct drm_i915_private *i915,
			      size_t count, loff_t pos)
{
	memset(ebuf, 0, sizeof(*ebuf));
	ebuf->i915 = i915;

	/* We need to have enough room to store any i915_error_state printf
	 * so that we can move it to start position.
	 */
	ebuf->size = count + 1 > PAGE_SIZE ? count + 1 : PAGE_SIZE;
	ebuf->buf = kmalloc(ebuf->size,
				GFP_TEMPORARY | __GFP_NORETRY | __GFP_NOWARN);

	if (ebuf->buf == NULL) {
		ebuf->size = PAGE_SIZE;
		ebuf->buf = kmalloc(ebuf->size, GFP_TEMPORARY);
	}

	if (ebuf->buf == NULL) {
		ebuf->size = 128;
		ebuf->buf = kmalloc(ebuf->size, GFP_TEMPORARY);
	}

	if (ebuf->buf == NULL)
		return -ENOMEM;

	ebuf->start = pos;

	return 0;
}

static void i915_error_object_free(struct drm_i915_error_object *obj)
{
	int page;

	if (obj == NULL)
		return;

	for (page = 0; page < obj->page_count; page++)
		free_page((unsigned long)obj->pages[page]);

	kfree(obj);
}

static void i915_error_state_free(struct kref *error_ref)
{
	struct drm_i915_error_state *error = container_of(error_ref,
							  typeof(*error), ref);
	int i;

	for (i = 0; i < ARRAY_SIZE(error->engine); i++) {
		struct drm_i915_error_engine *ee = &error->engine[i];

		i915_error_object_free(ee->batchbuffer);
		i915_error_object_free(ee->wa_batchbuffer);
		i915_error_object_free(ee->ringbuffer);
		i915_error_object_free(ee->hws_page);
		i915_error_object_free(ee->ctx);
		i915_error_object_free(ee->wa_ctx);

		kfree(ee->requests);
		if (!IS_ERR_OR_NULL(ee->waiters))
			kfree(ee->waiters);
	}

	i915_error_object_free(error->semaphore);
	i915_error_object_free(error->guc_log);

	for (i = 0; i < ARRAY_SIZE(error->active_bo); i++)
		kfree(error->active_bo[i]);
	kfree(error->pinned_bo);

	kfree(error->overlay);
	kfree(error->display);
	kfree(error);
}

static struct drm_i915_error_object *
i915_error_object_create(struct drm_i915_private *i915,
			 struct i915_vma *vma)
{
	struct i915_ggtt *ggtt = &i915->ggtt;
	const u64 slot = ggtt->error_capture.start;
	struct drm_i915_error_object *dst;
	struct compress compress;
	unsigned long num_pages;
	struct sgt_iter iter;
	dma_addr_t dma;

	if (!vma)
		return NULL;

	num_pages = min_t(u64, vma->size, vma->obj->base.size) >> PAGE_SHIFT;
	num_pages = DIV_ROUND_UP(10 * num_pages, 8); /* worstcase zlib growth */
	dst = kmalloc(sizeof(*dst) + num_pages * sizeof(u32 *),
		      GFP_ATOMIC | __GFP_NOWARN);
	if (!dst)
		return NULL;

	dst->gtt_offset = vma->node.start;
	dst->gtt_size = vma->node.size;
	dst->page_count = 0;
	dst->unused = 0;

	if (!compress_init(&compress)) {
		kfree(dst);
		return NULL;
	}

	for_each_sgt_dma(dma, iter, vma->pages) {
		void __iomem *s;
		int ret;

		ggtt->base.insert_page(&ggtt->base, dma, slot,
				       I915_CACHE_NONE, 0);

		s = io_mapping_map_atomic_wc(&ggtt->mappable, slot);
		ret = compress_page(&compress, (void  __force *)s, dst);
		io_mapping_unmap_atomic(s);

		if (ret)
			goto unwind;
	}
	goto out;

unwind:
	while (dst->page_count--)
		free_page((unsigned long)dst->pages[dst->page_count]);
	kfree(dst);
	dst = NULL;

out:
	compress_fini(&compress, dst);
	ggtt->base.clear_range(&ggtt->base, slot, PAGE_SIZE);
	return dst;
}

/* The error capture is special as tries to run underneath the normal
 * locking rules - so we use the raw version of the i915_gem_active lookup.
 */
static inline uint32_t
__active_get_seqno(struct i915_gem_active *active)
{
	struct drm_i915_gem_request *request;

	request = __i915_gem_active_peek(active);
	return request ? request->global_seqno : 0;
}

static inline int
__active_get_engine_id(struct i915_gem_active *active)
{
	struct drm_i915_gem_request *request;

	request = __i915_gem_active_peek(active);
	return request ? request->engine->id : -1;
}

static void capture_bo(struct drm_i915_error_buffer *err,
		       struct i915_vma *vma)
{
	struct drm_i915_gem_object *obj = vma->obj;
	int i;

	err->size = obj->base.size;
	err->name = obj->base.name;

	for (i = 0; i < I915_NUM_ENGINES; i++)
		err->rseqno[i] = __active_get_seqno(&vma->last_read[i]);
	err->wseqno = __active_get_seqno(&obj->frontbuffer_write);
	err->engine = __active_get_engine_id(&obj->frontbuffer_write);

	err->gtt_offset = vma->node.start;
	err->read_domains = obj->base.read_domains;
	err->write_domain = obj->base.write_domain;
	err->fence_reg = vma->fence ? vma->fence->id : -1;
	err->tiling = i915_gem_object_get_tiling(obj);
	err->dirty = obj->mm.dirty;
	err->purgeable = obj->mm.madv != I915_MADV_WILLNEED;
	err->userptr = obj->userptr.mm != NULL;
	err->cache_level = obj->cache_level;
}

static u32 capture_error_bo(struct drm_i915_error_buffer *err,
			    int count, struct list_head *head,
			    bool pinned_only)
{
	struct i915_vma *vma;
	int i = 0;

	list_for_each_entry(vma, head, vm_link) {
		if (pinned_only && !i915_vma_is_pinned(vma))
			continue;

		capture_bo(err++, vma);
		if (++i == count)
			break;
	}

	return i;
}

/* Generate a semi-unique error code. The code is not meant to have meaning, The
 * code's only purpose is to try to prevent false duplicated bug reports by
 * grossly estimating a GPU error state.
 *
 * TODO Ideally, hashing the batchbuffer would be a very nice way to determine
 * the hang if we could strip the GTT offset information from it.
 *
 * It's only a small step better than a random number in its current form.
 */
static uint32_t i915_error_generate_code(struct drm_i915_private *dev_priv,
					 struct drm_i915_error_state *error,
					 int *engine_id)
{
	uint32_t error_code = 0;
	int i;

	/* IPEHR would be an ideal way to detect errors, as it's the gross
	 * measure of "the command that hung." However, has some very common
	 * synchronization commands which almost always appear in the case
	 * strictly a client bug. Use instdone to differentiate those some.
	 */
	for (i = 0; i < I915_NUM_ENGINES; i++) {
		if (error->engine[i].hangcheck_stalled) {
			if (engine_id)
				*engine_id = i;

			return error->engine[i].ipehr ^
			       error->engine[i].instdone.instdone;
		}
	}

	return error_code;
}

static void i915_gem_record_fences(struct drm_i915_private *dev_priv,
				   struct drm_i915_error_state *error)
{
	int i;

	if (IS_GEN3(dev_priv) || IS_GEN2(dev_priv)) {
		for (i = 0; i < dev_priv->num_fence_regs; i++)
			error->fence[i] = I915_READ(FENCE_REG(i));
	} else if (IS_GEN5(dev_priv) || IS_GEN4(dev_priv)) {
		for (i = 0; i < dev_priv->num_fence_regs; i++)
			error->fence[i] = I915_READ64(FENCE_REG_965_LO(i));
	} else if (INTEL_GEN(dev_priv) >= 6) {
		for (i = 0; i < dev_priv->num_fence_regs; i++)
			error->fence[i] = I915_READ64(FENCE_REG_GEN6_LO(i));
	}
}

static inline u32
gen8_engine_sync_index(struct intel_engine_cs *engine,
		       struct intel_engine_cs *other)
{
	int idx;

	/*
	 * rcs -> 0 = vcs, 1 = bcs, 2 = vecs, 3 = vcs2;
	 * vcs -> 0 = bcs, 1 = vecs, 2 = vcs2, 3 = rcs;
	 * bcs -> 0 = vecs, 1 = vcs2. 2 = rcs, 3 = vcs;
	 * vecs -> 0 = vcs2, 1 = rcs, 2 = vcs, 3 = bcs;
	 * vcs2 -> 0 = rcs, 1 = vcs, 2 = bcs, 3 = vecs;
	 */

	idx = (other - engine) - 1;
	if (idx < 0)
		idx += I915_NUM_ENGINES;

	return idx;
}

static void gen8_record_semaphore_state(struct drm_i915_error_state *error,
					struct intel_engine_cs *engine,
					struct drm_i915_error_engine *ee)
{
	struct drm_i915_private *dev_priv = engine->i915;
	struct intel_engine_cs *to;
	enum intel_engine_id id;

	if (!error->semaphore)
		return;

	for_each_engine(to, dev_priv, id) {
		int idx;
		u16 signal_offset;
		u32 *tmp;

		if (engine == to)
			continue;

		signal_offset =
			(GEN8_SIGNAL_OFFSET(engine, id) & (PAGE_SIZE - 1)) / 4;
		tmp = error->semaphore->pages[0];
		idx = gen8_engine_sync_index(engine, to);

		ee->semaphore_mboxes[idx] = tmp[signal_offset];
	}
}

static void gen6_record_semaphore_state(struct intel_engine_cs *engine,
					struct drm_i915_error_engine *ee)
{
	struct drm_i915_private *dev_priv = engine->i915;

	ee->semaphore_mboxes[0] = I915_READ(RING_SYNC_0(engine->mmio_base));
	ee->semaphore_mboxes[1] = I915_READ(RING_SYNC_1(engine->mmio_base));
	if (HAS_VEBOX(dev_priv))
		ee->semaphore_mboxes[2] =
			I915_READ(RING_SYNC_2(engine->mmio_base));
}

static void error_record_engine_waiters(struct intel_engine_cs *engine,
					struct drm_i915_error_engine *ee)
{
	struct intel_breadcrumbs *b = &engine->breadcrumbs;
	struct drm_i915_error_waiter *waiter;
	struct rb_node *rb;
	int count;

	ee->num_waiters = 0;
	ee->waiters = NULL;

	if (RB_EMPTY_ROOT(&b->waiters))
		return;

	if (!spin_trylock_irq(&b->lock)) {
		ee->waiters = ERR_PTR(-EDEADLK);
		return;
	}

	count = 0;
	for (rb = rb_first(&b->waiters); rb != NULL; rb = rb_next(rb))
		count++;
	spin_unlock_irq(&b->lock);

	waiter = NULL;
	if (count)
		waiter = kmalloc_array(count,
				       sizeof(struct drm_i915_error_waiter),
				       GFP_ATOMIC);
	if (!waiter)
		return;

	if (!spin_trylock_irq(&b->lock)) {
		kfree(waiter);
		ee->waiters = ERR_PTR(-EDEADLK);
		return;
	}

	ee->waiters = waiter;
	for (rb = rb_first(&b->waiters); rb; rb = rb_next(rb)) {
		struct intel_wait *w = rb_entry(rb, typeof(*w), node);

		strcpy(waiter->comm, w->tsk->comm);
		waiter->pid = w->tsk->pid;
		waiter->seqno = w->seqno;
		waiter++;

		if (++ee->num_waiters == count)
			break;
	}
	spin_unlock_irq(&b->lock);
}

static void error_record_engine_registers(struct drm_i915_error_state *error,
					  struct intel_engine_cs *engine,
					  struct drm_i915_error_engine *ee)
{
	struct drm_i915_private *dev_priv = engine->i915;

	if (INTEL_GEN(dev_priv) >= 6) {
		ee->rc_psmi = I915_READ(RING_PSMI_CTL(engine->mmio_base));
		ee->fault_reg = I915_READ(RING_FAULT_REG(engine));
		if (INTEL_GEN(dev_priv) >= 8)
			gen8_record_semaphore_state(error, engine, ee);
		else
			gen6_record_semaphore_state(engine, ee);
	}

	if (INTEL_GEN(dev_priv) >= 4) {
		ee->faddr = I915_READ(RING_DMA_FADD(engine->mmio_base));
		ee->ipeir = I915_READ(RING_IPEIR(engine->mmio_base));
		ee->ipehr = I915_READ(RING_IPEHR(engine->mmio_base));
		ee->instps = I915_READ(RING_INSTPS(engine->mmio_base));
		ee->bbaddr = I915_READ(RING_BBADDR(engine->mmio_base));
		if (INTEL_GEN(dev_priv) >= 8) {
			ee->faddr |= (u64) I915_READ(RING_DMA_FADD_UDW(engine->mmio_base)) << 32;
			ee->bbaddr |= (u64) I915_READ(RING_BBADDR_UDW(engine->mmio_base)) << 32;
		}
		ee->bbstate = I915_READ(RING_BBSTATE(engine->mmio_base));
	} else {
		ee->faddr = I915_READ(DMA_FADD_I8XX);
		ee->ipeir = I915_READ(IPEIR);
		ee->ipehr = I915_READ(IPEHR);
	}

	intel_engine_get_instdone(engine, &ee->instdone);

	ee->waiting = intel_engine_has_waiter(engine);
	ee->instpm = I915_READ(RING_INSTPM(engine->mmio_base));
	ee->acthd = intel_engine_get_active_head(engine);
	ee->seqno = intel_engine_get_seqno(engine);
	ee->last_seqno = intel_engine_last_submit(engine);
	ee->start = I915_READ_START(engine);
	ee->head = I915_READ_HEAD(engine);
	ee->tail = I915_READ_TAIL(engine);
	ee->ctl = I915_READ_CTL(engine);
	if (INTEL_GEN(dev_priv) > 2)
		ee->mode = I915_READ_MODE(engine);

	if (!HWS_NEEDS_PHYSICAL(dev_priv)) {
		i915_reg_t mmio;

		if (IS_GEN7(dev_priv)) {
			switch (engine->id) {
			default:
			case RCS:
				mmio = RENDER_HWS_PGA_GEN7;
				break;
			case BCS:
				mmio = BLT_HWS_PGA_GEN7;
				break;
			case VCS:
				mmio = BSD_HWS_PGA_GEN7;
				break;
			case VECS:
				mmio = VEBOX_HWS_PGA_GEN7;
				break;
			}
		} else if (IS_GEN6(engine->i915)) {
			mmio = RING_HWS_PGA_GEN6(engine->mmio_base);
		} else {
			/* XXX: gen8 returns to sanity */
			mmio = RING_HWS_PGA(engine->mmio_base);
		}

		ee->hws = I915_READ(mmio);
	}

	ee->hangcheck_timestamp = engine->hangcheck.action_timestamp;
	ee->hangcheck_action = engine->hangcheck.action;
	ee->hangcheck_stalled = engine->hangcheck.stalled;

	if (USES_PPGTT(dev_priv)) {
		int i;

		ee->vm_info.gfx_mode = I915_READ(RING_MODE_GEN7(engine));

		if (IS_GEN6(dev_priv))
			ee->vm_info.pp_dir_base =
				I915_READ(RING_PP_DIR_BASE_READ(engine));
		else if (IS_GEN7(dev_priv))
			ee->vm_info.pp_dir_base =
				I915_READ(RING_PP_DIR_BASE(engine));
		else if (INTEL_GEN(dev_priv) >= 8)
			for (i = 0; i < 4; i++) {
				ee->vm_info.pdp[i] =
					I915_READ(GEN8_RING_PDP_UDW(engine, i));
				ee->vm_info.pdp[i] <<= 32;
				ee->vm_info.pdp[i] |=
					I915_READ(GEN8_RING_PDP_LDW(engine, i));
			}
	}
}

static void record_request(struct drm_i915_gem_request *request,
			   struct drm_i915_error_request *erq)
{
	erq->context = request->ctx->hw_id;
	erq->ban_score = request->ctx->ban_score;
	erq->seqno = request->global_seqno;
	erq->jiffies = request->emitted_jiffies;
	erq->head = request->head;
	erq->tail = request->tail;

	rcu_read_lock();
	erq->pid = request->ctx->pid ? pid_nr(request->ctx->pid) : 0;
	rcu_read_unlock();
}

static void engine_record_requests(struct intel_engine_cs *engine,
				   struct drm_i915_gem_request *first,
				   struct drm_i915_error_engine *ee)
{
	struct drm_i915_gem_request *request;
	int count;

	count = 0;
	request = first;
	list_for_each_entry_from(request, &engine->timeline->requests, link)
		count++;
	if (!count)
		return;

	ee->requests = kcalloc(count, sizeof(*ee->requests), GFP_ATOMIC);
	if (!ee->requests)
		return;

	ee->num_requests = count;

	count = 0;
	request = first;
	list_for_each_entry_from(request, &engine->timeline->requests, link) {
		if (count >= ee->num_requests) {
			/*
			 * If the ring request list was changed in
			 * between the point where the error request
			 * list was created and dimensioned and this
			 * point then just exit early to avoid crashes.
			 *
			 * We don't need to communicate that the
			 * request list changed state during error
			 * state capture and that the error state is
			 * slightly incorrect as a consequence since we
			 * are typically only interested in the request
			 * list state at the point of error state
			 * capture, not in any changes happening during
			 * the capture.
			 */
			break;
		}

		record_request(request, &ee->requests[count++]);
	}
	ee->num_requests = count;
}

static void error_record_engine_execlists(struct intel_engine_cs *engine,
					  struct drm_i915_error_engine *ee)
{
	unsigned int n;

	for (n = 0; n < ARRAY_SIZE(engine->execlist_port); n++)
		if (engine->execlist_port[n].request)
			record_request(engine->execlist_port[n].request,
				       &ee->execlist[n]);
}

static void record_context(struct drm_i915_error_context *e,
			   struct i915_gem_context *ctx)
{
	if (ctx->pid) {
		struct task_struct *task;

		rcu_read_lock();
		task = pid_task(ctx->pid, PIDTYPE_PID);
		if (task) {
			strcpy(e->comm, task->comm);
			e->pid = task->pid;
		}
		rcu_read_unlock();
	}

	e->handle = ctx->user_handle;
	e->hw_id = ctx->hw_id;
	e->ban_score = ctx->ban_score;
	e->guilty = ctx->guilty_count;
	e->active = ctx->active_count;
}

static void i915_gem_record_rings(struct drm_i915_private *dev_priv,
				  struct drm_i915_error_state *error)
{
	struct i915_ggtt *ggtt = &dev_priv->ggtt;
	int i;

	error->semaphore =
		i915_error_object_create(dev_priv, dev_priv->semaphore);

	for (i = 0; i < I915_NUM_ENGINES; i++) {
		struct intel_engine_cs *engine = dev_priv->engine[i];
		struct drm_i915_error_engine *ee = &error->engine[i];
		struct drm_i915_gem_request *request;

		ee->engine_id = -1;

		if (!engine)
			continue;

		ee->engine_id = i;

		error_record_engine_registers(error, engine, ee);
		error_record_engine_waiters(engine, ee);
		error_record_engine_execlists(engine, ee);

		request = i915_gem_find_active_request(engine);
		if (request) {
			struct intel_ring *ring;

			ee->vm = request->ctx->ppgtt ?
				&request->ctx->ppgtt->base : &ggtt->base;

			record_context(&ee->context, request->ctx);

			/* We need to copy these to an anonymous buffer
			 * as the simplest method to avoid being overwritten
			 * by userspace.
			 */
			ee->batchbuffer =
				i915_error_object_create(dev_priv,
							 request->batch);

			if (HAS_BROKEN_CS_TLB(dev_priv))
				ee->wa_batchbuffer =
					i915_error_object_create(dev_priv,
								 engine->scratch);

			ee->ctx =
				i915_error_object_create(dev_priv,
							 request->ctx->engine[i].state);

			error->simulated |=
				i915_gem_context_no_error_capture(request->ctx);

			ee->rq_head = request->head;
			ee->rq_post = request->postfix;
			ee->rq_tail = request->tail;

			ring = request->ring;
			ee->cpu_ring_head = ring->head;
			ee->cpu_ring_tail = ring->tail;
			ee->ringbuffer =
				i915_error_object_create(dev_priv, ring->vma);

			engine_record_requests(engine, request, ee);
		}

		ee->hws_page =
			i915_error_object_create(dev_priv,
						 engine->status_page.vma);

		ee->wa_ctx =
			i915_error_object_create(dev_priv, engine->wa_ctx.vma);
	}
}

static void i915_gem_capture_vm(struct drm_i915_private *dev_priv,
				struct drm_i915_error_state *error,
				struct i915_address_space *vm,
				int idx)
{
	struct drm_i915_error_buffer *active_bo;
	struct i915_vma *vma;
	int count;

	count = 0;
	list_for_each_entry(vma, &vm->active_list, vm_link)
		count++;

	active_bo = NULL;
	if (count)
		active_bo = kcalloc(count, sizeof(*active_bo), GFP_ATOMIC);
	if (active_bo)
		count = capture_error_bo(active_bo, count, &vm->active_list, false);
	else
		count = 0;

	error->active_vm[idx] = vm;
	error->active_bo[idx] = active_bo;
	error->active_bo_count[idx] = count;
}

static void i915_capture_active_buffers(struct drm_i915_private *dev_priv,
					struct drm_i915_error_state *error)
{
	int cnt = 0, i, j;

	BUILD_BUG_ON(ARRAY_SIZE(error->engine) > ARRAY_SIZE(error->active_bo));
	BUILD_BUG_ON(ARRAY_SIZE(error->active_bo) != ARRAY_SIZE(error->active_vm));
	BUILD_BUG_ON(ARRAY_SIZE(error->active_bo) != ARRAY_SIZE(error->active_bo_count));

	/* Scan each engine looking for unique active contexts/vm */
	for (i = 0; i < ARRAY_SIZE(error->engine); i++) {
		struct drm_i915_error_engine *ee = &error->engine[i];
		bool found;

		if (!ee->vm)
			continue;

		found = false;
		for (j = 0; j < i && !found; j++)
			found = error->engine[j].vm == ee->vm;
		if (!found)
			i915_gem_capture_vm(dev_priv, error, ee->vm, cnt++);
	}
}

static void i915_capture_pinned_buffers(struct drm_i915_private *dev_priv,
					struct drm_i915_error_state *error)
{
	struct i915_address_space *vm = &dev_priv->ggtt.base;
	struct drm_i915_error_buffer *bo;
	struct i915_vma *vma;
	int count_inactive, count_active;

	count_inactive = 0;
	list_for_each_entry(vma, &vm->active_list, vm_link)
		count_inactive++;

	count_active = 0;
	list_for_each_entry(vma, &vm->inactive_list, vm_link)
		count_active++;

	bo = NULL;
	if (count_inactive + count_active)
		bo = kcalloc(count_inactive + count_active,
			     sizeof(*bo), GFP_ATOMIC);
	if (!bo)
		return;

	count_inactive = capture_error_bo(bo, count_inactive,
					  &vm->active_list, true);
	count_active = capture_error_bo(bo + count_inactive, count_active,
					&vm->inactive_list, true);
	error->pinned_bo_count = count_inactive + count_active;
	error->pinned_bo = bo;
}

static void i915_gem_capture_guc_log_buffer(struct drm_i915_private *dev_priv,
					    struct drm_i915_error_state *error)
{
	/* Capturing log buf contents won't be useful if logging was disabled */
	if (!dev_priv->guc.log.vma || (i915.guc_log_level < 0))
		return;

	error->guc_log = i915_error_object_create(dev_priv,
						  dev_priv->guc.log.vma);
}

/* Capture all registers which don't fit into another category. */
static void i915_capture_reg_state(struct drm_i915_private *dev_priv,
				   struct drm_i915_error_state *error)
{
	int i;

	/* General organization
	 * 1. Registers specific to a single generation
	 * 2. Registers which belong to multiple generations
	 * 3. Feature specific registers.
	 * 4. Everything else
	 * Please try to follow the order.
	 */

	/* 1: Registers specific to a single generation */
	if (IS_VALLEYVIEW(dev_priv)) {
		error->gtier[0] = I915_READ(GTIER);
		error->ier = I915_READ(VLV_IER);
		error->forcewake = I915_READ_FW(FORCEWAKE_VLV);
	}

	if (IS_GEN7(dev_priv))
		error->err_int = I915_READ(GEN7_ERR_INT);

	if (INTEL_GEN(dev_priv) >= 8) {
		error->fault_data0 = I915_READ(GEN8_FAULT_TLB_DATA0);
		error->fault_data1 = I915_READ(GEN8_FAULT_TLB_DATA1);
	}

	if (IS_GEN6(dev_priv)) {
		error->forcewake = I915_READ_FW(FORCEWAKE);
		error->gab_ctl = I915_READ(GAB_CTL);
		error->gfx_mode = I915_READ(GFX_MODE);
	}

	/* 2: Registers which belong to multiple generations */
	if (INTEL_GEN(dev_priv) >= 7)
		error->forcewake = I915_READ_FW(FORCEWAKE_MT);

	if (INTEL_GEN(dev_priv) >= 6) {
		error->derrmr = I915_READ(DERRMR);
		error->error = I915_READ(ERROR_GEN6);
		error->done_reg = I915_READ(DONE_REG);
	}

	/* 3: Feature specific registers */
	if (IS_GEN6(dev_priv) || IS_GEN7(dev_priv)) {
		error->gam_ecochk = I915_READ(GAM_ECOCHK);
		error->gac_eco = I915_READ(GAC_ECO_BITS);
	}

	/* 4: Everything else */
	if (HAS_HW_CONTEXTS(dev_priv))
		error->ccid = I915_READ(CCID);

	if (INTEL_GEN(dev_priv) >= 8) {
		error->ier = I915_READ(GEN8_DE_MISC_IER);
		for (i = 0; i < 4; i++)
			error->gtier[i] = I915_READ(GEN8_GT_IER(i));
	} else if (HAS_PCH_SPLIT(dev_priv)) {
		error->ier = I915_READ(DEIER);
		error->gtier[0] = I915_READ(GTIER);
	} else if (IS_GEN2(dev_priv)) {
		error->ier = I915_READ16(IER);
	} else if (!IS_VALLEYVIEW(dev_priv)) {
		error->ier = I915_READ(IER);
	}
	error->eir = I915_READ(EIR);
	error->pgtbl_er = I915_READ(PGTBL_ER);
}

static void i915_error_capture_msg(struct drm_i915_private *dev_priv,
				   struct drm_i915_error_state *error,
				   u32 engine_mask,
				   const char *error_msg)
{
	u32 ecode;
	int engine_id = -1, len;

	ecode = i915_error_generate_code(dev_priv, error, &engine_id);

	len = scnprintf(error->error_msg, sizeof(error->error_msg),
			"GPU HANG: ecode %d:%d:0x%08x",
			INTEL_GEN(dev_priv), engine_id, ecode);

	if (engine_id != -1 && error->engine[engine_id].context.pid)
		len += scnprintf(error->error_msg + len,
				 sizeof(error->error_msg) - len,
				 ", in %s [%d]",
				 error->engine[engine_id].context.comm,
				 error->engine[engine_id].context.pid);

	scnprintf(error->error_msg + len, sizeof(error->error_msg) - len,
		  ", reason: %s, action: %s",
		  error_msg,
		  engine_mask ? "reset" : "continue");
}

static void i915_capture_gen_state(struct drm_i915_private *dev_priv,
				   struct drm_i915_error_state *error)
{
	error->iommu = -1;
#ifdef CONFIG_INTEL_IOMMU
	error->iommu = intel_iommu_gfx_mapped;
#endif
	error->reset_count = i915_reset_count(&dev_priv->gpu_error);
	error->suspend_count = dev_priv->suspend_count;

	memcpy(&error->device_info,
	       INTEL_INFO(dev_priv),
	       sizeof(error->device_info));
}

static int capture(void *data)
{
	struct drm_i915_error_state *error = data;

	do_gettimeofday(&error->time);
	error->boottime = ktime_to_timeval(ktime_get_boottime());
	error->uptime =
		ktime_to_timeval(ktime_sub(ktime_get(),
					   error->i915->gt.last_init_time));

	error->params = i915;

	i915_capture_gen_state(error->i915, error);
	i915_capture_reg_state(error->i915, error);
	i915_gem_record_fences(error->i915, error);
	i915_gem_record_rings(error->i915, error);
	i915_capture_active_buffers(error->i915, error);
	i915_capture_pinned_buffers(error->i915, error);
	i915_gem_capture_guc_log_buffer(error->i915, error);

	error->overlay = intel_overlay_capture_error_state(error->i915);
	error->display = intel_display_capture_error_state(error->i915);

	return 0;
}

#define DAY_AS_SECONDS(x) (24 * 60 * 60 * (x))

/**
 * i915_capture_error_state - capture an error record for later analysis
 * @dev: drm device
 *
 * Should be called when an error is detected (either a hang or an error
 * interrupt) to capture error state from the time of the error.  Fills
 * out a structure which becomes available in debugfs for user level tools
 * to pick up.
 */
void i915_capture_error_state(struct drm_i915_private *dev_priv,
			      u32 engine_mask,
			      const char *error_msg)
{
	static bool warned;
	struct drm_i915_error_state *error;
	unsigned long flags;

	if (!i915.error_capture)
		return;

	if (READ_ONCE(dev_priv->gpu_error.first_error))
		return;

	/* Account for pipe specific data like PIPE*STAT */
	error = kzalloc(sizeof(*error), GFP_ATOMIC);
	if (!error) {
		DRM_DEBUG_DRIVER("out of memory, not capturing error state\n");
		return;
	}

	kref_init(&error->ref);
	error->i915 = dev_priv;

	stop_machine(capture, error, NULL);

	i915_error_capture_msg(dev_priv, error, engine_mask, error_msg);
	DRM_INFO("%s\n", error->error_msg);

	if (!error->simulated) {
		spin_lock_irqsave(&dev_priv->gpu_error.lock, flags);
		if (!dev_priv->gpu_error.first_error) {
			dev_priv->gpu_error.first_error = error;
			error = NULL;
		}
		spin_unlock_irqrestore(&dev_priv->gpu_error.lock, flags);
	}

	if (error) {
		i915_error_state_free(&error->ref);
		return;
	}

	if (!warned &&
	    ktime_get_real_seconds() - DRIVER_TIMESTAMP < DAY_AS_SECONDS(180)) {
		DRM_INFO("GPU hangs can indicate a bug anywhere in the entire gfx stack, including userspace.\n");
		DRM_INFO("Please file a _new_ bug report on bugs.freedesktop.org against DRI -> DRM/Intel\n");
		DRM_INFO("drm/i915 developers can then reassign to the right component if it's not a kernel issue.\n");
		DRM_INFO("The gpu crash dump is required to analyze gpu hangs, so please always attach it.\n");
		DRM_INFO("GPU crash dump saved to /sys/class/drm/card%d/error\n",
			 dev_priv->drm.primary->index);
		warned = true;
	}
}

void i915_error_state_get(struct drm_device *dev,
			  struct i915_error_state_file_priv *error_priv)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	spin_lock_irq(&dev_priv->gpu_error.lock);
	error_priv->error = dev_priv->gpu_error.first_error;
	if (error_priv->error)
		kref_get(&error_priv->error->ref);
	spin_unlock_irq(&dev_priv->gpu_error.lock);
}

void i915_error_state_put(struct i915_error_state_file_priv *error_priv)
{
	if (error_priv->error)
		kref_put(&error_priv->error->ref, i915_error_state_free);
}

void i915_destroy_error_state(struct drm_i915_private *dev_priv)
{
	struct drm_i915_error_state *error;

	spin_lock_irq(&dev_priv->gpu_error.lock);
	error = dev_priv->gpu_error.first_error;
	dev_priv->gpu_error.first_error = NULL;
	spin_unlock_irq(&dev_priv->gpu_error.lock);

	if (error)
		kref_put(&error->ref, i915_error_state_free);
}
