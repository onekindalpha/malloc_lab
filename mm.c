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
    "3team",
    /* First member's full name */
    "KIMEUNGI",
    /* First member's email address */
    "corebuilder56@gmail.com",
    /* Second member's full name (leave blank if none) */
    "NODOHYUN",
    /* Second member's email address (leave blank if none) */
    "dhrho1208@gmail.com"};

/* single word (4) or double word (8) alignment */
/* 정렬 기준을 8바이트로 정함.  */
#define ALIGNMENT 8

/* rounds up to the nearest multiple of ALIGNMENT */
/* 마지막 3비트를 0으로 만든다 -> 크기를 8의 배수로 맞춤. */
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~0x7)

/* Size_t하나를 저장하는데 필요한 공간을 8바이트 정렬 기준으로 계산한다.  */
#define SIZE_T_SIZE (ALIGN(sizeof(size_t)))

/* ======기본 상수 ====== */
#define WSIZE 4             // Word and header/footer size (bytes)
#define DSIZE 8             // Double word size (bytes)
#define CHUNKSIZE (1 << 12) // Extend heap by this amount (bytes

#define MAX(x, y) ((x) > (y) ? (x) : (y))

#define PACK(size, alloc) ((size) | (alloc)) // Pack a size and allocated bit into a word

#define GET(p) (*(unsigned int *)(p))              // Read a word at address p
#define PUT(p, val) (*(unsigned int *)(p) = (val)) // Write a word at address p

#define GET_SIZE(p) (GET(p) & ~0x7) // Read the size from address p
#define GET_ALLOC(p) (GET(p) & 0x1) // Read the allocated bit from address p

#define HDRP(bp) ((char *)(bp) - WSIZE)                      // Given block ptr bp, compute address of its header
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE) // Given block ptr bp, compute address of its footer

#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE))) // Given block ptr bp, compute address of next block
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE))) // Given block ptr bp, compute address of previous block

static char *heap_listp; // Pointer to first block

static void *extend_heap(size_t words);
static void *find_fit(size_t asize);
static void *coalesce(void *bp);
static void place(void *bp, size_t asize);

/*
 * mm_init - initialize the malloc package.
 */
int mm_init(void)
{
    /* Create the initial empty heap */
    if ((heap_listp = mem_sbrk(4 * WSIZE)) == (void *)-1)
        return -1;
    PUT(heap_listp, 0);                            // Alignment padding
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1)); // Prologue header
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); // Prologue footer
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));     // Epilogue header
    heap_listp += (2 * WSIZE);

    /* Extend the empty heap with a free block of CHUNKSIZE bytes */
    if (extend_heap(CHUNKSIZE / WSIZE) == NULL)
        return -1;
    return 0;
}

/* extend_heap 함수 - 힙을 words 워드만큼 늘려 새 가용 블록을 만든다.
새 가용 블록의 bp
*/
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    /* Allocate an even number of words to maintain alignment */
    /* 워드 개수를 짝수로 맞추어야 8의 배수 바이트가 된다. */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;

    /* 힙을 size만큼 늘린다. 실패하면 -1이 오므로 정수로 바꿔 비교 */
    if ((long)(bp = mem_sbrk(size)) == -1)
        return NULL;

    /* Initialize free block header/footer and the epilogue header */
    PUT(HDRP(bp), PACK(size, 0));         // Free block header - 새 가용 블록 헤더 (옛 에필로그 자리)
    PUT(FTRP(bp), PACK(size, 0));         // Free block footer - 새 가용 블록 풋터
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); // New epilogue header

    /* Coalesce if the previous block was free */
    /* 앞 블록이 가용이면 합치기 */
    return coalesce(bp);
}

/* static find_fit */
static void *find_fit(size_t asize)
{
    /* First Fit Search*/
    void *bp;
    /* 현재 헤더에서 읽힌 크기가 0보다 클 동안 다음 블록으로 이동한다. */
    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp))
    {
        /* 블록이 가용이고 asize(필요한 블록 크기) 보다 현재 에서 읽힌 크기가 더 클 때 */
        if (!GET_ALLOC(HDRP(bp)) && (asize <= GET_SIZE(HDRP(bp))))
        {
            /* 시작 주소를 반환한다. */
            return bp;
        }
    }
    /*못 찾은 경우 extend_heap으로 값을 늘림 */
    return NULL;
}

/* static place */
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp)); // 현재 블록의 크기

    if ((csize - asize) >= (2 * DSIZE))
    {
        // 현재 블록 크기에서 요청한 크기를 뺀 것이 최소 블록 이상이면
        PUT(HDRP(bp), PACK(asize, 1));         // 현재 블록 헤더를 요청한 크기로 바꾼다.
        PUT(FTRP(bp), PACK(asize, 1));         // 현재 블록 풋터를 요청한 크기로 바꾼다.
        bp = NEXT_BLKP(bp);                    // bp를 다음 블록으로 이동
        PUT(HDRP(bp), PACK(csize - asize, 0)); // 다음 블록 헤더를 남는 크기로 바꾼다.
        PUT(FTRP(bp), PACK(csize - asize, 0)); // 다음 블록 풋터를 남는 크기로 바꾼다.
    }
    else
    {
        PUT(HDRP(bp), PACK(csize, 1)); // 통째로 할당함
        PUT(FTRP(bp), PACK(csize, 1));
    }
}
/*
 * mm_malloc - Allocate a block by incrementing the brk pointer.
 *     Always allocate a block whose size is a multiple of the alignment.
 */
