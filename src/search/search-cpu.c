#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#include "memory.h"
#include "search.h"
#include "shared-functions.h"
#include "utility.h"

static int my_strncmp(unsigned char *a, unsigned char *b, int n) {
  int i;
  for (i = 0; i < n; i++) {
    if (a[i] != b[i]) return -1;
  }
  return 0;
}

unsigned char my_tolower(unsigned char c) {
  if (c >= 65 && c <= 90) {
    return c + 32;
  }
  return c;
}

static int my_strncasecmp(unsigned char *a, unsigned char *b, int n) {
  int i;
  for (i = 0; i < n; i++) {
    if (my_tolower(a[i]) != my_tolower(b[i])) return -1;
  }
  return 0;
}

static bool doesPatternMatch(uint8_t *start, uint8_t *pattern, int length,
                             bool isCaseInsensitive) {
  if (isCaseInsensitive) {
    return !my_strncasecmp(start, pattern, length);
  }
  return !my_strncmp(start, pattern, length);
}

static void verifySmall(CompactTableSmallEntry *ct, PID_TYPE *pids,
                        DFC_FIXED_PATTERN *patterns, uint8_t *input,
                        int currentPos, int inputLength, VerifyResult *result) {
  uint8_t hash = input[0];

  int offset = (ct + hash)->offset;
  pids += offset;

  for (int i = 0; i < (ct + hash)->pidCount; ++i) {
    PID_TYPE pid = pids[i];

    int patternLength = (patterns + pid)->pattern_length;

    if (inputLength - currentPos >= patternLength &&
        doesPatternMatch(input, (patterns + pid)->original_pattern,
                         patternLength,
                         (patterns + pid)->is_case_insensitive)) {
      if (result->matchCount < MAX_MATCHES) {
        result->matches[result->matchCount] = pid;
      }
      ++result->matchCount;
    }
  }
}

