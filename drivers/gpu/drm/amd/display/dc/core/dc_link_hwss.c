/* Copyright 2015 Advanced Micro Devices, Inc. */


#include "dm_services.h"
#include "dc.h"
#include "inc/core_dc.h"
#include "include/ddc_service_types.h"
#include "include/i2caux_interface.h"
#include "link_hwss.h"
#include "hw_sequencer.h"
#include "dc_link_dp.h"
#include "dc_link_ddc.h"
#include "dm_helpers.h"
#include "dce/dce_link_encoder.h"
#include "dce/dce_stream_encoder.h"
#include "dpcd_defs.h"

enum dc_status core_link_read_dpcd(
	struct core_link* link,
	uint32_t address,
	uint8_t *data,
	uint32_t size)
{
	if (!dm_helpers_dp_read_dpcd(link->ctx,
			&link->public,
			address, data, size))
			return DC_ERROR_UNEXPECTED;

	return DC_OK;
}

enum dc_status core_link_write_dpcd(
	struct core_link* link,
	uint32_t address,
	const uint8_t *data,
	uint32_t size)
{
	if (!dm_helpers_dp_write_dpcd(link->ctx,
			&link->public,
			address, data, size))
				return DC_ERROR_UNEXPECTED;

	return DC_OK;
}

void dp_receiver_power_ctrl(struct core_link *link, bool on)
{
	uint8_t state;

	state = on ? DP_POWER_STATE_D0 : DP_POWER_STATE_D3;

	core_link_write_dpcd(link, DP_SET_POWER, &state,
			sizeof(state));
}

void dp_enable_link_phy(
	struct core_link *link,
	enum signal_type signal,
	enum clock_source_id clock_source,
	const struct dc_link_settings *link_settings)
{
	struct link_encoder *link_enc = link->link_enc;

	if (dc_is_dp_sst_signal(signal)) {
		if (signal == SIGNAL_TYPE_EDP) {
			link_enc->funcs->power_control(link_enc, true);
			link_enc->funcs->backlight_control(link_enc, true);
		}

		link_enc->funcs->enable_dp_output(
						link_enc,
						link_settings,
						clock_source);
	} else {
		link_enc->funcs->enable_dp_mst_output(
						link_enc,
						link_settings,
						clock_source);
	}

	dp_receiver_power_ctrl(link, true);
}

void dp_disable_link_phy(struct core_link *link, enum signal_type signal)
{
	if (!link->wa_flags.dp_keep_receiver_powered)
		dp_receiver_power_ctrl(link, false);

	if (signal == SIGNAL_TYPE_EDP)
		link->link_enc->funcs->backlight_control(link->link_enc, false);

	link->link_enc->funcs->disable_output(link->link_enc, signal);

	/* Clear current link setting.*/
	memset(&link->public.cur_link_settings, 0,
			sizeof(link->public.cur_link_settings));
}

void dp_disable_link_phy_mst(struct core_link *link, enum signal_type signal)
{
	/* MST disable link only when no stream use the link */
	if (link->mst_stream_alloc_table.stream_count > 0)
		return;

	dp_disable_link_phy(link, signal);
}

bool dp_set_hw_training_pattern(
	struct core_link *link,
	enum hw_dp_training_pattern pattern)
{
	enum dp_test_pattern test_pattern = DP_TEST_PATTERN_UNSUPPORTED;

	switch (pattern) {
	case HW_DP_TRAINING_PATTERN_1:
		test_pattern = DP_TEST_PATTERN_TRAINING_PATTERN1;
		break;
	case HW_DP_TRAINING_PATTERN_2:
		test_pattern = DP_TEST_PATTERN_TRAINING_PATTERN2;
		break;
	case HW_DP_TRAINING_PATTERN_3:
		test_pattern = DP_TEST_PATTERN_TRAINING_PATTERN3;
		break;
	case HW_DP_TRAINING_PATTERN_4:
		test_pattern = DP_TEST_PATTERN_TRAINING_PATTERN4;
		break;
	default:
		break;
	}

