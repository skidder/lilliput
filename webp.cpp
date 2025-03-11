#include "webp.hpp"
#include <opencv2/imgproc.hpp>
#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <stdbool.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

struct webp_decoder_struct {
    WebPMux* mux;
    int total_frame_count;
    uint32_t bgcolor;
    uint32_t loop_count;
    bool has_alpha;
    bool has_animation;
    int width;
    int height;

    int current_frame_index;
    int prev_frame_delay_time;
    int prev_frame_x_offset;
    int prev_frame_y_offset;
    WebPMuxAnimDispose prev_frame_dispose;
    WebPMuxAnimBlend prev_frame_blend;
    uint8_t* decode_buffer;
    size_t decode_buffer_size;
    int total_duration;
};

struct webp_encoder_struct {
    // input fields
    const uint8_t* icc;
    size_t icc_len;
    uint32_t bgcolor;
    uint32_t loop_count;

    // output fields
    WebPMux* mux;                  // Used for still images
    WebPAnimEncoder* anim;         // Used for animated images
    WebPConfig config;             // Encoding configuration
    WebPPicture picture;           // Picture for current frame
    int frame_count;
    int first_frame_delay;
    int first_frame_blend;
    int first_frame_dispose;
    int first_frame_x_offset;
    int first_frame_y_offset;
    uint8_t* dst;
    size_t dst_len;
    int canvas_width;              // Width of the animation canvas
    int canvas_height;             // Height of the animation canvas
    bool is_animation;             // Whether we're encoding an animation
    int timestamp_ms;              // Current timestamp in milliseconds
};

// Helper function to get current time as string
static std::string get_current_time() {
    auto now = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << now_ms.count();
    return ss.str();
}

/**
 * Creates a WebP decoder from the given OpenCV matrix.
 * @param buf The input OpenCV matrix containing the WebP image data.
 * @return A pointer to the created webp_decoder_struct, or nullptr if creation failed.
 */
webp_decoder webp_decoder_create(const opencv_mat buf)
{
    auto cvMat = static_cast<const cv::Mat*>(buf);
    WebPData src = { cvMat->data, cvMat->total() };
    WebPMux* mux = WebPMuxCreate(&src, 0);

    if (!mux) {
        return nullptr;
    }

    // Get features at the container level
    uint32_t flags;
    if (WebPMuxGetFeatures(mux, &flags) != WEBP_MUX_OK) {
        WebPMuxDelete(mux);
        return nullptr;
    }

    // Get the first frame to retrieve the image dimensions
    WebPMuxFrameInfo frame;
    if (WebPMuxGetFrame(mux, 1, &frame) != WEBP_MUX_OK) {
        WebPMuxDelete(mux);
        return nullptr;
    }

    WebPBitstreamFeatures features;
    if (WebPGetFeatures(frame.bitstream.bytes, frame.bitstream.size, &features) != VP8_STATUS_OK) {
        WebPDataClear(&frame.bitstream);
        WebPMuxDelete(mux);
        return nullptr;
    }
    WebPDataClear(&frame.bitstream);

    webp_decoder d = new webp_decoder_struct();
    memset(d, 0, sizeof(webp_decoder_struct));
    d->mux = mux;
    d->current_frame_index = 1;
    d->has_alpha = (flags & ALPHA_FLAG);

    // Get the canvas size
    if (WebPMuxGetCanvasSize(mux, &d->width, &d->height) != WEBP_MUX_OK) {
        WebPMuxDelete(mux);
        return nullptr;
    }

    // Calculate total frame count and duration
    d->total_frame_count = 0;
    d->total_duration = 0;
    do {
        d->total_frame_count++;
        d->total_duration += frame.duration;
        WebPDataClear(&frame.bitstream);
    } while (WebPMuxGetFrame(mux, d->total_frame_count + 1, &frame) == WEBP_MUX_OK);

    // Get animation parameters
    d->bgcolor = 0xFFFFFFFF; // Default to white background
    if (flags & ANIMATION_FLAG) {
        WebPMuxAnimParams anim_params;
        if (WebPMuxGetAnimationParams(mux, &anim_params) == WEBP_MUX_OK) {
            d->bgcolor = anim_params.bgcolor;
            d->loop_count = anim_params.loop_count;
        }
        d->has_animation = true;
    } else {
        // For static images, ensure duration is 0
        d->total_duration = 0;
    }

    // Pre-allocate decode buffer
    d->decode_buffer_size = d->width * d->height * 4; // 4 channels for RGBA
    d->decode_buffer = new uint8_t[d->decode_buffer_size];

    return d;
}

