/*
 * Copyright (C) 2010 Samsung Electronics Co.,LTD., for the ODROID code
 * Copyright (C) 2012 The Android Open Source Project, for the samsung_slsi/exynos5 code
 * Copyright (C) 2013 Paul Kocialkowski, for the V4L2 code
 * Copyright (C) 2017 The NamelessRom Project, for making it work
 * Copyright (C) 2017 The LineageOS Project, for making it work better
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <utils/String8.h>

#include "hwcomposer.h"
#include "hwcomposer_vsync.h"

const size_t BURSTLEN_BYTES = 16 * 8;
const size_t MAX_PIXELS = 12 * 1024 * 1000;

template<typename T> inline T max(T a, T b) { return (a > b) ? a : b; }
template<typename T> inline T min(T a, T b) { return (a < b) ? a : b; }

/*****************************************************************************/

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device);

static struct hw_module_methods_t hwc_module_methods = {
    .open = hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = HWC_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = HWC_HARDWARE_MODULE_ID,
        .name = "Exynos4 HWComposer",
        .author = "The NamelessRom Project",
        .methods = &hwc_module_methods,
        .dso = NULL,
        .reserved = {0},
    },
};

/******************************************************************************/

static void dump_layer(hwc_layer_1_t const* l, const char *function) {
    private_handle_t *handle = (private_handle_t *) l->handle;

    if (handle)
        ALOGV("%s \ttype=%d, flags=%08x, handle=%p, format=0x%x, tr=%02x, blend=%04x, {%d,%d,%d,%d}, {%d,%d,%d,%d}", function,
                l->compositionType, l->flags, l->handle, handle->format, l->transform, l->blending,
                l->sourceCrop.left,
                l->sourceCrop.top,
                l->sourceCrop.right,
                l->sourceCrop.bottom,
                l->displayFrame.left,
                l->displayFrame.top,
                l->displayFrame.right,
                l->displayFrame.bottom);
    else
        ALOGV("%s \ttype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, {%d,%d,%d,%d}, {%d,%d,%d,%d}", function,
                l->compositionType, l->flags, l->handle, l->transform, l->blending,
                l->sourceCrop.left,
                l->sourceCrop.top,
                l->sourceCrop.right,
                l->sourceCrop.bottom,
                l->displayFrame.left,
                l->displayFrame.top,
                l->displayFrame.right,
                l->displayFrame.bottom);
}

static int dup_or_warn(int fence)
{
    int dup_fd = dup(fence);
    if (dup_fd < 0)
        ALOGW("fence dup failed: %s", strerror(errno));
    return dup_fd;
}

static bool format_is_supported_by_fimg(int format)
{
    switch (format) {
    case HAL_PIXEL_FORMAT_RGB_565:
    case HAL_PIXEL_FORMAT_RGBA_4444:
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_BGRA_8888:
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_YCbCr_420_SP:
    case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_420_SP:
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
    case HAL_PIXEL_FORMAT_CUSTOM_YCrCb_420_SP:
        return true;

    default:
        ALOGV("%s format=%d false", __FUNCTION__, format);
        return false;
    }
}

static bool format_is_supported_by_fimc(int format)
{
    return (HAL_PIXEL_FORMAT_2_V4L2_PIX(format) > 0);
}

static enum gsc_map_t::mode format_requires_process(int format)
{
    switch (format) {
    case HAL_PIXEL_FORMAT_RGBA_8888: //format is supported by fimd so there's no need to check
    case HAL_PIXEL_FORMAT_RGBX_8888:
        return gsc_map_t::NONE;

    default:
        if (is_yuv_format(format))
            return gsc_map_t::FIMC;
        else
            return gsc_map_t::NONE;
    }
}

static bool is_transformed(const hwc_layer_1_t &layer)
{
    return layer.transform != 0;
}

static bool is_rotated(const hwc_layer_1_t &layer)
{
    return (layer.transform & HAL_TRANSFORM_ROT_90) ||
            (layer.transform & HAL_TRANSFORM_ROT_180);
}

static bool is_scaled(const hwc_layer_1_t &layer)
{
    hwc_rect_t crop = integerizeSourceCrop(layer.sourceCropf);
    return WIDTH(layer.displayFrame) != WIDTH(crop) ||
           HEIGHT(layer.displayFrame) != HEIGHT(crop);
}

static bool supports_fimg(const hwc_layer_1_t &layer)
{
    private_handle_t *handle = private_handle_t::dynamicCast(layer.handle);

    int max_w = 8000; //taken from kernel: drivers/media/video/samsung/fimg2d4x-exynos4/fimg2d_ctx.c
    int max_h = 8000; //fimg2d_check_params()

    return format_is_supported_by_fimg(handle->format) &&
           handle->stride <= max_w &&
           handle->height <= max_h;
}

static bool supports_fimc(const hwc_layer_1_t &layer)
{
    private_handle_t *handle = private_handle_t::dynamicCast(layer.handle);

/* TODO: look in kernel for min/max size */

    return format_is_supported_by_fimc(handle->format);
}

static bool format_is_supported(int format)
{
    return format_to_s3c_format(format) < S3C_FB_PIXEL_FORMAT_MAX;
}

static bool is_x_aligned(const hwc_layer_1_t &layer)
{
    private_handle_t *handle = private_handle_t::dynamicCast(layer.handle);

    if (!format_is_supported(handle->format))
        return true;

    uint8_t bpp = format_to_bpp(handle->format);
    uint8_t pixel_alignment = 32 / bpp;

    return (layer.displayFrame.left % pixel_alignment) == 0 &&
            (layer.displayFrame.right % pixel_alignment) == 0;
}

static bool is_contiguous(const hwc_layer_1_t &layer)
{
    private_handle_t *handle = private_handle_t::dynamicCast(layer.handle);
    ALOGV("%s: handle %p, flags=0x%x, usage=0x%x, paddr=0x%x", __func__, handle,
            handle->flags, handle->usage, handle->paddr);

    if (handle->paddr != 0) {
        return true;
    }
    return false;
}

static enum gsc_map_t::mode layer_requires_process(hwc_layer_1_t &layer)
{
    private_handle_t *handle = private_handle_t::dynamicCast(layer.handle);
    enum gsc_map_t::mode mode;

    mode = format_requires_process(handle->format);

    switch(mode) {
    case gsc_map_t::NONE:
        // FIMG adds noticable touch latency and also seems to break the panel.
        // Disable it for now
        /*if (is_scaled(layer) || is_transformed(layer) || !is_x_aligned(layer) || !is_contiguous(layer)) {
            ALOGV("%s: direct render -> fimg because is_scaled(%d) is_transformed(%d) is_x_aligned(%d)",
                    __FUNCTION__, is_scaled(layer), is_transformed(layer), is_x_aligned(layer));
            mode = gsc_map_t::FIMG;
        }*/
        break;

    case gsc_map_t::FIMC:
        if (is_scaled(layer)) {
            // FIMC _should_ be able to do scaling
            // but it doesn't seem to work well.
            // Disable it for now.
            mode = gsc_map_t::NONE;
        }
        break;

    default:
        break;
    }

    return mode;
}

