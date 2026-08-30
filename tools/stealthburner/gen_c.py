"""Emit src/gif/<name>.c -- the raw GIF bytes wrapped in an lv_img_dsc_t.

LVGL stores these as LV_IMG_CF_RAW_CHROMA_KEYED: the array is the .gif file
verbatim, decoded at runtime by LVGL's own GIF decoder (LV_USE_GIF). Layout
matches the surrounding generated files: 13 bytes per line.

    python3 build_gifs.py && python3 gen_c.py
"""
import pathlib
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
DST  = HERE.parents[1] / 'src' / 'gif'

TMPL = '''#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif


#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef {ATTR}
#define {ATTR}
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST {ATTR} uint8_t {n}_map[] = {{
{body}
}};

const lv_img_dsc_t {n} = {{
  .header.cf = LV_IMG_CF_RAW_CHROMA_KEYED,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = {w},
  .header.h = {h},
  .data_size = {size},
  .data = {n}_map,
}};
'''

for n in ('gif_probing', 'gif_qgling'):
    src = HERE / f'{n}.gif'
    data = src.read_bytes()
    w, h = Image.open(src).size
    lines = []
    for i in range(0, len(data), 13):
        chunk = data[i:i + 13]
        tail = '' if i + 13 >= len(data) else ', '
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + tail)
    (DST / f'{n}.c').write_text(TMPL.format(
        n=n, ATTR=f'LV_ATTRIBUTE_IMG_{n.upper()}',
        body='\n'.join(lines), w=w, h=h, size=len(data)))
    print(f'{n}.c  {w}x{h}  data_size={len(data)}')
