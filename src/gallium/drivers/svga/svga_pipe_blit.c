/**********************************************************
 * Copyright 2008-2017 VMware, Inc.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************/

#include "svga_context.h"
#include "svga_debug.h"
#include "svga_cmd.h"
#include "svga_format.h"
#include "svga_resource_buffer.h"
#include "svga_resource_texture.h"
#include "svga_surface.h"

//#include "util/u_blit_sw.h"
#include "util/u_format.h"
#include "util/u_surface.h"

#define FILE_DEBUG_FLAG DEBUG_BLIT


/**
 * Build a struct pipe_blit_info object from the arguments used by the
 * pipe::resource_copy_region() function.
 */
static void
build_blit_info(struct pipe_resource *dst_tex,
                unsigned dst_level,
                unsigned dst_x,
                unsigned dst_y,
                unsigned dst_z,
                struct pipe_resource *src_tex,
                unsigned src_level,
                const struct pipe_box *src_box,
                struct pipe_blit_info *blit)
{
   memset(blit, 0, sizeof(*blit));

   blit->src.format = src_tex->format;
   blit->dst.format = dst_tex->format;

   blit->mask = util_format_get_mask(blit->dst.format);
   blit->filter = PIPE_TEX_FILTER_NEAREST;
   blit->src.resource = src_tex;
   blit->src.level = src_level;
   blit->dst.resource = dst_tex;
   blit->dst.level = dst_level;
   blit->src.box = *src_box;
   u_box_3d(dst_x, dst_y, dst_z, src_box->width, src_box->height,
            src_box->depth, &blit->dst.box);
}


/**
 * Copy an image between textures with the vgpu10 CopyRegion command.
 */
static void
copy_region_vgpu10(struct svga_context *svga, struct pipe_resource *src_tex,
                    unsigned src_x, unsigned src_y, unsigned src_z,
                    unsigned src_level, unsigned src_face,
                    struct pipe_resource *dst_tex,
                    unsigned dst_x, unsigned dst_y, unsigned dst_z,
                    unsigned dst_level, unsigned dst_face,
                    unsigned width, unsigned height, unsigned depth)
{
   enum pipe_error ret;
   uint32 srcSubResource, dstSubResource;
   struct svga_texture *dtex, *stex;
   SVGA3dCopyBox box;

   stex = svga_texture(src_tex);
   dtex = svga_texture(dst_tex);

   svga_surfaces_flush(svga);

   box.x = dst_x;
   box.y = dst_y;
   box.z = dst_z;
   box.w = width;
   box.h = height;
   box.d = depth;
   box.srcx = src_x;
   box.srcy = src_y;
   box.srcz = src_z;

   srcSubResource = src_face * (src_tex->last_level + 1) + src_level;
   dstSubResource = dst_face * (dst_tex->last_level + 1) + dst_level;

   ret = SVGA3D_vgpu10_PredCopyRegion(svga->swc,
                                      dtex->handle, dstSubResource,
                                      stex->handle, srcSubResource, &box);
   if (ret != PIPE_OK) {
      svga_context_flush(svga, NULL);
      ret = SVGA3D_vgpu10_PredCopyRegion(svga->swc,
                                         dtex->handle, dstSubResource,
                                         stex->handle, srcSubResource, &box);
      assert(ret == PIPE_OK);
   }

   /* Mark the texture subresource as defined. */
   svga_define_texture_level(dtex, dst_face, dst_level);

   /* Mark the texture subresource as rendered-to. */
   svga_set_texture_rendered_to(dtex, dst_face, dst_level);
}


/**
 * Fallback to the copy region utility which uses map/memcpy for the copy
 */
