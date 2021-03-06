#include "shared-functions.h"

uint hashForLargeCompactTableCL(const uint32_t input) {
  return (input * 8389) & (CL_CT_LARGE_MASK);
}

ushort directFilterHashCL(const int32_t val) {
  return BINDEX((val * 8387) & CL_DF_MASK);
}

int my_strncmp(__global const unsigned char *a, __global const unsigned char *b,
               const int n) {
  for (int i = 0; i < n; ++i) {
    if (a[i] != b[i]) return -1;
  }
  return 0;
}

uchar tolower(const uchar c) {
  if (c >= 65 && c <= 90) {
    return c + 32;
  }
  return c;
}

int my_strncasecmp(__global const unsigned char *a,
                   __global const unsigned char *b, const int n) {
  for (int i = 0; i < n; ++i) {
    if (tolower(a[i]) != tolower(b[i])) return -1;
  }
  return 0;
}

bool doesPatternMatch(__global const uchar *start,
                      __global const uchar *pattern, const int length,
                      const bool isCaseInsensitive) {
  if (isCaseInsensitive) {
    return !my_strncasecmp(start, pattern, length);
  }
  return !my_strncmp(start, pattern, length);
}

void verifySmall(__global const CompactTableSmallEntry *ct,
                 __global const PID_TYPE *pids,
                 __global const DFC_FIXED_PATTERN *patterns,
                 __global uchar *input, const int currentPos,
                 const int inputLength, __global VerifyResult *result) {
  ct += input[0];  // input[0] is the "hash"

  for (ushort i = 0; i < ct->pidCount; ++i) {
    PID_TYPE pid = (pids + ct->offset)[i];

    if (inputLength - currentPos >= (patterns + pid)->pattern_length &&
        doesPatternMatch(input, (patterns + pid)->original_pattern,
                         (patterns + pid)->pattern_length,
                         (patterns + pid)->is_case_insensitive)) {
      if (result->matchCount < MAX_MATCHES_PER_THREAD) {
        result->matches[result->matchCount] = pid;
      }

      ++result->matchCount;
    }
  }
}

void verifyLarge(__global const CompactTableLargeBucket *buckets,
                 __global const CompactTableLargeEntry *entries,
                 __global const PID_TYPE *pids,
                 __global const DFC_FIXED_PATTERN *patterns,
                 const uint bytePattern, __global const uchar *input,
                 const int currentPos, const int inputLength,
                 __global VerifyResult *result) {
  buckets += hashForLargeCompactTable(bytePattern);
  ushort entryOffset = buckets->entryOffset;

  for (ushort i = 0; i < buckets->entryCount; ++i) {
    if ((entries + entryOffset + i)->pattern == bytePattern) {
      ushort pidOffset = (entries + entryOffset + i)->pidOffset;

      for (ushort j = 0; j < (entries + entryOffset + i)->pidCount; ++j) {
        PID_TYPE pid = pids[pidOffset + j];

        if (inputLength - currentPos >= (patterns + pid)->pattern_length) {
          if (doesPatternMatch(input, (patterns + pid)->original_pattern,
                               (patterns + pid)->pattern_length,
                               (patterns + pid)->is_case_insensitive)) {
            if (result->matchCount < MAX_MATCHES_PER_THREAD) {
              result->matches[result->matchCount] = pid;
            }

            ++result->matchCount;
          }
        }
      }

      break;
    }
  }
}

bool isInHashDf(__global const uchar *df, const uint data) {
  return df[directFilterHashCL(data)] & BMASK(data & CL_DF_MASK);
}

bool isInHashDfLocal(__local const uchar *df, const uint data) {
  return df[directFilterHashCL(data)] & BMASK(data & CL_DF_MASK);
}

