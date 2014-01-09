/*
 * Copyright (c) 2013, Freescale Semiconductor, Inc. All rights reserved.
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

#ifndef __IMX_V4L2_H__
#define __IMX_V4L2_H__

#include <gst/gst.h>
#include <gst/video/video-format.h>
#include <gst/video/video-frame.h>
#include <linux/videodev2.h>
#include "gstallocatorphymem.h"

typedef struct v4l2_rect IMXV4l2Rect;

GstCaps *gst_imx_v4l2_get_device_caps (gint type);
guint gst_imx_v4l2_fmt_gst2v4l2 (GstVideoFormat gstfmt);
guint gst_imx_v4l2_get_bits_per_pixel (guint v4l2fmt);
gpointer gst_imx_v4l2_open_device (gchar *device, int type);
gint gst_imx_v4l2_reset_device (gpointer v4l2handle);
gint gst_imx_v4l2_close_device (gpointer v4l2handle);
gint gst_imx_v4l2out_get_res (gpointer v4l2handle, guint *w, guint *h);
gint gst_imx_v4l2out_config_input (gpointer v4l2handle, guint fmt, guint w, guint h, IMXV4l2Rect *crop);
gint gst_imx_v4l2out_config_output (gpointer v4l2handle, IMXV4l2Rect *rect, gboolean keep_video_ratio);
gint gst_imx_v4l2_config_rotate (gpointer v4l2handle, gint rotate);
gint gst_imx_v4l2_config_deinterlace (gpointer v4l2handle, gboolean do_deinterlace, guint motion);
gint gst_imx_v4l2_set_buffer_count (gpointer v4l2handle, guint count);
gint gst_imx_v4l2_allocate_buffer (gpointer v4l2handle, PhyMemBlock *memblk);
gint gst_imx_v4l2_free_buffer (gpointer v4l2handle, PhyMemBlock *memblk);
gint gst_imx_v4l2_queue_buffer (gpointer v4l2handle, GstBuffer *buffer, GstVideoFrameFlags flags);
gint gst_imx_v4l2_dequeue_buffer (gpointer v4l2handle, GstBuffer **buffer);

#endif
