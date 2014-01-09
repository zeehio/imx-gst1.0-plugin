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

#include <string.h>
#include "gstvpuallocator.h"
#include "vpu_wrapper.h"

GST_DEBUG_CATEGORY_STATIC(vpu_allocator_debug);
#define GST_CAT_DEFAULT vpu_allocator_debug

static void gst_vpu_allocator_finalize(GObject *object);

static gboolean gst_vpu_alloc_phys_mem(GstAllocatorPhyMem *allocator, PhyMemBlock *memory);
static gboolean gst_vpu_free_phys_mem(GstAllocatorPhyMem *allocator, PhyMemBlock *memory);

G_DEFINE_TYPE(GstVpuAllocator, gst_vpu_allocator, GST_TYPE_ALLOCATOR_PHYMEM)

static void 
gst_vpu_mem_init(void)
{
	GstAllocator *allocator = g_object_new(gst_vpu_allocator_get_type(), NULL);
	gst_allocator_register(GST_VPU_ALLOCATOR_MEM_TYPE, allocator);
}

GstAllocator* 
gst_vpu_allocator_obtain(void)
{
	static GOnce dmabuf_allocator_once = G_ONCE_INIT;
	GstAllocator *allocator;

	g_once(&dmabuf_allocator_once, (GThreadFunc)gst_vpu_mem_init, NULL);

	allocator = gst_allocator_find(GST_VPU_ALLOCATOR_MEM_TYPE);
	if (allocator == NULL)
		GST_WARNING("No allocator named %s found", GST_VPU_ALLOCATOR_MEM_TYPE);

	return allocator;
}

static gboolean 
gst_vpu_alloc_phys_mem(G_GNUC_UNUSED GstAllocatorPhyMem *allocator, PhyMemBlock *memory)
{
	VpuDecRetCode ret;
	VpuMemDesc mem_desc;

	GST_DEBUG_OBJECT(allocator, "vpu allocator malloc size: %d\n", memory->size);
	memset(&mem_desc, 0, sizeof(VpuMemDesc));
	mem_desc.nSize = memory->size;
	ret = VPU_DecGetMem(&mem_desc);

	if (ret == VPU_DEC_RET_SUCCESS) {
		memory->size         = mem_desc.nSize;
		memory->paddr        = (guint8 *)(mem_desc.nPhyAddr);
		memory->vaddr         = (guint8 *)(mem_desc.nVirtAddr);
		return TRUE;
	} else
		return FALSE;
}

static gboolean 
gst_vpu_free_phys_mem(G_GNUC_UNUSED GstAllocatorPhyMem *allocator, PhyMemBlock *memory)
{
  VpuDecRetCode ret;
  VpuMemDesc mem_desc;

	GST_DEBUG_OBJECT(allocator, "vpu allocator free size: %d\n", memory->size);
  memset(&mem_desc, 0, sizeof(VpuMemDesc));
	mem_desc.nSize     = memory->size;
	mem_desc.nPhyAddr  = (unsigned long)(memory->paddr);
	mem_desc.nVirtAddr  = (unsigned long)(memory->vaddr);

	ret = VPU_DecFreeMem(&mem_desc);

	return (ret == VPU_DEC_RET_SUCCESS);
}

static void 
gst_vpu_allocator_class_init(GstVpuAllocatorClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GstAllocatorPhyMemClass *parent_class = GST_ALLOCATOR_PHYMEM_CLASS(klass);

	object_class->finalize       = GST_DEBUG_FUNCPTR(gst_vpu_allocator_finalize);
	parent_class->alloc_phymem = GST_DEBUG_FUNCPTR(gst_vpu_alloc_phys_mem);
	parent_class->free_phymem  = GST_DEBUG_FUNCPTR(gst_vpu_free_phys_mem);

	GST_DEBUG_CATEGORY_INIT(vpu_allocator_debug, "vpuallocator", 0, "VPU physical memory allocator");
}

static void 
gst_vpu_allocator_init(GstVpuAllocator *allocator)
{
	GstAllocator *base = GST_ALLOCATOR(allocator);
	base->mem_type = GST_VPU_ALLOCATOR_MEM_TYPE;
}

static void 
gst_vpu_allocator_finalize(GObject *object)
{
	GST_DEBUG_OBJECT(object, "shutting down VPU allocator");
	G_OBJECT_CLASS(gst_vpu_allocator_parent_class)->finalize(object);
}

