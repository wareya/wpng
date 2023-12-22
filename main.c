#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

//#define SINFL_IMPLEMENTATION
//#include "sinfl.h"

#include "inflate.h"
#include "deflate.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "buffers.h"

void cb_bytes_push(void * userdata, const unsigned char * bytes, size_t count)
{
    bytes_push((byte_buffer *)userdata, bytes, count);
}
void cb_bytes_copy(void * userdata, size_t distance, size_t count)
{
    byte_buffer * buf = (byte_buffer *)userdata;
    if (distance > buf->len)
        return;
    for (size_t i = 0; i < count; i += 1)
        byte_push(buf, buf->data[buf->len - distance]);
}
void cb_verify_checksum(void * userdata, unsigned (*checksummer)(const unsigned char *, size_t), size_t count, unsigned expected_checksum)
{
    byte_buffer * buf = (byte_buffer *)userdata;
    uint32_t actual_checksum = checksummer(buf->data, count < buf->len ? count : buf->len);
    if (expected_checksum != actual_checksum)
    {
        printf("checksums %08X and %08X don't match!!!\n", expected_checksum, actual_checksum);
        printf("%lld %lld %lld\n", buf->len, count, (size_t)userdata);
    }
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
    
    stbi_write_png("out.png", width, height, bpp, image_data, width * bpp);
}

int main()
{
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
    
    const char * test = "Ah, to be human. A pitiful thing. Come, I will bring to you oblivion, and pass upon you the Gnosis.";
    
    puts("compressing...");
    bit_buffer comp = do_deflate((const uint8_t *)test, strlen(test) + 1, 12);
    for (size_t i = 0; i < comp.buffer.len; i++)
        printf("%02X ", comp.buffer.data[i]);
    puts("");
    int error = 0;
    puts("decompressing...");
    byte_buffer decomp = do_inflate(&comp.buffer, &error);
    printf("%d\n", error);
    printf("%s\n", decomp.data);
    /*
    {
        FILE * f = fopen("pyout2.bin", "rb");
        
        fseek(f, 0, SEEK_END);
        size_t file_len = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        uint8_t * raw_data = (uint8_t *)malloc(file_len);
        fread(raw_data, file_len, 1, f);
        byte_buffer in_buf = {raw_data, file_len, file_len, 0};
        
        fclose(f);
        
        int error = 0;
        byte_buffer dec = do_inflate(&in_buf, &error); // decompresses into `dec` (declared earlier)
        dec.cur = 0;
        printf("%d\n", error);
        for (size_t i = 0; i < dec.len; i += 1)
            printf("%02X ", dec.data[i]);
    }
    */
	return 0;
}