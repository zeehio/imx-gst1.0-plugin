/*
 * Copyright (c) 2013-2014, Freescale Semiconductor, Inc. All rights reserved.
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
 #include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>

#include <linux/fb.h>
#include <linux/mxcfb.h>
#include <linux/ipu.h>
#include <linux/mxc_v4l2.h>

#include <gst/video/gstvideosink.h>

#include "gstimxcommon.h"
#include "gstimxv4l2.h"

//used in imx v4l2 core debug
GST_DEBUG_CATEGORY (imxv4l2_debug);
#define GST_CAT_DEFAULT imxv4l2_debug

#define V4L2_HOLDED_BUFFERS (2)
#define MX6Q_STREAMON_COUNT (1)
#define MX60_STREAMON_COUNT (1)

#define MAX_BUFFER (32)
#define UPALIGNTO8(a) ((a + 7) & (~7))
#define DOWNALIGNTO8(a) ((a) & (~7))

#define INVISIBLE_IN (0x1)
#define INVISIBLE_OUT (0x2)

#define MXC_V4L2_CAPTURE_NAME "mxc_v4l2"
#define MXC_V4L2_CAPTURE_CAMERA_NAME "ov"
#define MXC_V4L2_CAPTURE_TVIN_NAME "adv"
#define PXP_V4L2_CAPTURE_NAME "csi_v4l2"

typedef struct {
  struct v4l2_buffer v4l2buffer;
  PhyMemBlock *v4l2memblk;
  GstBuffer *gstbuffer;
} IMXV4l2BufferPair;

typedef gint (*V4l2outConfigInput) (void *handle, guint fmt, guint w, guint h, \
    IMXV4l2Rect *crop);
typedef gint (*V4l2outConfigOutput) (void *handle, struct v4l2_crop *crop);
typedef gint (*V4l2outConfigRotate) (void *handle, gint rotate);
typedef gint (*V4l2outConfigAlpha) (void *handle, guint alpha);
typedef gint (*V4l2outConfigColorkey) (void *handle, gboolean enable, guint color_key);
typedef gint (*V4l2captureConfig) (void *handle, guint fmt, guint w, guint h, \
    guint fps_n, guint fps_d);
static GstCaps *gst_imx_v4l2capture_get_device_caps ();

typedef struct {
  V4l2outConfigInput v4l2out_config_input;
  V4l2outConfigOutput v4l2out_config_output;
  V4l2outConfigRotate v4l2out_config_rotate;
  V4l2outConfigAlpha v4l2out_config_alpha;
  V4l2outConfigColorkey v4l2out_config_colorkey;
  V4l2captureConfig v4l2capture_config;
} IMXV4l2DeviceItf;

typedef struct {
  gchar *device;
  gint type;
  int v4l2_fd;
  gint disp_w;
  gint disp_h;
  gint device_map_id;
  gboolean streamon;
  gint invisible;
  gint streamon_count;
  gint queued_count;
  guint in_fmt;
  gint in_w;
  gint in_h;
  IMXV4l2Rect in_crop;
  gboolean do_deinterlace;
  gint buffer_count;
  gint allocated;
  IMXV4l2BufferPair buffer_pair[MAX_BUFFER];
  gint rotate;
  guint *support_format_table;
  IMXV4l2DeviceItf dev_itf;
  struct v4l2_buffer * v4lbuf_queued_before_streamon[MAX_BUFFER];
  gboolean prev_need_crop;
  guint alpha;
  guint color_key;
} IMXV4l2Handle;

typedef struct {
  const gchar * caps_str;
  guint v4l2fmt;
  GstVideoFormat gstfmt;
  guint bits_per_pixel;
  guint flags;
} IMXV4l2FmtMap;

typedef struct {
  const gchar *name;
  gboolean bg;
  const gchar *bg_fb_name;
} IMXV4l2DeviceMap;

static IMXV4l2FmtMap g_imxv4l2fmt_maps[] = {
  {GST_VIDEO_CAPS_MAKE("I420"), V4L2_PIX_FMT_YUV420, GST_VIDEO_FORMAT_I420, 12, 0},
  {GST_VIDEO_CAPS_MAKE("YV12"), V4L2_PIX_FMT_YVU420, GST_VIDEO_FORMAT_YV12, 12, 0},
  {GST_VIDEO_CAPS_MAKE("NV12"), V4L2_PIX_FMT_NV12, GST_VIDEO_FORMAT_NV12, 12, 0},
  {GST_VIDEO_CAPS_MAKE("Y42B"), V4L2_PIX_FMT_YUV422P, GST_VIDEO_FORMAT_Y42B, 16, 0},
  {GST_VIDEO_CAPS_MAKE("Y444"), IPU_PIX_FMT_YUV444P, GST_VIDEO_FORMAT_Y444, 24, 0},
  {GST_VIDEO_CAPS_MAKE("TNVP"), IPU_PIX_FMT_TILED_NV12, GST_VIDEO_FORMAT_UNKNOWN, 12, 0},
  {GST_VIDEO_CAPS_MAKE("TNVF"), IPU_PIX_FMT_TILED_NV12F, GST_VIDEO_FORMAT_UNKNOWN, 12, 0},
  {GST_VIDEO_CAPS_MAKE("UYVY"), V4L2_PIX_FMT_UYVY, GST_VIDEO_FORMAT_UYVY, 16, 0},
  {GST_VIDEO_CAPS_MAKE("YUY2"), V4L2_PIX_FMT_YUYV, GST_VIDEO_FORMAT_YUY2, 16, 0},
  {GST_VIDEO_CAPS_MAKE("RGBx"), V4L2_PIX_FMT_RGB32, GST_VIDEO_FORMAT_RGBx, 32, 0},
  {GST_VIDEO_CAPS_MAKE("BGRx"), V4L2_PIX_FMT_BGR32, GST_VIDEO_FORMAT_BGRx, 32, 0},
  {GST_VIDEO_CAPS_MAKE("RGB"), V4L2_PIX_FMT_RGB24, GST_VIDEO_FORMAT_RGB, 24, 0},
  {GST_VIDEO_CAPS_MAKE("BGR"), V4L2_PIX_FMT_BGR24, GST_VIDEO_FORMAT_RGB, 24, 0},
  {GST_VIDEO_CAPS_MAKE("RGB16"), V4L2_PIX_FMT_RGB565, GST_VIDEO_FORMAT_RGB16, 16, 0},
};

static guint g_camera_format[] = {
  V4L2_PIX_FMT_YUV420,
  V4L2_PIX_FMT_NV12,
  V4L2_PIX_FMT_YUYV,
  V4L2_PIX_FMT_UYVY,
  0,
};

static guint g_camera_format_IPU[] = {
  V4L2_PIX_FMT_YUV420,
  V4L2_PIX_FMT_NV12,
  V4L2_PIX_FMT_YUYV,
  V4L2_PIX_FMT_UYVY,
  0,
};

static guint g_camera_format_PXP[] = {
  V4L2_PIX_FMT_YUYV,
  V4L2_PIX_FMT_UYVY,
  0,
};

static IMXV4l2DeviceMap g_device_maps[] = {
  {"/dev/video0", FALSE, "/dev/fb0"},
  {"/dev/video16", TRUE, "/dev/fb0"},
  {"/dev/video17", FALSE, "/dev/fb0"},
  {"/dev/video18", TRUE, "/dev/fb2"},
  {"/dev/video19", FALSE, "/dev/fb2"},
  {"/dev/video20", TRUE, "/dev/fb4"}
};


//ipu device iterfaces
static gint
imx_ipu_v4l2out_config_input (IMXV4l2Handle *handle, guint fmt, guint w, guint h, IMXV4l2Rect *crop)
{
  struct v4l2_format v4l2fmt;
  struct v4l2_rect icrop;

  if (gst_imx_v4l2_reset_device ((gpointer)handle) < 0)
    return -1;

  GST_DEBUG ("config in, fmt(%x), res(%dx%d), crop((%d,%d) -> (%d,%d))",
      fmt, w, h, crop->left, crop->top, crop->width, crop->height);

  //align to 8 pixel for IPU limitation
  crop->left = UPALIGNTO8 (crop->left);
  crop->top = UPALIGNTO8 (crop->top);
  crop->width = DOWNALIGNTO8 (crop->width); 
  crop->height = DOWNALIGNTO8 (crop->height);

  if (crop->width <=0 || crop->height <= 0) {
    return 1;
  }

  memset(&v4l2fmt, 0, sizeof(struct v4l2_format));
  v4l2fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  v4l2fmt.fmt.pix.width = handle->in_w = w;
  v4l2fmt.fmt.pix.height = handle->in_h = h;
  v4l2fmt.fmt.pix.pixelformat = handle->in_fmt = fmt;
  icrop.left = crop->left;
  icrop.top = crop->top;
  icrop.width = crop->width;
  icrop.height = crop->height;
  v4l2fmt.fmt.pix.priv = (unsigned int)&icrop;

  if (ioctl(handle->v4l2_fd, VIDIOC_S_FMT, &v4l2fmt) < 0) {
    GST_ERROR ("Set format failed.");
    return -1;
  }

  if (ioctl(handle->v4l2_fd, VIDIOC_G_FMT, &v4l2fmt) < 0) {
    GST_ERROR ("Get format failed.");
    return -1;
  }

  return 0;
}

static gint
imx_ipu_v4l2out_config_output(IMXV4l2Handle *handle, struct v4l2_crop *crop)
{
  struct v4l2_cropcap cropcap;

  memset(&cropcap, 0, sizeof(cropcap));
  cropcap.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  if (ioctl(handle->v4l2_fd, VIDIOC_CROPCAP, &cropcap) < 0) {
    GST_ERROR ("Get crop capability failed.");
    return -1;
  }

  crop->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  if (ioctl(handle->v4l2_fd, VIDIOC_S_CROP, crop) < 0) {
    GST_ERROR ("Set crop failed.");
    return -1;
  }

  return 0;
}


static gint
imx_ipu_v4l2out_config_rotate(IMXV4l2Handle *handle, gint rotate)
{
  struct v4l2_control ctrl;

  ctrl.id = V4L2_CID_ROTATE;
  ctrl.value = rotate;
  if (ioctl(handle->v4l2_fd, VIDIOC_S_CTRL, &ctrl) < 0) {
    GST_ERROR ("Set ctrl rotate failed.");
    return -1;
  }

  return 0;
}

static void
imx_ipu_v4l2_config_alpha (IMXV4l2Handle *handle, guint alpha)
{
  struct mxcfb_gbl_alpha galpha;
  char *device = (char*) g_device_maps[handle->device_map_id].bg_fb_name;
  int fd;

  fd = open (device, O_RDWR, 0);
  if (fd < 0) {
    GST_ERROR ("Can't open %s.\n", device);
    return;
  }

  GST_DEBUG ("set alpha to (%d) for display (%s)", alpha, device);

  galpha.alpha = alpha;
  galpha.enable = 1;
  if (ioctl(fd, MXCFB_SET_GBL_ALPHA, &galpha) < 0) {
    GST_ERROR ("Set %s global alpha failed.", alpha);
  }

  close (fd);

  return;
}

static void
imx_ipu_v4l2_config_colorkey (IMXV4l2Handle *handle, gboolean enable, guint color_key)
{
  struct mxcfb_color_key colorKey;
  char *device = (char*)g_device_maps[handle->device_map_id].bg_fb_name;
  int fd;

  fd = open (device, O_RDWR, 0);
  if (fd < 0) {
    GST_ERROR ("Can't open %s.\n", device);
    return;
  }

  if (enable) {
    colorKey.color_key = color_key;
    colorKey.enable = 1; 
    GST_DEBUG ("set colorKey to (%x) for display (%s)", color_key, device);
  }
  else {
    colorKey.enable = 0; 
    GST_DEBUG ("disable colorKey for display (%s)", device);
  }

  if (ioctl (fd, MXCFB_SET_CLR_KEY, &colorKey) < 0) {
    GST_ERROR ("Set %s color key failed.", device);
  }

  close (fd);

  return;
}


//pxp device iterfaces
static gint
imx_pxp_v4l2out_config_input (IMXV4l2Handle *handle, guint fmt, guint w, guint h, IMXV4l2Rect *crop)
{
  struct v4l2_format v4l2fmt;
  struct v4l2_rect icrop;
  int out_idx = 1;

  if (gst_imx_v4l2_reset_device ((gpointer)handle) < 0)
    return -1;

  GST_DEBUG ("config in, fmt(%x), res(%dx%d), crop((%d,%d) -> (%d,%d))",
      fmt, w, h, crop->left, crop->top, crop->width, crop->height);

  if (crop->width <=0 || crop->height <= 0) {
    return 1;
  }

  if (ioctl(handle->v4l2_fd, VIDIOC_S_OUTPUT, &out_idx) < 0) {
    GST_ERROR("Set output failed.");
    return -1;
  }

  memset(&v4l2fmt, 0, sizeof(struct v4l2_format));
  v4l2fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  v4l2fmt.fmt.pix.width = handle->in_w = w;
  v4l2fmt.fmt.pix.height = handle->in_h = h;
  v4l2fmt.fmt.pix.pixelformat = handle->in_fmt = fmt;
  if (ioctl(handle->v4l2_fd, VIDIOC_S_FMT, &v4l2fmt) < 0) {
    GST_ERROR ("Set format failed.");
    return -1;
  }

  /* Set overlay source window */
  memset(&v4l2fmt, 0, sizeof(struct v4l2_format));
  v4l2fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY;
  v4l2fmt.fmt.win.global_alpha = handle->alpha;
  v4l2fmt.fmt.win.chromakey = handle->color_key;
  v4l2fmt.fmt.win.w.left = crop->left;
  v4l2fmt.fmt.win.w.top = crop->top;
  v4l2fmt.fmt.win.w.width = crop->width;
  v4l2fmt.fmt.win.w.height = crop->height;
  if (ioctl(handle->v4l2_fd, VIDIOC_S_FMT, &v4l2fmt) < 0) {
    GST_ERROR ("Set VIDIOC_S_FMT output overlay failed.");
    return -1;
  }

  return 0;
}