/**
 * Gets the width of the WebP image.
 * @param d The webp_decoder_struct pointer.
 * @return The width of the WebP image.
 */
int webp_decoder_get_width(const webp_decoder d)
{
    return d->width;
}

/**
 * Gets the height of the WebP image.
 * @param d The webp_decoder_struct pointer.
 * @return The height of the WebP image.
 */
int webp_decoder_get_height(const webp_decoder d)
{
    return d->height;
}

/**
 * Gets the pixel type of the WebP image.
 * @param d The webp_decoder_struct pointer.
 * @return The pixel type of the WebP image.
 */
int webp_decoder_get_pixel_type(const webp_decoder d)
{
    return d->has_alpha ? CV_8UC4 : CV_8UC3;
}

/**
 * Gets the delay time of the previous frame.
 * @param d The webp_decoder_struct pointer.
 * @return The delay time of the previous frame.
 */
int webp_decoder_get_prev_frame_delay(const webp_decoder d)
{
    return d->prev_frame_delay_time;
}

/**
 * Gets the x-offset of the previous frame.
 * @param d The webp_decoder_struct pointer.
 * @return The x-offset of the previous frame.
 */
int webp_decoder_get_prev_frame_x_offset(const webp_decoder d)
{
    return d->prev_frame_x_offset;
}

/**
 * Gets the y-offset of the previous frame.
 * @param d The webp_decoder_struct pointer.
 * @return The y-offset of the previous frame.
 */
int webp_decoder_get_prev_frame_y_offset(const webp_decoder d)
{
    return d->prev_frame_y_offset;
}

/**
 * Gets the dispose method of the previous frame.
 * @param d The webp_decoder_struct pointer.
 * @return The dispose method of the previous frame.
 */
int webp_decoder_get_prev_frame_dispose(const webp_decoder d)
{
    return d->prev_frame_dispose;
}

/**
 * Gets the blend method of the previous frame.
 * @param d The webp_decoder_struct pointer.
 * @return The blend method of the previous frame.
 */
int webp_decoder_get_prev_frame_blend(const webp_decoder d)
{
    return d->prev_frame_blend;
}

/**
 * Gets the background color of the WebP image.
 * @param d The webp_decoder_struct pointer.
 * @return The background color of the WebP image.
 */
uint32_t webp_decoder_get_bg_color(const webp_decoder d)
{
    return d->bgcolor;
}

/**
 * Gets the loop count of the WebP image.
 * @param d The webp_decoder_struct pointer.
 * @return The loop count of the WebP image.
 */
uint32_t webp_decoder_get_loop_count(const webp_decoder d)
{
    return d->loop_count;
}

/**
 * Gets the total number of frames in the WebP image.
 * @param d The webp_decoder_struct pointer.
 * @return The total number of frames in the WebP image.
 */
int webp_decoder_get_num_frames(const webp_decoder d)
{
    return d ? d->total_frame_count : 0;
}

/**
 * Gets the total duration of the WebP animation in milliseconds.
 * @param d The webp_decoder_struct pointer.
 * @return The total duration in milliseconds, 0 for static images.
 */
int webp_decoder_get_total_duration(const webp_decoder d)
{
    return d ? d->total_duration : 0;
}

/**
 * Gets the ICC profile data from the WebP image.
 * @param d The webp_decoder_struct pointer.
 * @param dst The destination buffer to store the ICC profile data.
 * @param dst_len The size of the destination buffer.
 * @return The size of the ICC profile data copied to the destination buffer.
 */
size_t webp_decoder_get_icc(const webp_decoder d, void* dst, size_t dst_len)
{
    WebPData icc = { nullptr, 0 };
    auto res = WebPMuxGetChunk(d->mux, "ICCP", &icc);
    if (icc.size > 0 && res == WEBP_MUX_OK) {
        if (icc.size <= dst_len) {
            memcpy(dst, icc.bytes, icc.size);
            return icc.size;
        }
    }
    return 0;
}

/**
 * Checks if there are more frames to decode in the WebP image.
 * @param d The webp_decoder_struct pointer.
 * @return True if there are more frames to decode, false otherwise.
 */
int webp_decoder_has_more_frames(webp_decoder d)
{
     return d->current_frame_index < d->total_frame_count;
}

/**
 * Advances to the next frame in the WebP image.
 * @param d The webp_decoder_struct pointer.
 */
void webp_decoder_advance_frame(webp_decoder d)
{
    d->current_frame_index++;
}

/**
 * Decodes the current frame of the WebP image and stores the decoded image in the provided OpenCV matrix.
 * @param d The webp_decoder_struct pointer.
 * @param mat The OpenCV matrix to store the decoded image.
 * @return True if the frame was successfully decoded, false otherwise.
 */
