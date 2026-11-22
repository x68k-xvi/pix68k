#!/usr/bin/env python3
"""Patch the pinned build tree; source files are regenerated from its Git HEAD."""
from pathlib import Path
import subprocess
import sys

ra, launcher = map(Path, sys.argv[1:])

def original(path):
    return subprocess.run(["git", "-C", str(ra), "show", "HEAD:" + path],
                          check=True, capture_output=True, text=True).stdout

def once(source, old, new):
    if source.count(old) != 1:
        raise SystemExit("Unexpected pinned RetroArch source: " + old[:100])
    return source.replace(old, new)

platform = original("frontend/drivers/platform_psp.c").replace('"ux0:/data/retroarch/"', '"ux0:/data/pix68k/"')
for root in (ra, launcher):
    (root / "frontend/drivers/platform_psp.c").write_text(platform)

path = "gfx/drivers/vita2d_gfx.c"
source = original(path)
source = once(source, "&vita2d->mvp_no_rot);", "vita2d->mvp_no_rot.data);")
source = once(source, "memcpy_neon(&tex32[i*stride],&frame32[i*pitch],pitch*sizeof(uint32_t));",
              "memcpy_neon(&tex32[i*stride],&frame32[i*pitch],width*sizeof(uint32_t));")
source = once(source, "#include <vita2d.h>", """#include <vita2d.h>
#include <stdbool.h>
/* Private bridge: the packaged pix68k core and vita2d backend are paired. */
extern void pix68k_vita_gpu_frontend(bool ready) __attribute__((weak));
extern void *pix68k_vita_gpu_frame(void) __attribute__((weak));""")
# Offscreen indexed composition uses non-MSAA render targets. The pinned
# vita2d shaders inherit the context's sample count, so use the same mode for
# both offscreen and display surfaces. Pixel-art output does not need 4x MSAA.
source = once(source, "vita2d_init_advanced_with_msaa((1 * 1024 * 1024), SCE_GXM_MULTISAMPLE_4X,",
              "vita2d_init_advanced_with_msaa((1 * 1024 * 1024), SCE_GXM_MULTISAMPLE_NONE,")
source = once(source, "   vita2d_set_vblank_wait(video->vsync);", """   vita2d_set_vblank_wait(video->vsync);
   if (pix68k_vita_gpu_frontend) pix68k_vita_gpu_frontend(!video->is_threaded
#ifdef HAVE_VIDEO_FILTER
         && string_is_empty(config_get_ptr()->paths.path_softfilter_plugin)
#endif
         );""")
source = once(source, "   vita2d_fini();", "   if (pix68k_vita_gpu_frontend) pix68k_vita_gpu_frontend(false);\n   vita2d_fini();")
start = source.index("static bool vita2d_frame(")
end = source.index("\nstatic void vita2d_set_nonblock_state", start)
frame = source[start:end]
frame = once(frame, "   if (frame)\n", """   /* Keep offscreen and onscreen vertices alive in the same pool. */
   vita2d_pool_reset();
   vita2d_texture *pix68k_gpu_texture = pix68k_vita_gpu_frame ? (vita2d_texture*)pix68k_vita_gpu_frame() : NULL;
   if (pix68k_gpu_texture)
   {
      if (vita->texture)
      {
         vita2d_wait_rendering_done();
         vita2d_free_texture(vita->texture);
         vita->texture = NULL;
      }
      vita->width = width;
      vita->height = height;
      vita2d_texture_set_filters(pix68k_gpu_texture, vita->tex_filter, vita->tex_filter);
   }
   if (frame && !pix68k_gpu_texture)
""")
frame = once(frame, "   vita2d_start_drawing();", """   vita2d_start_drawing_advanced(NULL, 0);
   if (pix68k_gpu_texture)
      vita2d_set_viewport(0, 0, temp_width, temp_height);""")
draw_start = frame.index("   if (vita->texture)\n   {\n      if (vita->fullscreen)")
draw_end = frame.index("   if (vita->menu.active)", draw_start)
draw = frame[draw_start:draw_end].replace("vita->texture", "pix68k_display_texture")
draw = "   vita2d_texture *pix68k_display_texture = pix68k_gpu_texture ? pix68k_gpu_texture : vita->texture;\n" + draw
frame = frame[:draw_start] + draw + frame[draw_end:]
source = source[:start] + frame + source[end:]
(ra / path).write_text(source)

path = "deps/libvita2d/source/vita2d_texture.c"
source = original(path)
source = once(source, "const int tex_size =  w * h * tex_format_to_bytespp(format);",
              "const int tex_size = ALIGN(w, 8) * h * tex_format_to_bytespp(format);")
(ra / path).write_text(source)

# Raw screenshots consume the canonical CPU cache. Resolve only on demand,
# avoiding a CPU color-conversion pass on every displayed GPU frame.
path = "tasks/task_screenshot.c"
source = original(path)
start = source.index("static bool take_screenshot_raw(")
body = source.index("{", start)
source = source[:body + 1] + """
#ifdef VITA
   extern void pix68k_vita_gpu_resolve(void) __attribute__((weak));
   if (pix68k_vita_gpu_resolve) pix68k_vita_gpu_resolve();
#endif
""" + source[body + 1:]
(ra / path).write_text(source)
