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

static int verifySmall(CompactTableSmallEntry *ct, DFC_FIXED_PATTERN *patterns,
                       uint8_t *input, int currentPos, int inputLength) {
  uint8_t hash = input[0];
  int matches = 0;
  for (int i = 0; i < (ct + hash)->pidCount; ++i) {
    PID_TYPE pid = (ct + hash)->pids[i];

    int patternLength = (patterns + pid)->pattern_length;
    if (patternLength == 3) {
      --input;
      --currentPos;
    }

    if (currentPos >= 0 && inputLength - currentPos >= patternLength) {
      matches += doesPatternMatch(input, (patterns + pid)->original_pattern,
                                  patternLength,
                                  (patterns + pid)->is_case_insensitive);
    }
  }

  return matches;
}

#define GET_ENTRY_LARGE_CT(hash, x) \
  ((ct + hash)->entries + sizeof(CompactTableLargeEntry) * x)
static int verifyLarge(CompactTableLarge *ct, DFC_FIXED_PATTERN *patterns,
                       uint8_t *input, int currentPos, int inputLength) {
  /*
   the last two bytes are used to match,
   hence we are now at least 2 bytes into the pattern
   */
  input -= 2;
  currentPos -= 2;

  uint32_t bytePattern =
      input[3] << 24 | input[2] << 16 | input[1] << 8 | input[0];
  uint32_t hash = hashForLargeCompactTable(bytePattern);

  int matches = 0;
  uint8_t multiplier = 0;
  for (; GET_ENTRY_LARGE_CT(hash, multiplier)->pidCount; ++multiplier) {
    if (GET_ENTRY_LARGE_CT(hash, multiplier)->pattern == bytePattern) {
      for (int i = 0; i < GET_ENTRY_LARGE_CT(hash, multiplier)->pidCount; ++i) {
        PID_TYPE pid = GET_ENTRY_LARGE_CT(hash, multiplier)->pids[i];

        int patternLength = (patterns + pid)->pattern_length;
        int startOfRelativeInput = currentPos - (patternLength - 4);
        if (startOfRelativeInput >= 0 &&
            inputLength - startOfRelativeInput >= patternLength) {
          uint8_t *relativeInput = input - (patternLength - 4);
          matches += doesPatternMatch(
              relativeInput, (patterns + pid)->original_pattern, patternLength,
              (patterns + pid)->is_case_insensitive);
        }
      }
    }
  }

  return matches;
}

static bool isInHashDf(uint8_t *df, uint8_t *input) {
  /*
   the last two bytes are used to match,
   hence we are now at least 2 bytes into the pattern
   */
  input -= 2;
  uint32_t data = input[3] << 24 | input[2] << 16 | input[1] << 8 | input[0];
  uint16_t byteIndex = directFilterHash(data);
  uint16_t bitMask = BMASK(data & DF_MASK);

  return df[byteIndex] & bitMask;
}

int searchCpu() {
  DFC_STRUCTURE *dfc = DFC_HOST_MEMORY.dfcStructure;
  DFC_PATTERNS *patterns = DFC_HOST_MEMORY.patterns;
  uint8_t *input = (uint8_t *)DFC_HOST_MEMORY.input;
  int inputLength = DFC_HOST_MEMORY.inputLength;

  int matches = 0;

  for (int i = 0; i < inputLength; ++i) {
    int16_t data = input[i + 1] << 8 | input[i];
    int16_t byteIndex = BINDEX(data & DF_MASK);
    int16_t bitMask = BMASK(data & DF_MASK);

    if (dfc->directFilterSmall[byteIndex] & bitMask) {
      matches += verifySmall(dfc->compactTableSmall, patterns->dfcMatchList,
                             input + i, i, inputLength);
    }

    if (i >= 2 && (dfc->directFilterLarge[byteIndex] & bitMask) &&
        isInHashDf(dfc->directFilterLargeHash, input + i)) {
      matches += verifyLarge(dfc->compactTableLarge, patterns->dfcMatchList,
                             input + i, i, inputLength);
    }
  }

  return matches;
}

int exactMatchingUponFiltering(uint8_t *result, int length) {
  DFC_STRUCTURE *dfc = DFC_HOST_MEMORY.dfcStructure;
  DFC_PATTERNS *patterns = DFC_HOST_MEMORY.patterns;
  uint8_t *input = (uint8_t *)DFC_HOST_MEMORY.input;

  int matches = 0;

  for (int i = 0; i < length; ++i) {
    if (result[i] & 0x01) {
      matches += verifySmall(dfc->compactTableSmall, patterns->dfcMatchList,
                             input + i, i, length);
    }

    if (result[i] & 0x02) {
      matches += verifyLarge(dfc->compactTableLarge, patterns->dfcMatchList,
                             input + i, i, length);
    }
  }

  return matches;
}