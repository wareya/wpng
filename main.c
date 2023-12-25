#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "inflate.h"
#include "deflate.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "buffers.h"

uint32_t compute_crc32(const uint8_t * data, size_t size, uint32_t init)
{
    uint32_t crc_table[256] = {0};

    for (size_t i = 0; i < 256; i += 1)
    {
        uint32_t c = i;
        for (size_t j = 0; j < 8; j += 1)
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320 : 0);
        crc_table[i] = c;
    }
    
    init ^= 0xFFFFFFFF;
    for (size_t i = 0; i < size; i += 1)
        init = crc_table[(init ^ data[i]) & 0xFF] ^ (init >> 8);
    
    return init ^ 0xFFFFFFFF;
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
    bytes_push_int(&out, byteswap_int(compute_crc32(&out.data[chunk_start], chunk_size, 0), 4), 4);
    
    // write IDAT chunks
    // first, collect pixel data
    byte_buffer pixel_data;
    memset(&pixel_data, 0, sizeof(pixel_data));
    for (size_t y = 0; y < height; y += 1)
    {
        byte_push(&pixel_data, 0); // no filter
        size_t start = bytes_per_scanline * y;
        for (size_t x = 0; x < width * bpp; x += bpp)
            bytes_push(&pixel_data, &image_data[start + x], bpp);
    }
    printf("raw pixel data byte count: %lld\n", pixel_data.len);
    bit_buffer pixel_data_comp = do_deflate(pixel_data.data, pixel_data.len, 12, 1);
    
    bytes_push_int(&out, byteswap_int(pixel_data_comp.buffer.len, 4), 4);
    chunk_start = out.len;
    bytes_push(&out, (const uint8_t *)"IDAT", 4);
    bytes_push(&out, pixel_data_comp.buffer.data, pixel_data_comp.buffer.len);
    chunk_size = out.len - chunk_start;
    bytes_push_int(&out, byteswap_int(compute_crc32(&out.data[chunk_start], chunk_size, 0), 4), 4);
    
    bytes_push_int(&out, 0, 4);
    bytes_push(&out, (const uint8_t *)"IEND\xAE\x42\x60\x82", 8);
    
    FILE * f = fopen(filename, "wb");
    
    fseek(f, 0, SEEK_END);
    size_t file_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    
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
    
    byte_buffer buf2 = {&idat.data[2], idat.len - 6, idat.len - 6, 0};
    int error = 0;
    byte_buffer dec = do_inflate(&buf2, &error); // decompresses into `dec` (declared earlier)
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
                
                image_data[y * width * bpp + x] += ref;
            }
        }
    }
    
    wpng_write("out.png", width, height, bpp, image_data, width * bpp);
}

int main()
{
    compute_crc32(0, 0, 0);
    
    if (0)
    {
        //FILE * f = fopen("unifont-jp.png", "rb");
        FILE * f = fopen("0-ufeff_tiles_v2.png", "rb");
        
        fseek(f, 0, SEEK_END);
        size_t file_len = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        uint8_t * raw_data = (uint8_t *)malloc(file_len);
        fread(raw_data, file_len, 1, f);
        byte_buffer in_buf = {raw_data, file_len, file_len, 0};
        
        fclose(f);
        
        wpng_load_and_save(&in_buf);
    }
    if (0)
    {
        const char * test = "When I waked it was broad day, the weather clear, and the storm abated, so that the sea did not rage and swell as before. But that which surprised me most was, that the ship was lifted off in the night from the sand where she lay by the swelling of the tide, and was driven up almost as far as the rock which I at first mentioned, where I had been so bruised by the wave dashing me against it. This being within about a mile from the shore where I was, and the ship seeming to stand upright still, I wished myself on board, that at least I might save some necessary things for my use.";
        
        puts("compressing...");
        bit_buffer comp = do_deflate((const uint8_t *)test, strlen(test) + 1, 12, 1);
        for (size_t i = 0; i < comp.buffer.len; i++)
            printf("%02X ", comp.buffer.data[i]);
        puts("");
        int error = 0;
        puts("decompressing...");
        comp.buffer.cur = 2;
        byte_buffer decomp = do_inflate(&comp.buffer, &error);
        printf("%d\n", error);
        printf("%s\n", decomp.data);
    }
    if (1)
    {
        // error 0x19B0ish (around 19BA)
        FILE * f = fopen("moby dick.txt", "rb");
        
        fseek(f, 0, SEEK_END);
        size_t file_len = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        uint8_t * raw_data = (uint8_t *)malloc(file_len);
        fread(raw_data, file_len, 1, f);
        byte_buffer in_buf = {raw_data, file_len, file_len, 0};
        
        fclose(f);
        
        int error = 0;
        bit_buffer comp = do_deflate(in_buf.data, in_buf.len, 12, 1); // compresses into `dec` (declared earlier)
        
        FILE * f2 = fopen("moby dick.txt.comp", "wb");
        fwrite(comp.buffer.data, comp.buffer.len, 1, f);
        fclose(f);
        
        comp.buffer.cur = 2;
        byte_buffer decomp = do_inflate(&comp.buffer, &error);
        
        //printf("%d\n", error);
        //printf("%s\n", decomp.data);
        
    }
	return 0;
}