static void verifyLarge(CompactTableLargeBucket *buckets,
                        CompactTableLargeEntry *entries, PID_TYPE *pids,
                        DFC_FIXED_PATTERN *patterns, uint8_t *input,
                        int currentPos, int inputLength, VerifyResult *result) {
  uint32_t bytePattern =
      input[3] << 24 | input[2] << 16 | input[1] << 8 | input[0];
  uint32_t hash = hashForLargeCompactTable(bytePattern);

  int32_t entryOffset = (buckets + hash)->entryOffset;

  for (int i = 0; i < (buckets + hash)->entryCount; ++i) {
    if ((entries + entryOffset + i)->pattern == bytePattern) {
      int32_t pidOffset = (entries + entryOffset + i)->pidOffset;

      for (int j = 0; j < (entries + entryOffset + i)->pidCount; ++j) {
        PID_TYPE pid = pids[pidOffset + j];

        int patternLength = (patterns + pid)->pattern_length;
        if (inputLength - currentPos >= patternLength) {
          if (doesPatternMatch(input, (patterns + pid)->original_pattern,
                               patternLength,
                               (patterns + pid)->is_case_insensitive)) {
            if (result->matchCount < MAX_MATCHES) {
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

static bool isInHashDf(uint8_t *df, uint8_t *input) {
  /*
   the last two bytes are used to match,
   hence we are now at least 2 bytes into the pattern
   */
  uint32_t data = input[3] << 24 | input[2] << 16 | input[1] << 8 | input[0];
  uint16_t byteIndex = directFilterHash(data);
  uint16_t bitMask = BMASK(data & DF_MASK);

  return df[byteIndex] & bitMask;
}

int searchCpuEmulateGpu(ReadFunction read, MatchFunction onMatch) {
  DFC_STRUCTURE *dfc = DFC_HOST_MEMORY.dfcStructure;
  DFC_PATTERNS *patterns = dfc->patterns;

  uint8_t *input = (uint8_t *)allocateInput(INPUT_READ_CHUNK_BYTES);

  VerifyResult *result =
      malloc(sizeInBytesOfResultVector(INPUT_READ_CHUNK_BYTES));
  if (!result) {
    fprintf(stderr, "Could not allocate result vector\n");
    exit(1);
  }

  int matches = 0;
  int readCount = 0;
  // read 1 byte less to allow matching of 1-byte patterns without accessing
  // invalid memory at the very last character
  while ((readCount = read(INPUT_READ_CHUNK_BYTES - 1, MAX_PATTERN_LENGTH,
                           (char *)input))) {
    for (int i = 0; i < readCount; ++i) {
      int16_t data = input[i + 1] << 8 | input[i];
      int16_t byteIndex = BINDEX(data & DF_MASK);
      int16_t bitMask = BMASK(data & DF_MASK);

      result[i].matchCount = 0;

      if (dfc->directFilterSmall[byteIndex] & bitMask) {
        verifySmall(dfc->ctSmallEntries, dfc->ctSmallPids,
                    patterns->dfcMatchList, input + i, i, readCount,
                    result + i);
      }

      if (i < readCount - 3 && (dfc->directFilterLarge[byteIndex] & bitMask) &&
          isInHashDf(dfc->directFilterLargeHash, input + i)) {
        verifyLarge(dfc->ctLargeBuckets, dfc->ctLargeEntries, dfc->ctLargePids,
                    patterns->dfcMatchList, input + i, i, readCount,
                    result + i);
      }
    }
    for (int i = 0; i < readCount; ++i) {
      VerifyResult *res = &result[i];

      for (int j = 0; j < res->matchCount && j < MAX_MATCHES; ++j) {
        onMatch(&patterns->dfcMatchList[res->matches[j]]);
        ++matches;
      }

      if (res->matchCount >= MAX_MATCHES) {
        printf(
            "%d patterns matched at position %d, but space was only allocated "
            "for %d patterns\n",
            res->matchCount, i, MAX_MATCHES);
      }
    }
  }

  free(result);
  freeDfcInput();

  return matches;
}

static int verifySmallRet(CompactTableSmallEntry *ct, PID_TYPE *pids,
                          DFC_FIXED_PATTERN *patterns, uint8_t *input,
                          int currentPos, int inputLength,
                          MatchFunction onMatch) {
  uint8_t hash = input[0];

  int offset = (ct + hash)->offset;
  pids += offset;

  int matches = 0;
  for (int i = 0; i < (ct + hash)->pidCount; ++i) {
    PID_TYPE pid = pids[i];

    int patternLength = (patterns + pid)->pattern_length;

    if (inputLength - currentPos >= patternLength &&
        doesPatternMatch(input, (patterns + pid)->original_pattern,
                         patternLength,
                         (patterns + pid)->is_case_insensitive)) {
      onMatch(&patterns[pid]);
      ++matches;
    }
  }

  return matches;
}

static int verifyLargeRet(CompactTableLargeBucket *buckets,
                          CompactTableLargeEntry *entries, PID_TYPE *pids,
                          DFC_FIXED_PATTERN *patterns, uint8_t *input,
                          int currentPos, int inputLength,
                          MatchFunction onMatch) {
  uint32_t bytePattern =
      input[3] << 24 | input[2] << 16 | input[1] << 8 | input[0];
  uint32_t hash = hashForLargeCompactTable(bytePattern);
  int32_t entryOffset = (buckets + hash)->entryOffset;

  int matches = 0;
  for (int i = 0; i < (buckets + hash)->entryCount; ++i) {
    if ((entries + entryOffset + i)->pattern == bytePattern) {
      int32_t pidOffset = (entries + entryOffset + i)->pidOffset;

      for (int j = 0; j < (entries + entryOffset + i)->pidCount; ++j) {
        PID_TYPE pid = pids[pidOffset + j];

        int patternLength = (patterns + pid)->pattern_length;
        if (inputLength - currentPos >= patternLength) {
          if (doesPatternMatch(input, (patterns + pid)->original_pattern,
                               patternLength,
                               (patterns + pid)->is_case_insensitive)) {
            onMatch(&patterns[pid]);
            ++matches;
          }
        }
      }

      break;
    }
  }

  return matches;
}

int searchCpu(ReadFunction read, MatchFunction onMatch) {
  DFC_STRUCTURE *dfc = DFC_HOST_MEMORY.dfcStructure;
  DFC_PATTERNS *patterns = dfc->patterns;

  uint8_t *input = (uint8_t *)allocateInput(INPUT_READ_CHUNK_BYTES);

  int matches = 0;
  int readCount = 0;
  // read 1 byte less to allow matching of 1-byte patterns without accessing
  // invalid memory at the very last character
  while ((readCount = read(INPUT_READ_CHUNK_BYTES - 1, MAX_PATTERN_LENGTH,
                           (char *)input))) {
    for (int i = 0; i < readCount; ++i) {
      int16_t data = input[i + 1] << 8 | input[i];
      int16_t byteIndex = BINDEX(data & DF_MASK);
      int16_t bitMask = BMASK(data & DF_MASK);

      if (dfc->directFilterSmall[byteIndex] & bitMask) {
        matches += verifySmallRet(dfc->ctSmallEntries, dfc->ctSmallPids,
                                  patterns->dfcMatchList, input + i, i,
                                  readCount, onMatch);
      }

      if (i < readCount - 3 && (dfc->directFilterLarge[byteIndex] & bitMask) &&
          isInHashDf(dfc->directFilterLargeHash, input + i)) {
        matches += verifyLargeRet(dfc->ctLargeBuckets, dfc->ctLargeEntries,
                                  dfc->ctLargePids, patterns->dfcMatchList,
                                  input + i, i, readCount, onMatch);
      }
    }
  }

  freeDfcInput();

  return matches;
}

int exactMatchingUponFiltering(uint8_t *input, uint8_t *result, int length,
                               DFC_PATTERNS *patterns, MatchFunction onMatch) {
  DFC_STRUCTURE *dfc = DFC_HOST_MEMORY.dfcStructure;

  int matches = 0;

  for (int i = 0; i < length; ++i) {
    if (result[i] & 0x01) {
      matches +=
          verifySmallRet(dfc->ctSmallEntries, dfc->ctSmallPids,
                         patterns->dfcMatchList, input + i, i, length, onMatch);
    }

    if (result[i] & 0x02) {
      matches += verifyLargeRet(dfc->ctLargeBuckets, dfc->ctLargeEntries,
                                dfc->ctLargePids, patterns->dfcMatchList,
                                input + i, i, length, onMatch);
    }
  }

  return matches;
}