__kernel void search(const int inputLength, __global const uchar *input,
                     __global const DFC_FIXED_PATTERN *patterns,
                     __global const uchar *const dfSmall,
                     __global const uchar *const dfLarge,
                     __global const uchar *const dfLargeHash,
                     __global const CompactTableSmallEntry *ctSmallEntries,
                     __global const PID_TYPE *ctSmallPids,
                     __global const CompactTableLargeBucket *ctLargeBuckets,
                     __global const CompactTableLargeEntry *ctLargeEntries,
                     __global const PID_TYPE *ctLargePids,
                     __global VerifyResult *result) {
  int i;
  {
    const uint threadId =
        (get_group_id(0) * get_local_size(0) + get_local_id(0));

    i = threadId * THREAD_GRANULARITY;

    if (i >= inputLength) {
      return;
    }

    input += i;
    result += threadId;
  }

  result->matchCount = 0;

  const int end = min(i + THREAD_GRANULARITY, inputLength);

  for (; i < end; ++i, ++input) {
    const short data = input[1] << 8 | input[0];
    const short byteIndex = BINDEX(data & CL_DF_MASK);
    const short bitMask = BMASK(data & CL_DF_MASK);

    if (dfSmall[byteIndex] & bitMask) {
      verifySmall(ctSmallEntries, ctSmallPids, patterns, input, i, inputLength,
                  result);
    }

    const uint dataLong =
        input[3] << 24 | input[2] << 16 | input[1] << 8 | input[0];
    if ((dfLarge[byteIndex] & bitMask) && isInHashDf(dfLargeHash, dataLong)) {
      verifyLarge(ctLargeBuckets, ctLargeEntries, ctLargePids, patterns,
                  dataLong, input, i, inputLength, result);
    }
  }
}
typedef union {
  uchar scalar[TEXTURE_CHANNEL_BYTE_SIZE];
  uint4 vector;
} img_read;

#define SHIFT_BY_CHANNEL_SIZE(x) (x >> 4)

__kernel void search_with_image(
    const int inputLength, __global const uchar *input,
    __global const DFC_FIXED_PATTERN *patterns,
    __read_only const image1d_t dfSmall, __read_only const image1d_t dfLarge,
    __global const uchar *dfLargeHash,
    __global const CompactTableSmallEntry *ctSmallEntries,
    __global const PID_TYPE *ctSmallPids,
    __global const CompactTableLargeBucket *ctLargeBuckets,
    __global const CompactTableLargeEntry *ctLargeEntries,
    __global const PID_TYPE *ctLargePids, __global VerifyResult *result) {
  int i;
  {
    const uint threadId =
        (get_group_id(0) * get_local_size(0) + get_local_id(0));

    i = threadId * THREAD_GRANULARITY;

    if (i >= inputLength) {
      return;
    }

    input += i;
    result += threadId;
  }

  result->matchCount = 0;

  const int end = min(i + THREAD_GRANULARITY, inputLength);

  for (; i < end; ++i, ++input) {
    const short data = input[1] << 8 | input[0];
    const short byteIndex = BINDEX(data & DF_MASK);
    const short bitMask = BMASK(data & DF_MASK);

    {
      // divide by 16 as we actually just want a single byte, but we're getting
      // 16
      const img_read df =
          (img_read)read_imageui(dfSmall, SHIFT_BY_CHANNEL_SIZE(byteIndex));
      if (df.scalar[byteIndex % TEXTURE_CHANNEL_BYTE_SIZE] & bitMask) {
        verifySmall(ctSmallEntries, ctSmallPids, patterns, input, i,
                    inputLength, result);
      }
    }

    {
      const uint dataLong =
          input[3] << 24 | input[2] << 16 | input[1] << 8 | input[0];
      const img_read df =
          (img_read)read_imageui(dfLarge, SHIFT_BY_CHANNEL_SIZE(byteIndex));
      if ((df.scalar[byteIndex % TEXTURE_CHANNEL_BYTE_SIZE] & bitMask) &&
          isInHashDf(dfLargeHash, dataLong)) {
        verifyLarge(ctLargeBuckets, ctLargeEntries, ctLargePids, patterns,
                    dataLong, input, i, inputLength, result);
      }
    }
  }
}