static void
copy_region_fallback(struct svga_context *svga, 
                     struct pipe_resource *dst_tex, unsigned dst_level,
                     unsigned dstx, unsigned dsty, unsigned dstz,
                     struct pipe_resource *src_tex, unsigned src_level,
                     const struct pipe_box *src_box)
{
   struct svga_winsys_screen *sws = svga_screen(svga->pipe.screen)->sws;

   SVGA_STATS_TIME_PUSH(sws, SVGA_STATS_TIME_COPYREGIONFALLBACK);
   util_resource_copy_region(&svga->pipe, dst_tex, dst_level, dstx,
                             dsty, dstz, src_tex, src_level, src_box);
   SVGA_STATS_TIME_POP(sws);
   (void) sws;
}


/**
 * For some texture types, we need to move the z (slice) coordinate
 * to the layer value.  For example, to select the z=3 slice of a 2D ARRAY
 * texture, we need to use layer=3 and set z=0.
 */
static void
adjust_z_layer(enum pipe_texture_target target,
               int z_in, unsigned *layer_out, unsigned *z_out)
{
   if (target == PIPE_TEXTURE_CUBE ||
       target == PIPE_TEXTURE_2D_ARRAY ||
       target == PIPE_TEXTURE_1D_ARRAY) {
      *layer_out = z_in;
      *z_out = 0;
   }
   else {
      *layer_out = 0;
      *z_out = z_in;
   }
}


/**
 * Are the given SVGA3D formats compatible, in terms of vgpu10's
 * PredCopyRegion() command?
 */
static bool
formats_compatible(const struct svga_screen *ss,
                   SVGA3dSurfaceFormat src_svga_fmt,
                   SVGA3dSurfaceFormat dst_svga_fmt)
{
   src_svga_fmt = svga_typeless_format(src_svga_fmt);
   dst_svga_fmt = svga_typeless_format(dst_svga_fmt);

   return src_svga_fmt == dst_svga_fmt;
}


/**
 * Check whether the blending is enabled or not
 */
static bool
is_blending_enabled(struct svga_context *svga,
                    const struct pipe_blit_info *blit)
{
   bool blend_enable = false;
   int i;
   if (svga->curr.blend) {
      if (svga->curr.blend->independent_blend_enable) {
         for (i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
            if (svga->curr.framebuffer.cbufs[i]->texture == blit->dst.resource) {
               if (svga->curr.blend->rt[i].blend_enable) {
                  blend_enable = true;
               }
               break;
            }
         }
      }
      else {
         if (svga->curr.blend->rt[0].blend_enable)
            blend_enable = true;
      }
   }
   return blend_enable;
}


/**
 * If GL_FRAMEBUFFER_SRGB is enabled, then output colorspace is
 * expected to be sRGB if blending is not enabled.
 * If GL_FRAMEBUFFER_SRGB is disabled, then we can use
 * copy_region_vgpu10()
 * Following table basically tells when copy_region_vgpu10 can be
 * used if GL_FRAMEBUFFER_SRGB is enabled.
 * ______________________________________________________________
 *  | src fmt     | dst_fmt   | blending  |Can use       |
 *  |             |           |           |copy_region   |
 * ______________________________________________________________
 *  | linear      | linear    |   N       |     Y        |
 *  | linear      | linear    |   Y       |     Y        |
 *  | linear      | sRGB      |   N       |     N        |
 *  | linear      | sRGB      |   Y       |     Y        |
 *  | sRGB        | linear    |   N       |     N        |
 *  | sRGB        | linear    |   Y       |     N        |
 *  | sRGB        | sRGB      |   N       |     Y        |
 *  | sRGB        | sRGB      |   Y       |     N        |
 * ______________________________________________________________
 *
 */
