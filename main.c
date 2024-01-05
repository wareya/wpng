#ifdef TEST_VS_LIBPNG
#include <png.h>
#endif

#include "wpng_read.h"
#include "wpng_write.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>

// todo:
// - text chunks
// - unknown chunk reading callback
// - chunk writing hooks

int main(int argc, char ** argv)
{
    defl_compute_crc32(0, 0, 0);
    
    if (argc < 2)
    {
        puts("error: need input file argument");
        return 0;
    }
    
    if (1)
    {
        //FILE * f = fopen("unifont-jp.png", "rb");
        //FILE * f = fopen("cc0_photo.png", "rb");
        //FILE * f = fopen("4_bit.png", "rb");
        //FILE * f = fopen("16bit.png", "rb");
        //FILE * f = fopen("48bit.png", "rb");
        //FILE * f = fopen("tests/basn0g01.png", "rb");
        //FILE * f = fopen("tests/basn3p02.png", "rb");
        //FILE * f = fopen("tests/basn2c08.png", "rb");
        //FILE * f = fopen("tests/basn3p08.png", "rb");
        //FILE * f = fopen("tests/basn2c16.png", "rb");
        //FILE * f = fopen("tests/basi0g01.png", "rb");
        //FILE * f = fopen("tests/basi3p02.png", "rb");
        //FILE * f = fopen("tests/basi2c08.png", "rb");
        //FILE * f = fopen("tests/basi3p08.png", "rb");
        //FILE * f = fopen("tests/basi2c16.png", "rb");
        printf("loading %s\n", argv[1]);
        FILE * f = fopen(argv[1], "rb");
        if (!f)
        {
            puts("error: invalid filename");
            return 0;
        }
        //FILE * f = fopen("Grayscale_2bit_palette_sample_image.png", "rb");
        //FILE * f = fopen("0-ufeff_tiles_v2.png", "rb");
        
        fseek(f, 0, SEEK_END);
        size_t file_len = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        uint8_t * raw_data = (uint8_t *)malloc(file_len);
        fread(raw_data, file_len, 1, f);
        byte_buffer in_buf = {raw_data, file_len, file_len, 0};
        
        fclose(f);
        
        wpng_load_output output;
        memset(&output, 0, sizeof(wpng_load_output));
        
#ifdef TEST_VS_LIBPNG
        wpng_load(&in_buf, WPNG_READ_FORCE_8BIT, &output);
#else
        wpng_load(&in_buf, 0, &output);
#endif
        
#ifdef TEST_VS_LIBPNG
        // skip test if original image has gamma and was 16 bit
        // also skip test if original image didn't have gamma or srgb, and was 16 bit (libpng erraneously interprets such images as linear)
        if (!(output.was_16bit && output.gamma != -1.0) && !(output.was_16bit && output.gamma == -1.0 && !output.was_srgb))
        {
            png_image image;
            memset(&image, 0, sizeof(image));
            image.version = PNG_IMAGE_VERSION;
            
            assert(png_image_begin_read_from_memory(&image, raw_data, file_len) != 0);
            
            uint8_t components = output.bytes_per_pixel / (output.is_16bit + 1);
            if (components == 1)
                image.format = PNG_FORMAT_GRAY;
            if (components == 2)
                image.format = PNG_FORMAT_GA;
            if (components == 3)
                image.format = PNG_FORMAT_RGB;
            if (components == 4)
                image.format = PNG_FORMAT_RGBA;

            png_bytep buffer;
            size_t libpng_size = PNG_IMAGE_SIZE(image);
            if (libpng_size != output.size)
            {
                printf("%zu %zu (%d (%d %d))\n", libpng_size, output.size, components, output.bytes_per_pixel, output.is_16bit + 1);
                assert(libpng_size == output.size);
            }
            buffer = (uint8_t *)malloc(libpng_size);

            assert(png_image_finish_read(&image, NULL, buffer, 0, NULL) != 0);
            
            size_t good_count = 0;
            if (output.gamma == -1.0)
            {
                if (output.was_16bit)
                {
                    while (good_count < output.size && abs((int16_t)(uint16_t)output.data[good_count] - (int16_t)(uint16_t)buffer[good_count]) <= 0)
                        good_count += 1;
                }
                else
                {
                    while (good_count < output.size && output.data[good_count] == buffer[good_count])
                        good_count += 1;
                }
            }
            else
            {
                while (good_count < output.size && abs((int16_t)(uint16_t)output.data[good_count] - (int16_t)(uint16_t)buffer[good_count]) <= 2)
                    good_count += 1;
            }
            if (good_count != output.size)
            {
                for (size_t i = 0; i < libpng_size && i < 512; i += 1)
                    printf("%02X ", buffer[i]);
                puts("");
                for (size_t i = 0; i < libpng_size && i < 512; i += 1)
                    printf("%02X ", output.data[i]);
                puts("");
                
                printf("%d\n", output.is_16bit);
                
                printf("%d\n", components);
                printf("%zu %zu\n", good_count, output.size);
                printf("vals: %02X %02X\n", output.data[good_count], buffer[good_count]);
                //assert(good_count == output.size);
            }
            
            free(buffer);
        }
#endif // TEST_VS_LIBPNG
        
        if (output.error == 0)
        {
            puts("writing out.png");
            
            uint32_t width = output.width;
            uint32_t height = output.height;
            size_t bytes_per_scanline = output.bytes_per_scanline;
            uint8_t bpp = output.bytes_per_pixel;
            uint8_t is_16bit = output.is_16bit;
            uint8_t * image_data = output.data;
            
            byte_buffer out = wpng_write(width, height, bpp, is_16bit, image_data, bytes_per_scanline, WPNG_WRITE_ALLOW_PALLETIZATION, 9);
            
            FILE * f2 = fopen("out.png", "wb");
            fwrite(out.data, out.len, 1, f2);
            fclose(f2);
        }
        else
        {
            printf("error: %d\n", output.error);
        }
    }
    
	return 0;
}