size_t visible_width(struct hwc_context_t *ctx, hwc_layer_1_t &layer)
{
    private_handle_t *handle = private_handle_t::dynamicCast(layer.handle);
    int bpp;

    if (layer_requires_process(layer))
        bpp = 32;
    else
        bpp = format_to_bpp(handle->format);

    int left = max(layer.displayFrame.left, 0);
    int right = min(layer.displayFrame.right, ctx->xres);

    return (right - left) * bpp / 8;
}

static bool blending_is_supported(int32_t blending)
{
    return blending_to_s3c_blending(blending) < S3C_FB_BLENDING_MAX;
}

bool is_offscreen(struct hwc_context_t *ctx, hwc_layer_1_t &layer)
{
    return layer.displayFrame.left > ctx->xres ||
            layer.displayFrame.right < 0 ||
            layer.displayFrame.top > ctx->yres ||
            layer.displayFrame.bottom < 0;
}

bool is_overlay_supported(struct hwc_context_t *ctx, hwc_layer_1_t &layer, size_t i)
{
   if(layer.flags & HWC_SKIP_LAYER  || !layer.handle) {
        ALOGV("%s::is_skip_layer  %d  layer.handle %x ",
                __func__, layer.flags & HWC_SKIP_LAYER, layer.handle);

        return false;
    }

    bool result = false;
    private_handle_t *prev_handle = (private_handle_t *)(layer.handle);

    if (i == 0) {
    /* check here....if we have any resolution constraints */
        if (((layer.sourceCrop.right - layer.sourceCrop.left + 1) < 16) ||
            ((layer.sourceCrop.bottom - layer.sourceCrop.top + 1) < 8))
            return result;

        if ((layer.transform == HAL_TRANSFORM_ROT_90) ||
            (layer.transform == HAL_TRANSFORM_ROT_270)) {
            if (((layer.displayFrame.right - layer.displayFrame.left + 1) < 4) ||
                ((layer.displayFrame.bottom - layer.displayFrame.top + 1) < 8))
                return result;
        } else if (((layer.displayFrame.right - layer.displayFrame.left + 1) < 8) ||
                   ((layer.displayFrame.bottom - layer.displayFrame.top + 1) < 4)) {
            return result;
        }

        switch (prev_handle->format) {
        case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_420_SP:
        case HAL_PIXEL_FORMAT_CUSTOM_YCrCb_420_SP:
        case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_420_SP_TILED:
            result = true;
            break;
        case HAL_PIXEL_FORMAT_YV12:                 /* YCrCb_420_P */
        case HAL_PIXEL_FORMAT_YCbCr_420_P:
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
        case HAL_PIXEL_FORMAT_YCbCr_420_SP:
            if ((prev_handle->usage & GRALLOC_USAGE_HWC_HWOVERLAY) &&
                 (layer.blending == HWC_BLENDING_NONE))
                result = true;
            else
                result = false;
            break;
        default:
            result = false;
            break;
        }
    }

    ALOGV("%s:(%s)=>0:FB,1:OVERLAY \r\n"
            "   format(0x%x),magic(0x%x),flags(%d),size(%d),offset(%d)"
            "b_addr(0x%x),usage(%d),w(%d),h(%d),bpp(%d)",
            __func__, result ? "true" : "false",
            prev_handle->format, prev_handle->magic, prev_handle->flags,
            prev_handle->size, prev_handle->offset, prev_handle->base,
            prev_handle->usage, prev_handle->width, prev_handle->height,
            prev_handle->bpp);

    return result;
}

void determineSupportedOverlays(hwc_context_t *ctx, hwc_display_contents_1_t *contents)
{
    // spammy
    ALOGV("%s: Preparing %u layers for FIMD", __FUNCTION__, contents->numHwLayers);

    for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
        ctx->win[i].layer_index = -1;
        ctx->win[i].src_buf = NULL;
    }

    ctx->fb_needed = false;
    ctx->first_fb = 0;
    ctx->last_fb = 0;

#if DEBUG_SPAMMY
    for (size_t i=0 ; i < contents->numHwLayers ; i++)
        dump_layer(&contents->hwLayers[i], __FUNCTION__);
#endif

    // find unsupported overlays
    for (size_t i=0 ; i < contents->numHwLayers ; i++) {
        hwc_layer_1_t &layer = contents->hwLayers[i];

        if (layer.compositionType == HWC_FRAMEBUFFER_TARGET) {
            ALOGV("\tlayer %u: framebuffer target", i);
            continue;
        }

        if (layer.compositionType == HWC_BACKGROUND && !ctx->force_fb) {
            ALOGV("\tlayer %u: background supported", i);
            continue;
        }

        if (is_overlay_supported(ctx, contents->hwLayers[i], i) && !ctx->force_fb) {
            ALOGV("\tlayer %u: overlay supported", i);
            layer.compositionType = HWC_OVERLAY;
            continue;
        } else {
            ALOGV("%s: layer %u: overlay not supported, force_fb(%d)", __FUNCTION__, i, ctx->force_fb);
        }

        if (!ctx->fb_needed) {
            ctx->first_fb = i;
            ctx->fb_needed = true;
        }
        ctx->last_fb = i;
        layer.compositionType = HWC_FRAMEBUFFER;
    }

    // If there is EGL composition, the FRAMEBUFFER_TARGET should be at least the last hw window
    ctx->first_fb = min(ctx->first_fb, (size_t)NUM_HW_WINDOWS-1);

    // can't composite overlays sandwiched between framebuffers
    if (ctx->fb_needed) {
        for (size_t i = ctx->first_fb; i < ctx->last_fb; i++) {
            if (contents->hwLayers[i].compositionType != HWC_FRAMEBUFFER) {
                ALOGV("%s layer %d sandwiched between FB, set to FB too", __FUNCTION__, i);
                contents->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
            }
        }
    }

    ALOGV("%s: fb_needed(%d) first_fb(%d) last_fb(%d)", __FUNCTION__, ctx->fb_needed, ctx->first_fb, ctx->last_fb);
#if DEBUG_SPAMMY
    for (size_t i=0 ; i < contents->numHwLayers ; i++)
        dump_layer(&contents->hwLayers[i], __FUNCTION__);
#endif
}

