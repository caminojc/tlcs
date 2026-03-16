#ifndef SMPL_DETECT_ARCH_H
#define SMPL_DETECT_ARCH_H

typedef enum SmplArchType_ {
    SMPL_ARCH_NONE = 0,
    SMPL_ARCH_NEON = 1,
} SmplArchType;

// #define SMPL_DISABLE_SIMD   // uncomment to turn off all SIMD intrinsics code
#if !defined(SMPL_DISABLE_SIMD) && (defined(__arm__) || defined(__aarch64__) || defined(__arm64__))
#define SMPL_USE_NEON  1
#else
#define SMPL_USE_NEON  0
#endif

#if SMPL_USE_NEON
#define arch_string "NEON"
#else
#define arch_string "C"
#endif

#define SMPL_BYTE_ALIGNMENT 32
#define IS_32BYTE_ALIGNED(ptr) (!(((int64_t)(ptr)) & 0x1F)) // Check for 32-byte alignment
#ifdef _MSC_VER /* visual c++ */
#if 0
#define ALIGN_PRE __declspec(align(SMPL_BYTE_ALIGNMENT))
#else // C++11
#define ALIGN_PRE alignas(SMPL_BYTE_ALIGNMENT)
#define ALIGN_POST
#endif
#else /* gcc or icc */
#define ALIGN_PRE
#define ALIGN_POST __attribute__((aligned(SMPL_BYTE_ALIGNMENT)))
#endif

#endif // SMPL_DETECT_ARCH_H