static gint
imx_pxp_v4l2out_config_output(IMXV4l2Handle *handle, struct v4l2_crop *crop)
{
  gboolean brotate = (handle->rotate == 90 || handle->rotate == 270) ? TRUE : FALSE;

  crop->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY;
  if (ioctl(handle->v4l2_fd, VIDIOC_S_CROP, crop) < 0) {
    GST_ERROR ("Set crop failed.");
    return -1;
  }

  return 0;
}

static gint
imx_pxp_v4l2out_config_rotate(IMXV4l2Handle *handle, gint rotate)
{
  struct v4l2_control ctrl;

  ctrl.id = V4L2_CID_PRIVATE_BASE;
  ctrl.value = rotate;
  if (ioctl(handle->v4l2_fd, VIDIOC_S_CTRL, &ctrl) < 0) {
    GST_ERROR ("Set ctrl rotate failed.");
    return -1;
  }

  return 0;
}

static void
imx_pxp_v4l2_config_alpha (IMXV4l2Handle *handle, guint alpha)
{
  struct v4l2_framebuffer fb;

  GST_DEBUG ("set alpha to (%d)", alpha);

  fb.flags = V4L2_FBUF_FLAG_OVERLAY;
  fb.flags |= V4L2_FBUF_FLAG_GLOBAL_ALPHA;
  if (ioctl(handle->v4l2_fd, VIDIOC_S_FBUF, &fb) < 0) {
    GST_ERROR ("VIDIOC_S_FBUF failed.");
    return;
  }

  handle->alpha = alpha;

  return;
}

