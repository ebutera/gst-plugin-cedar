/*
 * Cedar H264 Encoder Plugin
 * Copyright (C) 2014 Enrico Butera <ebutera@users.sourceforge.net>
 * 
 * Byte stream utils:
 * Copyright (c) 2014 Jens Kuske <jenskuske@gmail.com>
 * 
 * Gst template code:
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-cedar_h264enc
 *
 * H264 Encoder plugin using CedarX hardware engine
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -ve videotestsrc ! cedar_h264enc ! h264parse ! matroskamux ! filesink location="cedar.mkv"
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>
#include <unistd.h>
#include <gst/gst.h>

#include "gstcedarh264enc.h"
#include "ve.h"

GST_DEBUG_CATEGORY_STATIC (gst_cedarh264enc_debug);
#define GST_CAT_DEFAULT gst_cedarh264enc_debug

#define CEDAR_OUTPUT_BUF_SIZE	(1* 1024 * 1024)

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_SILENT
};

/* the capabilities of the inputs and outputs.
 *
 * describe the real formats here.
 */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
		"video/x-raw-yuv, "
			"format = (fourcc) NV12, "
			"width = (int) [16,1920], "
			"height = (int) [16,1080]"
			/*"framerate=(fraction)[1/1,25/1]"*/
    )
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
		"video/x-h264, "
			"stream-format = (string) byte-stream, "
			"alignment = (string) nal, "
			"profile = (string) { main }"
	)
    );

GST_BOILERPLATE (Gstcedarh264enc, gst_cedarh264enc, GstElement,
    GST_TYPE_ELEMENT);

static void gst_cedarh264enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_cedarh264enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_cedarh264enc_set_caps (GstPad * pad, GstCaps * caps);
static GstFlowReturn gst_cedarh264enc_chain (GstPad * pad, GstBuffer * buf);

static GstStateChangeReturn
	gst_cedarh264enc_change_state (GstElement *element, GstStateChange transition);

/* byte stream utils from:
 * https://github.com/jemk/cedrus/tree/master/h264enc
 */
static void put_bits(void* regs, uint32_t x, int num)
{
	writel(x, regs + VE_AVC_BASIC_BITS);
	writel(0x1 | ((num & 0x1f) << 8), regs + VE_AVC_TRIGGER);
	// again the problem, how to check for finish?
}

static void put_ue(void* regs, uint32_t x)
{
	x++;
	put_bits(regs, x, (32 - __builtin_clz(x)) * 2 - 1);
}

static void put_se(void* regs, int x)
{
	x = 2 * x - 1;
	x ^= (x >> 31);
	put_ue(regs, x);
}

static void put_start_code(void* regs)
{
	uint32_t tmp = readl(regs + VE_AVC_PARAM);

	// disable emulation_prevention_three_byte
	writel(tmp | (0x1 << 31), regs + VE_AVC_PARAM);

	put_bits(regs, 0, 31);
	put_bits(regs, 1, 1);

	writel(tmp, regs + VE_AVC_PARAM);
}

static void put_rbsp_trailing_bits(void* regs)
{
	unsigned int cur_bs_len = readl(regs + VE_AVC_VLE_LENGTH);

	int num_zero_bits = 8 - ((cur_bs_len + 1) & 0x7);
	put_bits(regs, 1 << num_zero_bits, num_zero_bits + 1);
}

static void put_seq_parameter_set(void* regs, int width, int height)
{
	put_bits(regs, 3 << 5 | 7 << 0, 8);	// NAL Header
	put_bits(regs, 77, 8);			// profile_idc
	put_bits(regs, 0x0, 8);			// constraints
	put_bits(regs, 4 * 10 + 1, 8);		// level_idc
	put_ue(regs, 0);			// seq_parameter_set_id

	put_ue(regs, 0);			// log2_max_frame_num_minus4
	put_ue(regs, 0);			// pic_order_cnt_type
	// if (pic_order_cnt_type == 0)
		put_ue(regs, 4);		// log2_max_pic_order_cnt_lsb_minus4

	put_ue(regs, 1);			// max_num_ref_frames
	put_bits(regs, 0, 1);			// gaps_in_frame_num_value_allowed_flag

	put_ue(regs, width - 1);		// pic_width_in_mbs_minus1
	put_ue(regs, height - 1);		// pic_height_in_map_units_minus1

	put_bits(regs, 1, 1);			// frame_mbs_only_flag
	// if (!frame_mbs_only_flag)

	put_bits(regs, 1, 1);			// direct_8x8_inference_flag
	put_bits(regs, 0, 1);			// frame_cropping_flag
	// if (frame_cropping_flag)

	put_bits(regs, 0, 1);			// vui_parameters_present_flag
	// if (vui_parameters_present_flag)
}