	dp_set_hw_test_pattern(link, test_pattern, NULL, 0);

	return true;
}

void dp_set_hw_lane_settings(
	struct core_link *link,
	const struct link_training_settings *link_settings)
{
	struct link_encoder *encoder = link->link_enc;

	/* call Encoder to set lane settings */
	encoder->funcs->dp_set_lane_settings(encoder, link_settings);
}

enum dp_panel_mode dp_get_panel_mode(struct core_link *link)
{
	/* We need to explicitly check that connector
	 * is not DP. Some Travis_VGA get reported
	 * by video bios as DP.
	 */
	if (link->public.connector_signal != SIGNAL_TYPE_DISPLAY_PORT) {

		switch (link->dpcd_caps.branch_dev_id) {
		case DP_BRANCH_DEVICE_ID_2:
			if (strncmp(
				link->dpcd_caps.branch_dev_name,
				DP_VGA_LVDS_CONVERTER_ID_2,
				sizeof(
				link->dpcd_caps.
				branch_dev_name)) == 0) {
				return DP_PANEL_MODE_SPECIAL;
			}
			break;
		case DP_BRANCH_DEVICE_ID_3:
			if (strncmp(link->dpcd_caps.branch_dev_name,
				DP_VGA_LVDS_CONVERTER_ID_3,
				sizeof(
				link->dpcd_caps.
				branch_dev_name)) == 0) {
				return DP_PANEL_MODE_SPECIAL;
			}
			break;
		default:
			break;
		}

		if (link->dpcd_caps.panel_mode_edp) {
			return DP_PANEL_MODE_EDP;
		}
	}

	return DP_PANEL_MODE_DEFAULT;
}

void dp_set_hw_test_pattern(
	struct core_link *link,
	enum dp_test_pattern test_pattern,
	uint8_t *custom_pattern,
	uint32_t custom_pattern_size)
{
	struct encoder_set_dp_phy_pattern_param pattern_param = {0};
	struct link_encoder *encoder = link->link_enc;

	pattern_param.dp_phy_pattern = test_pattern;
	pattern_param.custom_pattern = custom_pattern;
	pattern_param.custom_pattern_size = custom_pattern_size;
	pattern_param.dp_panel_mode = dp_get_panel_mode(link);

	encoder->funcs->dp_set_phy_pattern(encoder, &pattern_param);
}

void dp_retrain_link_dp_test(struct core_link *link,
			struct dc_link_settings *link_setting,
			bool skip_video_pattern)
{
	struct pipe_ctx *pipes =
			&link->dc->current_context->res_ctx.pipe_ctx[0];
	unsigned int i;

	for (i = 0; i < MAX_PIPES; i++) {
		if (pipes[i].stream != NULL &&
			pipes[i].stream->sink != NULL &&
			pipes[i].stream->sink->link != NULL &&
			pipes[i].stream_enc != NULL &&
			pipes[i].stream->sink->link == link) {
			dm_delay_in_microseconds(link->ctx, 100);

			pipes[i].stream_enc->funcs->dp_blank(
					pipes[i].stream_enc);

			/* disable any test pattern that might be active */
			dp_set_hw_test_pattern(link,
					DP_TEST_PATTERN_VIDEO_MODE, NULL, 0);

			dp_receiver_power_ctrl(link, false);

			link->link_enc->funcs->disable_output(
					link->link_enc,
					SIGNAL_TYPE_DISPLAY_PORT);

			/* Clear current link setting. */
			memset(&link->public.cur_link_settings, 0,
				sizeof(link->public.cur_link_settings));

			link->link_enc->funcs->enable_dp_output(
						link->link_enc,
						link_setting,
						pipes[i].clock_source->id);

			dp_receiver_power_ctrl(link, true);

			dc_link_dp_perform_link_training(
					&link->public,
					link_setting,
					skip_video_pattern);

			link->public.cur_link_settings = *link_setting;

			link->dc->hwss.unblank_stream(&pipes[i],
					link_setting);
		}
	}
}