void determineBandwidthSupport(hwc_context_t *ctx, hwc_display_contents_1_t *contents)
{
#if DEBUG_SPAMMY
    for (size_t i=0 ; i < contents->numHwLayers ; i++)
        dump_layer(&contents->hwLayers[i], __FUNCTION__);
#endif

    // Incrementally try to add our supported layers to hardware windows.
    // If adding a layer would violate a hardware constraint, force it
    // into the framebuffer and try again.  (Revisiting the entire list is
    // necessary because adding a layer to the framebuffer can cause other
    // windows to retroactively violate constraints.)
    bool changed;
    bool fimg_used;
    bool fimc_used;
    int yuv_layer; //which layer has yuv content
    int rgb_over_yuv_layer; //number of rgb layers over a yuv layer

    do {
        size_t pixels_left, windows_left;

        if (ctx->fb_needed) {
            pixels_left = MAX_PIXELS - (ctx->xres * ctx->yres);
            windows_left = NUM_HW_WINDOWS - 1;
        } else {
            pixels_left = MAX_PIXELS;
            windows_left = NUM_HW_WINDOWS;
        }

        changed = false;
#ifdef NO_FIMG
        // disable HW composition using FIMG
        fimg_used = true;
#else
        fimg_used = false;
#endif
        fimc_used = false;
        yuv_layer = -1;
        rgb_over_yuv_layer = 0;

        for (size_t i = 0; i < contents->numHwLayers; i++) {
            hwc_layer_1_t &layer = contents->hwLayers[i];

            if (layer.flags & HWC_SKIP_LAYER) {
                ALOGV("%s: layer %u skipping", __FUNCTION__, i);
                continue;
            }

            if (layer.compositionType == HWC_FRAMEBUFFER_TARGET) {
                ALOGV("%s: layer %u HWC_FRAMEBUFFER_TARGET", __FUNCTION__, i);
                continue;
            }

            // we've already accounted for the framebuffer above
            if (layer.compositionType == HWC_FRAMEBUFFER) {
                ALOGV("%s: layer %u HWC_FRAMEBUFFER", __FUNCTION__, i);
                continue;
            }

            // only layer 0 can be HWC_BACKGROUND, so we can unconditionally allow it without extra checks
            if (layer.compositionType == HWC_BACKGROUND) {
                ALOGV("%s: layer %u HWC_BACKGROUND", __FUNCTION__, i);
                windows_left--;
                continue;
            }

            size_t pixels_needed = WIDTH(layer.displayFrame) * HEIGHT(layer.displayFrame);
            bool can_compose = true;
            bool gsc_required = false;

            do {
                can_compose = windows_left && pixels_needed <= pixels_left;
                if (!can_compose) {
                    ALOGV("%s: can't compose layer %d, %s, pixels_needed(%d) pixels_left(%d)", 
                            __FUNCTION__, i, windows_left ? "no more pixels left" : "no more windows left", pixels_needed, pixels_left);
                    break;
                }

                enum gsc_map_t::mode mode = layer_requires_process(layer);
                private_handle_t *handle = private_handle_t::dynamicCast(layer.handle);

                switch (mode) {
                    case gsc_map_t::FIMC:
                        ALOGV("%s: layer %u: mode=FIMC", __FUNCTION__, i);
                        can_compose = can_compose && !fimc_used;
                        fimc_used = true;
                        if (!can_compose && format_is_supported_by_fimg(handle->format)) {
                            can_compose = true;
                            // fall through to FIMG
                            mode = gsc_map_t::FIMG;
                        } else {
                            yuv_layer = i;
                            break;
                        }

                    case gsc_map_t::FIMG:
                        ALOGV("%s: layer %u: mode=FIMG can_compose(%d) fimg_used(%d)", __FUNCTION__, i, can_compose, fimg_used);

                        if (yuv_layer >= 0 && i > yuv_layer) {
                            ++rgb_over_yuv_layer;

                            if (rgb_over_yuv_layer > 1) {
                                ALOGV("%s: layer %u can't be composed, since there are too much rgb layers over a yuv one", __FUNCTION__, i);
                                can_compose = false;
                            }
                        }

                        if (can_compose) {
#ifndef NO_FIMG
                            if (!ctx->multi_fimg)
#endif
                                can_compose = can_compose && !fimg_used;
                            fimg_used = true;
                        }

                        break;


                    default:
                        ALOGV("%s: layer %u: mode=%d", __FUNCTION__, i, mode);
                        can_compose = true;
                        break;
                };

                if (!can_compose) {
                    ALOGV("%s: can't compose layer %d, mode(%d)", __FUNCTION__, i, (int) mode);
                }

                break; //we only want to perform the "can_compose" check once
            } while (1);

            if (!can_compose) {
                layer.compositionType = HWC_FRAMEBUFFER;
                if (!ctx->fb_needed) {
                    ctx->first_fb = ctx->last_fb = i;
                    ctx->fb_needed = true;
                } else {
                    ctx->first_fb = min(i, ctx->first_fb);
                    ctx->last_fb = max(i, ctx->last_fb);
                }
                changed = true;
                ctx->first_fb = min(ctx->first_fb, (size_t)NUM_HW_WINDOWS-1); // odroid has this
                ALOGV("%s: i(%d) can't compose, fb_needed(%d) first_fb(%d) last_fb(%d)", __FUNCTION__, i, ctx->fb_needed, ctx->first_fb, ctx->last_fb);
                break;
            }

            pixels_left -= pixels_needed;
            windows_left--;
        }

        if (changed) {
            for (size_t i = ctx->first_fb; i < ctx->last_fb; i++) {
                if (contents->hwLayers[i].compositionType != HWC_FRAMEBUFFER) {
                    ALOGV("%s: changed, layer %d set to FB", __FUNCTION__, i);
                    contents->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
                }
            }
        }

    } while(changed);

    ALOGV("%s: fb_needed(%d) first_fb(%d) last_fb(%d)", __FUNCTION__, ctx->fb_needed, ctx->first_fb, ctx->last_fb);
#if DEBUG_SPAMMY
    for (size_t i=0 ; i < contents->numHwLayers ; i++)
        dump_layer(&contents->hwLayers[i], __FUNCTION__);
#endif
}

void assignWindows(hwc_context_t *ctx, hwc_display_contents_1_t *contents)
{
    unsigned int nextWindow = 0;
    enum gsc_map_t::mode mode;
    bool fimc_used = false;
    for (size_t i = 0; i < contents->numHwLayers; i++) {
        hwc_layer_1_t &layer = contents->hwLayers[i];

        if (ctx->fb_needed && i == ctx->first_fb) {
            ALOGV("%s: assigning framebuffer to window %u\n", __FUNCTION__, nextWindow);
            ctx->fb_window = nextWindow;
            ctx->win[nextWindow].gsc.mode = gsc_map_t::NONE;
            nextWindow++;
            continue;
        }

        if (layer.compositionType != HWC_FRAMEBUFFER && layer.compositionType != HWC_FRAMEBUFFER_TARGET) {
            ALOGV("assigning layer %u to window %u", i, nextWindow);
            ctx->win[nextWindow].layer_index = i;
            if (layer.compositionType == HWC_OVERLAY) {
                private_handle_t *handle = private_handle_t::dynamicCast(layer.handle);

                ctx->win[nextWindow].src_buf = handle;
                layer.hints = HWC_HINT_CLEAR_FB;

                mode = layer_requires_process(layer);

                switch (mode) {
                case gsc_map_t::FIMG:
                case gsc_map_t::FIMC:
                    if (mode == gsc_map_t::FIMC) {
                        if (fimc_used)
                            mode = gsc_map_t::FIMG;
                        else fimc_used = true;
                    }
                    ALOGV("\tlayer(%d) using FIM%c for format(%d)", i, (mode == gsc_map_t::FIMG)?'G':'C', handle->format);
                    ctx->win[nextWindow].gsc.mode = mode;

                    //Should ION memory for FIMC/FIMG operation be re-/allocated?
                    if ((WIDTH(layer.displayFrame) != ctx->win[nextWindow].rect_info.w) ||
                        (HEIGHT(layer.displayFrame) != ctx->win[nextWindow].rect_info.h)) {

                        window_buffer_deallocate(ctx, &ctx->win[nextWindow]);

                        ctx->win[nextWindow].rect_info.x = layer.displayFrame.left;
                        ctx->win[nextWindow].rect_info.y = layer.displayFrame.top;
                        ctx->win[nextWindow].rect_info.w = WIDTH(layer.displayFrame);
                        ctx->win[nextWindow].rect_info.h = HEIGHT(layer.displayFrame);

                        window_buffer_allocate(ctx, &ctx->win[nextWindow]);
                    } else {
                        if (!ctx->win[nextWindow].dst_buf[ctx->win[nextWindow].current_buf]) {
                            window_buffer_allocate(ctx, &ctx->win[nextWindow]);
                        }
                    }

                    break;

                default:
                    ctx->win[nextWindow].gsc.mode = mode;
                    break;
                }
            }
            nextWindow++;
        }
    }
}