static void
imx_pxp_v4l2_config_colorkey (IMXV4l2Handle *handle, gboolean enable, guint color_key)
{
  struct v4l2_framebuffer fb;

  fb.flags = V4L2_FBUF_FLAG_OVERLAY;
  if (enable) {
    fb.flags |= V4L2_FBUF_FLAG_CHROMAKEY;
    handle->color_key = color_key;
    GST_DEBUG ("set colorKey to (%x) ", color_key);
  }
  else {
    fb.flags |= V4L2_FBUF_FLAG_GLOBAL_ALPHA;
  }

  if (ioctl(handle->v4l2_fd, VIDIOC_S_FBUF, &fb) < 0) {
    GST_ERROR ("VIDIOC_S_FBUF failed.");
    return;
  }

  return;
}

gchar *
gst_imx_v4l2_get_default_device_name (gint type)
{
  char * devname;

  if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
    if (imx_chip_code () == CC_MX6Q)
      devname = (char*)"/dev/video17";
    else if (imx_chip_code () == CC_MX60)
      devname = (char*)"/dev/video0";
    else {
      GST_ERROR ("UNKNOWN imx SoC.");
      return NULL;
    }
  }
  else if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
    devname = (char*)"/dev/video0";
  }
  else {
    GST_ERROR ("UNKNOWN v4l2 type %d\n", type);
    devname = NULL;
  }

  return devname;
}

gint
gst_imx_v4l2_get_min_buffer_num (gint type)
{
  gint num;
  if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
    if (imx_chip_code () == CC_MX60)
      num = MAX (V4L2_HOLDED_BUFFERS, MX60_STREAMON_COUNT);
    else if (imx_chip_code () == CC_MX6Q)
      num = MAX (V4L2_HOLDED_BUFFERS, MX6Q_STREAMON_COUNT);
    else 
      num = V4L2_HOLDED_BUFFERS;

    num += 1;
  }

  return num;
}

GstCaps *
gst_imx_v4l2_get_device_caps (gint type)
{
  struct v4l2_fmtdesc fmtdesc;
  char * devname;
  int fd;
  gint i;
  GstCaps *caps = NULL;

  if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
    return gst_imx_v4l2capture_get_device_caps();
  }

  devname = gst_imx_v4l2_get_default_device_name (type);
  fd = open(devname, O_RDWR | O_NONBLOCK, 0);
  if (fd <= 0) {
    GST_ERROR ("Can't open %s\n", devname);
    return NULL;
  }

  fmtdesc.type = type;
  fmtdesc.index = 0;
  while (!(ioctl (fd, VIDIOC_ENUM_FMT, &fmtdesc))) {
    for (i=0; i<sizeof(g_imxv4l2fmt_maps)/sizeof(IMXV4l2FmtMap); i++) {
      if (fmtdesc.pixelformat == g_imxv4l2fmt_maps[i].v4l2fmt) {
        if (!caps)
          caps = gst_caps_new_empty ();
        if (caps) {
          GstStructure * structure = gst_structure_from_string(g_imxv4l2fmt_maps[i].caps_str, NULL);
          gst_caps_append_structure (caps, structure);
        }
        break;
      }
    }
    fmtdesc.index ++;
  };

  close (fd);

  return caps;
}

GstCaps *
gst_imx_v4l2_get_caps (gpointer v4l2handle)
{
	struct v4l2_fmtdesc fmt;
	struct v4l2_frmsizeenum frmsize;
	struct v4l2_frmivalenum frmival;
  gint i, index, vformat;
  GstCaps *caps = NULL;
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;

  if (handle->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
    fmt.index = 0;
    fmt.type = handle->type;
    //FIXME: driver should report v4l2 capture output format. not camera sensor
    //support format.
    //while (ioctl(handle->v4l2_fd, VIDIOC_ENUM_FMT, &fmt) >= 0) {
    while (g_camera_format[fmt.index]) {
      fmt.pixelformat = g_camera_format[fmt.index];
      vformat = fmt.pixelformat;
      GST_INFO ("frame format: %c%c%c%c",	vformat & 0xff, (vformat >> 8) & 0xff,
					(vformat >> 16) & 0xff, (vformat >> 24) & 0xff);
      frmsize.pixel_format = fmt.pixelformat;
      frmsize.index = 0;
      while (ioctl(handle->v4l2_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) >= 0) {
        GST_INFO ("frame size: %dx%d", frmsize.discrete.width, frmsize.discrete.height);
        GST_INFO ("frame size type: %d", frmsize.type);
        //FIXME: driver haven't set type.
        if (1) {//frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
          frmival.index = 0;
          frmival.pixel_format = fmt.pixelformat;
          frmival.width = frmsize.discrete.width;
          frmival.height = frmsize.discrete.height;
          while (ioctl(handle->v4l2_fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) >= 0) {
            GST_INFO ("frame rate: %d/%d", frmival.discrete.denominator, frmival.discrete.numerator);
            for (i=0; i<sizeof(g_imxv4l2fmt_maps)/sizeof(IMXV4l2FmtMap); i++) {
              if (fmt.pixelformat == g_imxv4l2fmt_maps[i].v4l2fmt) {
                if (!caps)
                  caps = gst_caps_new_empty ();
                if (caps) {
                  GstStructure * structure = gst_structure_from_string( \
                      g_imxv4l2fmt_maps[i].caps_str, NULL);
                  gst_structure_set (structure, "width", G_TYPE_INT, frmsize.discrete.width, NULL);
                  gst_structure_set (structure, "height", G_TYPE_INT, frmsize.discrete.height, NULL);
                  gst_structure_set (structure, "framerate", GST_TYPE_FRACTION, \
                      frmival.discrete.denominator, frmival.discrete.numerator, NULL);
                  gst_caps_append_structure (caps, structure);
                  GST_INFO ("Added one caps\n");
                }
              }
            }
            // Add hard code format.
            if (handle->support_format_table) {
              index = 0;
              while (handle->support_format_table[index]) {
                for (i=0; i<sizeof(g_imxv4l2fmt_maps)/sizeof(IMXV4l2FmtMap); i++) {
                  if (handle->support_format_table[index] == g_imxv4l2fmt_maps[i].v4l2fmt) {
                    if (!caps)
                      caps = gst_caps_new_empty ();
                    if (caps) {
                      GstStructure * structure = gst_structure_from_string( \
                          g_imxv4l2fmt_maps[i].caps_str, NULL);
                      gst_structure_set (structure, "width", G_TYPE_INT, frmsize.discrete.width, NULL);
                      gst_structure_set (structure, "height", G_TYPE_INT, frmsize.discrete.height, NULL);
                      gst_structure_set (structure, "framerate", GST_TYPE_FRACTION, \
                          frmival.discrete.denominator, frmival.discrete.numerator, NULL);
                      gst_caps_append_structure (caps, structure);
                      GST_INFO ("Added one caps\n");
                    }
                  }
                }
                index ++;
              }
            }
            frmival.index++;
          }
        }
        frmsize.index++;
      }
      fmt.index++;
    }
  }

  return gst_caps_simplify(caps);
}

