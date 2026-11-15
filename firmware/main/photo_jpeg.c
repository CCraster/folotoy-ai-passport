#include "photo_jpeg.h"
#include "passport.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "lvgl.h"
#include "src/draw/lv_image_decoder_private.h"

enum { ROW_BYTES = WIDTH * 2, MAX_STRIP_BYTES = ROW_BYTES * 16 };

typedef struct {
    jpeg_dec_handle_t codec;
    jpeg_dec_io_t io;
    int capacity;
    int first_row;
    int next_row;
} photo_stream_t;

static void close_stream(photo_stream_t *stream)
{
    if (stream->codec) jpeg_dec_close(stream->codec);
    if (stream->io.outbuf) jpeg_free_align(stream->io.outbuf);
    *stream = (photo_stream_t){0};
}

static bool open_stream(photo_stream_t *stream, const uint8_t *data, size_t size)
{
    if (size < 24 || size > PHOTO_JPEG_LIMIT || data[0] != 0xff || data[1] != 0xd8 ||
        data[size - 2] != 0xff || data[size - 1] != 0xd9) return false;

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.block_enable = true;
    if (jpeg_dec_open(&config, &stream->codec) != JPEG_ERR_OK) return false;

    stream->io.inbuf = (uint8_t *)data; // SDK uses a mutable pointer for its read-only input.
    stream->io.inbuf_len = size;
    jpeg_dec_header_info_t info;
    if (jpeg_dec_parse_header(stream->codec, &stream->io, &info) == JPEG_ERR_OK &&
        info.width == WIDTH && info.height == HEIGHT &&
        jpeg_dec_get_outbuf_len(stream->codec, &stream->capacity) == JPEG_ERR_OK &&
        stream->capacity > 0 && stream->capacity <= MAX_STRIP_BYTES) {
        stream->io.outbuf = jpeg_calloc_align(stream->capacity, 16);
        if (stream->io.outbuf) return true;
    }
    close_stream(stream);
    return false;
}

static bool decode_strip(photo_stream_t *stream)
{
    if (jpeg_dec_process(stream->codec, &stream->io) != JPEG_ERR_OK) return false;
    const int bytes = stream->io.out_size;
    if (bytes <= 0 || bytes > stream->capacity || bytes % ROW_BYTES) return false;
    stream->first_row = stream->next_row;
    stream->next_row += bytes / ROW_BYTES;
    return stream->next_row <= HEIGHT;
}

bool photo_jpeg_valid(const uint8_t *data, size_t size)
{
    photo_stream_t stream = {0};
    if (!open_stream(&stream, data, size)) return false;
    while (stream.next_row < HEIGHT && decode_strip(&stream)) {}
    const bool complete = stream.next_row == HEIGHT;
    close_stream(&stream);
    return complete;
}

size_t photo_jpeg_from_rgb565(const uint8_t *pixels, uint8_t *jpeg)
{
    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    config.width = WIDTH;
    config.height = HEIGHT;
    config.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.quality = 90;
    jpeg_enc_handle_t encoder;
    if (jpeg_enc_open(&config, &encoder) != JPEG_ERR_OK) return 0;
    int size = 0;
    jpeg_error_t result = jpeg_enc_process(encoder, pixels, PHOTO_BYTES, jpeg, PHOTO_JPEG_LIMIT, &size);
    jpeg_enc_close(encoder);
    return result == JPEG_ERR_OK && size > 0 && photo_jpeg_valid(jpeg, size) ? size : 0;
}

// LVGL uses one software draw unit. It opens each tile separately, so keep the
// decoder and its latest 8/16 rows until the complete display refresh finishes.
static photo_stream_t frame;
static const uint8_t *frame_source;
static lv_draw_buf_t strip;

void photo_jpeg_finish_frame(void)
{
    close_stream(&frame);
    frame_source = NULL;
}

static lv_result_t photo_decoder_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                                      lv_image_header_t *header)
{
    (void)decoder;
    if (dsc->src_type != LV_IMAGE_SRC_VARIABLE) return LV_RESULT_INVALID;
    const lv_image_dsc_t *image = dsc->src;
    if (image->header.cf != LV_COLOR_FORMAT_RAW || image->header.w != WIDTH ||
        image->header.h != HEIGHT || image->data_size > PHOTO_JPEG_LIMIT) return LV_RESULT_INVALID;
    *header = image->header;
    return LV_RESULT_OK;
}

static lv_result_t photo_decoder_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    (void)decoder;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.stride = ROW_BYTES;
    return LV_RESULT_OK;
}

static lv_result_t photo_decoder_area(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                                      const lv_area_t *requested, lv_area_t *decoded)
{
    (void)decoder;
    const lv_image_dsc_t *image = dsc->src;
    const int row = decoded->y1 == LV_COORD_MIN ? requested->y1 : decoded->y2 + 1;
    if (row < 0 || row >= HEIGHT || row > requested->y2) return LV_RESULT_INVALID;
    if (frame_source != image->data || row < frame.first_row) {
        photo_jpeg_finish_frame();
        if (!open_stream(&frame, image->data, image->data_size)) {
            ESP_LOGW("photo", "Cannot open JPEG for display");
            return LV_RESULT_INVALID;
        }
        frame_source = image->data;
    }
    while (row >= frame.next_row) {
        if (!decode_strip(&frame)) {
            photo_jpeg_finish_frame();
            ESP_LOGW("photo", "JPEG display decode failed");
            return LV_RESULT_INVALID;
        }
    }
    if (lv_draw_buf_init(&strip, WIDTH, frame.next_row - frame.first_row, LV_COLOR_FORMAT_RGB565,
                         ROW_BYTES, frame.io.outbuf, frame.capacity) != LV_RESULT_OK) return LV_RESULT_INVALID;
    dsc->decoded = &strip;
    *decoded = (lv_area_t){.x1 = 0, .y1 = frame.first_row, .x2 = WIDTH - 1, .y2 = frame.next_row - 1};
    return LV_RESULT_OK;
}

bool photo_jpeg_install_decoder(void)
{
    lv_image_decoder_t *decoder = lv_image_decoder_create();
    if (!decoder) return false;
    decoder->name = "Passport JPEG";
    lv_image_decoder_set_info_cb(decoder, photo_decoder_info);
    lv_image_decoder_set_open_cb(decoder, photo_decoder_open);
    lv_image_decoder_set_get_area_cb(decoder, photo_decoder_area);
    return true;
}