static int prepare_fimd(hwc_context_t *ctx, hwc_display_contents_1_t* contents)
{
    ctx->force_fb = ctx->force_gpu;

    if (ctx->bypass_count > 0) {
        ctx->force_fb = true;
    }

    determineSupportedOverlays(ctx, contents);
    determineBandwidthSupport(ctx, contents);
    assignWindows(ctx, contents);

    if (ctx->fb_needed)
        ctx->fb_window = ctx->first_fb;
    else
        ctx->fb_window = NO_FB_NEEDED;

    return 0;
}

static int hwc_prepare(hwc_composer_device_1_t *dev, size_t numDisplays, hwc_display_contents_1_t** displays)
{
    if (!numDisplays || !displays)
        return 0;

    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;
    hwc_display_contents_1_t *fimd_contents = displays[HWC_DISPLAY_PRIMARY];

    if (fimd_contents) {
        int err = prepare_fimd(ctx, fimd_contents);
        if (err)
            return err;
    }

    return 0;
}

static int perform_fimg(hwc_context_t *ctx, const hwc_layer_1_t &layer, struct hwc_win_info_t &win, int dst_format)
{
    private_handle_t *src_handle = private_handle_t::dynamicCast(layer.handle);
    int ret = 0;
    int retry = 0;
    enum color_format g2d_format = CF_XRGB_8888;
    enum pixel_order pixel_order = AX_RGB;
    unsigned int src_bpp = 0, dst_bpp = 0;
    struct fimg2d_image *g2d_src_img = &win.g2d_src_img;
    struct fimg2d_image *g2d_dst_img = &win.g2d_dst_img;
    struct fimg2d_blit *fimg_cmd = &win.fimg_cmd;
    enum rotation l_rotate;
    struct private_handle_t *dst_handle = private_handle_t::dynamicCast(win.dst_buf[win.current_buf]);
    uint32_t dst_addr = (uint32_t) dst_handle->ion_memory;

    //should we need to fill whole fimg_cmd struct or can we reuse the old command?
    memset(fimg_cmd, 0, sizeof(struct fimg2d_blit));
    memset(g2d_src_img, 0, sizeof(struct fimg2d_image));
    memset(g2d_dst_img, 0, sizeof(struct fimg2d_image));

    fimg_cmd->op = BLIT_OP_SRC;
    fimg_cmd->seq_no = SEQ_NO_BLT_HWC_NOSEC;
    fimg_cmd->sync = BLIT_SYNC;
    fimg_cmd->param.g_alpha = 255;
    fimg_cmd->param.rotate = rotateValueHAL2G2D(layer.transform);

    hwc_rect_t crop = integerizeSourceCrop(layer.sourceCropf);

    if (is_scaled(layer) || fimg_cmd->param.rotate != ORIGIN) {
        fimg_cmd->param.scaling.mode = SCALING_BILINEAR;
        fimg_cmd->param.scaling.src_w = WIDTH(crop);
        fimg_cmd->param.scaling.src_h = HEIGHT(crop);

        if (fimg_cmd->param.rotate == ROT_90 || fimg_cmd->param.rotate == ROT_270) {
            fimg_cmd->param.scaling.dst_w = HEIGHT(layer.displayFrame);
            fimg_cmd->param.scaling.dst_h = WIDTH(layer.displayFrame);
        } else {
            fimg_cmd->param.scaling.dst_w = WIDTH(layer.displayFrame);
            fimg_cmd->param.scaling.dst_h = HEIGHT(layer.displayFrame);
        }
    }

    if (src_handle->base) {
        formatValueHAL2G2D(src_handle->format, &g2d_format, &pixel_order, &src_bpp);

        g2d_src_img->addr.type = ADDR_USER;
        g2d_src_img->addr.start = src_handle->base;

        if (src_bpp == 1) {
            g2d_src_img->plane2.type = ADDR_USER;
            g2d_src_img->plane2.start = src_handle->base + src_handle->uoffset;
        }

        g2d_src_img->width = src_handle->width;
        g2d_src_img->height = src_handle->height;
        g2d_src_img->stride = g2d_src_img->width * src_bpp;
        g2d_src_img->order = (enum pixel_order) pixel_order;
        g2d_src_img->fmt = (enum color_format) g2d_format;
        g2d_src_img->rect.x1 = crop.left;
        g2d_src_img->rect.y1 = crop.top;
        g2d_src_img->rect.x2 = crop.right;
        g2d_src_img->rect.y2 = crop.bottom;
        g2d_src_img->need_cacheopr = false;

        fimg_cmd->src = g2d_src_img;
    }

    if (dst_addr > 0) {
        formatValueHAL2G2D(dst_format, &g2d_format, &pixel_order, &dst_bpp);

        g2d_dst_img->addr.type = ADDR_USER;
        g2d_dst_img->addr.start = dst_addr;

        g2d_dst_img->width = WIDTH(layer.displayFrame);
        g2d_dst_img->height = HEIGHT(layer.displayFrame);

        if (dst_bpp == 1) {
            g2d_dst_img->plane2.type = ADDR_USER;
            g2d_dst_img->plane2.start = dst_addr + (EXYNOS4_ALIGN(g2d_dst_img->width, 16) * EXYNOS4_ALIGN(g2d_dst_img->height, 16));
        }

        g2d_dst_img->stride = g2d_dst_img->width * dst_bpp;
        g2d_dst_img->order = (enum pixel_order) pixel_order;
        g2d_dst_img->fmt = (enum color_format) g2d_format;
        g2d_dst_img->rect.x1 = 0;
        g2d_dst_img->rect.y1 = 0;
        g2d_dst_img->rect.x2 = WIDTH(layer.displayFrame);
        g2d_dst_img->rect.y2 = HEIGHT(layer.displayFrame);
        g2d_dst_img->need_cacheopr = false;

        fimg_cmd->dst = g2d_dst_img;
    }

    if (layer.acquireFenceFd >= 0) {
        sync_wait(layer.acquireFenceFd, 1000);
    }

    ret = stretchFimgApi(fimg_cmd);
    if (ret < 0) {
        ALOGE("%s: stretch failed", __FUNCTION__);
        dump_layer(&layer, __FUNCTION__);
    }

    if (layer.acquireFenceFd >= 0) {
        close(layer.acquireFenceFd);
    }

    return ret;
}