static bool
check_blending_and_srgb_cond(struct svga_context *svga,
                             const struct pipe_blit_info *blit)
{
   enum pipe_format sFmt = blit->src.format;
   enum pipe_format dFmt = blit->dst.format;

   if (is_blending_enabled(svga, blit)) {
      if (!util_format_is_srgb(blit->src.format))
         return true;
   }
   else {
      if (util_format_is_srgb(sFmt) && util_format_is_srgb(dFmt))
         return true;
      else if (!util_format_is_srgb(sFmt)){
         if (!util_format_is_srgb(dFmt))
            return true;
         else {
           /**
            * State tracker converts all sRGB src blit format
            * to linear if GL_FRAMEBUFFER_SRGB is disabled.
            * So if src resource format is sRGB and
            * blit format is linear then it means,
            * GL_FRAMEBUFFER_SRGB is disabled. In this case also
            * we can use copy_region_vgpu10().
            */

            if (util_format_is_srgb(blit->src.resource->format))
               return true;
         }
      }
   }
   return false;
}

/**
 * Do common checks for svga surface copy.
 */
static bool
can_blit_via_svga_copy_region(struct svga_context *svga,
                              const struct pipe_blit_info *blit_info)
{
   struct pipe_blit_info local_blit = *blit_info;

   /* First basic checks to catch incompatibilities in new or locally unchecked
    * struct pipe_blit_info members but bypass the format check here.
    * Also since util_can_blit_via_copy_region() requires a dimension match,
    * PIPE_FILTER_LINEAR should be equal to PIPE_FILTER_NEAREST.
    */
   local_blit.dst.format = local_blit.src.format;
   if (local_blit.filter == PIPE_TEX_FILTER_LINEAR)
      local_blit.filter = PIPE_TEX_FILTER_NEAREST;
   if (!util_can_blit_via_copy_region(&local_blit, TRUE))
      return false;

   /* For depth+stencil formats, copy with mask != PIPE_MASK_ZS is not
    * supported
    */
   if (util_format_is_depth_and_stencil(blit_info->src.format) &&
      blit_info->mask != (PIPE_MASK_ZS))
     return false;

   return check_blending_and_srgb_cond(svga, blit_info);
}


/**
 * The state tracker implements some resource copies with blits (for
 * GL_ARB_copy_image).  This function checks if we should really do the blit
 * with a VGPU10 CopyRegion command or software fallback (for incompatible
 * src/dst formats).
 */
static bool
can_blit_via_copy_region_vgpu10(struct svga_context *svga,
                                const struct pipe_blit_info *blit_info)
{
   struct svga_texture *dtex, *stex;

   /* can't copy between different resource types */
   if (svga_resource_type(blit_info->src.resource->target) !=
       svga_resource_type(blit_info->dst.resource->target))
      return false;

   stex = svga_texture(blit_info->src.resource);
   dtex = svga_texture(blit_info->dst.resource);

   if (!svga_have_vgpu10(svga))
      return false;

   if (stex->handle == dtex->handle)
      return false;

   return formats_compatible(svga_screen(svga->pipe.screen),
                             stex->key.format,
                             dtex->key.format);
}


/**
 * Check whether we can blit using the surface_copy command.
 */
static bool
can_blit_via_surface_copy(struct svga_context *svga,
                          const struct pipe_blit_info *blit_info)
{
   struct svga_texture *dtex, *stex;

   /* Mimic the format tests in util_can_blit_via_copy_region(), but
    * skip the other tests that have already been performed.
    */
   if (blit_info->src.format != blit_info->dst.format) {
      const struct util_format_description *src_desc, *dst_desc;

      src_desc = util_format_description(blit_info->src.resource->format);
      dst_desc = util_format_description(blit_info->dst.resource->format);

      if (blit_info->src.resource->format != blit_info->src.format ||
          blit_info->dst.resource->format != blit_info->dst.format ||
          !util_is_format_compatible(src_desc, dst_desc))
         return false;
   }

   if (svga->render_condition && blit_info->render_condition_enable)
      return false;

   /* can't copy between different resource types */
   if (svga_resource_type(blit_info->src.resource->target) !=
       svga_resource_type(blit_info->dst.resource->target))
      return false;

   stex = svga_texture(blit_info->src.resource);
   dtex = svga_texture(blit_info->dst.resource);

   if (stex->handle == dtex->handle)
      return false;

   /*
    * This is what we've been using before, but it can probably be
    * relaxed. The device checks are less stringent.
    */
   return (stex->b.b.format == dtex->b.b.format);
}


