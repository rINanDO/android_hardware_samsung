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

#ifndef SEC_OMX_VENC_FIMC_H
#define SEC_OMX_VENC_FIMC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 on success; on failure *pFd is closed and set to -1 */
int SEC_OMX_Venc_FimcBGRAToNV12(int *pFd,
                                unsigned int srcPhys, unsigned int srcStride,
                                unsigned int width, unsigned int height,
                                unsigned int dstYPhys, unsigned int dstCPhys);

void SEC_OMX_Venc_FimcClose(int *pFd);

#ifdef __cplusplus
}
#endif

#endif