guint
gst_imx_v4l2_fmt_gst2v4l2 (GstVideoFormat gstfmt)
{
  guint v4l2fmt = 0;
  int i;

  for(i=0; i<sizeof(g_imxv4l2fmt_maps)/sizeof(IMXV4l2FmtMap); i++) {
    if (gstfmt == g_imxv4l2fmt_maps[i].gstfmt) {
      v4l2fmt = g_imxv4l2fmt_maps[i].v4l2fmt;
      break;
    }
  }

  return v4l2fmt;
}

guint
gst_imx_v4l2_get_bits_per_pixel (guint v4l2fmt)
{
  guint bits_per_pixel = 0;
  int i;
  for(i=0; i<sizeof(g_imxv4l2fmt_maps)/sizeof(IMXV4l2FmtMap); i++) {
    if (v4l2fmt == g_imxv4l2fmt_maps[i].v4l2fmt) {
      bits_per_pixel = g_imxv4l2fmt_maps[i].bits_per_pixel;
      break;
    }
  }

  return bits_per_pixel;
}

gboolean
gst_imx_v4l2_support_deinterlace (gint type)
{
  if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
    if (imx_chip_code () == CC_MX6Q)
      return TRUE;
    else if (imx_chip_code () == CC_MX60)
      return FALSE;
    else {
      GST_ERROR ("UNKNOWN imx SoC.");
      return FALSE;
    }
  }

  return FALSE;
}

static void
gst_imx_v4l2output_set_default_res (IMXV4l2Handle *handle)
{
  struct fb_var_screeninfo fb_var;
  IMXV4l2Rect rect;
  gint i;

  for (i=0; i<sizeof(g_device_maps)/sizeof(IMXV4l2DeviceMap); i++) {
    if (!strcmp (handle->device, g_device_maps[i].name)) {
      handle->device_map_id = i;
      break;
    }
  }

  if (g_device_maps[handle->device_map_id].bg)
    return;

  gst_imx_v4l2_get_display_resolution (handle->device, &handle->disp_w, &handle->disp_h);

  //set gblobal alpha to 0 to show video
  gst_imx_v4l2out_config_alpha (handle, 0);

  rect.left = 0;
  rect.top = 0;
  rect.width = handle->disp_w;
  rect.height = handle->disp_h;
  gst_imx_v4l2out_config_output (handle, &rect, TRUE);

  return;
}

static gint 
gst_imx_v4l2capture_config_usb_camera (IMXV4l2Handle *handle, guint fmt, guint w, guint h, guint fps_n, guint fps_d)
{
  struct v4l2_format v4l2_fmt = {0};
  struct v4l2_frmsizeenum fszenum = {0};
  struct v4l2_streamparm parm = {0};
  gint capture_mode = -1;

  fszenum.index = 0;
  fszenum.pixel_format = fmt;
  while (ioctl (handle->v4l2_fd, VIDIOC_ENUM_FRAMESIZES, &fszenum) >= 0){
    if (fszenum.discrete.width == w && fszenum.discrete.height == h) {
      capture_mode = fszenum.index;
      break;
    }
    fszenum.index ++;
  }
  if (capture_mode < 0) {
    GST_ERROR ("can't support resolution.");
    return -1;
  }

  GST_INFO ("capture mode %d: %dx%d", capture_mode, w, h);

  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator = fps_d;
  parm.parm.capture.timeperframe.denominator = fps_n;
  parm.parm.capture.capturemode = capture_mode;

  if (ioctl (handle->v4l2_fd, VIDIOC_S_PARM, &parm) < 0) {
    GST_ERROR ("VIDIOC_S_PARM failed");
    return -1;
  }

  v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  v4l2_fmt.fmt.pix.pixelformat = fmt;
  v4l2_fmt.fmt.pix.width = w;
  v4l2_fmt.fmt.pix.height = h;

  if (ioctl (handle->v4l2_fd, VIDIOC_S_FMT, &v4l2_fmt) < 0) {
    GST_ERROR ("VIDIOC_S_FMT failed");
    return -1;
  }

  return 0;
}

static gint 
gst_imx_v4l2capture_config_pxp (IMXV4l2Handle *handle, guint fmt, guint w, guint h, guint fps_n, guint fps_d)
{
	struct v4l2_crop crop = {0};

  if (gst_imx_v4l2capture_config_usb_camera (handle, fmt, w, h, fps_n, fps_d) < 0) {
    GST_ERROR ("camera config failed\n");
    return -1;
  }

  crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(handle->v4l2_fd, VIDIOC_G_CROP, &crop) < 0) {
    GST_ERROR ("VIDIOC_G_CROP failed\n");
    return -1;
  }

  crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  crop.c.width = w;
  crop.c.height = h;
  crop.c.top = 0;
  crop.c.left = 0;
  if (ioctl(handle->v4l2_fd, VIDIOC_S_CROP, &crop) < 0) {
    GST_ERROR ("VIDIOC_S_CROP failed\n");
    return -1;
  }

  return 0;
}

static gint 
gst_imx_v4l2capture_config_camera (IMXV4l2Handle *handle, guint fmt, guint w, guint h, guint fps_n, guint fps_d)
{
  gint input = 1;

  if (ioctl (handle->v4l2_fd, VIDIOC_S_INPUT, &input) < 0) {
    GST_ERROR ("VIDIOC_S_INPUT failed");
    return -1;
  }

  return gst_imx_v4l2capture_config_pxp (handle, fmt, w, h, fps_n, fps_d);
}

static gint 
gst_imx_v4l2capture_config_tvin (IMXV4l2Handle *handle, guint fmt, guint w, guint h, guint fps_n, guint fps_d)
{
  v4l2_std_id id;

  if (ioctl (handle->v4l2_fd, VIDIOC_G_STD, &id) < 0) {
    GST_ERROR ("VIDIOC_G_STD failed\n");
    return -1;
  }

  if (ioctl (handle->v4l2_fd, VIDIOC_S_STD, &id) < 0) {
    GST_ERROR ("VIDIOC_S_STD failed\n");
    return -1;
  }

  return gst_imx_v4l2capture_config_camera (handle, fmt, w, h, fps_n, fps_d);
}