#if 1
void *mm_malloc(size_t size)
{
    size_t asize;      // Adjusted block size
    size_t extendsize; // Amount to extend heap if no fit
    char *bp;

    /* Ignore spurious requests */
    if (size == 0)
        return NULL;
    if (size <= DSIZE)
        asize = 2 * DSIZE; // 최소 블록 크기 16바이트
    else
        asize = DSIZE * ((size + (DSIZE) + (DSIZE - 1)) / DSIZE); // size + 헤더/풋터 + 정렬
    /* 먼저 파인드 핏으로 찾아본다. */
    if ((bp = find_fit(asize)) != NULL)
    {
        /* 찾았으면 그 자리에 배치 */
        place(bp, asize);
        /* 반환*/
        return bp;
    }
    /* 여기까지 내려왔다는 것은 find_fit이 NULL을 줬다. */
    extendsize = MAX(asize, CHUNKSIZE);
    if ((bp = extend_heap(extendsize / WSIZE)) == NULL)
        /* 힙도 못 늘리면 실패다. */
        return NULL;
    /* 새로 생긴 빈 블록에 배치. */
    place(bp, asize);
    return bp;
}

#endif
#if 0
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

/*
 * mm_free - Freeing a block does nothing.
 */
void mm_free(void *ptr)
{
    size_t size = GET_SIZE(HDRP(ptr));
    PUT(HDRP(ptr), PACK(size, 0));
    PUT(FTRP(ptr), PACK(size, 0));
    coalesce(ptr);
}

void *coalesce(void *ptr)
{
    size_t prev_alloc = GET_ALLOC(HDRP(PREV_BLKP(ptr)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(ptr)));
    size_t size = GET_SIZE(HDRP(ptr));

    if (prev_alloc && next_alloc) // Case 1
    {
        return ptr;
    }
    else if (prev_alloc && !next_alloc) // Case 2
    {
        size += GET_SIZE(HDRP(NEXT_BLKP(ptr)));
        /* 여기서 이미 합쳐진 상태에서 HDRP랑 FTRP 사용 */
        PUT(HDRP(ptr), PACK(size, 0));
        PUT(FTRP(ptr), PACK(size, 0));
    }
    else if (!prev_alloc && next_alloc) // Case 3
    {
        /* 앞 블록의 크기 */
        size += GET_SIZE(HDRP(PREV_BLKP(ptr)));
        /* 합쳐진 블록의 마지막 위치에 풋터를 새로 기록함 */
        /* 경계 태그 방식에서는 헤더와 풋터에 모두 블록 크기와 할당여부가 들어감. */
        PUT(FTRP(ptr), PACK(size, 0));
        /* 이전 블록의 헤더를 새 전체 크기로 바꾼다. */
        PUT(HDRP(PREV_BLKP(ptr)), PACK(size, 0));
        ptr = PREV_BLKP(ptr);
    }
    else
    {
        /* 블록 두개 합친 것 */
        size += GET_SIZE(HDRP(PREV_BLKP(ptr))) + GET_SIZE(FTRP(NEXT_BLKP(ptr)));
        /* 이전 블록에 헤더에도 사이즈를 기록 */
        PUT(HDRP(PREV_BLKP(ptr)), PACK(size, 0));
        /* 다음 블록 풋터에도 사이즈를 기록 */
        PUT(FTRP(NEXT_BLKP(ptr)), PACK(size, 0));
        /* coalesce할 때는 맨 앞 Header와 맨 끝 Footer만 새로운 경계를 표시하면 되므로
        중간에 남는 기존 Header/Footer는 갱신하지 않는다. 이후. 블록을 쪼갤때는 새로운 블록 경계가 생기므로 필요한 Header/Footer를 새로 기록한다.
        */
        /* ptr을 이전 블록으로 옮김 */
        ptr = PREV_BLKP(ptr);
    }
    return ptr;
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

/* 디버깅용 : 힙의 모든 블록을 출력한다. */
static void print_heap(const char *msg)
{
    char *bp;
    printf("---- %s ----\n", msg);
    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp))
    {
        printf("bp=%p  헤더 %u/%u  풋터 %u/%u%s\n",
               bp,
               GET_SIZE(HDRP(bp)), GET_ALLOC(HDRP(bp)),
               GET_SIZE(FTRP(bp)), GET_ALLOC(FTRP(bp)),
               GET(HDRP(bp)) != GET(FTRP(bp)) ? "   ← 헤더≠풋터!" : "");
    }
    printf("에필로그 bp=%p  헤더 %u/%u\n\n", bp, GET_SIZE(HDRP(bp)), GET_ALLOC(HDRP(bp)));
}