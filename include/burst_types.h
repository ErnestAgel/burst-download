#ifndef BURST_TYPES_H
#define BURST_TYPES_H

/**
 * @file burst_types.h
 * @brief Portable integer types and boolean macros for Burst Download.
 *
 * The project coding standard forbids machine-dependent integer types for
 * logic and sizes.  This header provides the portable typedefs used by
 * new modules.
 */

#include <cstdint>

#ifndef u8
typedef std::uint8_t  u8;
#endif

#ifndef u16
typedef std::uint16_t u16;
#endif

#ifndef u32
typedef std::uint32_t u32;
#endif

#ifndef u64
typedef std::uint64_t u64;
#endif

#ifndef s8
typedef std::int8_t  s8;
#endif

#ifndef s16
typedef std::int16_t s16;
#endif

#ifndef s32
typedef std::int32_t s32;
#endif

#ifndef s64
typedef std::int64_t s64;
#endif

#ifndef BOOL32
typedef s32 BOOL32;
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#endif  // BURST_TYPES_H