static gint 
gst_imx_v4l2capture_set_function (IMXV4l2Handle *handle)
{
  struct v4l2_capability cap;

  if (ioctl(handle->v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) {
    GST_ERROR ("VIDIOC_QUERYCAP error.");
    return -1;
  }

  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
    GST_ERROR ("device can't capture.");
    return -1;
  }

  if (!strcmp (cap.driver, MXC_V4L2_CAPTURE_NAME)) {
    struct v4l2_dbg_chip_ident chip;
    if (ioctl(handle->v4l2_fd, VIDIOC_DBG_G_CHIP_IDENT, &chip)) {
      GST_ERROR ("VIDIOC_DBG_G_CHIP_IDENT failed.\n");
      return -1;
    }
    GST_INFO ("sensor chip is %s\n", chip.match.name);
    
    if (!strncmp (chip.match.name, MXC_V4L2_CAPTURE_CAMERA_NAME, 2)) {
      handle->dev_itf.v4l2capture_config = (V4l2captureConfig)gst_imx_v4l2capture_config_camera;
      handle->support_format_table = g_camera_format_IPU;
    } else if (!strncmp (chip.match.name, MXC_V4L2_CAPTURE_TVIN_NAME, 3)) {
      handle->dev_itf.v4l2capture_config = (V4l2captureConfig)gst_imx_v4l2capture_config_tvin;
      handle->support_format_table = g_camera_format_IPU;
    } else {
      GST_ERROR ("can't identify capture sensor type.\n");
      return -1;
    }
  } else if (!strcmp (cap.driver, PXP_V4L2_CAPTURE_NAME)) {
      handle->dev_itf.v4l2capture_config = (V4l2captureConfig)gst_imx_v4l2capture_config_pxp;
      handle->support_format_table = g_camera_format_PXP;
  } else {
      handle->dev_itf.v4l2capture_config = (V4l2captureConfig)gst_imx_v4l2capture_config_usb_camera;
  }

  return 0;
}

#define DEFAULTW (320)
#define DEFAULTH (240)
void gst_imx_v4l2_get_display_resolution (gchar *device, gint *w, gint *h)
{
  struct fb_var_screeninfo fb_var;
  gint i, device_map_id;
  int fd;

  *w = DEFAULTW;
  *h = DEFAULTH;

  for (i=0; i<sizeof(g_device_maps)/sizeof(IMXV4l2DeviceMap); i++) {
    if (!strcmp (device, g_device_maps[i].name)) {
      device_map_id = i;
      break;
    }
  }

  fd = open (g_device_maps[device_map_id].bg_fb_name, O_RDWR, 0);
  if (fd < 0) {
    GST_ERROR ("Can't open %s.\n", g_device_maps[device_map_id].bg_fb_name);
    return;
  }

  if (ioctl (fd, FBIOGET_VSCREENINFO, &fb_var) < 0) {
    GST_ERROR ("Can't get display resolution, use default (%dx%d).\n", DEFAULTW, DEFAULTH);
    close (fd);
    return;
  }

  *w = fb_var.xres;
  *h = fb_var.yres;
  GST_DEBUG ("display(%s) resolution is (%dx%d).", g_device_maps[device_map_id].bg_fb_name, fb_var.xres, fb_var.yres);

  close (fd);

  return;
}

gpointer gst_imx_v4l2_open_device (gchar *device, int type)
{
  int fd;
  struct v4l2_capability cap;
  IMXV4l2Handle *handle = NULL;

  GST_DEBUG_CATEGORY_INIT (imxv4l2_debug, "imxv4l2", 0, "IMX V4L2 Core");

  GST_INFO ("device name: %s", device);
  if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
    fd = open(device, O_RDWR, 0); 
  } else {
    fd = open(device, O_RDWR | O_NONBLOCK, 0); 
  }
  if (fd < 0) {
    GST_DEBUG ("Can't open %s.\n", device);
    return NULL;
  }

  if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
    GST_ERROR ("VIDIOC_QUERYCAP error.");
    close (fd);
    return NULL;
  }

  if (!(cap.capabilities & type)) {
    GST_DEBUG ("device can't capture.");
    close (fd);
    return NULL;
  }
 
  handle = (IMXV4l2Handle*) g_slice_alloc (sizeof(IMXV4l2Handle));
  if (!handle) {
    GST_ERROR ("allocate for IMXV4l2Handle failed.\n");
    close (fd);
    return NULL;
  }
  memset (handle, 0, sizeof(IMXV4l2Handle));

  handle->v4l2_fd = fd; 
  handle->device = device;
  handle->type = type;
  handle->streamon = FALSE;

  if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
    if (imx_chip_code () == CC_MX6Q) {
      handle->dev_itf.v4l2out_config_input = (V4l2outConfigInput)imx_ipu_v4l2out_config_input;
      handle->dev_itf.v4l2out_config_output = (V4l2outConfigOutput)imx_ipu_v4l2out_config_output;
      handle->dev_itf.v4l2out_config_rotate = (V4l2outConfigRotate)imx_ipu_v4l2out_config_rotate;
      handle->dev_itf.v4l2out_config_alpha = (V4l2outConfigAlpha) imx_ipu_v4l2_config_alpha;
      handle->dev_itf.v4l2out_config_colorkey = (V4l2outConfigColorkey) imx_ipu_v4l2_config_colorkey;
      handle->streamon_count = MX6Q_STREAMON_COUNT;
    }
    else if (imx_chip_code () == CC_MX60) {
      handle->dev_itf.v4l2out_config_input = (V4l2outConfigInput)imx_pxp_v4l2out_config_input;
      handle->dev_itf.v4l2out_config_output = (V4l2outConfigOutput)imx_pxp_v4l2out_config_output;
      handle->dev_itf.v4l2out_config_rotate = (V4l2outConfigRotate)imx_pxp_v4l2out_config_rotate;
      handle->dev_itf.v4l2out_config_alpha = (V4l2outConfigAlpha) imx_pxp_v4l2_config_alpha;
      handle->dev_itf.v4l2out_config_colorkey = (V4l2outConfigColorkey) imx_pxp_v4l2_config_colorkey;
      handle->streamon_count = MX60_STREAMON_COUNT;
    }

    gst_imx_v4l2output_set_default_res (handle);
  }

  if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
    if (gst_imx_v4l2capture_set_function (handle) < 0) {
      GST_ERROR ("v4l2 capture set function failed.\n");
      close (fd);
      return NULL;
    }
    handle->streamon_count = 2;
  }

  return (gpointer) handle;
}

gint gst_imx_v4l2_reset_device (gpointer v4l2handle)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  gint i;

  if (handle && handle->v4l2_fd) {
    if (handle->streamon) {
      if (ioctl (handle->v4l2_fd, VIDIOC_STREAMOFF, &handle->type) < 0) {
        GST_ERROR ("stream off failed\n");
        return -1;
      }
      handle->streamon = FALSE;
      GST_DEBUG ("V4L2 device is STREAMOFF.");
    }

    GST_DEBUG ("V4l2 device hold (%d) buffers when reset.", handle->queued_count);

    for (i=0; i<handle->buffer_count; i++) {
      GST_DEBUG ("unref v4l held gstbuffer(%p).", handle->buffer_pair[i].gstbuffer);
      if (handle->buffer_pair[i].gstbuffer) {
        gst_buffer_unref (handle->buffer_pair[i].gstbuffer);
        handle->buffer_pair[i].gstbuffer = NULL;
      }
    }

    handle->queued_count = 0;
  } 

  return 0;
}

