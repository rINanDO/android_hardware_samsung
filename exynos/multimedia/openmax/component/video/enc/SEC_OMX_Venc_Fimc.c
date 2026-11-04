/*
 * Copyright (C) 2026 The LineageOS Project
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

/*
 * Colour conversion of surface-input frames on FIMC instead of NEON.
 *
 * FIMC reads 32-bit RGB as B,G,R,X bytes and has no way to swap red and
 * blue, so this is only used for HAL_PIXEL_FORMAT_BGRA_8888 buffers. The
 * sequence follows libhwc's one-shot blit (v4l2.cpp / perform_fimc).
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include "SEC_OMX_Venc_Fimc.h"

#undef  SEC_LOG_TAG
#define SEC_LOG_TAG    "SEC_VIDEO_ENC_FIMC"
#define SEC_LOG_OFF
#include "SEC_OSAL_Log.h"

/* FIMC1 is free on galaxys2: the camera uses FIMC0/2, HWC FIMC2, HDMI FIMC3 */
#define FIMC_ENC_NODE               "/dev/video1"

#define V4L2_CID_OVLY_MODE          (V4L2_CID_PRIVATE_BASE + 9)
#define V4L2_CID_DST_INFO           (V4L2_CID_PRIVATE_BASE + 10)
#define FIMC_OVLY_NONE_MULTI_BUF    0x5

#define FIMC_ADDR_Y                 0
#define FIMC_ADDR_CB                1
#define FIMC_ADDR_CR                2

#define ALIGN16(x)                  (((x) + 15) & ~15)

struct fimc_buf {
    unsigned int base[3];
    unsigned int length[3];
};

static int fimc_s_ctrl(int fd, unsigned int id, int value)
{
    struct v4l2_control control;

    control.id = id;
    control.value = value;
    return ioctl(fd, VIDIOC_S_CTRL, &control);
}

static int fimc_reqbufs(int fd, int count)
{
    struct v4l2_requestbuffers req;

    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.count = count;
    req.memory = V4L2_MEMORY_USERPTR;
    return ioctl(fd, VIDIOC_REQBUFS, &req);
}

static int fimc_open(void)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    int fd;

    fd = open(FIMC_ENC_NODE, O_RDWR);
    if (fd < 0) {
        SEC_OSAL_Log(SEC_LOG_ERROR, "open %s failed: %d", FIMC_ENC_NODE, errno);
        return -1;
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
        !(cap.capabilities & V4L2_CAP_STREAMING) ||
        !(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
        SEC_OSAL_Log(SEC_LOG_ERROR, "%s has no streaming output", FIMC_ENC_NODE);
        goto err;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0)
        goto err;

    if (fimc_s_ctrl(fd, V4L2_CID_OVLY_MODE, FIMC_OVLY_NONE_MULTI_BUF) < 0) {
        SEC_OSAL_Log(SEC_LOG_ERROR, "setting overlay mode failed: %d", errno);
        goto err;
    }

    return fd;

err:
    close(fd);
    return -1;
}

int SEC_OMX_Venc_FimcBGRAToNV12(int *pFd,
                                unsigned int srcPhys, unsigned int srcStride,
                                unsigned int width, unsigned int height,
                                unsigned int dstYPhys, unsigned int dstCPhys)
{
    struct v4l2_format format;
    struct v4l2_crop crop;
    struct v4l2_framebuffer fbuf;
    struct v4l2_buffer buffer;
    struct fimc_buf src_buf, dst_buf;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    int fd;

    if (srcPhys == 0 || dstYPhys == 0 || dstCPhys == 0 || (width & 1) || (height & 1))
        return -1;

    if (srcStride < width)
        srcStride = width;

    if (*pFd < 0) {
        *pFd = fimc_open();
        if (*pFd < 0)
            return -1;
    }
    fd = *pFd;

    if (fimc_reqbufs(fd, 0) < 0)
        goto err;

    /* source: the BGRA gralloc buffer */
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = ALIGN16(srcStride);
    format.fmt.pix.height = ALIGN16(height);
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB32;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &format) < 0)
        goto err;

    memset(&crop, 0, sizeof(crop));
    crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    crop.c.width = width;
    crop.c.height = height;
    if (ioctl(fd, VIDIOC_S_CROP, &crop) < 0)
        goto err;

    if (fimc_reqbufs(fd, 1) < 0)
        goto err;

    /* destination: the MFC input buffer, linear NV12 */
    memset(&fbuf, 0, sizeof(fbuf));
    if (ioctl(fd, VIDIOC_G_FBUF, &fbuf) < 0)
        goto err;

    memset(&fbuf, 0, sizeof(fbuf));
    fbuf.base = (void *)dstYPhys;
    fbuf.fmt.width = width;
    fbuf.fmt.height = height;
    fbuf.fmt.pixelformat = V4L2_PIX_FMT_NV12;
    if (ioctl(fd, VIDIOC_S_FBUF, &fbuf) < 0)
        goto err;

    memset(&dst_buf, 0, sizeof(dst_buf));
    dst_buf.base[FIMC_ADDR_Y] = dstYPhys;
    dst_buf.base[FIMC_ADDR_CB] = dstCPhys;
    if (fimc_s_ctrl(fd, V4L2_CID_DST_INFO, (int)&dst_buf) < 0)
        goto err;

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
    format.fmt.win.w.width = width;
    format.fmt.win.w.height = height;
    if (ioctl(fd, VIDIOC_S_FMT, &format) < 0)
        goto err;

    /* one shot */
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
        goto err;

    memset(&src_buf, 0, sizeof(src_buf));
    src_buf.base[FIMC_ADDR_Y] = srcPhys;

    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buffer.memory = V4L2_MEMORY_USERPTR;
    buffer.index = 0;
    buffer.m.userptr = (unsigned long)&src_buf;
    if (ioctl(fd, VIDIOC_QBUF, &buffer) < 0)
        goto err_streamoff;

    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buffer.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0)
        goto err_streamoff;

    if (ioctl(fd, VIDIOC_STREAMOFF, &type) < 0)
        goto err;

    return 0;

err_streamoff:
    ioctl(fd, VIDIOC_STREAMOFF, &type);
err:
    SEC_OSAL_Log(SEC_LOG_ERROR, "FIMC conversion %ux%u failed: %d", width, height, errno);
    close(fd);
    *pFd = -1;
    return -1;
}

void SEC_OMX_Venc_FimcClose(int *pFd)
{
    if (*pFd >= 0) {
        close(*pFd);
        *pFd = -1;
    }
}
