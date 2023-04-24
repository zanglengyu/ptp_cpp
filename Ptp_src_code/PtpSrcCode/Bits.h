#ifndef BITS_H
#define BITS_H

#include <malloc.h>

#include <cstdint>

bool is_big_endian = false;

void bit_init() {
    union {
        uint32_t dat;
        uint8_t pdat[4];
    } endian_u;

    endian_u.dat = 0x12345678;

    if ((endian_u.pdat[0] == 0x12) && (endian_u.pdat[1] == 0x34) && (endian_u.pdat[2] == 0x56) &&
        (endian_u.pdat[3] == 0x78)) {
        is_big_endian = true;
        printf("Big Endian\n");
    } else if ((endian_u.pdat[3] == 0x12) && (endian_u.pdat[2] == 0x34) && (endian_u.pdat[1] == 0x56) &&
               (endian_u.pdat[0] == 0x78)) {
        is_big_endian = false;
        printf("Small Endian\n");
    }
}
struct bits_buffer_t {
    int i_size;

    int i_data;
    uint8_t i_mask;
    uint8_t* p_data;
};
typedef struct bits_buffer_t bits_buffer_t;

static int bits_initwrite(struct bits_buffer_t* p_buffer, int i_size, void* p_data) {
    p_buffer->i_size = i_size;
    p_buffer->i_data = 0;
    p_buffer->i_mask = 0x80;
    p_buffer->p_data = (uint8_t*)p_data;
    p_buffer->p_data[0] = 0;
    if (!p_buffer->p_data) {
        if (!(p_buffer->p_data = (uint8_t*)malloc(i_size))) {
            return (-1);
        } else {
            return (0);
        }
    } else {
        return (0);
    }
}

static void bits_align(struct bits_buffer_t* p_buffer) {
    if (p_buffer->i_mask != 0x80 && p_buffer->i_data < p_buffer->i_size) {
        p_buffer->i_mask = 0x80;
        p_buffer->i_data++;
        p_buffer->p_data[p_buffer->i_data] = 0x00;
    }
}

static void bits_write(struct bits_buffer_t* p_buffer, int i_count, uint64_t i_bits) {
    uint64_t i_bits_small_endian;
    if (is_big_endian) {
        // 大端模式需要逆序一下，不然写入后是小端模式写入
        for (int i = 0; i != 8; ++i) {
            ((char*)(&i_bits_small_endian))[i] = ((char*)(&i_bits))[8 - i - 1];
        }

    } else {
        //是数据是小端模式，写入后自动变成大端序列
        i_bits_small_endian = i_bits;
    }

    while (i_count > 0) {
        i_count--;

        if ((i_bits >> i_count) & 0x01) {
            p_buffer->p_data[p_buffer->i_data] |= p_buffer->i_mask;
        } else {
            p_buffer->p_data[p_buffer->i_data] &= ~p_buffer->i_mask;
        }
        p_buffer->i_mask >>= 1;
        if (p_buffer->i_mask == 0) {
            p_buffer->i_data++;
            p_buffer->i_mask = 0x80;
        }
    }
}

#endif  // BITS_H
