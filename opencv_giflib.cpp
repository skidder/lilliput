#include "opencv_giflib.hpp"
#include "gif_lib.h"
#include <stdbool.h>

struct giflib_decoder_struct {
    GifFileType *gif;
    const cv::Mat *mat;
    ptrdiff_t read_index;
};

// this structure will help save us work of "reversing" a palette
// we will bit-crush a RGB value and use it to look up one of these
// entries, which if present, prevents us from searching for the
// nearest palette entry for that color
typedef struct {
    uint8_t index;
    uint8_t present;
} encoder_palette_lookup;

struct giflib_encoder_struct {
    GifFileType *gif;
    std::vector<uchar> *dst;

    // palette lookup is a computational-saving structure to convert
    // (reduced-depth) RGB values into the frame's 256-entry palette
    encoder_palette_lookup *palette_lookup;

    // keep track of all of the things we've allocated
    // we could technically just stuff all of these into a vector
    // of void*s but it might be interesting to build a pool
    // for these later, so it makes sense to keep them separated
    // n.b. that even if we do that, giflib still uses hella mallocs
    // when building the decoder, so it would only save us on the encoder
    std::vector<ExtensionBlock *> extension_blocks;
    std::vector<GifByteType *> gif_bytes;
    std::vector<ColorMapObject *> color_maps;
    std::vector<GifColorType *> colors;
    std::vector<SavedImage *> saved_images;
};

int decode_func(GifFileType *gif, GifByteType *buf, int len) {
    auto d = static_cast<giflib_decoder>(gif->UserData);
    size_t buf_len = d->mat->total() - d->read_index;
    size_t read_len = (buf_len > len) ? len : buf_len;
    memmove(buf, d->mat->data + d->read_index, read_len);
    d->read_index += read_len;
    return read_len;
}

giflib_decoder giflib_decoder_create(const opencv_mat buf) {
    giflib_decoder d = new struct giflib_decoder_struct();
    d->gif = NULL;
    d->mat = static_cast<const cv::Mat *>(buf);
    d->read_index = 0;

    int error = 0;
    GifFileType *gif = DGifOpen(d, decode_func, &error);
    if (error) {
        delete d;
        return NULL;
    }
    d->gif = gif;

    return d;
}

int giflib_get_decoder_width(const giflib_decoder d) {
    return d->gif->SWidth;
}

int giflib_get_decoder_height(const giflib_decoder d) {
    return d->gif->SHeight;
}

int giflib_get_decoder_num_frames(const giflib_decoder d) {
    return d->gif->ImageCount;
}

void giflib_decoder_release(giflib_decoder d) {
    int error = 0;
    DGifCloseFile(d->gif, &error);
    delete d;
}

bool giflib_decoder_slurp(giflib_decoder d) {
    int error = DGifSlurp(d->gif);
    return error != GIF_ERROR;
}

