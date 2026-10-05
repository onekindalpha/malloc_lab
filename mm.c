/*
 * mm-naive.c - The fastest, least memory-efficient malloc package.
 *
 * In this naive approach, a block is allocated by simply incrementing
 * the brk pointer.  A block is pure payload. There are no headers or
 * footers.  Blocks are never coalesced or reused. Realloc is
 * implemented directly using mm_malloc and mm_free.
 *
 * NOTE TO STUDENTS: Replace this header comment with your own header
 * comment that gives a high level description of your solution.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

/*********************************************************
 * NOTE TO STUDENTS: Before you do anything else, please
 * provide your team information in the following struct.
 ********************************************************/
team_t team = {
    /* Team name */
    "ateam",
    /* First member's full name */
    "Harry Bovik",
    /* First member's email address */
    "bovik@cs.cmu.edu",
    /* Second member's full name (leave blank if none) */
    "",
    /* Second member's email address (leave blank if none) */
    ""};

/* single word (4) or double word (8) alignment */
#define ALIGNMENT 8

/* rounds up to the nearest multiple of ALIGNMENT */
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~0x7)

#define SIZE_T_SIZE (ALIGN(sizeof(size_t)))

/*
 * mm_init - initialize the malloc package.
 */
int mm_init(void)
{
    return 0;
}

#if 1   /* ← 지금은 naive 버전 사용. 일요일 저녁에 find_fit, place를 만든 뒤 이 1을 0으로, 아래 #if 0을 1로 바꾸기 */
/*
 * mm_malloc - Allocate a block by incrementing the brk pointer.
 *     Always allocate a block whose size is a multiple of the alignment.
 */
void *mm_malloc(size_t size)
{
    int newsize = ALIGN(size + SIZE_T_SIZE);
    void *p = mem_sbrk(newsize);
    if (p == (void *)-1)
        return NULL;
    else
    {
        *(size_t *)p = size;
        return (void *)((char *)p + SIZE_T_SIZE);
    }
}
#endif

#if 0   /* ← 교재 버전 (CS:APP 그림 9.47). 매크로, find_fit, place, extend_heap이 있어야 컴파일됨 */
/*
 * mm_malloc - size 바이트 이상을 담을 수 있는 블록을 할당하고 그 bp를 돌려준다
 *   1) 요청 크기를 실제 블록 크기(asize)로 조정 (헤더+풋터 포함, 8의 배수, 최소 16)
 *   2) find_fit으로 맞는 빈 블록을 찾으면 place로 배치
 *   3) 못 찾으면 extend_heap으로 힙을 늘려서 배치
 */
void *mm_malloc(size_t size)
{
    size_t asize;        /* 조정된 블록 크기 */
    size_t extendsize;   /* 맞는 블록이 없을 때 힙을 늘릴 양 */
    char *bp;

    if (size == 0)                                              /* 의미 없는 요청은 무시 */
        return NULL;

    if (size <= DSIZE)                                          /* 8바이트 이하면 최소 블록 16 */
        asize = 2*DSIZE;
    else                                                        /* 요청 + 8(헤더·풋터)을 8의 배수로 올림 */
        asize = DSIZE * ((size + (DSIZE) + (DSIZE-1)) / DSIZE);

    if ((bp = find_fit(asize)) != NULL) {                       /* 맞는 빈 블록 찾기 */
        place(bp, asize);                                       /* 찾았으면 배치 */
        return bp;
    }

    extendsize = MAX(asize, CHUNKSIZE);                         /* 못 찾으면 asize와 4096 중 큰 만큼 */
    if ((bp = extend_heap(extendsize/WSIZE)) == NULL)           /* 워드 개수로 넘겨 힙을 늘림 */
        return NULL;
    place(bp, asize);                                           /* 새 빈 블록에 배치 */
    return bp;
}
#endif

/*
 * mm_free - Freeing a block does nothing.
 */
void mm_free(void *ptr)
{
}

/*
 * mm_realloc - Implemented simply in terms of mm_malloc and mm_free
 */
void *mm_realloc(void *ptr, size_t size)
{
    void *oldptr = ptr;
    void *newptr;
    size_t copySize;

    newptr = mm_malloc(size);
    if (newptr == NULL)
        return NULL;
    copySize = *(size_t *)((char *)oldptr - SIZE_T_SIZE);
    if (size < copySize)
        copySize = size;
    memcpy(newptr, oldptr, copySize);
    mm_free(oldptr);
    return newptr;
}