static void put_pic_parameter_set(void *regs)
{
	put_bits(regs, 3 << 5 | 8 << 0, 8);	// NAL Header
	put_ue(regs, 0);			// pic_parameter_set_id
	put_ue(regs, 0);			// seq_parameter_set_id
	put_bits(regs, 1, 1);			// entropy_coding_mode_flag
	put_bits(regs, 0, 1);			// bottom_field_pic_order_in_frame_present_flag
	put_ue(regs, 0);			// num_slice_groups_minus1
	// if (num_slice_groups_minus1 > 0)

	put_ue(regs, 0);			// num_ref_idx_l0_default_active_minus1
	put_ue(regs, 0);			// num_ref_idx_l1_default_active_minus1
	put_bits(regs, 0, 1);			// weighted_pred_flag
	put_bits(regs, 0, 2);			// weighted_bipred_idc
	put_se(regs, 0);			// pic_init_qp_minus26
	put_se(regs, 0);			// pic_init_qs_minus26
	put_se(regs, 4);			// chroma_qp_index_offset
	put_bits(regs, 1, 1);			// deblocking_filter_control_present_flag
	put_bits(regs, 0, 1);			// constrained_intra_pred_flag
	put_bits(regs, 0, 1);			// redundant_pic_cnt_present_flag
}

static void put_slice_header(void* regs)
{
	put_bits(regs, 3 << 5 | 5 << 0, 8);	// NAL Header

	put_ue(regs, 0);			// first_mb_in_slice
	put_ue(regs, 2);			// slice_type
	put_ue(regs, 0);			// pic_parameter_set_id
	put_bits(regs, 0, 4);			// frame_num

	// if (IdrPicFlag)
		put_ue(regs, 0);		// idr_pic_id

	// if (pic_order_cnt_type == 0)
		put_bits(regs, 0, 8);		// pic_order_cnt_lsb

	// dec_ref_pic_marking
		put_bits(regs, 0, 1);		// no_output_of_prior_pics_flag
		put_bits(regs, 0, 1);		// long_term_reference_flag

	put_se(regs, 4);			// slice_qp_delta

	// if (deblocking_filter_control_present_flag)
		put_ue(regs, 0);		// disable_deblocking_filter_idc
		// if (disable_deblocking_filter_idc != 1)
			put_se(regs, 0);	// slice_alpha_c0_offset_div2
			put_se(regs, 0);	// slice_beta_offset_div2
}

static void put_aud(void* regs)
{
	put_bits(regs, 0 << 5 | 9 << 0, 8);	// NAL Header

	put_bits(regs, 7, 3);			// primary_pic_type
}

