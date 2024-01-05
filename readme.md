# WPNG

WPNG is a header-only PNG implementation written in C99/C++11. It supports decoding all standard formats, and encoding non-interlaced images with optional automatic palettization (if the image only has 256 or fewer colors).

## Compliance

WPNG's decoder produces bit-for-bit identical output when compared to libpng, for PngSuite and for libpng's test images, with the following exceptions:

- 16-bit gamma corrected images (non-srgb) -- the gamma correction libpng performs with these images is based on a LUT, and the LUT is inaccurate; WPNG mostly matches libpng on these images, except it's different by around 2%
- 16-bit images with no color metadata -- libpng interprets these as being linear and applies sRGB correction to them, even though the PNG spec never says to do this and implies throughout that 16-bit data is linearly proportional to 8-bit data
- libpng decodes gamma-corrected images (i.e. images that only have a gAMA chunk and no other colorspace metadata) into 2.2 gamma SRGB instead of into sRGB, even when you ask it for sRGB, which is wrong; so such images are different from libpng by a few value levels in WPNG.

