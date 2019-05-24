/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_SSEU_H__
#define __INTEL_SSEU_H__

#include <linux/types.h>
#include <linux/kernel.h>

struct drm_i915_private;

#define GEN_MAX_SLICES		(6) /* CNL upper bound */
#define GEN_MAX_SUBSLICES	(8) /* ICL upper bound */
#define GEN_SSEU_STRIDE(max_entries) DIV_ROUND_UP(max_entries, BITS_PER_BYTE)
#define GEN_MAX_SUBSLICE_STRIDE GEN_SSEU_STRIDE(GEN_MAX_SUBSLICES)

struct sseu_dev_info {
	u8 slice_mask;
	u8 subslice_mask[GEN_MAX_SLICES * GEN_MAX_SUBSLICE_STRIDE];
	u16 eu_total;
	u8 eu_per_subslice;
	u8 min_eu_in_pool;
	/* For each slice, which subslice(s) has(have) 7 EUs (bitfield)? */
	u8 subslice_7eu[3];
	u8 has_slice_pg:1;
	u8 has_subslice_pg:1;
	u8 has_eu_pg:1;

	/* Topology fields */
	u8 max_slices;
	u8 max_subslices;
	u8 max_eus_per_subslice;

	u8 ss_stride;
	u8 eu_stride;

	/* We don't have more than 8 eus per subslice at the moment and as we
	 * store eus enabled using bits, no need to multiply by eus per
	 * subslice.
	 */
	u8 eu_mask[GEN_MAX_SLICES * GEN_MAX_SUBSLICES];
};

/*
 * Powergating configuration for a particular (context,engine).
 */
struct intel_sseu {
	u8 slice_mask;
	u8 subslice_mask;
	u8 min_eus_per_subslice;
	u8 max_eus_per_subslice;
};

static inline struct intel_sseu
intel_sseu_from_device_info(const struct sseu_dev_info *sseu)
{
	struct intel_sseu value = {
		.slice_mask = sseu->slice_mask,
		.subslice_mask = sseu->subslice_mask[0],
		.min_eus_per_subslice = sseu->max_eus_per_subslice,
		.max_eus_per_subslice = sseu->max_eus_per_subslice,
	};

	return value;
}

static inline bool
intel_sseu_has_subslice(const struct sseu_dev_info *sseu, int slice,
			int subslice)
{
	u8 mask = sseu->subslice_mask[slice * sseu->ss_stride +
				      subslice / BITS_PER_BYTE];

	return mask & BIT(subslice % BITS_PER_BYTE);
}

void intel_sseu_set_info(struct sseu_dev_info *sseu, u8 max_slices,
			 u8 max_subslices, u8 max_eus_per_subslice);

unsigned int
intel_sseu_subslice_total(const struct sseu_dev_info *sseu);

unsigned int
intel_sseu_subslices_per_slice(const struct sseu_dev_info *sseu, u8 slice);

void intel_sseu_copy_subslices(const struct sseu_dev_info *sseu, int slice,
			       u8 *to_mask);

u32  intel_sseu_get_subslices(const struct sseu_dev_info *sseu, u8 slice);

void intel_sseu_set_subslices(struct sseu_dev_info *sseu, int slice,
			      u32 ss_mask);

u32 intel_sseu_make_rpcs(struct drm_i915_private *i915,
			 const struct intel_sseu *req_sseu);

#endif /* __INTEL_SSEU_H__ */