__kernel void search_with_local(
    const int inputLength, __global const uchar *input,
    __global const DFC_FIXED_PATTERN *patterns,
    __global const uchar *const dfSmall, __global const uchar *const dfLarge,
    __global const uchar *const dfLargeHash,
    __global const CompactTableSmallEntry *ctSmallEntries,
    __global const PID_TYPE *ctSmallPids,
    __global const CompactTableLargeBucket *ctLargeBuckets,
    __global const CompactTableLargeEntry *ctLargeEntries,
    __global const PID_TYPE *ctLargePids, __global VerifyResult *result) {
  __local uchar dfSmallLocal[DF_SIZE_REAL];
  __local uchar dfLargeLocal[DF_SIZE_REAL];
  __local uchar dfLargeHashLocal[DF_SIZE_REAL];

  for (int j = LOCAL_MEMORY_LOAD_PER_ITEM * get_local_id(0);
       j < (LOCAL_MEMORY_LOAD_PER_ITEM * (get_local_id(0) + 1)); ++j) {
    dfSmallLocal[j] = dfSmall[j];
    dfLargeLocal[j] = dfLarge[j];
    dfLargeHashLocal[j] = dfLargeHash[j];
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  int i;
  {
    const uint threadId =
        (get_group_id(0) * get_local_size(0) + get_local_id(0));

    i = threadId * THREAD_GRANULARITY;
    if (i >= inputLength) {
      return;
    }

    input += i;
    result += threadId;
  }

  result->matchCount = 0;

  const int end = min(i + THREAD_GRANULARITY, inputLength);

  for (; i < end; ++i, ++input) {
    const short data = input[1] << 8 | input[0];
    const short byteIndex = BINDEX(data & CL_DF_MASK);
    const short bitMask = BMASK(data & CL_DF_MASK);

    if (dfSmallLocal[byteIndex] & bitMask) {
      verifySmall(ctSmallEntries, ctSmallPids, patterns, input, i, inputLength,
                  result);
    }

    const uint dataLong =
        input[3] << 24 | input[2] << 16 | input[1] << 8 | input[0];
    if ((dfLargeLocal[byteIndex] & bitMask) &&
        isInHashDfLocal(dfLargeHashLocal, dataLong)) {
      verifyLarge(ctLargeBuckets, ctLargeEntries, ctLargePids, patterns,
                  dataLong, input, i, inputLength, result);
    }
  }
}

typedef union {
  uchar16 vector_raw;
  ushort8 vector;
  ushort scalar[8];
} Vec8;

#define BINDEX_VEC(x) ((x) >> (ushort8)(3))
#define BMASK_VEC(x) ((ushort8)(1) << ((x) & (ushort8)(0x7)))

__kernel void search_vec(const int inputLength, __global const uchar *input,
                         __global const DFC_FIXED_PATTERN *patterns,
                         __global const uchar *const dfSmall,
                         __global const uchar *const dfLarge,
                         __global const uchar *const dfLargeHash,
                         __global const CompactTableSmallEntry *ctSmallEntries,
                         __global const PID_TYPE *ctSmallPids,
                         __global const CompactTableLargeBucket *ctLargeBuckets,
                         __global const CompactTableLargeEntry *ctLargeEntries,
                         __global const PID_TYPE *ctLargePids,
                         __global VerifyResult *result) {
  uint threadId = (get_group_id(0) * get_local_size(0) + get_local_id(0));
  int i = threadId * THREAD_GRANULARITY;

  if (i >= inputLength) {
    return;
  }

  result += threadId;
  result->matchCount = 0;

  const int end = min(i + THREAD_GRANULARITY, inputLength);

  input += i;

  Vec8 matchesSmall[THREAD_GRANULARITY >> 3];
  Vec8 matchesLarge[THREAD_GRANULARITY >> 3];

  for (uchar matchIdx = 0; i < end; ++matchIdx, i += 8) {
    uchar8 dataThis = vload8(matchIdx, input);
    uchar16 shuffleMask =
        (uchar16)(0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 0);
    Vec8 overlappingData = (Vec8)shuffle(dataThis, shuffleMask);
    overlappingData.vector_raw.sf = input[(matchIdx + 1) << 3];

    overlappingData.vector = overlappingData.vector & (ushort8)(CL_DF_MASK);
    ushort8 bitMasks = BMASK_VEC(overlappingData.vector);
    Vec8 bitIndices = (Vec8)BINDEX_VEC(overlappingData.vector);

    Vec8 dfGatherSmall;
    Vec8 dfGatherLarge;

    for (int k = 0; k < 8; ++k) {
      dfGatherSmall.scalar[k] = dfSmall[bitIndices.scalar[k]];
      dfGatherLarge.scalar[k] = dfLarge[bitIndices.scalar[k]];
    }

    matchesSmall[matchIdx] = (Vec8)(dfGatherSmall.vector & bitMasks);
    matchesLarge[matchIdx] = (Vec8)(dfGatherLarge.vector & bitMasks);
  }

  i = threadId * THREAD_GRANULARITY;
  for (uchar k = 0; i < end; ++k, ++i, ++input) {
    if (matchesSmall[k >> 3].scalar[k % 8]) {
      verifySmall(ctSmallEntries, ctSmallPids, patterns, input, i, inputLength,
                  result);
    }

    const uint dataLong =
        input[3] << 24 | input[2] << 16 | input[1] << 8 | input[0];
    if (matchesLarge[k >> 3].scalar[k % 8] &&
        isInHashDf(dfLargeHash, dataLong)) {
      verifyLarge(ctLargeBuckets, ctLargeEntries, ctLargePids, patterns,
                  dataLong, input, i, inputLength, result);
    }
  }
}

__kernel void filter(int inputLength, __global uchar *input,
                     __global uchar *dfSmall, __global uchar *dfLarge,
                     __global uchar *dfLargeHash, __global uchar *result) {
  uint i = (get_group_id(0) * get_local_size(0) + get_local_id(0)) *
           THREAD_GRANULARITY;

  for (int j = 0; j < THREAD_GRANULARITY && i < inputLength; ++j, ++i) {
    short data = *(input + i + 1) << 8 | *(input + i);
    short byteIndex = BINDEX(data & DF_MASK);
    short bitMask = BMASK(data & DF_MASK);

    // set the first bit
    // (important that it's not an OR as we need to set it to 0 since the memory
    // might be uninitialized)
    result[i] = (dfSmall[byteIndex] & bitMask) > 0;

    // set the second bit
    result[i] |=
        ((dfLarge[byteIndex] & bitMask) && i < inputLength - 3 &&
         isInHashDf(dfLargeHash, (input[3 + i] << 24 | input[2 + i] << 16 |
                                  input[1 + i] << 8 | input[i])))
        << 1;
  }
}

__kernel void filter_with_image(int inputLength, __global uchar *input,
                                __read_only image1d_t dfSmall,
                                __read_only image1d_t dfLarge,
                                __global uchar *dfLargeHash,
                                __global uchar *result) {
  uint i = (get_group_id(0) * get_local_size(0) + get_local_id(0)) *
           THREAD_GRANULARITY;

  for (int j = 0; j < THREAD_GRANULARITY && i < inputLength; ++j, ++i) {
    uchar matches = 0;
    short data = *(input + i + 1) << 8 | *(input + i);
    short byteIndex = BINDEX(data & DF_MASK);
    short bitMask = BMASK(data & DF_MASK);

    img_read df =
        (img_read)read_imageui(dfSmall, SHIFT_BY_CHANNEL_SIZE(byteIndex));
    result[i] =
        (df.scalar[byteIndex % TEXTURE_CHANNEL_BYTE_SIZE] & bitMask) > 0;

    df = (img_read)read_imageui(dfLarge, SHIFT_BY_CHANNEL_SIZE(byteIndex));
    result[i] |=
        ((df.scalar[byteIndex % TEXTURE_CHANNEL_BYTE_SIZE] & bitMask) &&
         i < inputLength - 3 &&
         isInHashDf(dfLargeHash, (input[3 + i] << 24 | input[2 + i] << 16 |
                                  input[1 + i] << 8 | input[i])))
        << 1;
  }
}
__kernel void filter_with_local(int inputLength, __global uchar *input,
                                __global uchar *dfSmall,
                                __global uchar *dfLarge,
                                __global uchar *dfLargeHash,
                                __global uchar *result) {
  uint i = (get_group_id(0) * get_local_size(0) + get_local_id(0)) *
           THREAD_GRANULARITY;

  __local uchar dfSmallLocal[DF_SIZE_REAL];
  __local uchar dfLargeLocal[DF_SIZE_REAL];
  __local uchar dfLargeHashLocal[DF_SIZE_REAL];

  for (int j = LOCAL_MEMORY_LOAD_PER_ITEM * get_local_id(0);
       j < (LOCAL_MEMORY_LOAD_PER_ITEM * (get_local_id(0) + 1)); ++j) {
    dfSmallLocal[j] = dfSmall[j];
    dfLargeLocal[j] = dfLarge[j];
    dfLargeHashLocal[j] = dfLargeHash[j];
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  for (int j = 0; j < THREAD_GRANULARITY && i < inputLength; ++j, ++i) {
    short data = *(input + i + 1) << 8 | *(input + i);
    short byteIndex = BINDEX(data & DF_MASK);
    short bitMask = BMASK(data & DF_MASK);

    // set the first bit
    // (important that it's not an OR as we need to set it to 0 since the memory
    // might be uninitialized)
    result[i] = (dfSmallLocal[byteIndex] & bitMask) > 0;

    // set the second bit
    result[i] |= ((dfLargeLocal[byteIndex] & bitMask) && i < inputLength - 3 &&
                  isInHashDfLocal(dfLargeHashLocal,
                                  (input[3 + i] << 24 | input[2 + i] << 16 |
                                   input[1 + i] << 8 | input[i])))
                 << 1;
  }
}

typedef union {
  uchar8 vector;
  uchar scalar[8];
} UChar8;

__kernel void filter_vec(int inputLength, __global uchar *input,
                         __global uchar *dfSmall, __global uchar *dfLarge,
                         __global uchar *dfLargeHash, __global uchar *result) {
  uint i = (get_group_id(0) * get_local_size(0) + get_local_id(0)) *
           THREAD_GRANULARITY;

  for (int j = 0; j < THREAD_GRANULARITY && i < inputLength; j += 8, i += 8) {
    uchar8 dataThis = vload8(i >> 3, input);
    uchar8 dataNext = vload8((i >> 3) + 1, input);
    uchar16 shuffleMask =
        (uchar16)(0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 0);
    Vec8 overlappingData = (Vec8)shuffle(dataThis, shuffleMask);
    overlappingData.vector_raw.sf = dataNext.s0;

    overlappingData.vector = overlappingData.vector & (ushort8)(DF_MASK);
    ushort8 bitMasks = BMASK_VEC(overlappingData.vector);
    Vec8 bitIndices = (Vec8)BINDEX_VEC(overlappingData.vector);

    Vec8 dfGatherSmall;
    Vec8 dfGatherLarge;

    for (int k = 0; k < 8; ++k) {
      dfGatherSmall.scalar[k] = dfSmall[bitIndices.scalar[k]];
      dfGatherLarge.scalar[k] = dfLarge[bitIndices.scalar[k]];
    }

    UChar8 filterResultSmall =
        (UChar8)convert_uchar8(dfGatherSmall.vector & bitMasks);
    UChar8 filterResultLarge =
        (UChar8)convert_uchar8(dfGatherLarge.vector & bitMasks);

    UChar8 resultVector =
        (UChar8)convert_uchar8(filterResultSmall.vector > (uchar)0);

    for (int k = 0; k < 8 && i + k < inputLength; ++k) {
      resultVector.scalar[k] |=
          (filterResultLarge.scalar[k] && i + k < inputLength - 3 &&
           isInHashDf(dfLargeHash,
                      (input[3 + i + k] << 24 | input[2 + i + k] << 16 |
                       input[1 + i + k] << 8 | input[i + k])))
          << 1;
    }

    vstore8(resultVector.vector, i >> 3, result);
  }
}
