#ifndef PLATFORM_DATA_X86_APPLE_H
#define PLATFORM_DATA_X86_APPLE_H

#ifdef CONFIG_X86
/**
 * x86_apple_machine - whether the machine is an x86 Apple Macintosh
 */
extern bool x86_apple_machine;
/**
 * has_apple_t2_chip - whether the machine has the Apple T2 chip
 */
extern bool has_apple_t2_chip;
#else
#define x86_apple_machine false
#define has_apple_t2_chip false
#endif

#endif