static int perform_fimc(hwc_context_t *ctx, const hwc_layer_1_t &layer, struct hwc_win_info_t &win)
{
    private_handle_t *src_handle = private_handle_t::dynamicCast(layer.handle);
    int ret = 0;

    // before sending anything to FIMC
    if (layer.acquireFenceFd >= 0) {
        sync_wait(layer.acquireFenceFd, 1000);
    }

    ret = v4l2_reqbufs_out(ctx, 0);
    if (ret < 0) {
        ALOGE("%s: v4l2_reqbufs_out() rc=%d", __FUNCTION__, ret);
        return ret;
    }

    //src
    hwc_rect_t crop = integerizeSourceCrop(layer.sourceCropf);

    ret = v4l2_s_fmt_pix_out(ctx, EXYNOS4_ALIGN(src_handle->width, 16), EXYNOS4_ALIGN(src_handle->height, 16), HAL_PIXEL_FORMAT_2_V4L2_PIX(src_handle->format), 0);
    if (ret < 0) {
        ALOGE("%s: v4l2_s_fmt_pix_out() rc=%d", __FUNCTION__, ret);
        return ret;
    }

    //some yuv formats do need width and height to be multiple of 2
    ret = v4l2_s_crop(ctx, crop.left, crop.top, multipleOf2(WIDTH(crop)), multipleOf2(HEIGHT(crop)));
    if (ret < 0) {
        ALOGE("%s: v4l2_s_crop() rc=%d", __FUNCTION__, ret);
        return ret;
    }

    ret = v4l2_reqbufs_out(ctx, 1);
    if (ret < 0) {
        ALOGE("%s: v4l2_reqbufs_out() rc=%d", __FUNCTION__, ret);
        return ret;
    }

    //dst
    int rotate = rotateValueHAL2PP(layer.transform);
    ret = v4l2_s_ctrl(ctx, V4L2_CID_ROTATION, rotate);
    if (ret < 0) {
        ALOGE("%s: s_ctrl V4L2_CID_ROTATION rc=%d", __FUNCTION__, ret);
        return ret;
    }

    ret = v4l2_s_ctrl(ctx, V4L2_CID_HFLIP, layer.transform == HWC_TRANSFORM_FLIP_H?1:0);
    if (ret < 0) {
        ALOGE("%s: s_ctrl V4L2_CID_HFLIP rc=%d", __FUNCTION__, ret);
        return ret;
    }

    ret = v4l2_s_ctrl(ctx, V4L2_CID_VFLIP, layer.transform == HWC_TRANSFORM_FLIP_V?1:0);
    if (ret < 0) {
        ALOGE("%s: s_ctrl V4L2_CID_VFLIP rc=%d", __FUNCTION__, ret);
        return ret;
    }

    ret = v4l2_g_fbuf(ctx, NULL, NULL, NULL, NULL); //TODO, check whether so it is ok
    if (ret < 0) {
        ALOGE("%s: v4l2_g_fbuf() rc=%d", __FUNCTION__, ret);
        return ret;
    }

    struct private_handle_t *dst_handle = private_handle_t::dynamicCast(win.dst_buf[win.current_buf]);
    uint32_t dst_addr = (uint32_t) dst_handle->paddr;
    int w, h;

    if (rotate == 90 || rotate == 270) {
        w = HEIGHT(layer.displayFrame);
        h = EXYNOS4_ALIGN(WIDTH(layer.displayFrame), 16);
    } else {
        w = WIDTH(layer.displayFrame);
        h = EXYNOS4_ALIGN(HEIGHT(layer.displayFrame), 16);
    }

    ret = v4l2_s_fbuf(ctx, (void *)dst_addr, w, h, V4L2_PIX_FMT_RGB32);
    if (ret < 0) {
        ALOGE("%s: v4l2_s_fbuf(dst_addr(0x%x)) rc=%d", __FUNCTION__, dst_addr, ret);
        return ret;
    }

    struct fimc_buf dst_buf;
    memset(&dst_buf, 0, sizeof(struct fimc_buf));
    dst_buf.base[FIMC_ADDR_Y] = dst_addr;

    ret = v4l2_s_ctrl(ctx, V4L2_CID_DST_INFO, (int) &dst_buf);
    if (ret < 0) {
        ALOGE("%s: s_ctrl V4L2_CID_DST_INFO rc=%d", __FUNCTION__, ret);
        return ret;
    }

    ret = v4l2_s_fmt_win(ctx, 0, 0, w, h);
    if (ret < 0) {
        ALOGE("%s: v4l2_s_fmt_win() rc=%d", __FUNCTION__, ret);
        return ret;
    }

    //oneshot
    ret = v4l2_streamon_out(ctx);
    if (ret < 0) {
        ALOGE("%s: v4l2_streamon_out() rc=%d", __FUNCTION__, ret);
        return ret;
    }

    ALOGV("%s: src_handle usage(0x%x) base(0x%x) paddr(0x%x)", __FUNCTION__, src_handle->usage, src_handle->base, src_handle->paddr);

    struct fimc_buf buf;
    buf.base[FIMC_ADDR_Y] = src_handle->paddr + src_handle->offset;
    buf.base[FIMC_ADDR_CB] = src_handle->paddr + src_handle->offset + src_handle->uoffset;
    buf.base[FIMC_ADDR_CR] = src_handle->paddr + src_handle->offset + src_handle->uoffset + src_handle->voffset;

    ret = v4l2_qbuf(ctx, 0, (unsigned long) &buf);
    if (ret < 0) {
        ALOGE("%s: v4l2_qbuf(0x%x) rc=%d", __FUNCTION__, src_handle->base, ret);
        return ret;
    }

    //wait??
    ret = v4l2_dqbuf(ctx);
    if (ret < 0) {
        ALOGE("%s: v4l2_dqbuf() rc=%d", __FUNCTION__, ret);
        return ret;
    }

    ret = v4l2_streamoff(ctx);
    if (ret < 0) {
        ALOGE("%s: v4l2_streamoff() rc=%d", __FUNCTION__, ret);
        return ret;
    }

    if (layer.acquireFenceFd >= 0) {
        close(layer.acquireFenceFd);
    }

    return w;
}

static void config_overlay(hwc_context_t *ctx, hwc_layer_1_t &layer, s3c_fb_win_config &cfg)
{
    private_handle_t* hnd = (private_handle_t*) layer.handle;

    if (layer.compositionType == HWC_BACKGROUND) {
        hwc_color_t color = layer.backgroundColor;
        cfg.state = cfg.S3C_FB_WIN_STATE_COLOR;
        cfg.color = (color.r << 16) | (color.g << 8) | color.b;
        cfg.x = 0;
        cfg.y = 0;
        cfg.w = ctx->xres;
        cfg.h = ctx->yres;
    } else if (layer.compositionType == HWC_OVERLAY) {
        ALOGV("%s RGB overlay", __FUNCTION__);
        config_handle(ctx, layer, cfg);
        cfg.format = S3C_FB_PIXEL_FORMAT_RGBA_8888;

        cfg.phys_addr = hnd->paddr;

        if (layer.acquireFenceFd >= 0) {
            if (sync_wait(layer.acquireFenceFd, 1000) < 0)
                ALOGW("sync_wait error");

            close(layer.acquireFenceFd);
            layer.acquireFenceFd = -1;

            cfg.fence_fd = -1;
        }
    } else if (layer.compositionType == HWC_FRAMEBUFFER_TARGET) {
        ALOGV("%s HWC_FRAMEBUFFER_TARGET", __FUNCTION__);
        config_handle(ctx, layer, cfg);
        // dumpsys SurfaceFlinger says that HWC_FRAMEBUFFER_TARGET is
        // in format RGBA_8888, which in Android 7 is internally
        // already in BGRA order, so there's no need to overwrite the
        // format
        //cfg.format = S3C_FB_PIXEL_FORMAT_BGRA_8888;
        cfg.phys_addr = hnd->paddr;

        if (layer.acquireFenceFd >= 0) {
            if (sync_wait(layer.acquireFenceFd, 1000) < 0)
                ALOGW("sync_wait error");

            close(layer.acquireFenceFd);
            layer.acquireFenceFd = -1;

            cfg.fence_fd = -1;
        }
    }
}