static gboolean alloc_cedar_bufs(Gstcedarh264enc *cedarelement)
{
	cedarelement->tile_w = (cedarelement->width + 31) & ~31;
	cedarelement->tile_w2 = (cedarelement->width / 2 + 31) & ~31;
	cedarelement->tile_h = (cedarelement->height + 31) & ~31;
	cedarelement->tile_h2 = (cedarelement->height / 2 + 31) & ~31;
	cedarelement->mb_w = (cedarelement->width + 15) / 16;
	cedarelement->mb_h = (cedarelement->height + 15) / 16;
	cedarelement->plane_size = cedarelement->mb_w * 16 * cedarelement->mb_h * 16;
	
	cedarelement->output_buf = ve_malloc(CEDAR_OUTPUT_BUF_SIZE);
	if (!cedarelement->output_buf) {
		GST_ERROR("Cannot allocate Cedar output buffer");
		return FALSE;
	}
	
	/**
	 * TODO: avoid input buffer copy and let upstream element write directly to
	 *   a cedar buffer (pad alloc?)
	 */
	cedarelement->input_buf = ve_malloc(cedarelement->plane_size + cedarelement->plane_size / 2);
	if (!cedarelement->input_buf) {
		GST_ERROR("Cannot allocate Cedar output buffer");
		goto error_out1;
	}

	cedarelement->reconstruct_buf =
			ve_malloc(cedarelement->tile_w * cedarelement->tile_h + cedarelement->tile_w * cedarelement->tile_h2);
	if (!cedarelement->reconstruct_buf) {
		GST_ERROR("Cannot allocate Cedar reconstruct buffer");
		goto error_out2;
	}
	
	cedarelement->small_luma_buf = ve_malloc(cedarelement->tile_w2 * cedarelement->tile_h2);
	if (!cedarelement->small_luma_buf) {
		GST_ERROR("Cannot allocate Cedar small luma buffer");
		goto error_out3;
	}
	
	cedarelement->mb_info_buf = ve_malloc(0x1000);
	if (!cedarelement->mb_info_buf) {
		GST_ERROR("Cannot allocate Cedar mb info buffer");
		goto error_out4;
	}
	
	// activate AVC engine
	writel(0x0013000b, cedarelement->ve_regs + VE_CTRL);
	
	return TRUE;

error_out4:
	ve_free(cedarelement->small_luma_buf);
	cedarelement->small_luma_buf = NULL;
error_out3:
	ve_free(cedarelement->reconstruct_buf);
	cedarelement->reconstruct_buf = NULL;
error_out2:
	ve_free(cedarelement->input_buf);
	cedarelement->input_buf = NULL;
error_out1:
	ve_free(cedarelement->output_buf);
	cedarelement->output_buf = NULL;

	return FALSE;
}

/* GObject vmethod implementations */

static void
gst_cedarh264enc_base_init (gpointer gclass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (gclass);

  gst_element_class_set_details_simple(element_class,
    "cedar_h264enc",
    "CedarX H264 Encoder",
    "H264 Encoder Plugin for CedarX hardware",
    "Enrico Butera <ebutera@users.berlios.de>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_factory));
}

/* initialize the cedar_h264enc's class */
static void
gst_cedarh264enc_class_init (Gstcedarh264encClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  
  gobject_class->set_property = gst_cedarh264enc_set_property;
  gobject_class->get_property = gst_cedarh264enc_get_property;
  
  gstelement_class->change_state = gst_cedarh264enc_change_state;

  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
          FALSE, G_PARAM_READWRITE));
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
static void
gst_cedarh264enc_init (Gstcedarh264enc * filter,
    Gstcedarh264encClass * gclass)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_setcaps_function (filter->sinkpad,
                                GST_DEBUG_FUNCPTR(gst_cedarh264enc_set_caps));
  gst_pad_set_chain_function (filter->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_cedarh264enc_chain));

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_pad_use_fixed_caps(filter->srcpad);

  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);
  filter->silent = FALSE;
}