gint gst_imx_v4l2_close_device (gpointer v4l2handle)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;

  if (handle && handle->v4l2_fd) {
    close (handle->v4l2_fd);
    handle->v4l2_fd = 0;
  }

  g_slice_free1 (sizeof(IMXV4l2Handle), handle);

  return 0;
}

static GstCaps *
gst_imx_v4l2capture_get_device_caps ()
{
#define V4L2_DEVICE_MAX 32
  gint i;
  char devname[20];
  GstCaps *caps = NULL;
  gpointer v4l2handle;

  for (i=0; i<V4L2_DEVICE_MAX; i++){
    sprintf(devname, "/dev/video%d", i);
    v4l2handle = gst_imx_v4l2_open_device (devname, \
        V4L2_BUF_TYPE_VIDEO_CAPTURE);
    if (v4l2handle){
      if (!caps)
        caps = gst_caps_new_empty ();
      if (caps) {
        gst_caps_append (caps, gst_imx_v4l2_get_caps(v4l2handle));
      }
      gst_imx_v4l2_close_device (v4l2handle);
      v4l2handle = NULL;
    }
  }

  return caps;
}

gint gst_imx_v4l2out_get_res (gpointer v4l2handle, guint *w, guint *h)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;

  if (handle && handle->v4l2_fd) {
    *w = handle->disp_w;
    *h = handle->disp_h;
  }

  return 0;
}

gint gst_imx_v4l2capture_config (gpointer v4l2handle, guint fmt, guint w, guint h, guint fps_n, guint fps_d)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  return (*handle->dev_itf.v4l2capture_config) (v4l2handle, fmt, w, h, fps_n, fps_d);
}

gint gst_imx_v4l2out_config_input (gpointer v4l2handle, guint fmt, guint w, guint h, IMXV4l2Rect *crop)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  gint ret;

  memcpy (&handle->in_crop, crop, sizeof(IMXV4l2Rect));
  ret = (*handle->dev_itf.v4l2out_config_input) (handle, fmt, w, h, crop);
  if (ret == 1) {
    GST_DEBUG ("Video is invisible as all input are cropped.");
    handle->invisible |= INVISIBLE_IN;
    return 0;
  }

  handle->invisible &= ~INVISIBLE_IN;

  return ret;
}

static gboolean
gst_imx_v4l2out_calc_crop (IMXV4l2Handle *handle, 
    IMXV4l2Rect *rect_out, GstVideoRectangle *result, IMXV4l2Rect *rect_crop)
{
  gdouble ratio;
  gboolean need_crop = FALSE;

  ratio = (gdouble)handle->in_crop.width / (gdouble)result->w;
  if ((rect_out->left + result->x) < 0) {
    rect_crop->left = -(rect_out->left + result->x) * ratio;
    rect_crop->width = MIN (handle->in_crop.width - rect_crop->left, handle->disp_w * ratio);
    rect_out->width = MIN ((result->w + (rect_out->left + result->x)), handle->disp_w);
    rect_out->left = 0;
    need_crop = TRUE;
  }
  else if ((rect_out->left + result->x + result->w) > handle->disp_w) {
    rect_crop->left = 0;
    rect_crop->width = (handle->disp_w - (rect_out->left + result->x)) * ratio;
    if ((gint)rect_crop->width < 0)
      rect_crop->width = 0;
    rect_out->left = rect_out->left + result->x;
    rect_out->width = handle->disp_w - rect_out->left;
    if (rect_out->width < 0)
      rect_out->width = 0;
    need_crop = TRUE;
  }
  else {
    rect_crop->left = 0;
    rect_crop->width = handle->in_crop.width;
    rect_out->left += result->x;
    rect_out->width = result->w;
  }

  if ((rect_out->top + result->y) < 0) {
    rect_crop->top = -(rect_out->top + result->y) * ratio;
    rect_crop->height = MIN (handle->in_crop.height - rect_crop->top, handle->disp_h * ratio);
    rect_out->height = MIN ((result->h + (rect_out->top + result->y)), handle->disp_h);
    rect_out->top = 0;
    need_crop = TRUE;
  }
  else if ((rect_out->top + result->y + result->h) > handle->disp_h) {
    rect_crop->top = 0;
    rect_crop->height = (handle->disp_h - (rect_out->top + result->y)) * ratio;
    if ((gint)rect_crop->height < 0)
      rect_crop->height = 0;
    rect_out->top = rect_out->top + result->y;
    rect_out->height = handle->disp_h - rect_out->top;
    if (rect_out->height < 0)
      rect_out->height = 0;
    need_crop = TRUE;
  }
  else {
    rect_crop->top = 0;
    rect_crop->height = handle->in_crop.height;
    rect_out->top += result->y;
    rect_out->height = result->h;
  } 

  if (!need_crop && handle->prev_need_crop)
    need_crop = TRUE;

  handle->prev_need_crop = need_crop;

  return need_crop;
}

gint gst_imx_v4l2out_config_output (gpointer v4l2handle, IMXV4l2Rect *overlay, gboolean keep_video_ratio)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  struct v4l2_crop crop;
  IMXV4l2Rect *rect = &crop.c;;
  GstVideoRectangle src, dest, result;
  gboolean brotate; 

  memcpy (rect, overlay, sizeof(IMXV4l2Rect));

  GST_DEBUG ("config output, (%d, %d) -> (%d, %d)",
      rect->left, rect->top, rect->width, rect->height);

  if (handle->rotate != 0) {
    if (rect->left < 0 || rect->top < 0 
        || (rect->left + rect->width) > handle->disp_w
        || (rect->top + rect->height) > handle->disp_h) {
      g_print ("not support video out of screen if oritation is not landscape.\n");
      return 0;
    }
  }

  brotate = (handle->rotate == 90 || handle->rotate == 270) ? TRUE : FALSE;

  //keep video ratio with display
  src.x = src.y = 0;
  src.w = handle->in_crop.width;
  src.h = handle->in_crop.height;
  dest.x = dest.y = 0;
  if (brotate) {
    dest.w = rect->height;
    dest.h = rect->width;
  }
  else {
    dest.w = rect->width;
    dest.h = rect->height;
  }

  if (keep_video_ratio)
    gst_video_sink_center_rect (src, dest, &result, TRUE);
  else
    memcpy (&result, &dest, sizeof(GstVideoRectangle));

  GST_DEBUG ("src, (%d, %d) -> (%d, %d).", src.x, src.y, src.w, src.h);
  GST_DEBUG ("dest, (%d, %d) -> (%d, %d).", dest.x, dest.y, dest.w, dest.h);
  GST_DEBUG ("result, (%d, %d) -> (%d, %d).", result.x, result.y, result.w, result.h);

  if (handle->rotate == 0) {
    // only support video out of screen in landscape mode
    IMXV4l2Rect rect_crop;
    if (gst_imx_v4l2out_calc_crop (handle, rect, &result, &rect_crop)) {
      gint ret = (*handle->dev_itf.v4l2out_config_input) (handle, handle->in_fmt, handle->in_w, handle->in_h, &rect_crop);
      if (ret == 1) {
        handle->invisible |= INVISIBLE_OUT;
        GST_DEBUG ("Video is invisible as out of display.");
        return 0;
      }
      else if (ret < 0)
        return ret;
      else
        handle->invisible &= ~INVISIBLE_OUT;
    }
  }
  else {
    if (brotate) {
      rect->left += result.y;
      rect->top += result.x;
      rect->width = result.h;
      rect->height = result.w;
    }
    else {
      rect->left += result.x;
      rect->top += result.y;
      rect->width = result.w;
      rect->height = result.h;
    }
  }

  if (rect->left >= handle->disp_w || rect->top >= handle->disp_h) {
    handle->invisible |= INVISIBLE_OUT;
    GST_DEBUG ("Video is invisible as out of display.");
    return 1;
  }
  handle->invisible &= ~INVISIBLE_OUT;

  GST_DEBUG ("rect, (%d, %d) -> (%d, %d).", rect->left, rect->top, rect->width, rect->height);

  return (*handle->dev_itf.v4l2out_config_output) (handle, &crop);
}