static int post_fimd(hwc_context_t *ctx, hwc_display_contents_1_t* contents)
{
    private_handle_t* hnd = NULL;
    size_t i;
    unsigned int window;
    struct s3c_fb_win_config_data win_data;
    struct s3c_fb_win_config *config = win_data.config;
    int err;

    memset(config, 0, sizeof(win_data.config));
    for (size_t i = 0; i < NUM_HW_WINDOWS; i++)
        config[i].fence_fd = -1;

#if DEBUG_SPAMMY
    for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
        struct hwc_win_info_t &win = ctx->win[i];
        if (win.layer_index != -1)
            ALOGV("%s: win[%d] layer_idx(%d) gsc.mode(%d)", __FUNCTION__, i, win.layer_index, win.gsc.mode);
    }
#endif

    for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
        window = i;
        struct hwc_win_info_t &win = ctx->win[window];
        int layer_idx = win.layer_index;

        if (layer_idx != -1) {
            hwc_layer_1_t &layer = contents->hwLayers[layer_idx];
            private_handle_t* hnd = private_handle_t::dynamicCast(layer.handle);

            switch(win.gsc.mode) {
            case gsc_map_t::FIMG:
                ALOGV("%s: layer %d perform FIMG operation", __FUNCTION__, i);
                err = perform_fimg(ctx, layer, win, HAL_PIXEL_FORMAT_BGRA_8888);
                if (err < 0) {
                    ALOGE("failed to perform FIMG for layer %u", i);
                    win.gsc.mode = gsc_map_t::NONE;
                    continue;
                }

                layer.acquireFenceFd = -1;

                if (win.dst_buf[win.current_buf]) {
                    //use geometry of layer.handle and overwrite the rest
                    config_handle(ctx, layer, config[window]);
                    config[window].format = S3C_FB_PIXEL_FORMAT_BGRA_8888;
                    private_handle_t* dst_hnd = private_handle_t::dynamicCast(win.dst_buf[win.current_buf]);
                    config[window].phys_addr = dst_hnd->paddr;
                    config[window].offset = 0;
                    config[window].stride = win.g2d_dst_img.stride;
                }
                break;

            case gsc_map_t::FIMC:
                ALOGV("%s: layer %d perform FIMC operation", __FUNCTION__, i);
                err = perform_fimc(ctx, layer, win);
                if (err < 0) {
                    ALOGE("failed to perform FIMC for layer %u", i);
                    win.gsc.mode = gsc_map_t::NONE;
                    continue;
                }

                layer.acquireFenceFd = -1;

                if (win.dst_buf[win.current_buf]) {
                    //use geometry of layer.handle and overwrite the rest
                    config_handle(ctx, layer, config[window]);
                    if (format_is_supported(hnd->format)) {
                        // FIMC isn't doing colour conversion
                        // we will get RGBA output
                        config[window].format = S3C_FB_PIXEL_FORMAT_RGBA_8888;
                    } else {
                        config[window].format = S3C_FB_PIXEL_FORMAT_BGRA_8888;
                    }
                    private_handle_t* dst_hnd = private_handle_t::dynamicCast(win.dst_buf[win.current_buf]);
                    config[window].phys_addr = dst_hnd->paddr;
                    config[window].offset = 0;
                    config[window].stride = EXYNOS4_ALIGN(config[window].w,16) * 4;
                }
                break;
            case gsc_map_t::NONE:
                config_overlay(ctx, layer, config[window]);
                config[window].stride = EXYNOS4_ALIGN(config[window].w, 16) * 4;
            default:
                config_overlay(ctx, layer, config[window]);
            }

            if (window == 0 && config[window].blending != S3C_FB_BLENDING_NONE) {
                ALOGV("blending not supported on window 0; forcing BLENDING_NONE");
                config[window].blending = S3C_FB_BLENDING_NONE;
            }

            if (window == 0 && config[window].plane_alpha != 255) {
                ALOGV("alpha not supported on window 0; forcing 255");
                config[window].plane_alpha = 255;
            }
        }
        memcpy(&win.win_cfg, &config[window], sizeof(struct s3c_fb_win_config));
    }

    dump_fb_win_cfg(win_data);

    int wincfg_err = ioctl(ctx->fb0_fd, S3CFB_WIN_CONFIG, &win_data);

    for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
        if (config[i].fence_fd != -1)
            close(config[i].fence_fd);
    }

    if (wincfg_err < 0) {
        ALOGE("%s S3CFB_WIN_CONFIG failed: %s", __FUNCTION__, strerror(errno));
        return wincfg_err;
    }

    return win_data.fence;
}

static int set_fimd(hwc_context_t *ctx, hwc_display_contents_1_t *contents)
{
    hwc_layer_1_t *fb_layer = NULL;
    int err = 0;

    if (ctx->fb_window != NO_FB_NEEDED) {
        for (size_t i = 0; i < contents->numHwLayers; i++) {
            hwc_layer_1_t &layer = contents->hwLayers[i];

            if (layer.compositionType == HWC_FRAMEBUFFER_TARGET) {
                ALOGV("HWC_FRAMEBUFFER_TARGET found");
                ctx->win[ctx->fb_window].layer_index = i;
                fb_layer = &contents->hwLayers[i];
                break;
            }
        }

        if (UNLIKELY(!fb_layer)) {
            ALOGE("framebuffer target expected, but not provided");
            err = -EINVAL;
        }
    }

    int fence;
    if (!err) {
        fence = post_fimd(ctx, contents);
        if (fence < 0)
            err = fence;
    }

    if (err) {
        ALOGV("%s about to clear window", __FUNCTION__);
        fence = window_clear(ctx);
    }

    ctx->bypass_count = max(ctx->bypass_count-1, 0);
    if (ctx->bypass_count == 0)
        ctx->force_fb = ctx->force_gpu;

    for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
        int layer_idx = ctx->win[i].layer_index;

        if (layer_idx != -1) {
            hwc_layer_1_t &layer = contents->hwLayers[layer_idx];
            int dup_fd = dup_or_warn(fence);

            if (ctx->win[i].gsc.mode == gsc_map_t::FIMG) {
                if (ctx->win[i].dst_buf_fence[ctx->win[i].current_buf] >= 0)
                    close(ctx->win[i].dst_buf_fence[ctx->win[i].current_buf]);

                ctx->win[i].dst_buf_fence[ctx->win[i].current_buf] = dup_fd;
                ctx->win[i].current_buf = (ctx->win[i].current_buf + 1) % NUM_OF_WIN_BUF;

                layer.releaseFenceFd = -1;
            } else {
                layer.releaseFenceFd = dup_fd;
            }
        }
    }

    contents->retireFenceFd = fence;

    return err;
}

