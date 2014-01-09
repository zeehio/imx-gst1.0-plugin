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

#ifndef __IMX_COMMON_H__
#define __IMX_COMMON_H__

#include <gst/gst.h>

#define IMX_GST_PLUGIN_AUTHOR "Multimedia Team <shmmmw@freescale.com>"
#define IMX_GST_PLUGIN_PACKAGE_NAME "Freescle Gstreamer Multimedia Plugins"
#define IMX_GST_PLUGIN_PACKAGE_ORIG "http://www.freescale.com"
#define IMX_GST_PLUGIN_LICENSE "LGPL"

#define IMX_GST_PLUGIN_RANK (GST_RANK_PRIMARY+1)

#define IMX_GST_PLUGIN_DEFINE(name, description, initfunc)\
  GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,\
      GST_VERSION_MINOR,\
      name.imx,\
      description,\
      initfunc,\
      VERSION,\
      IMX_GST_PLUGIN_LICENSE,\
      IMX_GST_PLUGIN_PACKAGE_NAME, IMX_GST_PLUGIN_PACKAGE_ORIG)

#endif