/**
 * Try region copy using one of the region copy commands
 */
static bool
try_copy_region(struct svga_context *svga,
                const struct pipe_blit_info *blit)
{
   unsigned src_face, src_z, dst_face, dst_z;

   if (!can_blit_via_svga_copy_region(svga, blit))
      return false;

   adjust_z_layer(blit->src.resource->target, blit->src.box.z,
                  &src_face, &src_z);

   adjust_z_layer(blit->dst.resource->target, blit->dst.box.z,
                  &dst_face, &dst_z);

   if (can_blit_via_copy_region_vgpu10(svga, blit)) {
      svga_toggle_render_condition(svga, blit->render_condition_enable, FALSE);

      copy_region_vgpu10(svga,
                         blit->src.resource,
                         blit->src.box.x, blit->src.box.y, src_z,
                         blit->src.level, src_face,
                         blit->dst.resource,
                         blit->dst.box.x, blit->dst.box.y, dst_z,
                         blit->dst.level, dst_face,
                         blit->src.box.width, blit->src.box.height,
                         blit->src.box.depth);

      svga_toggle_render_condition(svga, blit->render_condition_enable, TRUE);

      return true;
   }

   if (can_blit_via_surface_copy(svga, blit)) {
      struct svga_texture *stex = svga_texture(blit->src.resource);
      struct svga_texture *dtex = svga_texture(blit->dst.resource);

      svga_surfaces_flush(svga);

      svga_texture_copy_handle(svga,
                               stex->handle,
                               blit->src.box.x, blit->src.box.y, src_z,
                               blit->src.level, src_face,
                               dtex->handle,
                               blit->dst.box.x, blit->dst.box.y, dst_z,
                               blit->dst.level, dst_face,
                               blit->src.box.width, blit->src.box.height,
                               blit->src.box.depth);

      svga_define_texture_level(dtex, dst_face, blit->dst.level);
      svga_set_texture_rendered_to(dtex, dst_face, blit->dst.level);
      return true;
   }

   return false;
}


/**
 * A helper function to determine if the specified view format
 * is compatible with the surface format.
 * It is compatible if the view format is the same as the surface format,
 * or the associated svga format for the surface is a typeless format, or
 * the view format is an adjusted format for BGRX/BGRA resource.
 */
static bool
is_view_format_compatible(enum pipe_format surf_fmt,
                          SVGA3dSurfaceFormat surf_svga_fmt,
                          enum pipe_format view_fmt)
{
   if (surf_fmt == view_fmt || svga_format_is_typeless(surf_svga_fmt))
      return true;

   if ((surf_fmt == PIPE_FORMAT_B8G8R8X8_UNORM &&
        view_fmt == PIPE_FORMAT_B8G8R8A8_UNORM) ||
       (surf_fmt == PIPE_FORMAT_B8G8R8A8_UNORM &&
        view_fmt == PIPE_FORMAT_B8G8R8X8_UNORM))
      return true;

   return false;
}


/**
 * Try issuing a quad blit.
 */
