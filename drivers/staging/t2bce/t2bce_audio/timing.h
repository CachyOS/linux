#ifndef T2AUDIO_TIMING_H
#define T2AUDIO_TIMING_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t u64;
#endif

/* Decode the nonnegative, integral IEEE-754 sampleTime without kernel FP. */
static inline bool t2audio_decode_sample_time(u64 bits, u64 *frames)
{
    unsigned int exponent = (bits >> 52) & 0x7ff;
    u64 fraction = bits & ((1ULL << 52) - 1);
    unsigned int shift;

    if (!bits) {
        *frames = 0;
        return true;
    }
    /* Up to 2^53 frames: doubles still represent every integer exactly. */
    if ((bits >> 63) || exponent < 1023 || exponent > 1076 ||
            (exponent == 1076 && fraction))
        return false;
    fraction |= 1ULL << 52;
    if (exponent == 1076) {
        *frames = fraction << 1;
        return true;
    }
    shift = 1075 - exponent;
    if (fraction & ((1ULL << shift) - 1))
        return false;
    *frames = fraction >> shift;
    return true;
}

/* Clamp before wrapping: a small correction must not look like a full ring. */
static inline u64 t2audio_position_frames(u64 sample, u64 elapsed_frames,
        u64 latency, u64 previous)
{
    u64 frames = sample + elapsed_frames;

    frames = frames > latency ? frames - latency : 0;
    return frames > previous ? frames : previous;
}

/* Preserve continuous DMA progress when callback-counted sampleTime lags. */
static inline u64 t2audio_reanchor_frames(u64 sample, u64 previous_anchor,
        u64 elapsed_frames)
{
    u64 projected = previous_anchor + elapsed_frames;

    return sample > projected ? sample : projected;
}

#endif
