 #include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

enum S10Status { S10_OK = 0, S10_E_STRIDE, S10_E_ARGUMENT, S10_E_CAPACITY, S10_E_ALLOC };

typedef struct {
    size_t width, height, stride;
    int delta, threshold;
    uint8_t *pixels;
    size_t pixels_len;
} S10Image;

static int checked_mul_size(size_t a, size_t b, size_t *out) {
    if (b != 0 && a > (size_t)-1 / b) return 0;
    *out = a * b;
    return 1;
}

static enum S10Status s10_image_alloc(uint8_t **out_buf, size_t bytes) {
    if (out_buf == NULL) return S10_E_ARGUMENT;
    *out_buf = NULL;
    if (bytes == 0) return S10_E_ARGUMENT;
    *out_buf = (uint8_t *)malloc(bytes);
    return *out_buf == NULL ? S10_E_ALLOC : S10_OK;
}

static void s10_image_free(uint8_t **buf) {
    if (buf != NULL && *buf != NULL) {
        free(*buf);
        *buf = NULL;
    }
}

static enum S10Status brighten_rgb24(const uint8_t *src, size_t src_len,
                                     uint8_t *dst, size_t dst_len,
                                     size_t width, size_t height, size_t stride,
                                     int delta) {
    size_t row_bytes, total;
    size_t row, col;
    if (src == NULL || dst == NULL || width == 0 || height == 0) return S10_E_ARGUMENT;
    if (src == dst) return S10_E_ARGUMENT;
    if (!checked_mul_size(width, 3u, &row_bytes)) return S10_E_ARGUMENT;
    if (stride < row_bytes) return S10_E_STRIDE;
    if (delta < -255 || delta > 255) return S10_E_ARGUMENT;
    if (!checked_mul_size(stride, height, &total)) return S10_E_ARGUMENT;
    if (src_len < total || dst_len < total) return S10_E_CAPACITY;
    for (row = 0; row < height; ++row) {
        const uint8_t *s = src + row * stride;
        uint8_t *d = dst + row * stride;
        memcpy(d, s, stride);
        for (col = 0; col < width; ++col) {
            size_t off = col * 3u;
            int r = (int)s[off] + delta;
            int g = (int)s[off + 1u] + delta;
            int b = (int)s[off + 2u] + delta;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            d[off] = (uint8_t)r; d[off + 1u] = (uint8_t)g; d[off + 2u] = (uint8_t)b;
        }
        if (stride > row_bytes) memset(d + row_bytes, 0, stride - row_bytes);
    }
    return S10_OK;
}

static enum S10Status histogram4_rgb24(const uint8_t *pixels, size_t pixels_len,
                                       size_t width, size_t height, size_t stride,
                                       int histogram[4]) {
    size_t row_bytes, total, row, col;
    if (pixels == NULL || histogram == NULL || width == 0 || height == 0) return S10_E_ARGUMENT;
    if (!checked_mul_size(width, 3u, &row_bytes)) return S10_E_ARGUMENT;
    if (stride < row_bytes) return S10_E_STRIDE;
    if (!checked_mul_size(stride, height, &total)) return S10_E_ARGUMENT;
    if (pixels_len < total) return S10_E_CAPACITY;
    histogram[0] = histogram[1] = histogram[2] = histogram[3] = 0;
    for (row = 0; row < height; ++row) {
        const uint8_t *base = pixels + row * stride;
        for (col = 0; col < width; ++col) {
            const uint8_t *p = base + col * 3u;
            int g = ((int)p[0] + (int)p[1] + (int)p[2]) / 3;
            ++histogram[g / 64];
        }
    }
    return S10_OK;
}

static int read_input(FILE *f, S10Image *im) {
    size_t count, i, total;
    if (f == NULL || im == NULL) return 0;
    memset(im, 0, sizeof(*im));
    if (fscanf(f, " { \"width\" : %zu , \"height\" : %zu , \"stride\" : %zu , \"pixels\" : [", &im->width, &im->height, &im->stride) != 3) return 0;
    if (!checked_mul_size(im->width, im->height, &count) || !checked_mul_size(im->stride, im->height, &total)) return 0;
    im->pixels_len = total;
    if (s10_image_alloc(&im->pixels, total) != S10_OK) return 0;
    memset(im->pixels, 0, total);
    for (i = 0; i < count; ++i) {
        unsigned int r, g, b;
        if (fscanf(f, " [%u , %u , %u ] %*[ ,]", &r, &g, &b) != 3 || r > 255u || g > 255u || b > 255u) { s10_image_free(&im->pixels); return 0; }
        im->pixels[(i / im->width) * im->stride + (i % im->width) * 3u] = (uint8_t)r;
        im->pixels[(i / im->width) * im->stride + (i % im->width) * 3u + 1u] = (uint8_t)g;
        im->pixels[(i / im->width) * im->stride + (i % im->width) * 3u + 2u] = (uint8_t)b;
    }
    if (fscanf(f, " ] , \"delta\" : %d , \"threshold\" : %d }", &im->delta, &im->threshold) != 2) { s10_image_free(&im->pixels); return 0; }
    return 1;
}

static int write_result(FILE *f, const S10Image *im, enum S10Status status, const uint8_t *out, const int hist[4]) {
    size_t row, col;
    if (f == NULL) return 0;
    if (status == S10_E_STRIDE) { fputs("{\"status\":\"E_STRIDE\"}", f); return 1; }
    if (status != S10_OK) { fputs("{\"status\":\"E_STRIDE\"}", f); return 1; }
    fprintf(f, "{\"status\":\"OK\",\"width\":%zu,\"height\":%zu,\"pixels\":[", im->width, im->height);
    for (row = 0; row < im->height; ++row) for (col = 0; col < im->width; ++col) {
        const uint8_t *p = out + row * im->stride + col * 3u;
        if (row != 0 || col != 0) fputc(',', f);
        fprintf(f, "[%u,%u,%u]", (unsigned)p[0], (unsigned)p[1], (unsigned)p[2]);
    }
    fprintf(f, "],\"metric\":{\"histogram\":[%d,%d,%d,%d]}}", hist[0], hist[1], hist[2], hist[3]);
    return 1;
}

static int process_input(void) {
    S10Image im;
    uint8_t *out = NULL;
    int hist[4] = {0, 0, 0, 0};
    enum S10Status st;
    printf("请输入 JSON：\n");
    if (!read_input(stdin, &im)) {
        printf("STATUS: INPUT_ERROR\n\n");
        return 0;
    }
    if (im.stride < im.width * 3u) st = S10_E_STRIDE;
    else if (s10_image_alloc(&out, im.pixels_len) != S10_OK) st = S10_E_ALLOC;
    else {
        st = brighten_rgb24(im.pixels, im.pixels_len, out, im.pixels_len, im.width, im.height, im.stride, im.delta);
        if (st == S10_OK) st = histogram4_rgb24(out, im.pixels_len, im.width, im.height, im.stride, hist);
    }
    if (!write_result(stdout, &im, st, out, hist)) st = S10_E_ARGUMENT;
    s10_image_free(&out);
    s10_image_free(&im.pixels);
    putchar('\n');
    return st == S10_OK || st == S10_E_STRIDE;
}

int main(void) {
    return process_input() ? 0 : 1;
}
