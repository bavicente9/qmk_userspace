#!/usr/bin/env python3
"""Convierte varias imágenes a arrays C RGB565 para la pantalla Halcyon (135x240, vertical).

Las imágenes se recortan centradas para rellenar la pantalla y se guardan en un único
<salida>.c / <salida>.h con:
    GFX_<NOMBRE>_COUNT, GFX_<NOMBRE>_WIDTH, GFX_<NOMBRE>_HEIGHT
    const uint8_t *const gfx_<nombre>[GFX_<NOMBRE>_COUNT];
Se dibujan con qp_viewport() + qp_pixdata(). No hace falta el CLI de qmk.

Uso:
    python tools/img2qp.py keyboards/.../my_display/graphics/machi keyboards/.../graphics/src/*.png
"""
import argparse
import pathlib
import sys

from PIL import Image, ImageOps

LCD_W, LCD_H = 135, 240


def to_rgb565(img: Image.Image, little_endian: bool) -> bytes:
    raw = img.tobytes()
    out = bytearray()
    for i in range(0, len(raw), 3):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out += bytes((v & 0xFF, v >> 8)) if little_endian else bytes((v >> 8, v & 0xFF))
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output", help="ruta de salida sin extensión, p.ej. .../graphics/machi")
    ap.add_argument("inputs", nargs="+", help="imágenes de entrada (png/jpg/webp...)")
    ap.add_argument("--little-endian", action="store_true",
                    help="usar si los colores salen raros en la pantalla (rojo/azul cambiados o ruido)")
    args = ap.parse_args()

    out = pathlib.Path(args.output)
    name = out.name
    sym, up = f"gfx_{name}", f"GFX_{name.upper()}"
    size = LCD_W * LCD_H * 2

    arrays = []
    for n, path in enumerate(args.inputs):
        img = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
        if img.size != (LCD_W, LCD_H):
            img = ImageOps.fit(img, (LCD_W, LCD_H), method=Image.LANCZOS)
        data = to_rgb565(img, args.little_endian)
        rows = "\n".join("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 24]) + ","
                         for i in range(0, len(data), 24))
        # aligned(4): la surface lee los píxeles como uint16_t y el Cortex-M0+ del RP2040
        # da HardFault con accesos desalineados.
        arrays.append(f"// {pathlib.Path(path).name}\n"
                      f"static const uint8_t {sym}_{n}[{size}] __attribute__((aligned(4))) = {{\n{rows}\n}};\n")

    header = "// Generado por tools/img2qp.py. No editar a mano.\n"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.with_suffix(".h").write_text(
        header + "#pragma once\n#include <stdint.h>\n\n"
        f"#define {up}_COUNT {len(arrays)}\n#define {up}_WIDTH {LCD_W}\n#define {up}_HEIGHT {LCD_H}\n\n"
        f"extern const uint8_t *const {sym}[{up}_COUNT];\n",
        encoding="utf-8")
    out.with_suffix(".c").write_text(
        header + f'#include "{name}.h"\n\n' + "\n".join(arrays)
        + f"\nconst uint8_t *const {sym}[{up}_COUNT] = {{ "
        + ", ".join(f"{sym}_{n}" for n in range(len(arrays))) + " };\n",
        encoding="utf-8")
    print(f"OK: {len(arrays)} imágenes -> {out.with_suffix('.c')} ({len(arrays) * size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