static bool
try_blit(struct svga_context *svga, const struct pipe_blit_info *blit_info)
{
   struct svga_winsys_screen *sws = svga_screen(svga->pipe.screen)->sws;
   struct pipe_resource *src = blit_info->src.resource;
   struct pipe_resource *dst = blit_info->dst.resource;
   struct pipe_resource *newSrc = NULL;
   struct pipe_resource *newDst = NULL;
   bool can_create_src_view;
   bool can_create_dst_view;
   bool ret = true;
   struct pipe_blit_info blit = *blit_info;

   SVGA_STATS_TIME_PUSH(sws, SVGA_STATS_TIME_BLITBLITTER);
   
   /**
    * If format is srgb and blend is enabled then color values need
    * to be converted into linear format.
    */
   if (is_blending_enabled(svga, &blit))
      blit.src.format = util_format_linear(blit.src.format);

   /* Check if we can create shader resource view and
    * render target view for the quad blitter to work
    */
   can_create_src_view =
      is_view_format_compatible(src->format, svga_texture(src)->key.format,
                                blit.src.format);

   can_create_dst_view =
      is_view_format_compatible(dst->format, svga_texture(dst)->key.format,
                                blit.dst.format);

   if ((blit.mask & PIPE_MASK_S) ||
       ((!can_create_dst_view || !can_create_src_view)
        && !svga_have_vgpu10(svga))) {
      /* Can't do stencil blits with textured quad blitter */
      debug_warn_once("using software stencil blit");
      ret = false;
      goto done;
   }

   if (!util_blitter_is_blit_supported(svga->blitter, &blit)) {
      debug_printf("svga: blit unsupported %s -> %s\n",
                   util_format_short_name(blit.src.resource->format),
                   util_format_short_name(blit.dst.resource->format));
      ret = false;
      goto done;
   }

   /* XXX turn off occlusion and streamout queries */

   util_blitter_save_vertex_buffer_slot(svga->blitter, svga->curr.vb);
   util_blitter_save_vertex_elements(svga->blitter, (void*)svga->curr.velems);
   util_blitter_save_vertex_shader(svga->blitter, svga->curr.vs);
   util_blitter_save_geometry_shader(svga->blitter, svga->curr.user_gs);
   util_blitter_save_so_targets(svga->blitter, svga->num_so_targets,
                     (struct pipe_stream_output_target**)svga->so_targets);
   util_blitter_save_rasterizer(svga->blitter, (void*)svga->curr.rast);
   util_blitter_save_viewport(svga->blitter, &svga->curr.viewport);
   util_blitter_save_scissor(svga->blitter, &svga->curr.scissor);
   util_blitter_save_fragment_shader(svga->blitter, svga->curr.fs);
   util_blitter_save_blend(svga->blitter, (void*)svga->curr.blend);
   util_blitter_save_depth_stencil_alpha(svga->blitter,
                                         (void*)svga->curr.depth);
   util_blitter_save_stencil_ref(svga->blitter, &svga->curr.stencil_ref);
   util_blitter_save_sample_mask(svga->blitter, svga->curr.sample_mask);
   util_blitter_save_framebuffer(svga->blitter, &svga->curr.framebuffer);
   util_blitter_save_fragment_sampler_states(svga->blitter,
                     svga->curr.num_samplers[PIPE_SHADER_FRAGMENT],
                     (void**)svga->curr.sampler[PIPE_SHADER_FRAGMENT]);
   util_blitter_save_fragment_sampler_views(svga->blitter,
                     svga->curr.num_sampler_views[PIPE_SHADER_FRAGMENT],
                     svga->curr.sampler_views[PIPE_SHADER_FRAGMENT]);

   if (!can_create_src_view) {
      struct pipe_resource template;
      struct pipe_blit_info copy_region_blit;

      /**
       * If the source blit format is not compatible with the source resource
       * format, we will not be able to create a shader resource view.
       * In order to avoid falling back to software blit, we'll create
       * a new resource in the blit format, and use DXCopyResource to
       * copy from the original format to the new format. The new
       * resource will be used for the blit in util_blitter_blit().
       */
      template = *src;
      template.format = blit.src.format;
      newSrc = svga_texture_create(svga->pipe.screen, &template);
      if (newSrc == NULL) {
         debug_printf("svga_blit: fails to create temporary src\n");
         ret = false;
         goto done;
      }

      /* increment the mksStats for blitter with extra copy */
      SVGA_STATS_COUNT_INC(sws, SVGA_STATS_COUNT_BLITBLITTERCOPY);
      build_blit_info(newSrc,
                      blit.src.level, blit.src.box.x,
                      blit.src.box.y, blit.src.box.z,
                      blit.src.resource,
                      blit.src.level, &blit.src.box,
                      &copy_region_blit);
      if (!try_copy_region(svga, &copy_region_blit)) {
         debug_printf("svga: Source blit format conversion failed.\n");
         ret = false;
         goto done;
      }

      blit.src.resource = newSrc;
   }

   if (!can_create_dst_view) {
      struct pipe_resource template;

      /*
       * If the destination blit format is not compatible with the destination
       * resource format, we will not be able to create a render target view.
       * In order to avoid falling back to software blit, we'll create
       * a new resource in the blit format, and use DXPredCopyRegion
       * after the blit to copy from the blit format back to the resource
       * format.
       */
      template = *dst;
      template.format = blit.dst.format;
      newDst = svga_texture_create(svga->pipe.screen, &template);
      if (newDst == NULL) {
         debug_printf("svga_blit: fails to create temporary dst\n");
         ret = false;
         goto done;
      }

      blit.dst.resource = newDst;
   }

   svga_toggle_render_condition(svga, blit.render_condition_enable, FALSE);

   util_blitter_blit(svga->blitter, &blit);

   svga_toggle_render_condition(svga, blit.render_condition_enable, TRUE);

   if (blit.dst.resource != dst) {
      struct pipe_blit_info copy_region_blit;

      /* increment the mksStats for blitter with extra copy */
      SVGA_STATS_COUNT_INC(sws, SVGA_STATS_COUNT_BLITBLITTERCOPY);

      /*
       * A temporary resource was created for the blit, we need to
       * copy from the temporary resource back to the original destination.
       */
      build_blit_info(dst,
                      blit.dst.level, blit.dst.box.x,
                      blit.dst.box.y, blit.dst.box.z,
                      newDst,
                      blit.dst.level, &blit.dst.box,
                      &copy_region_blit);
      if (!try_copy_region(svga, &copy_region_blit)) {
         debug_printf("svga: Destination blit format conversion failed.\n");
         ret = false;
         goto done;
      }
   }

done:
   /* unreference the temporary resources if needed */
   pipe_resource_reference(&newDst, NULL);
   pipe_resource_reference(&newSrc, NULL);

   SVGA_STATS_TIME_POP(sws);  /* SVGA_STATS_TIME_BLITBLITTER */
   (void) sws;

   return ret;
}