static void
gst_cedarh264enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  Gstcedarh264enc *filter = GST_CEDAR_H264ENC (object);

  switch (prop_id) {
    case PROP_SILENT:
      filter->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_cedarh264enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  Gstcedarh264enc *filter = GST_CEDAR_H264ENC (object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, filter->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstElement vmethod implementations */

/* this function handles the link with other elements */
static gboolean
gst_cedarh264enc_set_caps (GstPad * pad, GstCaps * caps)
{
	Gstcedarh264enc *filter;
	GstPad *otherpad;
	GstCaps *othercaps;

	filter = GST_CEDAR_H264ENC (gst_pad_get_parent (pad));
	otherpad = (pad == filter->srcpad) ? filter->sinkpad : filter->srcpad;
  
	if (pad == filter->sinkpad) {
		int ret;
		int fps_num, fps_den;
		
		gst_video_format_parse_caps(caps, NULL, &filter->width, &filter->height);
		gst_video_parse_caps_framerate(caps, &fps_num, &fps_den);
		
		othercaps = gst_caps_copy (gst_pad_get_pad_template_caps(filter->srcpad));
		gst_caps_set_simple (othercaps,
			"width", G_TYPE_INT, filter->width,
			"height", G_TYPE_INT, filter->height,
			"framerate", GST_TYPE_FRACTION, fps_num, fps_den,
			"profile", G_TYPE_STRING, "main", NULL);
		
		gst_object_unref (filter);
		ret = gst_pad_set_caps (otherpad, othercaps);
		gst_caps_unref(othercaps);
		
		return ret;
	}

	gst_object_unref (filter);
	return gst_pad_set_caps (otherpad, caps);
}

/* chain function
 * this function does the actual processing
 */
static GstFlowReturn
gst_cedarh264enc_chain (GstPad * pad, GstBuffer * buf)
{
	Gstcedarh264enc *filter;
	GstBuffer *outbuf;

	filter = GST_CEDAR_H264ENC (GST_OBJECT_PARENT (pad));

	if (!filter->input_buf || !filter->output_buf) {
		if (!alloc_cedar_bufs(filter)) {
			GST_ERROR("Cannot allocate cedar buffers");
			return GST_FLOW_ERROR;
		}
	}
	
	if (!GST_BUFFER_DATA(buf)) {
		// TODO: needed?
		GST_WARNING("Received empty buffer");
		outbuf = gst_buffer_new();
		gst_buffer_set_caps(outbuf, GST_PAD_CAPS(filter->srcpad));
		GST_BUFFER_TIMESTAMP(outbuf) = GST_BUFFER_TIMESTAMP(buf);
		
		return gst_pad_push (filter->srcpad, outbuf);
	}
	
	memcpy(filter->input_buf, GST_BUFFER_DATA(buf), GST_BUFFER_SIZE(buf));
	
	ve_flush_cache(filter->input_buf, filter->plane_size + filter->plane_size / 2);

	// output buffer
	// flush output buffer, otherwise we might read old cached data
	ve_flush_cache(filter->output_buf, CEDAR_OUTPUT_BUF_SIZE);
	
	writel(0x0, filter->ve_regs + VE_AVC_VLE_OFFSET);
	writel(ve_virt2phys(filter->output_buf), filter->ve_regs + VE_AVC_VLE_ADDR);
	writel(ve_virt2phys(filter->output_buf) + CEDAR_OUTPUT_BUF_SIZE - 1, filter->ve_regs + VE_AVC_VLE_END);

	writel(0x04000000, filter->ve_regs + 0xb8c); // ???

	// input size
	writel(filter->mb_w << 16, filter->ve_regs + VE_ISP_INPUT_STRIDE);
	writel((filter->mb_w << 16) | (filter->mb_h << 0), filter->ve_regs + VE_ISP_INPUT_SIZE);

	// input buffer
	writel(ve_virt2phys(filter->input_buf), filter->ve_regs + VE_ISP_INPUT_LUMA);
	writel(ve_virt2phys(filter->input_buf) + filter->plane_size, filter->ve_regs + VE_ISP_INPUT_CHROMA);

	put_start_code(filter->ve_regs);
	put_aud(filter->ve_regs);
	put_rbsp_trailing_bits(filter->ve_regs);

	// reference output
	writel(ve_virt2phys(filter->reconstruct_buf), filter->ve_regs + VE_AVC_REC_LUMA);
	writel(ve_virt2phys(filter->reconstruct_buf) + filter->tile_w * filter->tile_h, filter->ve_regs + VE_AVC_REC_CHROMA);
	writel(ve_virt2phys(filter->small_luma_buf), filter->ve_regs + VE_AVC_REC_SLUMA);
	writel(ve_virt2phys(filter->mb_info_buf), filter->ve_regs + VE_AVC_MB_INFO);

	if (GST_BUFFER_OFFSET(buf) == 0)
	{
		// TODO: put sps/pps at regular interval
		put_start_code(filter->ve_regs);
		put_seq_parameter_set(filter->ve_regs, filter->mb_w, filter->mb_h);
		put_rbsp_trailing_bits(filter->ve_regs);

		put_start_code(filter->ve_regs);
		put_pic_parameter_set(filter->ve_regs);
		put_rbsp_trailing_bits(filter->ve_regs);
	}

	put_start_code(filter->ve_regs);
	put_slice_header(filter->ve_regs);

	writel(readl(filter->ve_regs + VE_AVC_CTRL) | 0xf, filter->ve_regs + VE_AVC_CTRL);
	writel(readl(filter->ve_regs + VE_AVC_STATUS) | 0x7, filter->ve_regs + VE_AVC_STATUS);

	// parameters
	writel(0x00000100, filter->ve_regs + VE_AVC_PARAM);
	writel(0x00041e1e, filter->ve_regs + VE_AVC_QP);
	writel(0x00000104, filter->ve_regs + VE_AVC_MOTION_EST);

	writel(0x8, filter->ve_regs + VE_AVC_TRIGGER);
	ve_wait(1);

	writel(readl(filter->ve_regs + VE_AVC_STATUS), filter->ve_regs + VE_AVC_STATUS);

	// TODO: use gst_pad_alloc_buffer
	outbuf = gst_buffer_new_and_alloc(readl(filter->ve_regs + VE_AVC_VLE_LENGTH) / 8);
	gst_buffer_set_caps(outbuf, GST_PAD_CAPS(filter->srcpad));
	memcpy(GST_BUFFER_DATA(outbuf), filter->output_buf, GST_BUFFER_SIZE(outbuf));
	GST_BUFFER_TIMESTAMP(outbuf) = GST_BUFFER_TIMESTAMP(buf);
	
	gst_buffer_unref(buf);
	return gst_pad_push (filter->srcpad, outbuf);
}

static GstStateChangeReturn
	gst_cedarh264enc_change_state (GstElement *element, GstStateChange transition)
{
	GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
	Gstcedarh264enc *cedarelement = GST_CEDAR_H264ENC(element);
	
	switch(transition) {
		case GST_STATE_CHANGE_NULL_TO_READY:
			if (!ve_open()) {
				GST_ERROR("Cannot open VE");
				return GST_STATE_CHANGE_FAILURE;
			}
			
			cedarelement->ve_regs = ve_get_regs();

			if (!cedarelement->ve_regs) {
				GST_ERROR("Cannot get VE regs");
				ve_close();
				return GST_STATE_CHANGE_FAILURE;
			}
			
			break;
		case GST_STATE_CHANGE_READY_TO_PAUSED:
			break;
		case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
			break;
		default:
			// silence compiler warning...
			break;
	}
	
	ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
	if (ret == GST_STATE_CHANGE_FAILURE)
		return ret;

	switch (transition) {
		case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
			break;
		case GST_STATE_CHANGE_PAUSED_TO_READY:
			if (cedarelement->mb_info_buf) {
				ve_free(cedarelement->mb_info_buf);
				cedarelement->mb_info_buf = NULL;
			}
			
			if (cedarelement->small_luma_buf) {
				ve_free(cedarelement->small_luma_buf);
				cedarelement->small_luma_buf = NULL;
			}
			
			if (cedarelement->reconstruct_buf) {
				ve_free(cedarelement->reconstruct_buf);
				cedarelement->reconstruct_buf = NULL;
			}
			
			if (cedarelement->input_buf) {
				ve_free(cedarelement->input_buf);
				cedarelement->input_buf = NULL;
			}
			
			if (cedarelement->output_buf) {
				ve_free(cedarelement->output_buf);
				cedarelement->output_buf = NULL;
			}
			writel(0x00130007, cedarelement->ve_regs + VE_CTRL);
			
			break;
		case GST_STATE_CHANGE_READY_TO_NULL:
			cedarelement->width = cedarelement->height = 0;
			cedarelement->tile_w = cedarelement->tile_w2 = cedarelement->tile_h = cedarelement->tile_h2 = 0;
			cedarelement->mb_w = cedarelement->mb_h = cedarelement->plane_size = 0;
			cedarelement->ve_regs = NULL;
			ve_close();
			break;
		default:
			// silence compiler warning...
			break;
	}
	
	return ret;
}

/* entry point to initialize the plug-in */
static gboolean
cedar_h264enc_init (GstPlugin * cedar_h264enc)
{
  // debug category for fltering log messages
  GST_DEBUG_CATEGORY_INIT (gst_cedarh264enc_debug, "cedar_h264enc",
      0, "CedarX H264 Encoder");

  return gst_element_register (cedar_h264enc, "cedar_h264enc", GST_RANK_NONE,
      GST_TYPE_CEDAR_H264ENC);
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "myfirstcedar_h264enc"
#endif

// gstreamer looks for this structure to register cedar_h264encs
GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "cedar_h264enc",
    "CedarX H264 Encoder",
    cedar_h264enc_init,
    VERSION,
    "LGPL",
    "Sunxi",
    "http://github.com/ebutera/gst-plugin-cedar"
)