bool giflib_decoder_decode(giflib_decoder d, int frame_index, opencv_mat mat) {
    auto cvMat = static_cast<cv::Mat *>(mat);
    GraphicsControlBlock FirstGCB;
    DGifSavedExtensionToGCB(d->gif, 0, &FirstGCB);
    int first_transparency_index = FirstGCB.TransparentColor;
    uint8_t bg_red, bg_green, bg_blue, bg_alpha;
    if (d->gif->SBackGroundColor == first_transparency_index) {
        bg_red = bg_green = bg_blue = bg_alpha = 0;
    } else {
        bg_red = d->gif->SColorMap->Colors[d->gif->SBackGroundColor].Red;
        bg_green = d->gif->SColorMap->Colors[d->gif->SBackGroundColor].Green;
        bg_blue = d->gif->SColorMap->Colors[d->gif->SBackGroundColor].Blue;
        bg_alpha = 255;
    }

    GraphicsControlBlock GCB;
    DGifSavedExtensionToGCB(d->gif, frame_index, &GCB);
    int transparency_index = GCB.TransparentColor;

    SavedImage im = d->gif->SavedImages[frame_index];

    int frame_left = im.ImageDesc.Left;
    int frame_top = im.ImageDesc.Top;
    int frame_width = im.ImageDesc.Width;
    int frame_height = im.ImageDesc.Height;

    int buf_width = cvMat->cols;
    int buf_height = cvMat->rows;

    if (frame_left < 0) {
        return false;
    }

    if (frame_top < 0) {
        return false;
    }

    if (frame_width < 0) {
        return false;
    }

    if (frame_height < 0) {
        return false;
    }

    if (frame_left + frame_width > buf_width) {
        return false;
    }

    if (frame_top + frame_height > buf_height) {
        return false;
    }

    ColorMapObject *globalColorMap = d->gif->SColorMap;
    ColorMapObject *frameColorMap = im.ImageDesc.ColorMap;
    ColorMapObject *colorMap = frameColorMap ? frameColorMap : globalColorMap;

    if (!colorMap) {
        return false;
    }

    if (frame_index == 0) {
        // first frame -- draw the background
        for (size_t y = 0; y < buf_height; y++) {
            uint8_t *dst = cvMat->data + y * cvMat->step;
            for (size_t x = 0; x < buf_width; x++) {
                *dst++ = bg_blue;
                *dst++ = bg_green;
                *dst++ = bg_red;
                *dst++ = bg_alpha;
            }
        }
    }

    if (frame_index > 0) {
        int previous_frame_index = frame_index - 1;
        GraphicsControlBlock prevGCB;
        DGifSavedExtensionToGCB(d->gif, previous_frame_index, &prevGCB);
        int prev_disposal = prevGCB.DisposalMode;
        if (prev_disposal == DISPOSE_BACKGROUND) {
            // draw over the previous frame with the BG color
            // TODO should we do bounds checking here?
            SavedImage prevIM = d->gif->SavedImages[previous_frame_index];
            int prev_frame_left = prevIM.ImageDesc.Left;
            int prev_frame_top = prevIM.ImageDesc.Top;
            int prev_frame_width = prevIM.ImageDesc.Width;
            int prev_frame_height = prevIM.ImageDesc.Height;
            for (int y = prev_frame_top; y < prev_frame_top + prev_frame_height; y++) {
                uint8_t *dst = cvMat->data + y * cvMat->step + (prev_frame_left * 4);
                for (int x = prev_frame_left; x < prev_frame_left + prev_frame_width; x++) {
                    *dst++ = bg_blue;
                    *dst++ = bg_green;
                    *dst++ = bg_red;
                    *dst++ = bg_alpha;
                }
            }
        } else if (prev_disposal == DISPOSE_PREVIOUS) {
            // TODO or maybe not to do
            // should we at least log this happened so that we know this exists?
            // tldr this crazy method requires you to walk back across all previous
            //    frames until you reach one with DISPOSAL_DO_NOT
            //    and "undraw them", most likely would be done by building a temp
            //    buffer when first one is encountered
        }
    }

    // TODO handle interlaced gifs?

    // TODO if top > 0 or left > 0, we could actually just return an ROI
    // of the pixel buffer and then resize just the ROI frame
    // we would then have to rescale the origin coordinates of that frame
    // when encoding back to gif, so that the resized frame is drawn to the
    // correct location

    int bit_index = 0;
    for (int y = frame_top; y < frame_top + frame_height; y++) {
        uint8_t *dst = cvMat->data + y * cvMat->step + (frame_left * 4);
        for (int x = frame_left; x < frame_left + frame_width; x++) {
            GifByteType palette_index = im.RasterBits[bit_index++];
            if (palette_index == transparency_index) {
                // TODO: don't hardcode 4 channels (8UC4) here
                dst += 4;
                continue;
            }
            *dst++ = colorMap->Colors[palette_index].Blue;
            *dst++ = colorMap->Colors[palette_index].Green;
            *dst++ = colorMap->Colors[palette_index].Red;
            *dst++ = 255;
        }
    }

    return true;
}

ExtensionBlock *giflib_encoder_allocate_extension_blocks(giflib_encoder e, size_t count) {
    ExtensionBlock *blocks = (ExtensionBlock*)(malloc(count * sizeof(ExtensionBlock)));
    e->extension_blocks.push_back(blocks);
    return blocks;
}

GifByteType *giflib_encoder_allocate_gif_bytes(giflib_encoder e, size_t count) {
    GifByteType *bytes = (GifByteType*)(malloc(count * sizeof(GifByteType)));
    e->gif_bytes.push_back(bytes);
    return bytes;
}

ColorMapObject *giflib_encoder_allocate_color_maps(giflib_encoder e, size_t count) {
    ColorMapObject *color_maps = (ColorMapObject*)(malloc(count * sizeof(ColorMapObject)));
    e->color_maps.push_back(color_maps);
    return color_maps;
}

GifColorType *giflib_encoder_allocate_colors(giflib_encoder e, size_t count) {
    GifColorType *colors = (GifColorType*)(malloc(count * sizeof(GifColorType)));
    e->colors.push_back(colors);
    return colors;
}

SavedImage *giflib_encoder_allocate_saved_images(giflib_encoder e, size_t count) {
    SavedImage *saved_images = (SavedImage*)(malloc(count * sizeof(SavedImage)));
    e->saved_images.push_back(saved_images);
    return saved_images;
}

