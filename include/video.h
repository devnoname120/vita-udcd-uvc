#ifndef VIDEO_H
#define VIDEO_H

#include <stddef.h>
#include <stdint.h>
#include <psp2kern/udcd.h>

#define MAX_UVC_VIDEO_FRAME_SIZE (1280u * 720u * 3u / 2u)
#define UVC_PAYLOAD_HEADER_SIZE 12
#define UVC_FRAME_PADDING_SIZE (16 - UVC_PAYLOAD_HEADER_SIZE)
#define UVC_PAYLOAD_SIZE(frame_size) (UVC_PAYLOAD_HEADER_SIZE + (frame_size))
#define MAX_UVC_PAYLOAD_TRANSFER_SIZE UVC_PAYLOAD_SIZE(MAX_UVC_VIDEO_FRAME_SIZE)

struct uvc_frame {
	unsigned char padding[UVC_FRAME_PADDING_SIZE];
	unsigned char header[UVC_PAYLOAD_HEADER_SIZE];
	unsigned char data[];
} __attribute__((packed));

_Static_assert(offsetof(struct uvc_frame, data) == 16,
	"IFTU destination must be 16-byte aligned");

struct uvc_video_buffer {
	struct uvc_frame *frame;
	uintptr_t data_paddr;
};

int uvc_video_init(SceUdcdEndpoint *endpoint);
int uvc_video_prepare(unsigned int frame_size, struct uvc_video_buffer *buffer);
int uvc_video_wait(void);
int uvc_video_submit(int fid);
void uvc_video_cancel(void);
int uvc_video_release(void);
int uvc_video_term(void);

#endif
