/** @file T70 console cell scaling, independent of the GOP driver.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef CONSOLE_SCALE_H_
#define CONSOLE_SCALE_H_
STATIC UINTN
ConsoleScale (UINTN Width, UINTN Height)
{
#ifdef T70PRO_RAM_DIAGNOSTICS
  // Preserve the mandatory 80x25 console on smaller outputs.
  return (Width >= 80 * EFI_GLYPH_WIDTH * 2 &&
          Height >= 25 * EFI_GLYPH_HEIGHT * 2) ? 2 : 1;
#else
  (VOID)Width;
  (VOID)Height;
  return 1;
#endif
}

STATIC VOID
ConsoleDoublePixels (
  CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Source,
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Destination,
  UINTN Width, UINTN Height)
{
  UINTN X;
  UINTN Y;
  for (Y = 0; Y < Height * 2; Y++) {
    for (X = 0; X < Width * 2; X++) {
      Destination[Y * Width * 2 + X] = Source[(Y / 2) * Width + X / 2];
    }
  }
}
#endif