gint gst_imx_v4l2_config_rotate (gpointer v4l2handle, gint rotate)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;

  GST_DEBUG ("set rotation to (%d).", rotate);

  if ((*handle->dev_itf.v4l2out_config_rotate) (handle, rotate) < 0) {
    return -1;
  }
  handle->rotate = rotate;

  return 0;
}

gint gst_imx_v4l2_config_deinterlace (gpointer v4l2handle, gboolean do_deinterlace, guint motion)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  struct v4l2_control ctrl;

  if (do_deinterlace) {
    ctrl.id = V4L2_CID_MXC_MOTION;
    ctrl.value = motion;
    if (ioctl(handle->v4l2_fd, VIDIOC_S_CTRL, &ctrl) < 0) {
      GST_ERROR("Set ctrl motion failed\n");
      return -1;
    }
  }

  handle->do_deinterlace = do_deinterlace;

  return 0;
}

void gst_imx_v4l2out_config_alpha (gpointer v4l2handle, guint alpha)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;

  if (handle->type != V4L2_BUF_TYPE_VIDEO_OUTPUT) {
    GST_ERROR ("Can't set alpha for non output device.");
    return;
  }

  (*handle->dev_itf.v4l2out_config_alpha) (handle, alpha);

  return;
}

void gst_imx_v4l2out_config_color_key (gpointer v4l2handle, gboolean enable, guint color_key)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;

  if (handle->type != V4L2_BUF_TYPE_VIDEO_OUTPUT) {
    GST_ERROR ("Can't set color key for non output device.");
    return;
  }

  (*handle->dev_itf.v4l2out_config_colorkey) (handle, enable, color_key);

  return ;
}

gint gst_imx_v4l2_set_buffer_count (gpointer v4l2handle, guint count, guint memory_mode)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  struct v4l2_requestbuffers buf_req;

  GST_DEBUG ("requeset for (%d) buffers.", count);

  memset(&buf_req, 0, sizeof(buf_req));

  buf_req.type = handle->type;
  buf_req.count = count;
  buf_req.memory = memory_mode;
  if (ioctl(handle->v4l2_fd, VIDIOC_REQBUFS, &buf_req) < 0) {
    GST_ERROR("Request %d buffers failed\n", count);
    return -1;
  }
  handle->buffer_count = count;

  return 0;
}

gint gst_imx_v4l2_allocate_buffer (gpointer v4l2handle, PhyMemBlock *memblk)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  struct v4l2_buffer *v4l2buf;

  if (handle->allocated >= handle->buffer_count) {
    GST_ERROR ("No more v4l2 buffer for allocating.\n");
    return -1;
  }

  v4l2buf = &handle->buffer_pair[handle->allocated].v4l2buffer;
  memset (v4l2buf, 0, sizeof(struct v4l2_buffer));
  v4l2buf->type = handle->type;
  v4l2buf->memory = V4L2_MEMORY_MMAP;;
  v4l2buf->index = handle->allocated;

  if (ioctl(handle->v4l2_fd, VIDIOC_QUERYBUF, v4l2buf) < 0) {
    GST_ERROR ("VIDIOC_QUERYBUF error.");
    return -1;
  }

  handle->allocated ++;
  memblk->size = v4l2buf->length;
  memblk->vaddr = mmap (NULL, v4l2buf->length, PROT_READ | PROT_WRITE, MAP_SHARED, handle->v4l2_fd, v4l2buf->m.offset);
  if (!memblk->vaddr) {
    GST_ERROR ("mmap v4lbuffer address failed\n");
    return -1;
  }

  //FIXME: need to query twice to get the physical address
  if (ioctl(handle->v4l2_fd, VIDIOC_QUERYBUF, v4l2buf) < 0) {
    GST_ERROR ("VIDIOC_QUERYBUF for physical address failed.");
    return -1;
  }
  memblk->paddr = (guint8*) v4l2buf->m.offset;
  memblk->user_data = (gpointer) v4l2buf;

  GST_DEBUG ("Allocated v4l2buffer(%p), index(%d), vaddr(%p), paddr(%p), size(%d).", 
      v4l2buf, handle->allocated - 1, memblk->vaddr, memblk->paddr, memblk->size);

  return 0;
}

gint gst_imx_v4l2_register_buffer (gpointer v4l2handle, PhyMemBlock *memblk)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  struct v4l2_buffer *v4l2buf;

  if (handle->allocated >= handle->buffer_count) {
    GST_ERROR ("No more v4l2 buffer for allocating.\n");
    return -1;
  }

  v4l2buf = &handle->buffer_pair[handle->allocated].v4l2buffer;
  memset (v4l2buf, 0, sizeof(struct v4l2_buffer));
  v4l2buf->type = handle->type;
  v4l2buf->memory = V4L2_MEMORY_USERPTR;
  v4l2buf->index = handle->allocated;
  v4l2buf->m.userptr = memblk->paddr;
  v4l2buf->length = memblk->size;

  if (ioctl(handle->v4l2_fd, VIDIOC_QUERYBUF, v4l2buf) < 0) {
    GST_ERROR ("VIDIOC_QUERYBUF error.");
    return -1;
  }

  //FIXME: workaround for conflict when v4l2src ! v4l2sink.
  if (!memblk->user_data) {
    memblk->user_data = (gpointer) v4l2buf;
  }
  handle->allocated ++;

  GST_DEBUG ("Allocated v4l2buffer(%p), index(%d).", v4l2buf, handle->allocated - 1);

  return 0;
}

gint gst_imx_v4l2_free_buffer (gpointer v4l2handle, PhyMemBlock *memblk)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  struct v4l2_buffer *v4l2buf;

  if (memblk->vaddr)
    munmap(memblk->vaddr, memblk->size);
  handle->allocated --;
  if (handle->allocated < 0) {
    GST_ERROR ("freed buffer more than allocated.");
    handle->allocated = 0;
  }

  v4l2buf =  (struct v4l2_buffer*) memblk->user_data;
  GST_DEBUG ("Free v4l2buffer(%p), index(%d).", v4l2buf, v4l2buf->index);
  memset (v4l2buf, 0, sizeof(struct v4l2_buffer));

  return 0;
}

