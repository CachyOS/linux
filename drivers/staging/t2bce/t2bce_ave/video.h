#ifndef T2BCE_AVE_VIDEO_H
#define T2BCE_AVE_VIDEO_H

struct t2bce_core_client;
struct t2bce_ave_device;

int t2bce_ave_video_create(struct t2bce_core_client *client,
			   struct t2bce_ave_device **result);
void t2bce_ave_video_suspend(struct t2bce_ave_device *adev);
void t2bce_ave_video_destroy(struct t2bce_ave_device *adev);

#endif