bool webp_decoder_decode(const webp_decoder d, opencv_mat mat)
{
    if (!d) {
        return false;
    }

    WebPMuxFrameInfo frame;
    WebPMuxError mux_error = WebPMuxGetFrame(d->mux, d->current_frame_index, &frame);
    
    if (mux_error != WEBP_MUX_OK) {
        return false;
    }

    WebPBitstreamFeatures features;
    if (WebPGetFeatures(frame.bitstream.bytes, frame.bitstream.size, &features) != VP8_STATUS_OK) {
        WebPDataClear(&frame.bitstream);
        return false;
    }

    // Set the cv::Mat dimensions to the frame's width and height
    auto cvMat = static_cast<cv::Mat*>(mat);
    cvMat->create(features.height, features.width, webp_decoder_get_pixel_type(d));

    // Recalculate row size based on the new dimensions
    int row_size = cvMat->cols * cvMat->elemSize();

    // Store frame properties for future use
    d->prev_frame_delay_time = frame.duration;
    d->prev_frame_x_offset = frame.x_offset;
    d->prev_frame_y_offset = frame.y_offset;
    d->prev_frame_dispose = frame.dispose_method;
    d->prev_frame_blend = frame.blend_method;

    // Decode the frame
    uint8_t* res = nullptr;
    switch (webp_decoder_get_pixel_type(d)) {
        case CV_8UC4:
            res = WebPDecodeBGRAInto(frame.bitstream.bytes, frame.bitstream.size,
                                     d->decode_buffer, d->decode_buffer_size, row_size);
            break;
        case CV_8UC3:
            res = WebPDecodeBGRInto(frame.bitstream.bytes, frame.bitstream.size,
                                d->decode_buffer, d->decode_buffer_size, row_size);
            break;
        default:
            return false;
    }

    if (res) {
        memcpy(cvMat->data, d->decode_buffer, cvMat->total() * cvMat->elemSize());
    }

    WebPDataClear(&frame.bitstream);
    return res != nullptr;
}

/**
 * Releases the resources allocated for the webp_decoder_struct.
 * @param d The webp_decoder_struct pointer.
 */
void webp_decoder_release(webp_decoder d)
{
    if (d) {
        if (d->mux) WebPMuxDelete(d->mux);
        delete[] d->decode_buffer;
        delete d;
    }
}

/**
 * Creates a WebP encoder with the given parameters.
 * @param buf The output buffer to store the encoded WebP data.
 * @param buf_len The size of the output buffer.
 * @param icc The ICC profile data.
 * @param icc_len The size of the ICC profile data.
 * @param bgcolor The background color for the WebP image.
 * @param gif_encode_paletted Whether to use delta palette for encoding of GIF source images.
 * @return A pointer to the created webp_encoder_struct, or nullptr if creation failed.
 */
webp_encoder webp_encoder_create(void* buf, size_t buf_len, const void* icc, size_t icc_len, uint32_t bgcolor, int loop_count)
{
    webp_encoder e = new struct webp_encoder_struct();
    memset(e, 0, sizeof(struct webp_encoder_struct));
    e->dst = (uint8_t*)(buf);
    e->dst_len = buf_len;
    e->mux = WebPMuxNew();
    e->anim = nullptr;
    e->frame_count = 1;
    e->first_frame_delay = 0;
    e->bgcolor = bgcolor;
    e->loop_count = loop_count;
    e->is_animation = false;
    e->canvas_width = 0;
    e->canvas_height = 0;
    e->timestamp_ms = 0;

    // Initialize WebP configuration with preset
    if (!WebPConfigPreset(&e->config, WEBP_PRESET_DEFAULT, 75.0f)) {
        delete e;
        return nullptr;
    }

    // Initialize WebP picture
    if (!WebPPictureInit(&e->picture)) {
        delete e;
        return nullptr;
    }

    if (icc_len) {
        e->icc = (const uint8_t*)(icc);
        e->icc_len = icc_len;
    }
    return e;
}

/**
 * Encodes the given OpenCV matrix as a WebP image and writes the encoded data to the output buffer.
 * @param e The webp_encoder_struct pointer.
 * @param src The OpenCV matrix containing the image to encode.
 * @param opt The encoding options.
 * @param opt_len The number of encoding options.
 * @param delay The delay time for the current frame.
 * @param blend The blend method for the current frame.
 * @param dispose The dispose method for the current frame.
 * @param x_offset The x-offset for the current frame.
 * @param y_offset The y-offset for the current frame.
 * @return The size of the encoded WebP data, or 0 if encoding failed.
 */