static gint imx_v4l2_do_queue_buffer (IMXV4l2Handle *handle, struct v4l2_buffer *v4l2buf)
{
  struct timeval queuetime;

  if (!v4l2buf) {
    GST_ERROR ("queue buffer is NULL!!");
    return -1;
  }

  /*display immediately */
  gettimeofday (&queuetime, NULL);
  v4l2buf->timestamp = queuetime;

  if (ioctl (handle->v4l2_fd, VIDIOC_QBUF, v4l2buf) < 0) {
    GST_ERROR ("queue v4l2 buffer failed.\n");
    return -1;
  }

  return 0;
}

gint gst_imx_v4l2_queue_v4l2memblk (gpointer v4l2handle, PhyMemBlock *memblk, GstVideoFrameFlags flags)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  struct v4l2_buffer *v4l2buf;
  gint index;

  v4l2buf = (struct v4l2_buffer *) memblk->user_data;
  index = v4l2buf->index;

  GST_DEBUG ("queue v4lbuffer memblk (%p), index(%d), flags(%x).",
      memblk, index, flags);

  v4l2buf->field = V4L2_FIELD_NONE;
  if ((flags & GST_VIDEO_FRAME_FLAG_INTERLACED) && handle->do_deinterlace) {
    if (flags & GST_VIDEO_FRAME_FLAG_TFF)
      v4l2buf->field = V4L2_FIELD_INTERLACED_TB;
    else
      v4l2buf->field = V4L2_FIELD_INTERLACED_BT;
  }

  if (flags & GST_VIDEO_FRAME_FLAG_ONEFIELD) {
    if (flags & GST_VIDEO_FRAME_FLAG_TFF)
      v4l2buf->field = V4L2_FIELD_TOP;
    else
      v4l2buf->field = V4L2_FIELD_BOTTOM;
  }

  handle->buffer_pair[v4l2buf->index].v4l2memblk = memblk;

  if (!handle->streamon) {
    int i;
    GST_DEBUG ("streamon count (%d), queue count (%d)\n", handle->streamon_count, handle->queued_count);

    handle->v4lbuf_queued_before_streamon[handle->queued_count] = v4l2buf;
    handle->queued_count ++;
    if (handle->queued_count < handle->streamon_count)
      return 0;

    for (i=0; i<handle->streamon_count; i++) {
      if (imx_v4l2_do_queue_buffer (handle, handle->v4lbuf_queued_before_streamon[i]) < 0) {
        handle->buffer_pair[handle->v4lbuf_queued_before_streamon[i]->index].v4l2memblk = NULL;
        GST_ERROR ("queue buffers before streamon failed.");
        return -1;
      }
    }

    if (ioctl (handle->v4l2_fd, VIDIOC_STREAMON, &handle->type) < 0) {
      GST_ERROR ("Stream on V4L2 device failed.\n");
      return -1;
    }
    handle->streamon = TRUE;
    GST_DEBUG ("V4L2 device is STREAMON.");
    return 0;
  }

  if (imx_v4l2_do_queue_buffer (handle, v4l2buf) < 0) {
    handle->buffer_pair[v4l2buf->index].v4l2memblk = NULL;
    GST_ERROR ("queue buffer failed.");
    return -1;
  }

  handle->queued_count ++;

  GST_DEBUG ("queued (%d)\n", handle->queued_count);

  return 0;
}

gint gst_imx_v4l2_queue_gstbuffer (gpointer v4l2handle, GstBuffer *buffer, GstVideoFrameFlags flags)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  struct v4l2_buffer *v4l2buf;
  PhyMemBlock *memblk;
  gint ret;

  if (handle->invisible) {
    gst_buffer_unref (buffer);
    return 0;
  }

  memblk = gst_buffer_query_phymem_block(buffer);
  if (!memblk) {
    GST_ERROR ("Can't get physical memory block from gstbuffer.\n");
    return -1;
  }

#if 0
  {
    FILE *fp = fopen("dump.yuv", "ab");
    if(fp) {
      GstMapInfo info;
      gst_buffer_map (buffer, &info, GST_MAP_READ);
      fwrite (info.data, 1, info.size, fp);
      fclose (fp);
      gst_buffer_unmap (buffer, &info);
    }
  }
#endif

  GST_DEBUG ("queue gstbuffer(%p).", buffer);
  v4l2buf = (struct v4l2_buffer *) memblk->user_data;
  if (handle->buffer_pair[v4l2buf->index].gstbuffer)
    GST_ERROR ("gstbuffer(%p) not dequeued yet but queued again, index(%d).", buffer, index);

  if (gst_imx_v4l2_queue_v4l2memblk (v4l2handle, memblk, flags) < 0) {
    GST_ERROR ("queue gstbuffer (%p) failed.", buffer);
    return ret;
  }

  handle->buffer_pair[v4l2buf->index].gstbuffer = buffer;

  return 0;
}

#define TRY_TIMEOUT (500000) //500ms
#define TRY_INTERVAL (10000) //10ms
#define MAX_TRY_CNT (TRY_TIMEOUT/TRY_INTERVAL)

gint gst_imx_v4l2_dequeue_v4l2memblk (gpointer v4l2handle, PhyMemBlock **memblk)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  struct v4l2_buffer v4l2buf;
  gint trycnt = 0;

  if (handle->queued_count <= MAX(V4L2_HOLDED_BUFFERS, handle->streamon_count)) {
    *memblk = NULL;
    return 0;
  }

  memset (&v4l2buf, 0, sizeof(v4l2buf));
  v4l2buf.type = handle->type;
  v4l2buf.memory = V4L2_MEMORY_MMAP;

  while (ioctl (handle->v4l2_fd, VIDIOC_DQBUF, &v4l2buf) < 0) {
    trycnt ++;
    if(trycnt >= MAX_TRY_CNT) {
      GST_ERROR ("Dequeue buffer from v4l2 device failed.");
      return -1;
    }

    usleep (TRY_INTERVAL);
  }

  *memblk = handle->buffer_pair[v4l2buf.index].v4l2memblk;
  handle->buffer_pair[v4l2buf.index].v4l2memblk = NULL;
  handle->queued_count--;

  GST_DEBUG ("deque v4l2buffer memblk (%p), index (%d)", v4l2buf.index, handle->buffer_pair[v4l2buf.index].v4l2memblk);

  return 0;
}

gint gst_imx_v4l2_dequeue_gstbuffer (gpointer v4l2handle, GstBuffer **buffer)
{
  IMXV4l2Handle *handle = (IMXV4l2Handle*)v4l2handle;
  PhyMemBlock *memblk = NULL;
  struct v4l2_buffer *v4l2buf;

  if (handle->invisible) {
    return 0;
  }

  if (gst_imx_v4l2_dequeue_v4l2memblk (handle, &memblk) < 0) {
    GST_ERROR ("dequeue memblk failed.");
    return -1;
  }

  if (!memblk)
    return 0;

  v4l2buf = (struct v4l2_buffer *) memblk->user_data;
  *buffer = handle->buffer_pair[v4l2buf->index].gstbuffer;
  handle->buffer_pair[v4l2buf->index].gstbuffer = NULL;

  GST_DEBUG ("dequeue gstbuffer(%p), v4l2buffer index(%d).", *buffer, v4l2buf->index);

  return 0;
}
