# WPNG

WPNG is a public domain (CC0) header-only PNG library written in C99/C++11. It supports decoding all standard formats, and encoding non-interlaced images with optional automatic palettization (if the image only has 256 or fewer colors).

Has been fuzzed with Jackalope, but not extensively.

## TODO

- chunk read/write callbacks
- text chunks
- CLI regression automation (main.c has code to test against libpng, but it's invoked manually for now)

## Usage

Add all the `.h` files from this repository to your project, then include `wpng_write.h` and/or `wpng_read.h`.

In the future, a single-file version will be available.

## Usage

```c
        // READING:
        
        // raw_data is a (uint8_t *) pointing to raw PNG data; file_len is a size_t containing how many bytes there are in that data
        byte_buffer in_buf = {raw_data, file_len, file_len, 0};

        wpng_load_output output;
        memset(&output, 0, sizeof(wpng_load_output));
        wpng_load(&in_buf, 0 /* <- flags */, &output);

        // Supported flags:
        // WPNG_READ_SKIP_CRC = 1, // don't check chunk CRCs 
        // WPNG_READ_SKIP_CRITICAL_CHUNKS = 2, // skip unknown critical chunks
        // WPNG_READ_SKIP_GAMMA_CORRECTION = 4, // don't apply gamma correction
        // WPNG_READ_SKIP_IDAT_CRC = 8, // like chrome
        // WPNG_READ_ERROR_ON_BAD_ANCILLARY_CRC = 16, // treat chunks with bad CRCs like unknown chunks
        // WPNG_READ_FORCE_8BIT = 256, // convert 16-bit images to 8-bit on load
        
        // WRITING:
        
        byte_buffer out = wpng_write(width, height, bytes_per_pixel, is_16bit, image_data /* <- (uint8_t *) */, bytes_per_scanline, WPNG_WRITE_ALLOW_PALLETIZATION /* <- flags */, 9 /* <- DEFLATE compression quality */ );
        // PNG file data now resides in out.data (uint8_t *) and is out.len (size_t) bytes long

        // supported flags:
        // WPNG_WRITE_ALLOW_PALLETIZATION
```

## Documentation

```c
        // Layout of wpng_load_output:
        
        typedef struct {
            uint8_t * data;               // u8 array
            size_t size;                  // size of array
            size_t bytes_per_scanline;    // width
            uint32_t width;               // height
            uint32_t height;              // bytes per pixel
            float gamma;                  // is 16 bit or not (decoder output)
            uint8_t bytes_per_pixel;      // was originally 16 bit or not (original png file)
            uint8_t is_16bit;             // png file specified that it was srgb or not
            uint8_t was_16bit;            // scanline byte count
            uint8_t was_srgb;             // gamma (-1 if unset or srgb)
            uint8_t error;                
        } wpng_load_output;
        // NOTE: the decoder ALWAYS output srgb data, even if was_srgb is unset!

        // Decoder error codes:
        
        // 1 - chunk size error
        // 2 - buffer overflow
        // 3 - invalid chunk name
        // 4 - failed crc
        // 5 - chunk syntax error or invalid value within chunk
        // 6 - chunk ordering error
        // 7 - unknown critical chunk
        // 8 - missing mandatory chunk (IHDR/IDAT/IEND)
        // 9 - has chunk that's forbidden for given color format
        // 10 - missing contextual mandatory chunk (PLTE on indexed images)
        // 11 - invalid zlib data
        // 255 - not a png file
```

## Compliance

WPNG's decoder produces bit-for-bit identical output when compared to libpng, for PngSuite and for libpng's test images, with the following exceptions:

- 16-bit gamma corrected images (non-srgb) -- the gamma correction libpng performs with these images is based on a LUT, and the LUT is inaccurate; WPNG mostly matches libpng on these images, except it's different by around 2%
- 16-bit images with no color metadata -- libpng interprets these as being linear and applies sRGB correction to them, even though the PNG spec never says to do this and implies throughout that 16-bit data is linearly proportional to 8-bit data
- libpng decodes gamma-corrected images (i.e. images that only have a gAMA chunk and no other colorspace metadata) into 2.2 gamma SRGB instead of into sRGB, even when you ask it for sRGB, which is wrong; so such images are different from libpng by a few value levels in WPNG.