size_t webp_encoder_write(webp_encoder e, const opencv_mat src, const int* opt, size_t opt_len, int delay, int blend, int dispose, int x_offset, int y_offset)
{
    if (!e) {
        return 0;
    }

    // Process encoding options at the start for both static and animated images
    for (size_t i = 0; i + 1 < opt_len; i += 2) {
        int key = opt[i];
        int value = opt[i + 1];
        float quality;
        
        switch (key) {
            case CV_IMWRITE_WEBP_QUALITY:
                quality = std::max(1.0f, (float)value);
                e->config.quality = quality;
                e->config.lossless = (quality > 100.0f);
                break;
            case WEBP_METHOD:
                e->config.method = value;
                break;
            case WEBP_FILTER_STRENGTH:
                e->config.filter_strength = value;
                break;
            case WEBP_FILTER_TYPE:
                e->config.filter_type = value;
                break;
            case WEBP_AUTOFILTER:
                e->config.autofilter = value;
                break;
            case WEBP_PARTITIONS:
                e->config.partitions = value;
                break;
            case WEBP_SEGMENTS:
                e->config.segments = value;
                break;
            case WEBP_PREPROCESSING:
                e->config.preprocessing = value;
                break;
            case WEBP_THREAD_LEVEL:
                e->config.thread_level = value;
                break;
            case WEBP_PALETTE:
                e->config.use_delta_palette = value;
                break;
        }
    }

    // Handle finalization case
    if (!src) {
        if (e->frame_count == 1) {
            // No frames were added
            WebPMuxDelete(e->mux);
            e->mux = nullptr;
            return 0;
        }

        size_t size = 0;
        if (e->is_animation) {
            // Finalize animation
            WebPData webp_data;
            if (WebPAnimEncoderAssemble(e->anim, &webp_data)) {
                if (webp_data.size <= e->dst_len) {
                    memcpy(e->dst, webp_data.bytes, webp_data.size);
                    size = webp_data.size;
                } else {
                    fprintf(stderr, "[%s][WebP] Error: Encoded animation size (%zu) exceeds buffer size (%zu)\n",
                            get_current_time().c_str(), webp_data.size, e->dst_len);
                }
                WebPDataClear(&webp_data);
            } else {
                fprintf(stderr, "[%s][WebP] Failed to assemble animation: %s\n",
                        get_current_time().c_str(), WebPAnimEncoderGetError(e->anim));
            }
            WebPAnimEncoderDelete(e->anim);
            e->anim = nullptr;
        } else {
            // Finalize still image using existing WebPMux code
            WebPData out_mux = { nullptr, 0 };
            if (WebPMuxAssemble(e->mux, &out_mux) == WEBP_MUX_OK) {
                if (out_mux.size <= e->dst_len) {
                    memcpy(e->dst, out_mux.bytes, out_mux.size);
                    size = out_mux.size;
                }
                WebPDataClear(&out_mux);
            }
            WebPMuxDelete(e->mux);
            e->mux = nullptr;
        }
        return size;
    }

    // Process input matrix
    auto mat = static_cast<const cv::Mat*>(src);
    if (!mat || mat->empty()) {
        return 0;
    }

    // Handle color conversion if needed
    cv::Mat bgr_mat;
    if (mat->channels() == 1) {
        cv::cvtColor(*mat, bgr_mat, CV_GRAY2BGR);
        mat = &bgr_mat;
    }

    if (mat->channels() != 3 && mat->channels() != 4) {
        return 0;
    }

    // Initialize animation encoder if this is the second frame
    if (e->frame_count == 2 && !e->is_animation) {
        e->is_animation = true;
        e->canvas_width = mat->cols;
        e->canvas_height = mat->rows;
        e->timestamp_ms = 0;
        
        WebPAnimEncoderOptions anim_config;
        if (!WebPAnimEncoderOptionsInit(&anim_config)) {
            fprintf(stderr, "[%s][WebP] Failed to initialize animation encoder options: %s\n",
                    get_current_time().c_str(), WebPAnimEncoderGetError(e->anim));
            return 0;
        }
        anim_config.anim_params.loop_count = e->loop_count;
        anim_config.anim_params.bgcolor = e->bgcolor;
        anim_config.kmin = 1;              // Minimize distance between keyframes since source is already optimized
        anim_config.kmax = 2;              // Keep keyframes close together for faster encoding
        
        e->anim = WebPAnimEncoderNew(e->canvas_width, e->canvas_height, &anim_config);
        if (!e->anim) {
            fprintf(stderr, "[%s][WebP] Failed to create animation encoder\n",
                    get_current_time().c_str());
            return 0;
        }

        // Add the first frame to the animation encoder
        WebPPicture first_frame;
        WebPPictureInit(&first_frame);
        first_frame.width = e->canvas_width;
        first_frame.height = e->canvas_height;
        first_frame.use_argb = 1;

        if (!WebPPictureAlloc(&first_frame)) {
            return 0;
        }

        // Import the first frame
        if (mat->channels() == 3) {
            WebPPictureImportBGR(&first_frame, mat->data, mat->step);
        } else {
            WebPPictureImportBGRA(&first_frame, mat->data, mat->step);
        }

        // Add first frame with its stored delay
        if (WebPAnimEncoderAdd(e->anim, &first_frame, e->timestamp_ms, &e->config) != 1) {
            fprintf(stderr, "[%s][WebP] Failed to add first frame to animation at timestamp %d: %s\n",
                    get_current_time().c_str(), e->timestamp_ms, WebPAnimEncoderGetError(e->anim));
            WebPPictureFree(&first_frame);
            return 0;
        }
        e->timestamp_ms += e->first_frame_delay;  // Add first frame delay to timestamp
        WebPPictureFree(&first_frame);
    }

    // Handle current frame
    if (e->is_animation) {
        // Add frame to animation
        WebPPicture frame;
        WebPPictureInit(&frame);
        frame.width = mat->cols;
        frame.height = mat->rows;
        frame.use_argb = 1;

        if (!WebPPictureAlloc(&frame)) {
            fprintf(stderr, "[%s][WebP] Failed to allocate picture for frame %d\n",
                    get_current_time().c_str(), e->frame_count);
            return 0;
        }

        // Import the frame
        if (mat->channels() == 3) {
            WebPPictureImportBGR(&frame, mat->data, mat->step);
        } else {
            WebPPictureImportBGRA(&frame, mat->data, mat->step);
        }

        // Add frame at current timestamp
        if (WebPAnimEncoderAdd(e->anim, &frame, e->timestamp_ms, &e->config) != 1) {
            WebPPictureFree(&frame);
            return 0;
        }
        e->timestamp_ms += delay;  // Add frame delay to timestamp
        WebPPictureFree(&frame);
    } else {
        // Handle single frame using existing WebPMux code
        size_t size = 0;
        uint8_t* out_picture = nullptr;

        if (e->config.lossless) {
            if (mat->channels() == 3) {
                size = WebPEncodeLosslessBGR(mat->data, mat->cols, mat->rows, mat->step, &out_picture);
            } else {
                size = WebPEncodeLosslessBGRA(mat->data, mat->cols, mat->rows, mat->step, &out_picture);
            }
        } else {
            if (mat->channels() == 3) {
                size = WebPEncodeBGR(mat->data, mat->cols, mat->rows, mat->step, e->config.quality, &out_picture);
            } else {
                size = WebPEncodeBGRA(mat->data, mat->cols, mat->rows, mat->step, e->config.quality, &out_picture);
            }
        }

        if (size == 0) {
            return 0;
        }

        WebPData picture = { out_picture, size };
        WebPMuxError mux_error = WebPMuxSetImage(e->mux, &picture, 1);
        WebPFree(out_picture);

        if (mux_error != WEBP_MUX_OK) {
            return 0;
        }

        // Store first frame parameters in case we need them later
        e->first_frame_delay = delay;
        e->first_frame_blend = blend;
        e->first_frame_dispose = dispose;
        e->first_frame_x_offset = x_offset;
        e->first_frame_y_offset = y_offset;
        e->timestamp_ms = 0;  // Initialize timestamp for first frame
    }

    e->frame_count++;
    return mat->total() * mat->elemSize();  // Return approximate size of processed data
}

/**
 * Releases the resources allocated for the webp_encoder_struct.
 * @param e The webp_encoder_struct pointer.
 */
void webp_encoder_release(webp_encoder e)
{
    if (e) {
        if (e->mux) {
            WebPMuxDelete(e->mux);
        }
        if (e->anim) {
            WebPAnimEncoderDelete(e->anim);
        }
        WebPPictureFree(&e->picture);
        delete e;
    }
}

/**
 * Flushes the remaining data in the webp_encoder_struct and finalizes the WebP image.
 * @param e The webp_encoder_struct pointer.
 * @return The size of the encoded WebP data, or 0 if encoding failed.
 */
size_t webp_encoder_flush(webp_encoder e)
{
    return webp_encoder_write(e, nullptr, nullptr, 0, 0, 0, 0, 0, 0);
}