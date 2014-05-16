/*
 * Copyright (c) 2014, Freescale Semiconductor, Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-vpuenc
 *
 * VPU based video encoder.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v videotestsrc num-buffers=1000 ! vpuenc ! qtmux ! filesink location=videotestsrc.mp4
 * ]| This example pipeline will encode a test video source to AVC muxed in an
 * MP4 container.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <string.h>

#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include "gstimxcommon.h"
#include "gstvpuallocator.h"
#include "gstvpuenc.h"

#define DEFAULT_BITRATE 0
#define DEFAULT_GOP_SIZE 15
#define DEFAULT_QUANT -1
#define DEFAULT_H264_QUANT 35
#define DEFAULT_MPEG4_QUANT 15

enum
{
  PROP_0,
  PROP_BITRATE,
  PROP_GOP_SIZE,
  PROP_QUANT,
};

static GstStaticPadTemplate static_sink_template = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(
		"video/x-raw,"
		"format = (string) { NV12, I420, YV12 }, "
		"width = (int) [ 48, 1920, 8 ], "
		"height = (int) [ 32, 1080, 8 ], "
		"framerate = (fraction) [ 0, MAX ]"
	)
);

static GstStaticPadTemplate static_src_template = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(
		"video/x-h264, "
		"stream-format = (string) { avc, byte-stream }, "
		"alignment = (string) { au, nal }; "

		"video/mpeg, "
		"mpegversion = (int) 4," 
		"systemstream = (boolean) false, "
		"width = (int) [ 48, 1920, 8 ], "
		"height = (int) [ 32, 1080, 8 ], "
		"framerate = (fraction) [ 0, MAX ]; "

		"video/x-h263, "
		"variant = (string) itu, "
		"width = (int) [ 48, 1920, 8 ], "
		"height = (int) [ 32, 1080, 8 ], "
		"framerate = (fraction) [ 0, MAX ]; "

		"image/jpeg; "
	)
);

GST_DEBUG_CATEGORY_STATIC (vpu_enc_debug);
#define GST_CAT_DEFAULT vpu_enc_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_PERFORMANCE);

static void gst_vpu_enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_vpu_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static gboolean gst_vpu_enc_open (GstVideoEncoder * benc);
static gboolean gst_vpu_enc_close (GstVideoEncoder * benc);
static gboolean gst_vpu_enc_start (GstVideoEncoder * benc);
static gboolean gst_vpu_enc_stop (GstVideoEncoder * benc);
static gboolean gst_vpu_enc_set_format (GstVideoEncoder * benc,
    GstVideoCodecState * state);
static GstFlowReturn gst_vpu_enc_handle_frame (GstVideoEncoder * benc,
    GstVideoCodecFrame * frame);
static gboolean gst_vpu_enc_propose_allocation (GstVideoEncoder * benc,
    GstQuery * query);

#define gst_vpu_enc_parent_class parent_class
G_DEFINE_TYPE (GstVpuEnc, gst_vpu_enc, GST_TYPE_VIDEO_ENCODER);

static void
gst_vpu_enc_class_init (GstVpuEncClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstVideoEncoderClass *venc_class;

  gobject_class = (GObjectClass *) klass;
  element_class = (GstElementClass *) klass;
  venc_class = (GstVideoEncoderClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_vpu_enc_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_vpu_enc_get_property);

  g_object_class_install_property (gobject_class, PROP_BITRATE,
      g_param_spec_uint ("bitrate", "bit rate",
        "set bit rate in bps (0 for automatic)",
        0, G_MAXINT, DEFAULT_BITRATE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_GOP_SIZE,
      g_param_spec_uint ("gop-size", "Group-of-picture size",
        "How many frames a group-of-picture shall contain",
        0, 32767, DEFAULT_GOP_SIZE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_QUANT,
      g_param_spec_int ("quant", "quant",
        "set quant value: H.264(0-51), Mpeg4/H.263(1-31) (-1 for automatic)", 
        -1, 51, DEFAULT_QUANT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&static_sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&static_src_template));
  gst_element_class_set_static_metadata (element_class,
      "VPU-based video encoder", "Codec/Encoder/Video",
      "Encode raw data to compressed video",
      IMX_GST_PLUGIN_AUTHOR);

  venc_class->open = GST_DEBUG_FUNCPTR (gst_vpu_enc_open);
  venc_class->close = GST_DEBUG_FUNCPTR (gst_vpu_enc_close);
  venc_class->start = GST_DEBUG_FUNCPTR (gst_vpu_enc_start);
  venc_class->stop = GST_DEBUG_FUNCPTR (gst_vpu_enc_stop);
  venc_class->set_format = GST_DEBUG_FUNCPTR (gst_vpu_enc_set_format);
  venc_class->handle_frame = GST_DEBUG_FUNCPTR (gst_vpu_enc_handle_frame);
  venc_class->propose_allocation = GST_DEBUG_FUNCPTR (gst_vpu_enc_propose_allocation);

  GST_DEBUG_CATEGORY_INIT (vpu_enc_debug, "vpuenc", 0, "VPU encoder");
  GST_DEBUG_CATEGORY_GET (GST_CAT_PERFORMANCE, "GST_PERFORMANCE");
}

static void
gst_vpu_enc_init (GstVpuEnc * enc)
{
  GST_DEBUG ("initializing");

  enc->bitrate = DEFAULT_BITRATE;
  enc->gop_size = DEFAULT_GOP_SIZE;
  enc->quant = DEFAULT_QUANT;
  enc->gstbuffer_in_vpuenc = NULL;
  enc->output_gst_memory = NULL;
  enc->gop_count = 0;
  enc->handle = NULL;
  enc->state = NULL;
}

static void
gst_vpu_enc_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstVpuEnc *enc;

  g_return_if_fail (GST_IS_VPU_ENC (object));
  enc = GST_VPU_ENC (object);

  switch (prop_id) {
    case PROP_BITRATE:
      g_value_set_uint (value, enc->bitrate);
      break;
    case PROP_GOP_SIZE:
      g_value_set_uint (value, enc->gop_size);
      break;
    case PROP_QUANT:
      g_value_set_int (value, enc->quant);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vpu_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVpuEnc *enc;

  g_return_if_fail (GST_IS_VPU_ENC (object));
  enc = GST_VPU_ENC (object);

  switch (prop_id) {
    case PROP_BITRATE:
      enc->bitrate = g_value_get_uint (value);
      break;
    case PROP_GOP_SIZE:
      enc->gop_size = g_value_get_uint (value);
      break;
    case PROP_QUANT:
      enc->quant = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gchar const *
gst_vpu_enc_strerror(VpuEncRetCode code)
{
  switch (code) {
    case VPU_ENC_RET_SUCCESS: return "success";
    case VPU_ENC_RET_FAILURE: return "failure";
    case VPU_ENC_RET_INVALID_PARAM: return "invalid param";
    case VPU_ENC_RET_INVALID_HANDLE: return "invalid handle";
    case VPU_ENC_RET_INVALID_FRAME_BUFFER: return "invalid frame buffer";
    case VPU_ENC_RET_INSUFFICIENT_FRAME_BUFFERS: return "insufficient frame buffers";
    case VPU_ENC_RET_INVALID_STRIDE: return "invalid stride";
    case VPU_ENC_RET_WRONG_CALL_SEQUENCE: return "wrong call sequence";
    case VPU_ENC_RET_FAILURE_TIMEOUT: return "failure timeout";
    default: return NULL;
  }
}

static gboolean
gst_vpu_enc_open (GstVideoEncoder * benc)
{
  GstVpuEnc *enc = (GstVpuEnc *) benc;
	VpuEncRetCode ret;

	ret = VPU_EncLoad();
	if (ret != VPU_ENC_RET_SUCCESS) {
		GST_ERROR_OBJECT(enc, "VPU_EncLoad fail: %s", \
                gst_vpu_enc_strerror(ret));
		return FALSE;
	}

  return TRUE;
}

static gboolean
gst_vpu_enc_close (GstVideoEncoder * benc)
{
  GstVpuEnc *enc = (GstVpuEnc *) benc;
	VpuEncRetCode ret;

	ret = VPU_EncUnLoad();
	if (ret != VPU_ENC_RET_SUCCESS) {
		GST_ERROR_OBJECT(enc, "VPU_EncUnLoad fail: %s", \
                gst_vpu_enc_strerror(ret));
		return FALSE;
	}

  return TRUE;
}

static gboolean
gst_vpu_enc_start (GstVideoEncoder * benc)
{
  GstVpuEnc *enc = (GstVpuEnc *) benc;
  VpuEncRetCode ret;
  VpuVersionInfo version;
  VpuWrapperVersionInfo wrapper_version;

  ret = VPU_EncGetVersionInfo(&version);
  if (ret != VPU_ENC_RET_SUCCESS) {
    GST_WARNING_OBJECT(enc, "VPU_EncGetVersionInfo fail: %s", \
        gst_vpu_enc_strerror(ret));
  }

  ret = VPU_EncGetWrapperVersionInfo(&wrapper_version);
  if (ret != VPU_ENC_RET_SUCCESS) {
    GST_WARNING_OBJECT(enc, "VPU_EncGetWrapperVersionInfo fail: %s", \
        gst_vpu_enc_strerror(ret));
  }

  GST_INFO_OBJECT(enc, "VPU firmware version %d.%d.%d_r%d", \
      version.nFwMajor, version.nFwMinor, version.nFwRelease, version.nFwCode);
  GST_INFO_OBJECT(enc, "VPU library version %d.%d.%d", version.nLibMajor, \
      version.nLibMinor, version.nLibRelease);
  GST_INFO_OBJECT(enc, "VPU wrapper version %d.%d.%d %s", \
      wrapper_version.nMajor, wrapper_version.nMinor, wrapper_version.nRelease, \
      wrapper_version.pBinary);

  /* mem_info contains information about how to set up memory blocks
   * the VPU uses as temporary storage (they are "work buffers") */
  memset(&(enc->vpu_internal_mem.mem_info), 0, sizeof(VpuMemInfo));
  ret = VPU_EncQueryMem(&(enc->vpu_internal_mem.mem_info));
  if (ret != VPU_ENC_RET_SUCCESS) {
    GST_ERROR_OBJECT(enc, "could not get VPU memory information: %s", \
        gst_vpu_enc_strerror(ret));
    return FALSE;
  }

  if (!gst_vpu_allocate_internal_mem (&(enc->vpu_internal_mem))) {
    GST_ERROR_OBJECT(enc, "gst_vpu_allocate_internal_mem fail");
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_vpu_enc_reset (GstVpuEnc * enc)
{
  VpuEncRetCode ret;

  if (enc->handle) {
    ret = VPU_EncClose(enc->handle);
    if (ret != VPU_ENC_RET_SUCCESS) {
      GST_ERROR_OBJECT(enc, "closing encoder failed: %s", \
          gst_vpu_enc_strerror(ret));
      return FALSE;
    }
    enc->handle = NULL;
  }

	if (enc->output_gst_memory) {
    gst_memory_unref (enc->output_gst_memory);
    enc->output_gst_memory = NULL;
  }

  if (enc->gstbuffer_in_vpuenc) {
    g_list_foreach (enc->gstbuffer_in_vpuenc, (GFunc) gst_buffer_unref, NULL);
    g_list_free (enc->gstbuffer_in_vpuenc);
    enc->gstbuffer_in_vpuenc = NULL;
  }

  if (enc->pool) {
    gst_buffer_pool_set_active (enc->pool, FALSE);
    gst_object_unref (enc->pool);
    enc->pool = NULL;
  }

  if (enc->state) {
    gst_video_codec_state_unref (enc->state);
    enc->state = NULL;
  }

  return TRUE;
}

static gboolean
gst_vpu_enc_stop (GstVideoEncoder * benc)
{
  GstVpuEnc *enc = (GstVpuEnc *) benc;

  if (!gst_vpu_enc_reset (enc)) {
    GST_ERROR_OBJECT(enc, "gst_enc_free_output_buffer fail");
    return FALSE;
  }

  if (!gst_vpu_free_internal_mem (&(enc->vpu_internal_mem))) {
    GST_ERROR_OBJECT(enc, "gst_vpu_free_internal_mem fail");
    return FALSE;
  }

  return TRUE;
}

static GstCaps *
gst_vpu_enc_decide_output_caps (GstVideoEncoder * benc)
{
  GstVpuEnc *enc = (GstVpuEnc *) benc;
  GstCaps *thiscaps;
  GstCaps *caps = NULL;
  GstCaps *peercaps = NULL;
  gboolean result = FALSE;

  /* first see what is possible on our source pad */
  thiscaps = gst_pad_query_caps (GST_VIDEO_ENCODER_SRC_PAD (enc), NULL);
  GST_DEBUG_OBJECT (enc, "caps of src: %" GST_PTR_FORMAT, thiscaps);
  /* nothing or anything is allowed, we're done */
  if (thiscaps == NULL || gst_caps_is_any (thiscaps))
    goto no_nego_needed;

  if (G_UNLIKELY (gst_caps_is_empty (thiscaps)))
    goto no_caps;

  /* get the peer caps */
  peercaps = gst_pad_peer_query_caps (GST_VIDEO_ENCODER_SRC_PAD (enc), thiscaps);
  GST_DEBUG_OBJECT (enc, "caps of peer: %" GST_PTR_FORMAT, peercaps);
  if (peercaps) {
    /* The result is already a subset of our caps */
    caps = peercaps;
    gst_caps_unref (thiscaps);
  } else {
    /* no peer, work with our own caps then */
    caps = thiscaps;
  }
  if (caps && !gst_caps_is_empty (caps)) {
    /* now fixate */
    GST_DEBUG_OBJECT (enc, "have caps: %" GST_PTR_FORMAT, caps);
    if (gst_caps_is_any (caps)) {
      GST_DEBUG_OBJECT (enc, "any caps, we stop");
      /* hmm, still anything, so element can do anything and
       * nego is not needed */
      result = TRUE;
    } else {
      caps = gst_caps_fixate (caps);
      GST_DEBUG_OBJECT (enc, "fixated to: %" GST_PTR_FORMAT, caps);
    }
  } else {
    GST_DEBUG_OBJECT (enc, "no common caps");
  }
  return caps;

no_nego_needed:
  {
    GST_DEBUG_OBJECT (enc, "no negotiation needed");
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return caps;
  }
no_caps:
  {
    GST_ELEMENT_ERROR (enc, STREAM, FORMAT,
        ("No supported formats found"),
        ("This element did not produce valid caps"));
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return caps;
  }
}

static gboolean 
gst_vpu_enc_decide_output_video_format (GstVideoEncoder * benc)
{
  GstVpuEnc *enc = (GstVpuEnc *) benc;
  GstCaps *caps = NULL;
  GstStructure *s;
  const gchar *video_format_str = NULL;

  caps = gst_vpu_enc_decide_output_caps(benc);
  if (!caps) {
    GST_ERROR_OBJECT(enc, "can't decide output caps.");
    return FALSE;
  }

  enc->open_param.eFormat = gst_vpu_find_std (caps);
  if (enc->open_param.eFormat < 0) {
    GST_ERROR_OBJECT(enc, "can't find VPU encoder output format");
    gst_caps_unref(caps);
    return FALSE;
  }

  if (enc->open_param.eFormat == VPU_V_AVC && enc->quant == -1) {
    enc->quant = DEFAULT_H264_QUANT;
  } else {
    enc->quant = DEFAULT_MPEG4_QUANT;
  }

  GST_DEBUG_OBJECT (enc, "output caps: %" GST_PTR_FORMAT, caps);
  s = gst_caps_get_structure(caps, 0);

  GST_DEBUG_OBJECT (enc, "output structure: %" GST_PTR_FORMAT, s);
  video_format_str = gst_structure_get_string(s, "stream-format");

  if (video_format_str == NULL) {
    gst_caps_unref(caps);
    return TRUE;
  }

  GST_DEBUG_OBJECT(enc, "stream-format: %s", video_format_str);
  if (!g_strcmp0(video_format_str, "avc"))
    enc->open_param.nIsAvcc = 1;

  gst_caps_unref(caps);

  return TRUE;
}

static gboolean
gst_vpu_enc_set_caps (GstVideoEncoder * benc, guint8 * codec_data, gint codec_data_len)
{
  GstVpuEnc *enc = (GstVpuEnc *) benc;
	GstVideoCodecState *output_state;
  GstCaps *out_caps;
  GstStructure *s;
  GstBuffer *gstbuf = NULL;

  if (codec_data) {
    gstbuf = gst_buffer_new_and_alloc (codec_data_len);
    gst_buffer_fill(gstbuf, 0, codec_data, codec_data_len);
    GST_DEBUG_OBJECT (enc,"set codec data for caps len=%d", codec_data_len);
  }

  out_caps = gst_vpu_enc_decide_output_caps(benc);
  s = gst_caps_get_structure (out_caps, 0);
  gst_structure_set (s, "width", G_TYPE_INT, GST_VIDEO_INFO_WIDTH(&(enc->state->info)), NULL);
  gst_structure_set (s, "height", G_TYPE_INT, GST_VIDEO_INFO_HEIGHT(&(enc->state->info)), NULL);
  gst_structure_set (s, "framerate", GST_TYPE_FRACTION, \
      GST_VIDEO_INFO_FPS_N(&(enc->state->info)), \
      GST_VIDEO_INFO_FPS_D(&(enc->state->info)), NULL);

  if (enc->open_param.eFormat == VPU_V_AVC) {
    if (enc->open_param.nIsAvcc == 1) {
      if (gstbuf != NULL) {
        gst_caps_set_simple (out_caps, "codec_data", GST_TYPE_BUFFER, gstbuf, NULL);
      }
      gst_structure_set (s, "stream-format", G_TYPE_STRING, "avc", NULL);
    } else {
      gst_structure_set (s, "stream-format", G_TYPE_STRING, "byte-stream",
          NULL);
    }
    gst_structure_set (s, "alignment", G_TYPE_STRING, "au", NULL);
  } else {
    if (gstbuf != NULL) {
      gst_caps_set_simple (out_caps, "codec_data", GST_TYPE_BUFFER, gstbuf, NULL);
    }
  }

  if (gstbuf != NULL) 
    gst_buffer_unref (gstbuf);

  GST_DEBUG_OBJECT (enc, "output caps: %" GST_PTR_FORMAT, out_caps);
	output_state = gst_video_encoder_set_output_state(benc, \
      out_caps, enc->state);
	gst_video_codec_state_unref(output_state);

  return TRUE;
}

static gboolean
gst_vpu_enc_set_format (GstVideoEncoder * benc, GstVideoCodecState * state)
{
  GstVpuEnc *enc = (GstVpuEnc *) benc;
  const gchar *video_format_str = NULL;
  GstStructure *s;
  VpuEncRetCode ret;
	
	if (!gst_vpu_enc_reset (enc)) {
		GST_ERROR_OBJECT (enc, "gst_vpu_enc_reset fail.");
		return FALSE;
  }

	memset(&(enc->open_param), 0, sizeof(VpuEncOpenParam));
  if (!gst_vpu_enc_decide_output_video_format (benc)) {
		GST_ERROR_OBJECT (enc, "gst_vpu_enc_decide_output_video_format fail.");
		return FALSE;
  }

	enc->open_param.nPicWidth = GST_VIDEO_INFO_WIDTH(&(state->info));
	enc->open_param.nPicHeight = GST_VIDEO_INFO_HEIGHT(&(state->info));
	enc->open_param.nFrameRate = (GST_VIDEO_INFO_FPS_N(&(state->info)) \
      & 0xffffUL) | (((GST_VIDEO_INFO_FPS_D(&(state->info)) - 1) & 0xffffUL) << 16);
	enc->open_param.sMirror = VPU_ENC_MIRDIR_NONE;
  enc->open_param.nBitRate = enc->bitrate;
  enc->open_param.nGOPSize = enc->gop_size;
  enc->open_param.nChromaInterleave = 0;
  enc->open_param.nMapType = 0;
  enc->open_param.nLinear2TiledEnable = 0;

  GST_DEBUG_OBJECT (enc, "input caps: %" GST_PTR_FORMAT, state->caps);
  s = gst_caps_get_structure(state->caps, 0);
  video_format_str = gst_structure_get_string(s, "format");

  if (video_format_str && !g_strcmp0(video_format_str, "NV12"))
    enc->open_param.nChromaInterleave = 1;

	GST_INFO_OBJECT(enc, "setting bitrate to %u kbps and GOP size to %u", \
      enc->open_param.nBitRate, enc->open_param.nGOPSize);

	ret = VPU_EncOpenSimp(&(enc->handle), &(enc->vpu_internal_mem.mem_info), \
      &(enc->open_param));
	if (ret != VPU_ENC_RET_SUCCESS) {
		GST_ERROR_OBJECT(enc, "opening new VPU handle failed: %s", \
        gst_vpu_enc_strerror(ret));
		return FALSE;
	}

	ret = VPU_EncConfig(enc->handle, VPU_ENC_CONF_NONE, NULL);
	if (ret != VPU_ENC_RET_SUCCESS) {
		GST_ERROR_OBJECT(enc, "could not apply default configuration: %s", \
        gst_vpu_enc_strerror(ret));
		return FALSE;
	}

	ret = VPU_EncGetInitialInfo(enc->handle, &(enc->init_info));
	if (ret != VPU_ENC_RET_SUCCESS) {
		GST_ERROR_OBJECT(enc, "retrieving init info failed: %s", \
        gst_vpu_enc_strerror(ret));
		return FALSE;
	}

	enc->state = gst_video_codec_state_ref(state);

  if (!gst_vpu_enc_set_caps(benc, NULL, 0)) {
    GST_ERROR_OBJECT(enc, "gst_vpu_enc_set_caps fail.");
    return FALSE;
  }

	return TRUE;
}

static gboolean
gst_vpu_enc_setup_internal_bufferpool (GstVpuEnc * enc)
{
  GstAllocationParams params = { 0 };
  GstAllocator *allocator = NULL;
  GstCaps *caps;
  GstStructure *config;
  guint i;

  enc->pool = gst_video_buffer_pool_new ();
  if (!enc->pool) {
    GST_ERROR_OBJECT (enc, "New video buffer pool failed.");
    return FALSE;
  }

  allocator = gst_vpu_allocator_obtain();
  if (!allocator) {
    GST_ERROR_OBJECT (enc, "New VPU allocator failed.");
    return FALSE;
  }

  params.align = enc->init_info.nAddressAlignment;
  memset(&(enc->video_align), 0, sizeof(GstVideoAlignment));
  if (enc->open_param.nPicWidth % DEFAULT_FRAME_BUFFER_ALIGNMENT_H)
    enc->video_align.padding_right = DEFAULT_FRAME_BUFFER_ALIGNMENT_H \
      - enc->open_param.nPicWidth % DEFAULT_FRAME_BUFFER_ALIGNMENT_H;
  if (enc->open_param.nPicHeight % DEFAULT_FRAME_BUFFER_ALIGNMENT_V)
    enc->video_align.padding_bottom = DEFAULT_FRAME_BUFFER_ALIGNMENT_V\
      - enc->open_param.nPicHeight % DEFAULT_FRAME_BUFFER_ALIGNMENT_V;

  for (i = 0; i < GST_VIDEO_MAX_PLANES; i++)
    enc->video_align.stride_align[i] = DEFAULT_FRAME_BUFFER_ALIGNMENT_H - 1;

  config = gst_buffer_pool_get_config(enc->pool);
  gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
  gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  caps = gst_video_info_to_caps (&(enc->state->info));
  gst_buffer_pool_config_set_params(config, caps, enc->state->info.size, 2, 0);
  gst_buffer_pool_config_set_video_alignment (config, &enc->video_align);
  gst_buffer_pool_config_set_allocator(config, allocator, &params);
  gst_buffer_pool_set_config(enc->pool, config);

  if (allocator)
    gst_object_unref (allocator);

  gst_caps_unref (caps);
  if (gst_buffer_pool_set_active (enc->pool, TRUE) != TRUE) {
    GST_ERROR_OBJECT (enc, "active pool(%p) failed.", enc->pool);
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_vpu_enc_allocate_physical_mem (GstVpuEnc * enc, gint src_stride)
{
  VpuFrameBuffer *vpuframebuffers = NULL;
	VpuEncRetCode ret;
  GstBuffer * buffer;

	if (enc->gstbuffer_in_vpuenc == NULL) {
    if (enc->pool == NULL) {
      if (!gst_vpu_enc_setup_internal_bufferpool (enc)) {
        GST_ERROR_OBJECT (enc, "gst_vpu_enc_setup_internal_bufferpool failed.");
        return FALSE;
      }
    }

    while (g_list_length (enc->gstbuffer_in_vpuenc) \
        < enc->init_info.nMinFrameBufferCount) {
      gst_buffer_pool_acquire_buffer (enc->pool, &buffer, NULL);
      if (!buffer) {
        GST_ERROR_OBJECT (enc, "acquire buffer from pool(%p) failed.", \
            enc->pool);
        return FALSE;
      }

      enc->gstbuffer_in_vpuenc = g_list_append ( \
          enc->gstbuffer_in_vpuenc, buffer);
    }

    vpuframebuffers = (VpuFrameBuffer *)g_malloc ( \
        sizeof (VpuFrameBuffer) * enc->init_info.nMinFrameBufferCount);
    if (vpuframebuffers == NULL) {
      GST_ERROR_OBJECT (enc, "Could not allocate memory");
      return FALSE;
    }
    memset (vpuframebuffers, 0, sizeof (VpuFrameBuffer) \
        * enc->init_info.nMinFrameBufferCount);

    if (!gst_vpu_register_frame_buffer (enc->gstbuffer_in_vpuenc, \
          &enc->state->info, vpuframebuffers)) {
      GST_ERROR_OBJECT (enc, "gst_vpu_register_frame_buffer fail.");
      g_free(vpuframebuffers);
      return FALSE;
    }

    ret = VPU_EncRegisterFrameBuffer (enc->handle, \
        vpuframebuffers, enc->init_info.nMinFrameBufferCount, src_stride);
    if (ret != VPU_ENC_RET_SUCCESS) {
      GST_ERROR_OBJECT(enc, "registering framebuffers failed: %s", \
          gst_vpu_enc_strerror(ret));
      g_free(vpuframebuffers);
      return FALSE;
    }

    g_free(vpuframebuffers);
  }

  if (enc->output_gst_memory == NULL) {
    enc->output_gst_memory = gst_allocator_alloc( \
        gst_vpu_allocator_obtain(), enc->state->info.size, NULL);
    enc->output_phys_buffer = gst_memory_query_phymem_block (enc->output_gst_memory);
		if (enc->output_phys_buffer == NULL) {
			GST_ERROR_OBJECT(enc, "could not allocate physical buffer for output data");
			return FALSE;
		}
	}

	return TRUE;
}

static GstFlowReturn
gst_vpu_enc_handle_frame (GstVideoEncoder * benc, GstVideoCodecFrame * frame)
{
  GstVpuEnc *enc = (GstVpuEnc *) benc;
  GstFlowReturn ret = GST_FLOW_OK;
	VpuEncRetCode enc_ret;
	VpuEncEncParam enc_enc_param;
	VpuFrameBuffer input_framebuf;
	GstBuffer *input_buffer;
	GstBuffer *pool_buffer = NULL;
	gint src_stride;

	memset(&enc_enc_param, 0, sizeof(enc_enc_param));
	memset(&input_framebuf, 0, sizeof(input_framebuf));

  if (!gst_buffer_is_phymem (frame->input_buffer)) {
    GstVideoInfo info = enc->state->info;
    GstVideoFrame frame1, frame2;

    GST_DEBUG_OBJECT(enc, "not physical continues memory. allocate internal memory pool.");
    if (enc->pool == NULL) {
      if (!gst_vpu_enc_setup_internal_bufferpool (enc)) {
        GST_ERROR_OBJECT (enc, "acquire buffer from pool(%p) failed.", enc->pool);
        return GST_FLOW_ERROR;
      }
    }

    gst_buffer_pool_acquire_buffer (enc->pool, &pool_buffer, NULL);
    if (!pool_buffer) {
      GST_ERROR_OBJECT (enc, "acquire buffer from pool(%p) failed.", enc->pool);
      return GST_FLOW_ERROR;
    }

    gst_video_frame_map (&frame1, &info, pool_buffer, GST_MAP_WRITE);
    gst_video_frame_map (&frame2, &info, frame->input_buffer, GST_MAP_READ);

    gst_video_frame_copy (&frame1, &frame2);

    gst_video_frame_unmap (&frame1);
    gst_video_frame_unmap (&frame2);

    input_buffer = pool_buffer;
	} else {
    GST_DEBUG_OBJECT(enc, "is physical continues memory.");
    input_buffer = frame->input_buffer;
	}

	/* Set up physical addresses for the input framebuffer */
	{
		gsize *plane_offsets;
		gint *plane_strides;
		GstVideoMeta *video_meta;
    PhyMemBlock *input_phys_buffer;
    unsigned char *phys_ptr;

		/* Try to use plane offset and stride information from the video
		 * metadata if present, since these can be more accurate than
		 * the information from the video info */
		video_meta = gst_buffer_get_video_meta(input_buffer);
		if (video_meta != NULL) {
			plane_offsets = video_meta->offset;
			plane_strides = video_meta->stride;
		} else {
			plane_offsets = enc->state->info.offset;
			plane_strides = enc->state->info.stride;
		}

    input_phys_buffer = gst_buffer_query_phymem_block (input_buffer);
		if (input_phys_buffer == NULL) {
      GST_ERROR_OBJECT(enc, "could not get physical address from input buffer.");
      if (pool_buffer)
        gst_buffer_unref (pool_buffer);
      return GST_FLOW_ERROR;
    }

		phys_ptr = (unsigned char*)(input_phys_buffer->paddr);

		input_framebuf.pbufY = phys_ptr;
		input_framebuf.pbufCb = phys_ptr + plane_offsets[1];
		input_framebuf.pbufCr = phys_ptr + plane_offsets[2];
		input_framebuf.pbufMvCol = NULL; /* not used by the VPU encoder */
		input_framebuf.nStrideY = plane_strides[0];
		input_framebuf.nStrideC = plane_strides[1];

		/* this is needed for framebuffers registration below */
		src_stride = plane_strides[0];

		GST_TRACE_OBJECT(enc, "width: %d   height: %d   stride 0: %d   stride 1: %d   offset 0: %d   offset 1: %d   offset 2: %d", GST_VIDEO_INFO_WIDTH(&(enc->state->info)), GST_VIDEO_INFO_HEIGHT(&(enc->state->info)), plane_strides[0], plane_strides[1], plane_offsets[0], plane_offsets[1], plane_offsets[2]);
	}

  // Allocate needed physical buffer.
  if (!gst_vpu_enc_allocate_physical_mem (enc, src_stride)) {
			GST_ERROR_OBJECT(enc, "gst_vpu_enc_allocate_physical_mem failed.");
      if (pool_buffer)
        gst_buffer_unref (pool_buffer);
			return GST_FLOW_ERROR;
  }

	/* Set up encoding parameters */
	enc_enc_param.nInVirtOutput = (unsigned int)(enc->output_phys_buffer->vaddr);
	enc_enc_param.nInPhyOutput = (unsigned int)(enc->output_phys_buffer->paddr);
	enc_enc_param.nInOutputBufLen = enc->output_phys_buffer->size;
	enc_enc_param.nPicWidth = enc->open_param.nPicWidth;
	enc_enc_param.nPicHeight = enc->open_param.nPicHeight;
	enc_enc_param.nFrameRate = enc->open_param.nFrameRate;
	enc_enc_param.pInFrame = &input_framebuf;
	enc_enc_param.eFormat = enc->open_param.eFormat;
	enc_enc_param.nQuantParam = enc->quant;
	enc_enc_param.nForceIPicture = 0;

	if (GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME(frame) \
      || GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME_HEADERS(frame) \
      || !(enc->gop_count % enc->gop_size)) {
		enc_enc_param.nForceIPicture = 1;
		GST_LOG_OBJECT(enc, "got request to make this a keyframe - forcing I frame");
	}

	{
		GstBuffer *output_buffer = NULL;
		gsize output_buffer_offset = 0;
		gboolean frame_finished = FALSE;

		frame->output_buffer = NULL;

		do
		{
			/* Feed input data */
			enc_ret = VPU_EncEncodeFrame(enc->handle, &enc_enc_param);
			if (enc_ret != VPU_ENC_RET_SUCCESS) {
				GST_ERROR_OBJECT(enc, "failed to encode frame: %s", gst_vpu_enc_strerror(enc_ret));
				VPU_EncReset(enc->handle);
        if (pool_buffer)
          gst_buffer_unref (pool_buffer);
        return GST_FLOW_ERROR;
			}

      if (enc_enc_param.eOutRetCode & VPU_ENC_OUTPUT_SEQHEADER) {
        if (!gst_vpu_enc_set_caps(benc, enc->output_phys_buffer->vaddr, \
            enc_enc_param.nOutOutputSize)) {
          GST_ERROR_OBJECT(enc, "gst_vpu_enc_set_caps fail.");
          if (pool_buffer)
            gst_buffer_unref (pool_buffer);
          return GST_FLOW_ERROR;
        }
        continue;
      }

      if (enc_enc_param.eOutRetCode & VPU_ENC_OUTPUT_DIS) {
        if (output_buffer == NULL) {
          output_buffer = gst_video_encoder_allocate_output_buffer(benc,
              enc->output_phys_buffer->size);
          if (output_buffer == NULL) {
            GST_ERROR_OBJECT(enc, "can't get output buffer from video encoder.");
            if (pool_buffer)
              gst_buffer_unref (pool_buffer);
            return GST_FLOW_ERROR;
          }
          frame->output_buffer = output_buffer;
        }

        GST_LOG_OBJECT(enc, "processing output data: %u bytes, output buffer offset %u", \
            enc_enc_param.nOutOutputSize, output_buffer_offset);

        if (!(enc->gop_count % enc->gop_size)) { //frame_type == IDR) {
          GST_LOG_OBJECT(enc, "setting sync point");
          GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT(frame);
        }
        enc->gop_count ++;
        gst_buffer_fill(output_buffer, output_buffer_offset,\
            enc->output_phys_buffer->vaddr, \
            enc_enc_param.nOutOutputSize);
        output_buffer_offset += enc_enc_param.nOutOutputSize;

        gst_buffer_set_size(output_buffer, output_buffer_offset);
        frame->dts = frame->pts;
        gst_video_encoder_finish_frame(benc, frame);
        output_buffer = NULL;
        frame_finished = TRUE;

        if (!(enc_enc_param.eOutRetCode & VPU_ENC_INPUT_USED))
          GST_WARNING_OBJECT(enc, "frame finished, but VPU did not report the input as used");
        break;
      }
		}
		while (!(enc_enc_param.eOutRetCode & VPU_ENC_INPUT_USED));

      if (pool_buffer)
        gst_buffer_unref (pool_buffer);

		/* If output_buffer is NULL at this point, it means VPU_ENC_OUTPUT_DIS was never communicated
		 * by the VPU, and the buffer is unfinished. -> Drop it. */
		if (output_buffer != NULL) {
			GST_WARNING_OBJECT(enc, "frame unfinished ; dropping");
			gst_buffer_unref(output_buffer);
			frame->output_buffer = NULL; /* necessary to make finish_frame() drop the frame */
			gst_video_encoder_finish_frame(benc, frame);
		}
	}

  return ret;
}

static gboolean
gst_vpu_enc_propose_allocation (GstVideoEncoder * benc, GstQuery * query)
{
  GstVpuEnc *enc = (GstVpuEnc *) benc;
  GstCaps *caps;
  GstVideoInfo info;
  GstBufferPool *pool;
  guint size;

  gst_query_parse_allocation (query, &caps, NULL);

  if (caps == NULL)
    return FALSE;

  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;

  size = GST_VIDEO_INFO_SIZE (&info);

  if (gst_query_get_n_allocation_pools (query) == 0) {
    GstStructure *structure;
    GstAllocator *allocator = NULL;
    GstAllocationParams params = { 0 };

    allocator = gst_vpu_allocator_obtain();

    pool = gst_video_buffer_pool_new ();

    params.align = enc->init_info.nAddressAlignment;
    structure = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (structure, caps, size, 0, 0);
    gst_buffer_pool_config_set_allocator (structure, allocator, &params);

    if (!gst_buffer_pool_set_config (pool, structure)) {
      GST_ERROR_OBJECT (enc, "failed to set config");
      gst_object_unref (allocator);
      gst_object_unref (pool);
      return FALSE;
    }

    gst_query_add_allocation_pool (query, pool, size, 3, 0);
    gst_query_add_allocation_param (query, allocator, NULL);
    gst_object_unref (allocator);
    gst_object_unref (pool);
    gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  }

  return TRUE;
}