int encode_func(GifFileType *gif, const GifByteType *buf, int len) {
    giflib_encoder e = static_cast<giflib_encoder>(gif->UserData);
    e->dst->insert(e->dst->end(), &buf[0], &buf[len]);
    return len;
}

giflib_encoder giflib_encoder_create(vec buf, const giflib_decoder d) {
    giflib_encoder e = new struct giflib_encoder_struct();
    memset(e, 0, sizeof(struct giflib_encoder_struct));
    auto dst = static_cast<std::vector<uchar> *>(buf);
    e->gif = NULL;
    e->dst = dst;

    int error = 0;
    GifFileType *gif_out = EGifOpen(e, encode_func, &error);
    if (error) {
        delete e;
        return NULL;
    }
    e->gif = gif_out;

    GifFileType *gif_in = d->gif;

    // preserve # of palette entries and aspect ratio of original gif
    gif_out->SColorResolution = gif_in->SColorResolution;
    gif_out->AspectByte = gif_in->AspectByte;

    // set up "trailing" extension blocks, which appear after all the frames
    // brian note: what do these do? do we actually need them?
    gif_out->ExtensionBlockCount = gif_in->ExtensionBlockCount;
    gif_out->ExtensionBlocks = NULL;
    if (gif_out->ExtensionBlockCount > 0) {
        gif_out->ExtensionBlocks = giflib_encoder_allocate_extension_blocks(e, gif_out->ExtensionBlockCount);
        for (int i = 0; i < gif_out->ExtensionBlockCount; i++) {
            ExtensionBlock *eb = &(gif_out->ExtensionBlocks[i]);
            eb->ByteCount = gif_in->ExtensionBlocks[i].ByteCount;
            eb->Function = gif_in->ExtensionBlocks[i].Function;
            eb->Bytes = giflib_encoder_allocate_gif_bytes(e, eb->ByteCount);
            memmove(eb->Bytes, gif_in->ExtensionBlocks[i].Bytes, eb->ByteCount);
        }
    }

    // copy global color palette, if any
    if (gif_in->SColorMap) {
        gif_out->SColorMap = giflib_encoder_allocate_color_maps(e, 1);
        memmove(gif_out->SColorMap, gif_in->SColorMap, sizeof(ColorMapObject));
        gif_out->SColorMap->Colors = giflib_encoder_allocate_colors(e, gif_out->SColorMap->ColorCount);
        memmove(gif_out->SColorMap->Colors, gif_in->SColorMap->Colors, gif_out->SColorMap->ColorCount * sizeof(GifColorType));
    }

    // prepare # of frames to match input gif's # of frames
    gif_out->ImageCount = gif_in->ImageCount;
    gif_out->SavedImages = giflib_encoder_allocate_saved_images(e, gif_out->ImageCount);

    // now initialize all frames with input gif's frame metadata
    // this includes, amongst other things, inter-frame delays
    for (size_t frame_index = 0; frame_index < gif_out->ImageCount; frame_index++) {
        SavedImage *im_in = &(gif_in->SavedImages[frame_index]);
        SavedImage *im_out = &(gif_out->SavedImages[frame_index]);

        GifImageDesc *desc = &(im_out->ImageDesc);

        // XXX we're just going to copy here, but this probably isn't right since
        // the decoder doesn't handle interlacing correctly. might be worthwhile to
        // just set this to false always (or enhance the decoder)
        desc->Interlace = im_in->ImageDesc.Interlace;

        // prepare per-frame local palette, if any
        desc->ColorMap = NULL;
        if (im_in->ImageDesc.ColorMap) {
            desc->ColorMap = giflib_encoder_allocate_color_maps(e, 1);
            memmove(desc->ColorMap, im_in->ImageDesc.ColorMap, sizeof(ColorMapObject));
            // copy all of the RGB color values from input frame palette to output frame palette
            desc->ColorMap->Colors = giflib_encoder_allocate_colors(e, desc->ColorMap->ColorCount);
            memmove(desc->ColorMap->Colors, im_in->ImageDesc.ColorMap->Colors, desc->ColorMap->ColorCount * sizeof(GifColorType));
        }

        // copy extension blocks specific to this frame
        // this sets up the frame delay as well as which palette entry is transparent, if any
        im_out->ExtensionBlockCount = im_in->ExtensionBlockCount;
        im_out->ExtensionBlocks = NULL;
        if (im_out->ExtensionBlockCount > 0) {
            // TODO here and in global extension blocks, we should filter out worthless blocks
            // we're only really interested in ExtensionBlock.Function = GRAPHICS_EXT_FUNC_CODE
            // other values like COMMENT_ and PLAINTEXT_ are not essential to viewing the image
            im_out->ExtensionBlocks = giflib_encoder_allocate_extension_blocks(e, im_out->ExtensionBlockCount);
            for (int i = 0; i < im_out->ExtensionBlockCount; i++) {
                ExtensionBlock *eb = &(im_out->ExtensionBlocks[i]);
                eb->ByteCount = im_in->ExtensionBlocks[i].ByteCount;
                eb->Function = im_in->ExtensionBlocks[i].Function;
                eb->Bytes = giflib_encoder_allocate_gif_bytes(e, eb->ByteCount);
                memmove(eb->Bytes, im_in->ExtensionBlocks[i].Bytes, eb->ByteCount);
            }
        }

        // we won't allocate raster bits here since that depends on each frame's dimensions
        // since those will change with resizing, we can't guess here
        im_out->RasterBits = NULL;
    }

    // encoder is now set up with the correct number of frames and image metadata, delays etc
    // ready to receive the rasterized frames

    // set up palette lookup table. we need 2^15 entries because we will be
    // using bit-crushed RGB values, 5 bits each. this is a reasonable compromise
    // between fidelity and computation/storage
    e->palette_lookup = (encoder_palette_lookup*)(malloc((1 << 15) * sizeof(encoder_palette_lookup)));

    return e;
}

