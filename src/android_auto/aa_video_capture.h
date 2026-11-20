#ifndef NBT_AA_VIDEO_CAPTURE_H
#define NBT_AA_VIDEO_CAPTURE_H
#ifdef __cplusplus
extern "C" {
#endif
#define NBT_CAPTURE_MAX_TRANSFER 65536U
/* One reader and one writer run concurrently. Transfer must bound its waits
 * and use separate buffers/URBs for each direction. Returns 1 on success
 * and updates the size; a successful IN may return zero bytes. */
typedef int (*nbt_capture_transfer)(void*, int input, unsigned char*, unsigned*);
int nbt_video_capture_run(nbt_capture_transfer transfer, void* context, const char* prefix, unsigned read_size);
#ifdef __cplusplus
}
#endif
#endif
