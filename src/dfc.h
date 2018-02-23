/*********************************/
/* Author  - Byungkwon Choi      */
/* Contact - cbkbrad@kaist.ac.kr */
/*********************************/
#ifndef DFC_H
#define DFC_H

#include <ctype.h>
#include <emmintrin.h>
#include <inttypes.h>
#include <nmmintrin.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "dfc_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compact Table (CT1) */
typedef struct pid_list_ {
  PID_TYPE pid[CT_TYPE1_PID_CNT_MAX];
  uint16_t cnt;
} CT_Type_1;

typedef struct CompactTableSmallBucket_ {
  uint32_t pattern;
  PID_TYPE pids[MAX_PID_PER_BUCKET];
} CompactTableSmallBucket;

typedef struct CompactTableSmall_ {
  CompactTableSmallBucket buckets[MAX_BUCKETS_FOR_INDEX];
} CompactTableSmall;

typedef struct CT_Type_2_2B_Array_ {
  uint16_t pat;      // 2B pattern
  PID_CNT_TYPE cnt;  // Number of PIDs
  PID_TYPE *pid;     // list of PIDs
} CT_Type_2_2B_Array;

/* Compact Table (CT2) */
typedef struct CT_Type_2_2B_ {
  BUC_CNT_TYPE cnt;
  CT_Type_2_2B_Array *array;
} CT_Type_2_2B;
/****************************************************/

typedef struct CT_Type_2_Array_ {
  uint32_t pat;      // Maximum 4B pattern
  PID_CNT_TYPE cnt;  // Number of PIDs
  PID_TYPE *pid;     // list of PIDs
} CT_Type_2_Array;

/* Compact Table (CT2) */
typedef struct CT_Type_2_ {
  BUC_CNT_TYPE cnt;
  CT_Type_2_Array *array;
} CT_Type_2;

/****************************************************/
/*                For New designed CT8              */
/****************************************************/
typedef struct CT_Type_2_8B_Array_ {
  uint64_t pat;      // 8B pattern
  PID_CNT_TYPE cnt;  // Number of PIDs
  PID_TYPE *pid;     // list of PIDs
} CT_Type_2_8B_Array;

/* Compact Table (CT2) */
typedef struct CT_Type_2_8B_ {
  BUC_CNT_TYPE cnt;
  CT_Type_2_8B_Array *array;
} CT_Type_2_8B;

typedef struct _dfc_pattern {
  struct _dfc_pattern *next;

  unsigned char *patrn;      // upper case pattern
  unsigned char *casepatrn;  // original pattern
  int n;                     // Patternlength
  int is_case_insensitive;

  uint32_t sids_size;
  PID_TYPE *sids;  // external id (unique)
  PID_TYPE iid;    // internal id (used in DFC library only)

} DFC_PATTERN;

typedef struct {
  DFC_PATTERN **init_hash;  // To cull duplicate patterns
  DFC_PATTERN *dfcPatterns;
  DFC_PATTERN **dfcMatchList;

  int numPatterns;

  uint8_t directFilterSmall[DF_SIZE_REAL];
  CompactTableSmall compactTableSmall[COMPACT_TABLE_SIZE_SMALL];

  /* Direct Filter (DF1) for all patterns */
  uint8_t DirectFilter1[DF_SIZE_REAL];
  uint8_t cDF0[256];
  uint8_t cDF1[DF_SIZE_REAL];
  uint8_t cDF2[DF_SIZE_REAL];

  uint8_t ADD_DF_4_plus[DF_SIZE_REAL];
  uint8_t ADD_DF_4_1[DF_SIZE_REAL];

  uint8_t ADD_DF_8_1[DF_SIZE_REAL];
  uint8_t ADD_DF_8_2[DF_SIZE_REAL];

  /* Compact Table (CT1) for 1B patterns */
  CT_Type_1 CompactTable1[CT1_TABLE_SIZE];

  /* Compact Table (CT2) for 2B patterns */
  CT_Type_2 CompactTable2[CT2_TABLE_SIZE];

  /* Compact Table (CT4) for 4B ~ 7B patterns */
  CT_Type_2 CompactTable4[CT4_TABLE_SIZE];

  /* Compact Table (CT8) for 8B ~ patterns */
  CT_Type_2_8B CompactTable8[CT8_TABLE_SIZE];

} DFC_STRUCTURE;

DFC_STRUCTURE *DFC_New(void);
int DFC_AddPattern(DFC_STRUCTURE *dfc, unsigned char *pat, int n,
                   int is_case_insensitive, PID_TYPE sid);
int DFC_Compile(DFC_STRUCTURE *dfc);

int DFC_Search(SEARCH_ARGUMENT);
void DFC_PrintInfo(DFC_STRUCTURE *dfc);
void DFC_FreeStructure(DFC_STRUCTURE *dfc);

#ifdef __cplusplus
}
#endif

#endif