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

uint8_t paeth_get_ref_raw(int16_t left, int16_t up, int16_t upleft)
{
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
uint8_t paeth_get_ref(uint8_t * image_data, uint32_t bytes_per_scanline, uint32_t x, uint32_t y, uint8_t bpp)
{
    int16_t left   = x >= bpp ? image_data[y * bytes_per_scanline + x - bpp] : 0;
    int16_t up     = y >    0 ? image_data[(y - 1) * bytes_per_scanline + x] : 0;
    int16_t upleft = y >    0 &&
                     x >= bpp ? image_data[(y - 1) * bytes_per_scanline + x - bpp] : 0;
    
    return paeth_get_ref_raw(left, up, upleft);
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
        
        for (size_t x = 0; x < bytes_per_scanline; x++)
        {
            uint8_t val = image_data[start + x];
            hit_vals[val] += 1;
            if (hit_vals[val] > most_common_count)
            {
                most_common_count = hit_vals[val];
                most_common_val = val;
            }
        }
        
        for (size_t x = 0; x < bytes_per_scanline; x++)
            sum_abs_null += abs((int32_t)(int8_t)(image_data[start + x] - most_common_val));
        
        // bias in favor of storing unfiltered if enough (25%) earlier scanlines are stored unfiltered
        // this helps with deflate's lz77 pass
        if (num_unfiltered > y / height / 4)
            sum_abs_null /= 3;
        
        for (size_t x = 0; x < bpp; x++)
            sum_abs_left += abs((int32_t)(int8_t)(image_data[start + x]));
        for (size_t x = bpp; x < bytes_per_scanline; x++)
            sum_abs_left += abs((int32_t)(image_data[start + x]) - (int32_t)(image_data[start + x - bpp]));
        
        if (y > 0)
        {
            sum_abs_top = 0;
            for (size_t x = 0; x < bytes_per_scanline; x++)
                sum_abs_top += abs((int32_t)(image_data[start + x]) - (int32_t)(image_data[start - bytes_per_scanline + x]));
        }
        
        for (size_t x = 0; x < bytes_per_scanline; x++)
        {
            uint16_t up   = y >    0 ? image_data[start - bytes_per_scanline + x] : 0;
            uint16_t left = x >= bpp ? image_data[start + x - bpp] : 0;
            uint8_t avg = (up + left) / 2;
            sum_abs_avg += abs((int32_t)(image_data[start + x]) - (int32_t)(avg));
        }
        
        for (size_t x = 0; x < bytes_per_scanline; x++)
        {
            uint8_t ref = paeth_get_ref(image_data, bytes_per_scanline, x, y, bpp);
            sum_abs_paeth += abs((int32_t)(image_data[y * bytes_per_scanline + x]) - (int32_t)(ref));
        }
        
        if (sum_abs_null <= sum_abs_left && sum_abs_null <= sum_abs_top && sum_abs_null <= sum_abs_avg && sum_abs_null <= sum_abs_paeth)
        {
            num_unfiltered += 1;
            //puts("picked filter mode 0");
            byte_push(&pixel_data, 0); // no filter
            bytes_push(&pixel_data, &image_data[start], bytes_per_scanline);
        }
        else if (sum_abs_left <= sum_abs_top && sum_abs_left <= sum_abs_avg && sum_abs_left <= sum_abs_paeth)
        {
            //puts("picked filter mode 1");
            byte_push(&pixel_data, 1); // left filter
            for (size_t x = 0; x < bpp; x++)
                byte_push(&pixel_data, image_data[start + x]);
            for (size_t x = bpp; x < bytes_per_scanline; x++)
                byte_push(&pixel_data, image_data[start + x] - image_data[start + x - bpp]);
        }
        else if (sum_abs_top <= sum_abs_avg && sum_abs_top <= sum_abs_paeth)
        {
            //puts("picked filter mode 2");
            byte_push(&pixel_data, 2); // top filter
            for (size_t x = 0; x < bytes_per_scanline; x++)
                byte_push(&pixel_data, image_data[start + x] - image_data[start - bytes_per_scanline + x]);
        }
        else if (sum_abs_avg <= sum_abs_paeth)
        {
            //puts("picked filter mode 3");
            byte_push(&pixel_data, 3); // avg filter
            for (size_t x = 0; x < bytes_per_scanline; x++)
            {
                uint16_t up   = y >    0 ? image_data[start - bytes_per_scanline + x] : 0;
                uint16_t left = x >= bpp ? image_data[start + x - bpp] : 0;
                uint8_t avg = (up + left) / 2;
                byte_push(&pixel_data, image_data[start + x] - avg);
            }
        }
        else
        {
            //puts("picked filter mode 4");
            byte_push(&pixel_data, 4); // paeth filter
            for (size_t x = 0; x < bytes_per_scanline; x++)
            {
                uint8_t ref = paeth_get_ref(image_data, bytes_per_scanline, x, y, bpp);
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

void defilter(uint8_t * image_data, byte_buffer * dec, uint32_t width, uint32_t height, uint8_t interlace_layer, uint8_t bit_depth, uint8_t components)
{
    // interlace_layer:
    // 0: not interlaced
    // 1~7: adam7 interlace layers
    //  1 6 4 6 2 6 4 6
    //  7 7 7 7 7 7 7 7
    //  5 6 5 6 5 6 5 6
    //  7 7 7 7 7 7 7 7
    //  3 6 4 6 3 6 4 6
    //  7 7 7 7 7 7 7 7
    //  5 6 5 6 5 6 5 6
    //  7 7 7 7 7 7 7 7
    
    uint8_t y_inits[] = {0, 0, 0, 4, 0, 2, 0, 1};
    uint8_t y_gaps[] = {1, 8, 8, 8, 4, 4, 2, 2};
    uint8_t x_inits[] = {0, 0, 4, 0, 2, 0, 1, 0};
    uint8_t x_gaps[] = {1, 8, 8, 4, 4, 2, 2, 1};
    
    size_t y_init = y_inits[interlace_layer];
    size_t y_gap = y_gaps[interlace_layer];
    size_t x_init = x_inits[interlace_layer];
    size_t x_gap = x_gaps[interlace_layer];
    
    // note: bit_depth can only be less than 8 if there is only one component
    
    size_t bytes_per_scanline = (((size_t)width - x_init + x_gap - 1) / x_gap * bit_depth + 7) / 8 * components;
    //printf("%d %d %d %lld\n", width, bit_depth, components, bytes_per_scanline);
    size_t min_bpp = (bit_depth + 7) / 8;
    size_t output_bps = min_bpp * components * width;
    //printf("%lld %d %d %d\n", dec->len, output_bps, output_bps * (height - y_init + y_gap - 1) / y_gap, height);
    
    uint8_t * y_prev = (uint8_t *)malloc(bytes_per_scanline);
    uint8_t * y_prev_next = (uint8_t *)malloc(bytes_per_scanline);
    memset(y_prev, 0, bytes_per_scanline);
    memset(y_prev_next, 0, bytes_per_scanline);
    
    //printf("%d\n", min_bpp * components);
    
    size_t scanline_count = ((size_t)height - y_init + y_gap - 1) / y_gap;
    //printf("%lld %lld %lld\n", scanline_count, scanline_count * (bytes_per_scanline + 1), dec->len);
    assert(scanline_count * (bytes_per_scanline + 1) <= dec->len);
    
    for (size_t y = y_init; y < height; y += y_gap)
    {
        // double buffered y_prev
        uint8_t * temp = y_prev;
        y_prev = y_prev_next;
        y_prev_next = temp;
        
        uint8_t filter_type = byte_pop(dec);
        //printf("%lld %lld\n", dec->cur + bytes_per_scanline, dec->len);
        assert(dec->cur + bytes_per_scanline <= dec->len);
        
        //printf("at y %d ", y);
        //if (filter_type == 0)
        //    puts("filter mode 0");
        //if (filter_type == 1)
        //    puts("filter mode 1");
        //if (filter_type == 2)
        //    puts("filter mode 2");
        //if (filter_type == 3)
        //    puts("filter mode 3");
        //if (filter_type == 4)
        //    puts("filter mode 4");
        
        for (size_t x = 0; x < bytes_per_scanline; x += 1)
        {
            size_t cur = dec->cur;
            uint8_t byte = byte_pop(dec);
            int16_t left = x >= min_bpp * components ? y_prev_next[x - min_bpp * components] : 0;
            int16_t up   = y_prev[x];
            int16_t upleft = x >= min_bpp * components ? y_prev[x - min_bpp * components] : 0;
            
            if (filter_type == 1)
                byte += left;
            if (filter_type == 2)
                byte += up;
            if (filter_type == 3)
                byte += (up + left) / 2;
            if (filter_type == 4)
                byte += paeth_get_ref_raw(left, up, upleft);
            
            // note: bits_per_pixel can only be less than 8 if there is only one component
            if (bit_depth < 8)
            {
                for (size_t i = 0; i < 8 / bit_depth; i++)
                {
                    uint8_t val = byte >> (8 - (bit_depth * (i + 1)));
                    val &= (1 << bit_depth) - 1;
                    image_data[y * output_bps + (x * 8 / bit_depth + i) * x_gap + x_init] = val;
                }
            }
            else
            {
                size_t true_x = x / (min_bpp * components) * min_bpp * components;
                size_t leftover_x = x - true_x;
                size_t x_out = true_x * x_gap + leftover_x + x_init * min_bpp * components;
                image_data[y * output_bps + x_out] = byte;
            }
            
            y_prev_next[x] = byte;
        }
    }
    
    free(y_prev);
    free(y_prev_next);
}

// TODO: support loading indexed PNGs
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
    
    uint8_t palette[1024] = {0};
    uint16_t palette_size = 0;
    uint8_t has_idat = 0;
    uint8_t has_iend = 0;
    uint8_t has_trns = 0;
    
    uint32_t transparent_r = 0xFFFFFFFF;
    uint32_t transparent_g = 0xFFFFFFFF;
    uint32_t transparent_b = 0xFFFFFFFF;
    
    uint64_t chunk_count = 0;
    
    uint8_t prev_was_idat = 0; // multiple idat chunks must be consecutive
    while (buf->cur < buf->len)
    {
        chunk_count += 1;
        
        uint32_t size = byteswap_int(bytes_pop_int(buf, 4), 4);
        
        char name[5] = {0};
        for (size_t i = 0; i < 4; i += 1)
            name[i] = byte_pop(buf);
        
        printf("found chunk: %s\n", name);
        
        size_t cur_start = buf->cur;
        if (memcmp(name, "IHDR", 4) == 0)
        {
            assert(chunk_count == 1); // must be first
            assert(width == 0 && height == 0); // must not have multiple
            
            width = byteswap_int(bytes_pop_int(buf, 4), 4);
            height = byteswap_int(bytes_pop_int(buf, 4), 4);
            
            bit_depth = byte_pop(buf);
            assert(bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8 || bit_depth == 16);
            color_type = byte_pop(buf);
            assert(color_type <= 6 && color_type != 1 && color_type != 5);
            
            assert(byte_pop(buf) == 0); // compression method, must always be 0 for PNGs
            assert(byte_pop(buf) == 0); // filter method, must always be 0 for PNGs
            interlacing = byte_pop(buf);
            
            // FIXME: not supported yet
            //assert(interlacing == 0);
        }
        else if (memcmp(name, "IDAT", 4) == 0)
        {
            assert(!has_idat || prev_was_idat); // multiple idat chunks must be consecutive
            bytes_push(&idat, &buf->data[buf->cur], size);
            has_idat = 1;
        }
        else if (memcmp(name, "PLTE", 4) == 0)
        {
            assert(!palette_size); // must not have multiple
            assert(!has_idat); // must precede the first idat chunk
            assert(!has_trns); // must not come after trns chunk
            
            assert(size % 3 == 0); // item count must be an integer
            assert(size > 0); // minimum allowed item count is 1
            assert(size <= 768); // maximum allowed item count is 256
            for (size_t i = 0; i < size / 3; i += 1)
            {
                palette[i * 4 + 0] = byte_pop(buf);
                palette[i * 4 + 1] = byte_pop(buf);
                palette[i * 4 + 2] = byte_pop(buf);
                palette[i * 4 + 3] = 0xFF;
            }
            palette_size = size / 3;
        }
        else if (memcmp(name, "tRNS", 4) == 0)
        {
            assert(!has_trns); // must not have multiple
            assert(!has_idat); // must precede the first idat chunk
            
            assert(color_type == 0 || color_type == 2 || color_type == 3);
            // indexed
            if (color_type == 3)
            {
                assert(size <= palette_size); // must not contain more entries than palette entries (but is allowed to contain less)
                for (size_t i = 0; i < size; i += 1)
                    palette[i * 4 + 3] = byte_pop(buf);
            }
            // grayscale
            if (color_type == 0)
            {
                assert(size == 2);
                uint16_t val = byteswap_int(bytes_pop_int(buf, 2), 2);
                // ... "(If the image bit depth is less than 16, the least significant bits are used and the others are 0.)`
                // interpreting the above language from the PNG spec as a hard requirement
                assert(val < (1 << bit_depth));
                transparent_r = val;
                transparent_g = val;
                transparent_b = val;
            }
            // rgb
            if (color_type == 2)
            {
                assert(size == 6);
                uint16_t val_r = byteswap_int(bytes_pop_int(buf, 2), 2);
                assert(val_r < (1 << bit_depth));
                uint16_t val_g = byteswap_int(bytes_pop_int(buf, 2), 2);
                assert(val_g < (1 << bit_depth));
                uint16_t val_b = byteswap_int(bytes_pop_int(buf, 2), 2);
                assert(val_b < (1 << bit_depth));
                transparent_r = val_r;
                transparent_g = val_g;
                transparent_b = val_b;
            }
            has_trns = 1;
        }
        else if (memcmp(name, "IEND", 4) == 0)
        {
            has_iend = 1;
            break;
        }
        
        prev_was_idat = (memcmp(name, "IDAT", 4) == 0);
        
        assert(width != 0 && height != 0); // header must exist and give valid width/height values
        
        buf->cur = cur_start + size + 4;
    }
    assert(has_idat);
    assert(has_iend);
    assert(width != 0 && height != 0); // header must exist and give valid width/height values
    
    palette[0] = palette[0];
    transparent_r = transparent_r;
    transparent_g = transparent_g;
    transparent_b = transparent_b;
    
    if (color_type == 4 || color_type == 6)
        assert(has_trns == 0);
    if (color_type == 0 || color_type == 4)
        assert(has_trns == 0);
    
    uint8_t temp[] = {1, 0, 3, 1, 2, 0, 4};
    uint8_t components = temp[color_type];
    
    uint8_t bpp = components * ((bit_depth + 7) / 8);
    size_t bytes_per_scanline = width * bpp;
    
    idat.cur = 0;
    int error = 0;
    byte_buffer dec = do_inflate(&idat, &error, 1); // decompresses into `dec` (declared earlier)
    dec.cur = 0;
    printf("%d\n", error);
    if (error != 0)
    {
        for (size_t i = 0; i < dec.len; i += 1)
            printf("%02X ", dec.data[i]);
        puts("");
    }
    assert(error == 0);
    
    uint8_t * image_data = (uint8_t *)malloc(height * bytes_per_scanline);
    memset(image_data, 0, height * bytes_per_scanline);
    
    if (!interlacing)
        defilter(image_data, &dec, width, height, 0, bit_depth, components);
    else
    {
        for (uint8_t i = 1; i <= 7; i += 1)
            defilter(image_data, &dec, width, height, i, bit_depth, components);
    }
    
    // convert to Y/YA/RGB/RGBA if needed
    
    uint8_t out_bpp = components + has_trns;
    if (palette_size)
        out_bpp = 3 + has_trns;
    
    if (out_bpp != bpp || bit_depth != 8 || palette_size)
    {
        uint8_t * out_image_data = (uint8_t *)malloc(height * width * out_bpp);
        memset(out_image_data, 0, height * width * out_bpp);
        
        puts("converting...");
        
        if (palette_size)
        {
            puts("depalettizing...");
            for (size_t y = 0; y < height; y += 1)
            {
                for (size_t x = 0; x < width; x += 1)
                {
                    uint8_t val = image_data[y * bytes_per_scanline + x];
                    out_image_data[(y * width + x) * out_bpp + 0] = palette[val * 4 + 0];
                    out_image_data[(y * width + x) * out_bpp + 1] = palette[val * 4 + 1];
                    out_image_data[(y * width + x) * out_bpp + 2] = palette[val * 4 + 2];
                    out_image_data[(y * width + x) * out_bpp + 3] = palette[val * 4 + 3];
                }
            }
        }
        else if (components == 1)
        {
            assert(has_trns == (out_bpp == 2));
            for (size_t y = 0; y < height; y += 1)
            {
                for (size_t x = 0; x < width; x += 1)
                {
                    if (bit_depth == 16)
                    {
                        size_t i = y * bytes_per_scanline;
                        uint16_t val = ((uint16_t)image_data[i + x * 2] << 8) | image_data[i + x * 2 + 1];
                        
                        out_image_data[(y * width + x) * out_bpp] = val >> 8;
                        if (has_trns)
                            out_image_data[(y * width + x) * out_bpp + 1] = val == transparent_r ? 0 : 0xFF;
                    }
                    else
                    {
                        uint8_t val = image_data[y * bytes_per_scanline + x];
                        
                        if (bit_depth == 1)
                            val *= 0xFF;
                        else if (bit_depth == 2)
                            val *= 0x55;
                        else if (bit_depth == 4)
                            val *= 0x11;
                        
                        out_image_data[(y * width + x) * out_bpp] = val;
                        if (has_trns)
                            out_image_data[(y * width + x) * out_bpp + 1] = val == transparent_r ? 0 : 0xFF;
                    }
                }
            }
        }
        else if (components == 3)
        {
            assert(bit_depth == 16);
            assert(has_trns == (out_bpp == 4));
            for (size_t y = 0; y < height; y += 1)
            {
                for (size_t x = 0; x < width; x += 1)
                {
                    size_t i = y * bytes_per_scanline;
                    uint16_t val_r = ((uint16_t)image_data[i + x * 6 + 0] << 8) | image_data[i + x * 6 + 1];
                    uint16_t val_g = ((uint16_t)image_data[i + x * 6 + 2] << 8) | image_data[i + x * 6 + 3];
                    uint16_t val_b = ((uint16_t)image_data[i + x * 6 + 4] << 8) | image_data[i + x * 6 + 5];
                    
                    printf("%d ", val_r);
                    
                    out_image_data[(y * width + x) * out_bpp + 0] = val_r >> 8;
                    out_image_data[(y * width + x) * out_bpp + 1] = val_g >> 8;
                    out_image_data[(y * width + x) * out_bpp + 2] = val_b >> 8;
                    if (has_trns)
                        out_image_data[(y * width + x) * out_bpp + 1] =
                            (val_r == transparent_r && val_g == transparent_g && val_b == transparent_b)
                            ? 0 : 0xFF;
                }
                printf("%d\n", (y * width * out_bpp));
            }
        }
        else // components == 2 or 4
        {
            assert(bit_depth == 16);
            for (size_t y = 0; y < height; y += 1)
            {
                for (size_t x = 0; x < width * components; x += 1)
                {
                    size_t i = y * bytes_per_scanline;
                    uint16_t val = ((uint16_t)image_data[i + x * 2] << 8) | image_data[i + x * 2 + 1];
                    out_image_data[(y * width + x) * out_bpp] = val >> 8;
                }
            }
        }
        wpng_write("out.png", width, height, out_bpp, out_image_data, width * out_bpp);
    }
    else
    {
        wpng_write("out.png", width, height, bpp, image_data, bytes_per_scanline);
    }
}

int main(int argc, char ** argv)
{
    defl_compute_crc32(0, 0, 0);
    
    if (argc < 2)
    {
        puts("error: need input file argument");
        return;
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
        FILE * f = fopen(argv[1], "rb");
        if (!f)
        {
            puts("error: invalid filename");
            return;
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
        
        wpng_load_and_save(&in_buf);
    }
    
	return 0;
}