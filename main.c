#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "inflate.h"
#include "deflate.h"

/*
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
*/

#include "buffers.h"

uint8_t paeth_get_ref(uint8_t * image_data, uint32_t width, uint32_t x, uint32_t y, uint8_t bpp)
{
    int16_t left   = x >= bpp ? image_data[y * width * bpp + x - bpp] : 0;
    int16_t up     = y >    0 ? image_data[(y - 1) * width * bpp + x] : 0;
    int16_t upleft = y >    0 &&
                     x >= bpp ? image_data[(y - 1) * width * bpp + x - bpp] : 0;
    
    int16_t lin = left + up - upleft;
    int16_t diff_left   = abs(lin - left);
    int16_t diff_up     = abs(lin - up);
    int16_t diff_upleft = abs(lin - upleft);
    
    int16_t ref = 0;
    if (diff_left <= diff_up && diff_left <= diff_upleft)
        ref = left;
    else if (diff_up <= diff_upleft)
        ref = up;
    else
        ref = upleft;
    
    return ref;
}

void wpng_write(const char * filename, uint32_t width, uint32_t height, uint8_t bpp, uint8_t  * image_data, size_t bytes_per_scanline)
{
    byte_buffer out;
    memset(&out, 0, sizeof(byte_buffer));
    
    bytes_push(&out, (const uint8_t *)"\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8);
    
    // write header chunk
    
    bytes_push_int(&out, byteswap_int(13, 4), 4);
    size_t chunk_start = out.len;
    bytes_push(&out, (const uint8_t *)"IHDR", 4);
    bytes_push_int(&out, byteswap_int(width, 4), 4);
    bytes_push_int(&out, byteswap_int(height, 4), 4);
    byte_push(&out, 8);
    byte_push(&out, bpp == 1 ? 0 : bpp == 2 ? 4 : bpp == 3 ? 2 : bpp == 4 ? 6 : 0);
    byte_push(&out, 0); // compression method (deflate)
    byte_push(&out, 0); // filter method (adaptive x5)
    byte_push(&out, 0); // interlacing method (none)
    size_t chunk_size = out.len - chunk_start;
    bytes_push_int(&out, byteswap_int(defl_compute_crc32(&out.data[chunk_start], chunk_size, 0), 4), 4);
    
    // write IDAT chunks
    // first, collect pixel data
    byte_buffer pixel_data;
    memset(&pixel_data, 0, sizeof(pixel_data));
    uint64_t num_unfiltered = 0;
    for (size_t y = 0; y < height; y += 1)
    {
        // pick a filter based on the sum-of-absolutes heuristic
        size_t start = bytes_per_scanline * y;
        
        uint64_t sum_abs_null = 0;
        uint64_t sum_abs_left = 0;
        uint64_t sum_abs_top = -1;
        uint64_t sum_abs_avg = 0;
        uint64_t sum_abs_paeth = 0;
        
        uint64_t hit_vals[256] = {0};
        uint8_t most_common_val = 0;
        uint64_t most_common_count = 0;
        
        for (size_t x = 0; x < width * bpp; x++)
        {
            uint8_t val = image_data[start + x];
            hit_vals[val] += 1;
            if (hit_vals[val] > most_common_count)
            {
                most_common_count = hit_vals[val];
                most_common_val = val;
            }
        }
        
        for (size_t x = 0; x < width * bpp; x++)
            sum_abs_null += abs((int32_t)(int8_t)(image_data[start + x] - most_common_val));
        
        // bias in favor of storing unfiltered if enough (25%) earlier scanlines are stored unfiltered
        // this helps with deflate's lz77 pass
        if (num_unfiltered > y / height / 4)
            sum_abs_null /= 3;
        
        for (size_t x = 0; x < bpp; x++)
            sum_abs_left += abs((int32_t)(int8_t)(image_data[start + x]));
        for (size_t x = bpp; x < width * bpp; x++)
            sum_abs_left += abs((int32_t)(image_data[start + x]) - (int32_t)(image_data[start + x - bpp]));
        
        if (y > 0)
        {
            sum_abs_top = 0;
            for (size_t x = 0; x < width * bpp; x++)
                sum_abs_top += abs((int32_t)(image_data[start + x]) - (int32_t)(image_data[start - bytes_per_scanline + x]));
        }
        
        for (size_t x = 0; x < width * bpp; x++)
        {
            uint16_t up   = y >    0 ? image_data[start - bytes_per_scanline + x] : 0;
            uint16_t left = x >= bpp ? image_data[start + x - bpp] : 0;
            uint8_t avg = (up + left) / 2;
            sum_abs_avg += abs((int32_t)(image_data[start + x]) - (int32_t)(avg));
        }
        
        for (size_t x = 0; x < width * bpp; x++)
        {
            uint8_t ref = paeth_get_ref(image_data, width, x, y, bpp);
            sum_abs_paeth += abs((int32_t)(image_data[y * width * bpp + x]) - (int32_t)(ref));
        }
        
        if (sum_abs_null <= sum_abs_left && sum_abs_null <= sum_abs_top && sum_abs_null <= sum_abs_avg && sum_abs_null <= sum_abs_paeth)
        {
            num_unfiltered += 1;
            puts("picked filter mode 0");
            byte_push(&pixel_data, 0); // no filter
            bytes_push(&pixel_data, &image_data[start], width * bpp);
        }
        else if (sum_abs_left <= sum_abs_top && sum_abs_left <= sum_abs_avg && sum_abs_left <= sum_abs_paeth)
        {
            puts("picked filter mode 1");
            byte_push(&pixel_data, 1); // left filter
            for (size_t x = 0; x < bpp; x++)
                byte_push(&pixel_data, image_data[start + x]);
            for (size_t x = bpp; x < width * bpp; x++)
                byte_push(&pixel_data, image_data[start + x] - image_data[start + x - bpp]);
        }
        else if (sum_abs_top <= sum_abs_avg && sum_abs_top <= sum_abs_paeth)
        {
            puts("picked filter mode 2");
            byte_push(&pixel_data, 2); // top filter
            for (size_t x = 0; x < width * bpp; x++)
                byte_push(&pixel_data, image_data[start + x] - image_data[start - bytes_per_scanline + x]);
        }
        else if (sum_abs_avg <= sum_abs_paeth)
        {
            puts("picked filter mode 3");
            byte_push(&pixel_data, 3); // avg filter
            for (size_t x = 0; x < width * bpp; x++)
            {
                uint16_t up   = y >    0 ? image_data[start - bytes_per_scanline + x] : 0;
                uint16_t left = x >= bpp ? image_data[start + x - bpp] : 0;
                uint8_t avg = (up + left) / 2;
                byte_push(&pixel_data, image_data[start + x] - avg);
            }
        }
        else
        {
            puts("picked filter mode 4");
            byte_push(&pixel_data, 4); // paeth filter
            for (size_t x = 0; x < width * bpp; x++)
            {
                uint8_t ref = paeth_get_ref(image_data, width, x, y, bpp);
                byte_push(&pixel_data, image_data[start + x] - ref);
            }
        }
    }
    printf("raw pixel data byte count: %lld\n", pixel_data.len);
    bit_buffer pixel_data_comp = do_deflate(pixel_data.data, pixel_data.len, 12, 1);
    
    bytes_push_int(&out, byteswap_int(pixel_data_comp.buffer.len, 4), 4);
    chunk_start = out.len;
    bytes_push(&out, (const uint8_t *)"IDAT", 4);
    bytes_push(&out, pixel_data_comp.buffer.data, pixel_data_comp.buffer.len);
    chunk_size = out.len - chunk_start;
    bytes_push_int(&out, byteswap_int(defl_compute_crc32(&out.data[chunk_start], chunk_size, 0), 4), 4);
    
    bytes_push_int(&out, 0, 4);
    bytes_push(&out, (const uint8_t *)"IEND\xAE\x42\x60\x82", 8);
    
    FILE * f = fopen(filename, "wb");
    fwrite(out.data, out.len, 1, f);
    fclose(f);
    
    printf("bpp %d\n", bpp);
    
    puts("saved");
}

void wpng_load_and_save(byte_buffer * buf)
{
	if (buf->len < 8)
		return;
	if (memcmp(buf->data, "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8) != 0)
		return;
    buf->cur = 8;
    
    byte_buffer idat;
    memset(&idat, 0, sizeof(byte_buffer));
    
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bit_depth = 0;
    uint8_t color_type = 0;
    uint8_t interlacing = 0;
    
    while (buf->cur < buf->len)
    {
        uint32_t size = byteswap_int(bytes_pop_int(buf, 4), 4);
        
        char name[5] = {0};
        for (size_t i = 0; i < 4; i += 1)
            name[i] = byte_pop(buf);
        
        printf("found chunk: %s\n", name);
        
        size_t cur_start = buf->cur;
        
        if (memcmp(name, "IHDR", 4) == 0)
        {
            width = byteswap_int(bytes_pop_int(buf, 4), 4);
            height = byteswap_int(bytes_pop_int(buf, 4), 4);
            bit_depth = byte_pop(buf);
            color_type = byte_pop(buf);
            assert(byte_pop(buf) == 0); // 0
            assert(byte_pop(buf) == 0); // 0
            interlacing = byte_pop(buf);
        }
        else if (memcmp(name, "IDAT", 4) == 0)
            bytes_push(&idat, &buf->data[buf->cur], size);
        else if (memcmp(name, "IEND", 4) == 0)
            break;
        
        buf->cur = cur_start + size + 4;
    }
    
    bytes_reserve(&idat, idat.cap + 8);
    
    assert(bit_depth == 8);
    assert(color_type == 0 || color_type == 2 || color_type == 4 || color_type == 6);
    assert(interlacing == 0);
    
    uint8_t temp = color_type / 2;
    uint8_t bpp = (((temp & 1) << 1) + ((temp & 2) >> 1)) + 1;
    
    idat.cur = 0;
    int error = 0;
    byte_buffer dec = do_inflate(&idat, &error, 1); // decompresses into `dec` (declared earlier)
    dec.cur = 0;
    printf("%d\n", error);
    
    uint8_t * image_data = (uint8_t *)malloc(height * width * bpp);
    memset(image_data, 0, height * width * bpp);
    
    for (size_t y = 0; y < height; y++)
    {
        uint8_t filter_type = byte_pop(&dec);
        assert(dec.cur + width * bpp <= dec.len);
        memcpy(&image_data[y * width * bpp], &dec.data[dec.cur], width * bpp);
        dec.cur += width * bpp;
        
        if (filter_type == 1)
        {
            for (size_t x = bpp; x < width * bpp; x++)
                image_data[y * width * bpp + x] += image_data[y * width * bpp + x - bpp];
        }
        if (filter_type == 2 && y > 0)
        {
            for (size_t x = 0; x < width * bpp; x++)
                image_data[y * width * bpp + x] += image_data[(y - 1) * width * bpp + x];
        }
        if (filter_type == 3)
        {
            for (size_t x = 0; x < width * bpp; x++)
            {
                uint16_t up   = y >    0 ? image_data[(y - 1) * width * bpp + x] : 0;
                uint16_t left = x >= bpp ? image_data[y * width * bpp + x - bpp] : 0;
                uint8_t avg = (up + left) / 2;
                image_data[y * width * bpp + x] += avg;
            }
        }
        if (filter_type == 4)
        {
            for (size_t x = 0; x < width * bpp; x++)
            {
                uint8_t ref = paeth_get_ref(image_data, width, x, y, bpp);
                image_data[y * width * bpp + x] += ref;
            }
        }
    }
    
    wpng_write("out.png", width, height, bpp, image_data, width * bpp);
}

int main()
{
    defl_compute_crc32(0, 0, 0);
    
    if (1)
    {
        //FILE * f = fopen("unifont-jp.png", "rb");
        FILE * f = fopen("cc0_photo.png", "rb");
        //FILE * f = fopen("0-ufeff_tiles_v2.png", "rb");
        
        fseek(f, 0, SEEK_END);
        size_t file_len = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        uint8_t * raw_data = (uint8_t *)malloc(file_len);
        fread(raw_data, file_len, 1, f);
        byte_buffer in_buf = {raw_data, file_len, file_len, 0};
        
        fclose(f);
        
        wpng_load_and_save(&in_buf);
    }
    
	return 0;
}