/**
 * Try a cpu copy_region fallback.
 */
static bool
try_cpu_copy_region(struct svga_context *svga,
                    const struct pipe_blit_info *blit)
{
   if (util_can_blit_via_copy_region(blit, TRUE) ||
       util_can_blit_via_copy_region(blit, FALSE)) {

      if (svga->render_condition && blit->render_condition_enable) {
         debug_warning("CPU copy_region doesn't support "
                       "conditional rendering.\n");
         return false;
      }

      copy_region_fallback(svga, blit->dst.resource,
                           blit->dst.level,
                           blit->dst.box.x, blit->dst.box.y,
                           blit->dst.box.z, blit->src.resource,
                           blit->src.level, &blit->src.box);
      return true;
   }

   return false;
}


/**
 * The pipe::blit member.
 */
static void
svga_blit(struct pipe_context *pipe,
          const struct pipe_blit_info *blit)
{
   struct svga_context *svga = svga_context(pipe);
   struct svga_winsys_screen *sws = svga_screen(pipe->screen)->sws;

   if (!svga_have_vgpu10(svga) &&
       blit->src.resource->nr_samples > 1 &&
       blit->dst.resource->nr_samples <= 1 &&
       !util_format_is_depth_or_stencil(blit->src.resource->format) &&
       !util_format_is_pure_integer(blit->src.resource->format)) {
      debug_printf("svga: color resolve unimplemented\n");
      return;
   }

   SVGA_STATS_TIME_PUSH(sws, SVGA_STATS_TIME_BLIT);

   if (try_copy_region(svga, blit))
      goto done;

   if (try_blit(svga, blit))
      goto done;

   if (!try_cpu_copy_region(svga, blit))
      debug_printf("svga: Blit failed.\n");
   
done:
   SVGA_STATS_TIME_POP(sws);  /* SVGA_STATS_TIME_BLIT */
   (void) sws;
}