static int hwc_set(hwc_composer_device_1_t *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    if (!numDisplays || !displays)
        return 0;

    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;
    hwc_display_contents_1_t *fimd_contents = displays[HWC_DISPLAY_PRIMARY];
    int fimd_err = 0;

    if (fimd_contents)
        fimd_err = set_fimd(ctx, fimd_contents);

    if (fimd_err)
        return fimd_err;

    return 0;
}

static int hwc_device_close(struct hw_device_t *dev)
{
    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;

    ALOGV("%s", __FUNCTION__);

    // Close V4L2 for FIMC
    v4l2_close(ctx);

    // Close FB0
    close(ctx->fb0_fd);

    // Close vsync
    close_vsync_thread(ctx);

    // Close gralloc
    gralloc_close(ctx->alloc_device);

    if (ctx) {
        free(ctx);
    }
    return 0;
}

static int hwc_eventControl(struct hwc_composer_device_1* dev, __unused int dpy,
        int event, int enabled)
{
    int val = 0, rc = 0;
    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;

    switch (event) {
    case HWC_EVENT_VSYNC:
        val = enabled;
        ALOGV("%s: HWC_EVENT_VSYNC, enabled=%d", __FUNCTION__, val);

        rc = ioctl(ctx->fb0_fd, S3CFB_SET_VSYNC_INT, &val);
        if (rc < 0) {
            ALOGE("%s: could not set vsync using ioctl: %s", __FUNCTION__,
                strerror(errno));
            return -errno;
        }
        return rc;
    }
    return -EINVAL;
}

static int hwc_setPowerMode(struct hwc_composer_device_1 *dev, int dpy, int mode)
{
    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;
    int fence = 0;
    int blank;

    ALOGV("%s mode=%d", __FUNCTION__, mode);

    fence = window_clear(ctx);
    if (fence != -1)
        close(fence);


    switch (mode) {
        case HWC_POWER_MODE_OFF:
            blank = FB_BLANK_POWERDOWN;
            break;
        case HWC_POWER_MODE_NORMAL:
            blank = FB_BLANK_UNBLANK;
            break;
        default:
            // FIXME DOZE and DOZE_SUSPEND are unsupported by the fb driver
            return -EINVAL;
    }

    if (ioctl(ctx->fb0_fd, FBIOBLANK, blank) < 0) {
        ALOGE("%s Error %s in FBIOBLANK blank=%d", __FUNCTION__, strerror(errno), blank);
    }

    return 0;
}

static int hwc_getActiveConfig(struct hwc_composer_device_1 *dev, int disp)
{
    // we only support the primary display
    return 0;
}

static int hwc_setActiveConfig(struct hwc_composer_device_1 *dev, int dpy, int idx)
{
    // Only 1 config supported for the primary display
    return (idx == 0) ? idx : -EINVAL;
}

static int hwc_query(struct hwc_composer_device_1* dev,
        int what, int* value)
{
    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;

    ALOGV("%s", __FUNCTION__);

    switch (what) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        // stock blob do support background layer
        value[0] = 1;
        break;
    case HWC_VSYNC_PERIOD:
        // vsync period in nanosecond
        value[0] = _VSYNC_PERIOD / ctx->gralloc_module->fps;
        break;
    default:
        // unsupported query
        return -EINVAL;
    }
    return 0;
}

static void hwc_registerProcs(struct hwc_composer_device_1* dev,
        hwc_procs_t const* procs)
{
    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;
    ctx->procs = const_cast<hwc_procs_t *>(procs);
}

static int hwc_getDisplayConfigs(struct hwc_composer_device_1* dev, int disp,
    uint32_t* configs, size_t* numConfigs)
{
    ALOGV("%s", __FUNCTION__);

    if (*numConfigs == 0)
        return 0;

    if (disp == HWC_DISPLAY_PRIMARY) {
        configs[0] = 0;
        *numConfigs = 1;
        return 0;
    }

    return -EINVAL;
}

static int hwc_getDisplayAttributes(struct hwc_composer_device_1* dev, int disp,
    __unused uint32_t config, const uint32_t* attributes, int32_t* values)
{
    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;
    int i = 0;

    ALOGV("%s", __FUNCTION__);

    while(attributes[i] != HWC_DISPLAY_NO_ATTRIBUTE) {
        switch(disp) {
        case 0:

            switch(attributes[i]) {
            case HWC_DISPLAY_VSYNC_PERIOD: /* The vsync period in nanoseconds */
                values[i] = ctx->vsync_period;
                break;

            case HWC_DISPLAY_WIDTH: /* The number of pixels in the horizontal and vertical directions. */
                values[i] = ctx->xres;
                break;

            case HWC_DISPLAY_HEIGHT:
                values[i] = ctx->yres;
                break;

            case HWC_DISPLAY_DPI_X:
                values[i] = ctx->xdpi;
                break;

            case HWC_DISPLAY_DPI_Y:
                values[i] = ctx->ydpi;
                break;

            default:
                ALOGE("%s:unknown display attribute %d", __FUNCTION__, attributes[i]);
                return -EINVAL;
            }
            break;

        case 1:
            // TODO: no hdmi at the moment
            break;

        default:
            ALOGE("%s:unknown display %d", __FUNCTION__, disp);
            return -EINVAL;
        }

        i++;
    }
    return 0;
}

