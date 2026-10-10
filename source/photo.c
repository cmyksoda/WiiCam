/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */

#include "photo.h"
#include <jpeglib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} PhotoError;

static void jpeg_failure(j_common_ptr common)
{
    PhotoError *error = (PhotoError *)common->err;
    longjmp(error->jump, 1);
}

static void jpeg_message(j_common_ptr common, int level)
{
    if (level < 0) jpeg_failure(common);
}

bool photo_decode(const uint8_t *jpeg, size_t length, uint8_t *rgb, unsigned width, unsigned height)
{
    struct jpeg_decompress_struct *decoder = calloc(1, sizeof(*decoder));
    PhotoError error;
    if (!decoder) return false;
    decoder->err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_failure;
    error.base.emit_message = jpeg_message;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(decoder);
        free(decoder);
        return false;
    }
    jpeg_create_decompress(decoder);
    jpeg_mem_src(decoder, jpeg, length);
    jpeg_read_header(decoder, TRUE);
    if (decoder->image_width != width || decoder->image_height != height || width > 640
        || height > 480 || !width || !height || decoder->progressive_mode || decoder->arith_code)
        jpeg_failure((j_common_ptr)decoder);
    /* libjpeg-turbo supplies standard Huffman tables for abbreviated MJPEG. */
    decoder->out_color_space = JCS_RGB;
    jpeg_start_decompress(decoder);
    while (decoder->output_scanline < decoder->output_height) {
        JSAMPROW row = rgb + (size_t)decoder->output_scanline * width * 3;
        jpeg_read_scanlines(decoder, &row, 1);
    }
    jpeg_finish_decompress(decoder);
    jpeg_destroy_decompress(decoder);
    free(decoder);
    return true;
}

bool photo_encode(FILE *file, const uint8_t *rgb, unsigned width, unsigned height)
{
    struct jpeg_compress_struct *encoder = calloc(1, sizeof(*encoder));
    PhotoError error;
    if (!encoder || !width || !height || width > 640 || height > 480) {
        free(encoder);
        return false;
    }
    encoder->err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_failure;
    error.base.emit_message = jpeg_message;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(encoder);
        free(encoder);
        return false;
    }
    jpeg_create_compress(encoder);
    jpeg_stdio_dest(encoder, file);
    encoder->image_width = width;
    encoder->image_height = height;
    encoder->input_components = 3;
    encoder->in_color_space = JCS_RGB;
    jpeg_set_defaults(encoder);
    jpeg_set_quality(encoder, 90, TRUE);
    jpeg_start_compress(encoder, TRUE);
    while (encoder->next_scanline < height) {
        JSAMPROW row = (JSAMPROW)(rgb + (size_t)encoder->next_scanline * width * 3);
        jpeg_write_scanlines(encoder, &row, 1);
    }
    jpeg_finish_compress(encoder);
    jpeg_destroy_compress(encoder);
    free(encoder);
    return !ferror(file);
}