/**
 * The pipe::resource_copy_region member.
 */
static void
svga_resource_copy_region(struct pipe_context *pipe,
                          struct pipe_resource *dst_tex,
                          unsigned dst_level,
                          unsigned dstx, unsigned dsty, unsigned dstz,
                          struct pipe_resource *src_tex,
                          unsigned src_level,
                          const struct pipe_box *src_box)
{
   struct svga_context *svga = svga_context(pipe);
   struct svga_winsys_screen *sws = svga_screen(svga->pipe.screen)->sws;

   SVGA_STATS_TIME_PUSH(sws, SVGA_STATS_TIME_COPYREGION);

   if (dst_tex->target == PIPE_BUFFER && src_tex->target == PIPE_BUFFER) {
      /* can't copy within the same buffer, unfortunately */
      if (svga_have_vgpu10(svga) && src_tex != dst_tex) {
         enum pipe_error ret;
         struct svga_winsys_surface *src_surf;
         struct svga_winsys_surface *dst_surf;
         struct svga_buffer *dbuffer = svga_buffer(dst_tex);
         struct svga_buffer *sbuffer = svga_buffer(src_tex);

         src_surf = svga_buffer_handle(svga, src_tex, sbuffer->bind_flags);
         dst_surf = svga_buffer_handle(svga, dst_tex, dbuffer->bind_flags);

         ret = SVGA3D_vgpu10_BufferCopy(svga->swc, src_surf, dst_surf,
                                        src_box->x, dstx, src_box->width);
         if (ret != PIPE_OK) {
            svga_context_flush(svga, NULL);
            ret = SVGA3D_vgpu10_BufferCopy(svga->swc, src_surf, dst_surf,
                                           src_box->x, dstx, src_box->width);
            assert(ret == PIPE_OK);
         }

         dbuffer->dirty = TRUE;
      }
      else {
         /* use map/memcpy fallback */
         copy_region_fallback(svga, dst_tex, dst_level, dstx,
                              dsty, dstz, src_tex, src_level, src_box);
      }
   } else {
      struct pipe_blit_info blit;

      build_blit_info(dst_tex, dst_level, dstx, dsty, dstz,
                      src_tex, src_level, src_box, &blit);

      if (try_copy_region(svga, &blit))
         goto done;

      /* Blits are format-converting which is not what we want, so perform a
       * strict format-check.
       * FIXME: Need to figure out why srgb blits (tf2) and
       * 3D blits (piglit) are broken here. Perhaps we set up the
       * struct pipe_blit_info incorrectly.
       */
      if (src_tex->format == dst_tex->format &&
          !util_format_is_srgb(src_tex->format) &&
          svga_resource_type(src_tex->target) != SVGA3D_RESOURCE_TEXTURE3D &&
          try_blit(svga, &blit))
         goto done;

      copy_region_fallback(svga, dst_tex, dst_level, dstx, dsty, dstz,
                           src_tex, src_level, src_box);
   }

done:
   SVGA_STATS_TIME_POP(sws);
   (void) sws;
}


/**
 * The pipe::flush_resource member.
 */
static void
svga_flush_resource(struct pipe_context *pipe,
                    struct pipe_resource *resource)
{
}


/**
 * Setup the pipe blit, resource_copy_region and flush_resource members.
 */
void
svga_init_blit_functions(struct svga_context *svga)
{
   svga->pipe.resource_copy_region = svga_resource_copy_region;
   svga->pipe.blit = svga_blit;
   svga->pipe.flush_resource = svga_flush_resource;
}