// this function should be called just once when we know the global dimensions
bool giflib_encoder_init(giflib_encoder e, int width, int height) {
    e->gif->SWidth = width;
    e->gif->SHeight = height;
    return true;
}

// TODO this probably should be the euclidean distance
// the manhattan distance will still be "good enough"
// euclidean requires calculating pow(2) and sqrt()?
static inline int rgb_distance(int r0, int g0, int b0, int r1, int g1, int b1) {
    int dist = 0;
    dist += (r0 > r1) ? r0 - r1 : r1 - r0;
    dist += (g0 > g1) ? g0 - g1 : g1 - g0;
    dist += (b0 > b1) ? b0 - b1 : b1 - b0;
    return dist;
}

bool giflib_encoder_encode_frame(giflib_encoder e, int frame_index, const opencv_mat opaque_frame) {
    GifFileType *gif_out = e->gif;
    auto frame = static_cast<const cv::Mat *>(opaque_frame);

    // basic bounds checking - would this frame be wider than the global gif width?
    // if we do partial frames, we'll need to change this to account for top/left
    if (frame->cols > gif_out->SWidth) {
        return false;
    }

    if (frame->rows > gif_out->SHeight) {
        return false;
    }

    SavedImage *im_out = &(gif_out->SavedImages[frame_index]);
    // TODO some day consider making partial frames/make these not 0
    GifImageDesc *desc = &(im_out->ImageDesc);
    desc->Left = 0;
    desc->Top = 0;
    desc->Width = frame->cols;
    desc->Height = frame->rows;

    // each gif frame pixel is an entry in a (at most) 256-sized palette
    // so each pixel needs one byte
    im_out->RasterBits = giflib_encoder_allocate_gif_bytes(e, desc->Width * desc->Height);

    ColorMapObject *global_color_map = e->gif->SColorMap;
    ColorMapObject *frame_color_map = desc->ColorMap;
    ColorMapObject *color_map = frame_color_map ? frame_color_map : global_color_map;

    if (!color_map) {
        return false;
    }

    // prepare our palette lookup table. if we used the same (byte-equal) palette table last
    // frame, we can just reuse it this frame. otherwise we need to clear the lookup out
    bool clear_palette_lookup = true;
    // on the first frame, we will always clear
    if (frame_index != 0) {
        int last_frame_index = frame_index - 1;
        SavedImage *last_im_out = &(gif_out->SavedImages[last_frame_index]);
        ColorMapObject *last_frame_color_map = last_im_out->ImageDesc.ColorMap;
        ColorMapObject *last_color_map = last_frame_color_map ? last_frame_color_map : global_color_map;
        if (last_color_map && last_color_map->ColorCount == color_map->ColorCount) {
            int cmp = memcmp(last_color_map->Colors, color_map->Colors, color_map->ColorCount * sizeof(GifColorType));
            clear_palette_lookup = (cmp != 0);
        }
    }

    if (clear_palette_lookup) {
        memset(e->palette_lookup, 0, (1 << 15) * sizeof(encoder_palette_lookup));
    }

    GraphicsControlBlock GCB;
    // technically meant for use by decoder only, but the type is the same (*GifFile)
    // and we've copied all of these over
    DGifSavedExtensionToGCB(e->gif, frame_index, &GCB);
    int transparency_index = GCB.TransparentColor;
    bool have_transparency = (transparency_index != NO_TRANSPARENT_COLOR);

    // convenience names for these dimensions
    int frame_left = im_out->ImageDesc.Left;
    int frame_top = im_out->ImageDesc.Top;
    int frame_width = im_out->ImageDesc.Width;
    int frame_height = im_out->ImageDesc.Height;

    GifByteType *raster_out = im_out->RasterBits;

    int raster_index = 0;
    for (int y = frame_top; y < frame_top + frame_height; y++) {
        uint8_t *src = frame->data + y * frame->step + (frame_left * 4);
        for (int x = frame_left; x < frame_left + frame_width; x++) {
            uint32_t B = *src++;
            uint32_t G = *src++;
            uint32_t R = *src++;
            uint32_t A = *src++;

            // TODO come up with what this threshold value should be
            // probably ought to be a lot smaller, but greater than 0
            // for now we just pick halfway
            if (A < 128 && have_transparency) {
                // this composite frame pixel is actually transparent
                // what this means is that the background color must be transparent
                // AND this frame pixel must be transparent
                // for now we'll just assume bg is transparent since otherwise decoder
                // could not have generated this frame pixel with a low opacity
                *raster_out++ = transparency_index;
                continue;
            }

            uint32_t crushed = ((R >> 3) << 10) | ((G >> 3) << 5) | ((B >> 3));
            if (!(e->palette_lookup[crushed].present)) {
                // calculate the best palette entry based on the midpoint of the crushed colors
                // what this means is that we drop the crushed bits (& 0xf8)
                // and then OR the highest-order crushed bit back in, which is approx midpoint
                uint32_t R_center = (R & 0xf8) | 4;
                uint32_t G_center = (G & 0xf8) | 4;
                uint32_t B_center = (B & 0xf8) | 4;

                // we're calculating the best, so keep track of which palette entry has least distance
                int least_dist = INT_MAX;
                int best_color = 0;
                int count = color_map->ColorCount;
                for (int i = 0; i < count; i++) {
                    int dist = rgb_distance(R_center, G_center, B_center, color_map->Colors[i].Red,
                                            color_map->Colors[i].Green, color_map->Colors[i].Blue);
                    if (dist < least_dist) {
                        least_dist = dist;
                        best_color = i;
                    }
                }
                e->palette_lookup[crushed].present = 1;
                e->palette_lookup[crushed].index = best_color;
            }

            // now that we for sure know which palette entry to pick, we have one more test
            // to perform. it's possible that the best color for this pixel is actually
            // the color of this pixel in the previous frame. if that's true, we'll just
            // choose the transparency color, which will compress better on average
            // (plus it improves color range of image)
            // TODO implement this by having encoder preserve previous BGRA frame somewhere

            *raster_out++ = e->palette_lookup[crushed].index;
        }
    }

    return true;
}