static void hwc_dump(struct hwc_composer_device_1* dev, char *buff, int buff_len)
{
    const char *modes[] = {
        "NONE",
        "FIMG",
        "FIMC",
        "MAX",
    };

    struct hwc_context_t *ctx = (hwc_context_t *)dev;
    android::String8 tmp("");
    tmp.appendFormat("Exynos HWC: force_fb=%d force_gpu=%d bypass_count=%d multi_fimg=%d\n", ctx->force_fb, ctx->force_gpu,
            ctx->bypass_count, ctx->multi_fimg);
    tmp.appendFormat("win | mode | layer_index |    paddr    |     hnd     | alpha |\n");
    //                3-- | 4--- | 11--------- | 0x100000000 | 0x100000000 |  255  |
    int fimc_win = -1;
    int fimg_win = -1;
    for (int i = 0; i < NUM_HW_WINDOWS; i++) {
        struct hwc_win_info_t *win = &ctx->win[i];
        if (win->gsc.mode == gsc_map_t::FIMC) fimc_win = i;
        else if (win->gsc.mode == gsc_map_t::FIMG) fimg_win = i;
        if (win->src_buf)
            tmp.appendFormat("  %d | %4s |          % 2d | 0x%09x | 0x%09x |  % 3d  |\n",
                    i, win->layer_index < 0 ? "OFF" : modes[win->gsc.mode],
                    win->layer_index, win->src_buf->paddr, win->src_buf, win->win_cfg.plane_alpha);
        else
            tmp.appendFormat("  %d | %4s |          % 2d | 0x%09x | 0x%09x |  % 3d  |\n",
                    i, win->layer_index < 0 ? "OFF" : modes[win->gsc.mode],
                    win->layer_index, 0xdead, 0x0, win->win_cfg.plane_alpha);

    }

    if (fimc_win != -1) {
        tmp.appendFormat("\n\n--FIMC info (%d)--\n", fimc_win);
        struct s3c_fb_win_config *cfg = &ctx->win[fimc_win].win_cfg;
        tmp.appendFormat("paddr=0x%09x, offset=%d, stride=%d, fmt=%d, blending=%d, alpha=%d\n",
                cfg->phys_addr, cfg->offset, cfg->stride, cfg->format, cfg->blending, cfg->plane_alpha);
    }

    if (fimg_win != -1) {
        tmp.appendFormat("\n\n--FIMG info (%d)--\n", fimg_win);
        struct s3c_fb_win_config *cfg = &ctx->win[fimc_win].win_cfg;
        tmp.appendFormat("paddr=0x%09x, offset=%d, stride=%d, fmt=%d, blending=%d, alpha=%d\n",
                cfg->phys_addr, cfg->offset, cfg->stride, cfg->format, cfg->blending, cfg->plane_alpha);
    }

    ctx->multi_fimg = property_get_int32("persist.sys.hwc.multi_fimg", 0);
    strlcpy(buff, tmp.string(), buff_len);
}

/*****************************************************************************/

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device)
{
    int status = -EINVAL;
    struct hwc_context_t *dev = NULL;
    struct hwc_win_info_t *win = NULL;
    int refreshRate = 0;
    unsigned int i = 0, j = 0;

    ALOGV("%s", __FUNCTION__);

    if (!strcmp(name, HWC_HARDWARE_COMPOSER)) {
        dev = (hwc_context_t *)malloc(sizeof(*dev));

        // initialize our state here
        memset(dev, 0, sizeof(*dev));

        // get gralloc module
        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (const hw_module_t **) &dev->gralloc_module) != 0) {
            ALOGE("failed to get gralloc hw module");
            status = -EINVAL;
            goto err_get_module;
        }

        if (gralloc_open((const hw_module_t *)dev->gralloc_module, &dev->alloc_device)) {
            ALOGE("failed to open gralloc");
            status = -EINVAL;
            goto err_get_module;
        }

        // initialize the procs
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = HWC_DEVICE_API_VERSION_1_4;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = hwc_device_close;

        dev->device.prepare = hwc_prepare;
        dev->device.set = hwc_set;
        dev->device.eventControl = hwc_eventControl;
        dev->device.setPowerMode = hwc_setPowerMode;
        dev->device.query = hwc_query;
        dev->device.registerProcs = hwc_registerProcs;
        dev->device.dump = hwc_dump;
        dev->device.getDisplayConfigs = hwc_getDisplayConfigs;
        dev->device.getDisplayAttributes = hwc_getDisplayAttributes;
        dev->device.getActiveConfig = hwc_getActiveConfig;
        dev->device.setActiveConfig = hwc_setActiveConfig;

        *device = &dev->device.common;


        // property to disable HWC taken from exynos5
        char value[PROPERTY_VALUE_MAX];
        property_get("debug.hwc.force_gpu", value, "0");
        dev->force_gpu = atoi(value);

        property_get("persist.sys.hwc.multi_fimg", value, "0");
        dev->multi_fimg = atoi(value);

        // Init Vsync
        init_vsync_thread(dev);

        // query LCD info
        dev->fb0_fd = open("/dev/graphics/fb0", O_RDWR);
        if (dev->fb0_fd < 0) {
            ALOGE("%s: Failed to open framebuffer", __FUNCTION__);
            status = -EINVAL;
            goto err_open_fb;
        }

        struct fb_var_screeninfo lcd_info;
        if (ioctl(dev->fb0_fd, FBIOGET_VSCREENINFO, &lcd_info) < 0) {
            ALOGE("%s: window_get_var_info is failed : %s", __FUNCTION__, strerror(errno));
            status = -EINVAL;
            goto err_ioctl;
        }

        refreshRate = 1000000000000LLU /
            (
                uint64_t( lcd_info.upper_margin + lcd_info.lower_margin + lcd_info.yres)
                * ( lcd_info.left_margin  + lcd_info.right_margin + lcd_info.xres)
                * lcd_info.pixclock
            );

        if (refreshRate == 0) {
            ALOGW("%s: Invalid refresh rate, using 60 Hz", __FUNCTION__);
            refreshRate = 60;  /* 60 Hz */
        }

        dev->vsync_period = _VSYNC_PERIOD / refreshRate;

        dev->xres = lcd_info.xres;
        dev->yres = lcd_info.yres;
        dev->xdpi = (lcd_info.xres * 25.4f * 1000.0f) / lcd_info.width;
        dev->ydpi = (lcd_info.yres * 25.4f * 1000.0f) / lcd_info.height;

        ALOGI("using\nxres         = %d px\nyres         = %d px\nwidth        = %d mm (%f dpi)\nheight       = %d mm (%f dpi)\nrefresh rate = %d Hz",
                dev->xres, dev->yres, lcd_info.width, dev->xdpi / 1000.0f, lcd_info.height, dev->ydpi / 1000.0f, refreshRate);

        dev->win[WIN_FB0].rect_info.x = 0;
        dev->win[WIN_FB0].rect_info.y = 0;
        dev->win[WIN_FB0].rect_info.w = lcd_info.xres;
        dev->win[WIN_FB0].rect_info.h = lcd_info.yres;

        struct fb_fix_screeninfo fix_info;
        if (ioctl(dev->fb0_fd, FBIOGET_FSCREENINFO, &fix_info) < 0) {
            ALOGE("%s: window_get_info is failed : %s", __FUNCTION__, strerror(errno));
            fix_info.smem_start = 0;
            status = -EINVAL;
            goto err_ioctl;
        }

        dev->fb0_size = lcd_info.yres * fix_info.line_length;

        dev->win[WIN_FB0].current_buf = 0;

        // initialize the window context */
        for (i = 0; i < NUM_HW_WINDOWS; i++) {
            win = &dev->win[i];

            win->rect_info.x = 0;
            win->rect_info.y = 0;
            win->rect_info.w = lcd_info.xres;
            win->rect_info.h = lcd_info.yres;

            for (j = 0; j < NUM_OF_WIN_BUF; j++) {
                win->dst_buf_fence[j] = -1;
            }
        }

        // Initialize V4L2 for FIMC
        if (v4l2_open(dev) < 0)
            ALOGE("%s Failed to initialize FIMC", __FUNCTION__);

        //force re-fill of fimg_cmd
        for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
            dev->win[i].fimg_cmd.op = (enum blit_op) -1;
        }

        status = 0;
    }
    return status;

err_ioctl:
    close(dev->fb0_fd);

err_open_fb:
    gralloc_close(dev->alloc_device);

err_get_module:
    if (dev) {
        free(dev);
    }

    return status;
}
