/* SPDX-License-Identifier: MIT */
#include <framelab/framelab.h>

#include <stddef.h>
#include <stdio.h>

#define SIZE_ALIGN(type) sizeof(type), _Alignof(type)

int main(void)
{
    printf("%zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu\n",
        SIZE_ALIGN(FrameLabExtent),
        SIZE_ALIGN(FrameLabImage), offsetof(FrameLabImage, lease),
        SIZE_ALIGN(FrameLabCamera), offsetof(FrameLabCamera, flags),
        SIZE_ALIGN(FrameLabProducerSync), offsetof(FrameLabProducerSync, signal_lease),
        SIZE_ALIGN(FrameLabFgFrame), offsetof(FrameLabFgFrame, color), offsetof(FrameLabFgFrame, camera),
        offsetof(FrameLabFgFrame, producer), offsetof(FrameLabFgFrame, flags));
    return 0;
}
