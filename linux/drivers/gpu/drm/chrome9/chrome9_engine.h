/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 Chrome9 contributors */
#ifndef __CHROME9_ENGINE_H__
#define __CHROME9_ENGINE_H__

#include <linux/types.h>

struct chrome9_device;
struct chrome9_sf_mapping;
struct drm_device;
struct drm_file;
struct drm_gem_object;

int chrome9_engine_init(struct chrome9_device *cdev);
void chrome9_engine_fini(struct chrome9_device *cdev);
bool chrome9_engine_ready(const struct chrome9_device *cdev);
bool chrome9_textured_triangle_run_enabled(void);
bool chrome9_textured_triangle_mip_enabled(void);
bool chrome9_rhw_chain_enabled(void);
bool chrome9_ps_add_enabled(void);
int chrome9_sf_pin(struct chrome9_device *cdev, struct drm_gem_object *obj,
		   struct chrome9_sf_mapping **mapping);
void chrome9_sf_unpin(struct chrome9_sf_mapping *mapping);
u32 chrome9_sf_mapping_base(const struct chrome9_sf_mapping *mapping);
void chrome9_sf_sync_for_device(struct chrome9_sf_mapping *mapping);
int chrome9_engine_display_begin(struct chrome9_device *cdev);
void chrome9_engine_display_end(struct chrome9_device *cdev);
int chrome9_fixed_quad_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file);
int chrome9_draw_triangle_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file);
int chrome9_draw_triangle_blend_ioctl(struct drm_device *drm, void *data,
				      struct drm_file *file);
int chrome9_draw_triangle_d16_ioctl(struct drm_device *drm, void *data,
				    struct drm_file *file);
int chrome9_draw_triangle_d16_blend_ioctl(struct drm_device *drm, void *data,
					  struct drm_file *file);
int chrome9_draw_triangle_batch_ioctl(struct drm_device *drm, void *data,
				       struct drm_file *file);
int chrome9_draw_triangle_d16_rhw_batch_ioctl(struct drm_device *drm,
					       void *data,
					       struct drm_file *file);
int chrome9_draw_triangle_d16_rhw_run_ioctl(struct drm_device *drm,
					     void *data,
					     struct drm_file *file);
int chrome9_draw_triangle_d16_rhw_chain_ioctl(struct drm_device *drm,
					       void *data,
					       struct drm_file *file);
int chrome9_draw_program_triangle_d16_rhw_batch_ioctl(
		struct drm_device *drm, void *data, struct drm_file *file);
int chrome9_draw_fragment_program_ioctl(struct drm_device *drm, void *data,
					struct drm_file *file);
int chrome9_draw_textured_triangle_ioctl(struct drm_device *drm, void *data,
					 struct drm_file *file);
int chrome9_draw_textured_triangle_batch_ioctl(struct drm_device *drm,
						void *data,
						struct drm_file *file);
int chrome9_draw_textured_triangle_run_ioctl(struct drm_device *drm,
					     void *data,
					     struct drm_file *file);
int chrome9_draw_textured_triangle_mip_ioctl(struct drm_device *drm,
					     void *data,
					     struct drm_file *file);
int chrome9_copy_rect_ioctl(struct drm_device *drm, void *data,
			    struct drm_file *file);
int chrome9_draw_textured_triangle_d16_ioctl(struct drm_device *drm, void *data,
					     struct drm_file *file);
int chrome9_draw_textured_triangle_d16_blend_ioctl(struct drm_device *drm,
						   void *data,
						   struct drm_file *file);
int chrome9_draw_two_textured_triangle_ioctl(struct drm_device *drm,
					      void *data,
					      struct drm_file *file);
int chrome9_draw_two_textured_triangle_d16_ioctl(struct drm_device *drm,
						 void *data,
						 struct drm_file *file);
int chrome9_draw_two_textured_triangle_d16_blend_ioctl(
		struct drm_device *drm, void *data, struct drm_file *file);
int chrome9_draw_textured_triangle_blend_ioctl(struct drm_device *drm,
					       void *data,
					       struct drm_file *file);

#endif