bool giflib_encoder_spew(giflib_encoder e) {
    int error = EGifSpew(e->gif);
    if (error == GIF_ERROR) {
        return false;
    }
    // for some reason giflib closes/frees the gif when you spew
    // ??????????????
    // so we'll set it to NULL now
    e->gif = NULL;
    return true;
}

void giflib_encoder_release(giflib_encoder e) {
    // don't free dst -- we're borrowing it

    if (e->palette_lookup) {
        free(e->palette_lookup);
    }

    for (std::vector<ExtensionBlock *>::iterator it = e->extension_blocks.begin(); it != e->extension_blocks.end(); ++it) {
        free(*it);
    }
    e->extension_blocks.clear();

    for (std::vector<GifByteType *>::iterator it = e->gif_bytes.begin(); it != e->gif_bytes.end(); ++it) {
        free(*it);
    }
    e->gif_bytes.clear();

    for (std::vector<ColorMapObject *>::iterator it = e->color_maps.begin(); it != e->color_maps.end(); ++it) {
        free(*it);
    }
    e->color_maps.clear();

    for (std::vector<GifColorType *>::iterator it = e->colors.begin(); it != e->colors.end(); ++it) {
        free(*it);
    }
    e->colors.clear();

    for (std::vector<SavedImage *>::iterator it = e->saved_images.begin(); it != e->saved_images.end(); ++it) {
        free(*it);
    }
    e->saved_images.clear();

    if (e->gif) {
        // we most likely won't actually call this since Spew() does it
        // but in exceptional cases we'll need it for cleanup
        EGifCloseFile(e->gif, NULL);
    }

    delete e;
}
