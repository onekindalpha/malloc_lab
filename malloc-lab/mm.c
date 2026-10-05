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
/* 마지막 3비트를 0으로 만든다 -> 크기를 8의 배수로 맞춘다. 올린다.  */
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~0x7)

/* Size_t하나를 저장하는데 필요한 공간을 8바이트 정렬 기준으로 계산한다.  */
// #define SIZE_T_SIZE (ALIGN(sizeof(size_t)))
/* SIZE_T_SIZE는 8바이트 크기 칸이 몇 바이트인지 나타내던 매크로로, 지금은 헤더가 대신함.
HDRP와 GET_SIZE 매크로로 대신하고 있음.
*/

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

/* Seg_list를 만들기 위해 새로 만든 매크로들 */
/* 최소 블록 크기 24 */
#define MINBLOCK 24
#define PRED(bp) (*(char **)(bp))                   /* bp 위치의 8바이트 = 앞 블록 주소 */
#define SUCC(bp) (*(char **)((char *)(bp) + DSIZE)) /* bp +8 위치의 8바이트 =. ㅟ 블록 주소 */

/* 전역변수를 많이 쓰지 말라고 함. 아래 정도면 괜찮겠지 */

static char *heap_listp; // Pointer to first block
static char *free_listp; //

static void *extend_heap(size_t words);
static void *find_fit(size_t asize);
static void *coalesce(void *bp);
static void place(void *bp, size_t asize);
static void remove_block(void *bp);
static void insert_block(void *bp);

int mm_check(void);

// #define DEBUG // "DEBUG라는 이름이 존재한다"고 표시만 함 (값은 없음)
//  이 줄을 주석 처리하면 → DEBUG가 없는 상태

#ifdef DEBUG // 만약 DEBUG가 존재하면 (= 검사 켜짐)
#define CHECKHEAP()                            \
    do                                         \
    {                                          \
        if (!mm_check())                       \
        {                                      \
            printf("힙 깨짐! %s\n", __func__); \
            exit(1);                           \
        }                                      \
    } while (0)
//   CHECKHEAP()를 → "mm_check를 실행하고, 0이 나오면 메시지 찍고 프로그램 종료"로 바꿔라

#else               // DEBUG가 없으면 (= 검사 꺼짐)
#define CHECKHEAP() //   CHECKHEAP()를 → 아무것도 없는 것으로 바꿔라 (그 줄이 사라진 것과 같음)
#endif              // 조건 끝

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
    free_listp = NULL; // for seglist
    /* Extend the empty heap with a free block of CHUNKSIZE bytes */
    if (extend_heap(CHUNKSIZE / WSIZE) == NULL)
        return -1;
    CHECKHEAP();
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
    for (bp = free_listp; bp != NULL; bp = SUCC(bp))
    {
        // 할당여부는 검사할 필요가 없음.
        if (asize <= GET_SIZE(HDRP(bp)))
        {
            return bp;
        }
    }
    // 여기서 널을 반환하면 Mm_malloc에서 검사를 해서 알맞은 공간이 없으면 extend_heap을 한다.
    return NULL;
}

/* static seglist의 remove block*/
static void remove_block(void *bp)
{

    /*일단 세개의 케이스를 나눠서 지운다. */
    /* 일단 내 앞이 있으면 */
    if (PRED(bp) != NULL)
        SUCC(PRED(bp)) = SUCC(bp);
    /* 맨 앞이 나일때*/
    else
        free_listp = SUCC(bp);
    /* 일단 내 뒤가 있으면 */
    if (SUCC(bp) != NULL)
        PRED(SUCC(bp)) = PRED(bp);
}

/* static seglist의 insert block*/
static void insert_block(void *bp)
{
    /* 프리리스트 관리할때 맨 앞에 넣어서 관리를 함. 그게 편하기 때문임.*/
    /* 처음에 아예 프리리스트가 비어있다고 생각을 하고 구현을 함 */
    /* 내 뒤와 앞을 정한다. */
    SUCC(bp) = free_listp;
    PRED(bp) = NULL;
    if (free_listp != NULL)
    {
        PRED(free_listp) = bp;
    }
    free_listp = bp;
}

/* static seglist의 place */
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp)); // 현재 블록의 크기

    /* 지금 프리리스트로 빼는 애를 remove를 먼저 한다. */
    remove_block(bp);
    /* 그 다음에 insert를 진행을 한다. */
    if ((csize - asize) >= (MINBLOCK))
    {
        // 현재 블록 크기에서 요청한 크기를 뺀 것이 최소 블록 이상이면
        PUT(HDRP(bp), PACK(asize, 1)); // 현재 블록 헤더를 요청한 크기로 바꾼다.
        PUT(FTRP(bp), PACK(asize, 1)); // 현재 블록 풋터를 요청한 크기로 바꾼다.
        bp = NEXT_BLKP(bp);
        PUT(HDRP(bp), PACK(csize - asize, 0)); // 다음 블록 헤더를 남는 크기로 바꾼다.
        PUT(FTRP(bp), PACK(csize - asize, 0)); // 다음 블록 풋터를 남는 크기로 바꾼다.
        insert_block(bp);                      // insert_block을 진행을 한다.
    }
    else
    {
        PUT(HDRP(bp), PACK(csize, 1)); //
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
        asize = MINBLOCK; // 최소 블록 크기 24바이트
    else
        asize = DSIZE * ((size + (DSIZE) + (DSIZE - 1)) / DSIZE); // size + 헤더/풋터 + 정렬
    /* 먼저 파인드 핏으로 찾아본다. */
    if ((bp = find_fit(asize)) != NULL)
    {
        /* 찾았으면 그 자리에 배치 */
        place(bp, asize);
        CHECKHEAP();
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
    CHECKHEAP();
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
    CHECKHEAP();
}

void *coalesce(void *ptr)
{
    size_t prev_alloc = GET_ALLOC(HDRP(PREV_BLKP(ptr)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(ptr)));
    size_t size = GET_SIZE(HDRP(ptr));

    if (prev_alloc && next_alloc) // Case 1
    {
    }
    else if (prev_alloc && !next_alloc) // Case 2
    {
        remove_block(NEXT_BLKP(ptr));
        size += GET_SIZE(HDRP(NEXT_BLKP(ptr)));
        /* 여기서 이미 합쳐진 상태에서 HDRP랑 FTRP 사용 */
        PUT(HDRP(ptr), PACK(size, 0));
        PUT(FTRP(ptr), PACK(size, 0));
    }
    else if (!prev_alloc && next_alloc) // Case 3
    {
        remove_block(PREV_BLKP(ptr));
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
        remove_block(PREV_BLKP(ptr));
        remove_block(NEXT_BLKP(ptr));
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
    insert_block(ptr);
    return ptr;
}
/*
 * mm_realloc - Implemented simply in terms of mm_malloc and mm_free
 */
/* ptr 크기를 바꿀 옛 블록의 주소(bp). size = 새로 원하는 크기 */
void *mm_realloc(void *ptr, size_t size)
{
    /* 0. 예외 처리 */
    if (ptr == NULL)
        return mm_malloc(size);
    if (size == 0)
    {
        mm_free(ptr);
        return NULL;
    }
    /* 1. 필요한 크기 계산 */
    size_t asize;    // 새로 필요한 블록 크기 - 사이즈랑은 다름. 아마 헤더랑 풋터 제외해야 할 것.
    size_t oldblock; // 지금 블록 전체 크기 - 사이즈랑은 다름. 아마
    if (size <= DSIZE)
        /* 최소블록 24*/
        /* seglist에서는 다를 텐데. */
        asize = MINBLOCK;
    else
        asize = DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);
    oldblock = GET_SIZE(HDRP(ptr));
    /* 2. 제자리: 이미 충분히 큰 경우 */
    if (oldblock >= asize)
        return ptr;

    /* 3. 제자리: 뒤 빈 블록과 합치는 경우 */
    void *next = NEXT_BLKP(ptr);

    /* 뒤가 에필로그면 힙을 늘려서 뒤에 빈 블록 만들기*/
    if (GET_SIZE(HDRP(next)) == 0)
    {
        size_t need = asize - oldblock;
        if (need < MINBLOCK)
            ;
        need = MINBLOCK;
        if (extend_heap(need / WSIZE) == NULL)
            return NULL;
        next = NEXT_BLKP(ptr);
    }
    size_t nextsize = GET_SIZE(HDRP(next));
    /* 현재 블록과 뒤 빈 블록과 합쳤을 때 큰 경우 */
    if (!GET_ALLOC(HDRP(next)) && (oldblock + nextsize) >= asize)
    {
        /* TODO: 뒤 블록을 리스트에서 빼기 */
        remove_block(next);
        /* TODO: 내 헤더에 (합친 크기, 1) */
        PUT(HDRP(ptr), PACK(oldblock + nextsize, 1));
        /* TODO: 내 풋터에 (합친 크기, 1) */
        PUT(FTRP(ptr), PACK(oldblock + nextsize, 1));
        CHECKHEAP();
        return ptr;
    }
    /*  4. 제자리로 안 되면: 새 블록 + 복사 + 반납 */
    void *newptr = mm_malloc(size);
    if (newptr == NULL)
        return NULL;
    size_t oldsize = oldblock - DSIZE;
    size_t copysize = (size < oldsize) ? size : oldsize; /* TODO: size와 oldsize 중 작은 쪽 */

    /* TODO: 옛 데이터를 새 블록으로 복사 */
    memcpy(newptr, ptr, copysize);
    mm_free(ptr);
    CHECKHEAP();
    return newptr;
}

/*
* mm_check - 힙을 처음부터 끝까지 훑으면서 일관성을 검사한다.
힙이 정상이면 1(0이 아닌 값), 문제가 있으면 오류를 출력하고 0을 반환한다.
검사 항목: 1) 프롤로그 2) 정렬 3) 최소 크기 4) 헤더 = 풋터 5) 힙 범위
6) 연속된 빈 블록 7) 에필로그)
*/
/*
 * mm_check - 힙과 가용 리스트를 훑으면서 일관성을 검사한다.
 *   정상이면 1, 문제가 있으면 오류를 출력하고 0을 반환한다.
 *   [힙 검사]    1) 프롤로그  2) 정렬  3) 최소 크기  4) 헤더 = 풋터
 *               5) 힙 범위   6) 연속된 빈 블록  7) 에필로그  8) 겹치는 블록
 *   [리스트 검사] 9) 리스트 블록이 free인가  10) SUCC가 힙 안인가
 *               11) 앞뒤 연결  12) 고리 방지  13) 맨 앞 PRED == NULL
 *               14) 힙의 빈 블록 수 == 리스트 블록 수
 */
int mm_check(void)
{
    char *bp = heap_listp;
    int ok = 1;        // 하나라도 실패하면 0
    int prev_free = 0; // 바로 앞 블록이 빈 블록이었는지
    int heap_free = 0; // 힙을 훑으며 센 빈 블록 수
    int list_free = 0; // 리스트를 따라가며 센 블록 수

    // 1) 프롤로그: 크기 DSIZE, 할당 1
    if (GET_SIZE(HDRP(bp)) != DSIZE || !GET_ALLOC(HDRP(bp)))
    {
        printf("[mm_check] 프롤로그 오류\n");
        ok = 0;
    }

    /* ===== 힙 순회: 모든 블록을 주소 순서대로 ===== */
    for (bp = NEXT_BLKP(heap_listp); GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp))
    {
        // 2) 정렬: bp가 8의 배수인가
        if ((size_t)bp % ALIGNMENT)
        {
            printf("[mm_check] 정렬 오류: bp=%p\n", bp);
            ok = 0;
        }
        // 3) 크기: 8의 배수이고 최소 블록 이상인가
        if (GET_SIZE(HDRP(bp)) % ALIGNMENT || GET_SIZE(HDRP(bp)) < MINBLOCK)
        {
            printf("[mm_check] 크기 오류: bp=%p, 크기=%u\n", bp, GET_SIZE(HDRP(bp)));
            ok = 0;
        }
        // 4) 헤더 == 풋터
        if (GET(HDRP(bp)) != GET(FTRP(bp)))
        {
            printf("[mm_check] 헤더 != 풋터: bp=%p, 헤더=%u/%u, 풋터=%u/%u\n", bp,
                   GET_SIZE(HDRP(bp)), GET_ALLOC(HDRP(bp)),
                   GET_SIZE(FTRP(bp)), GET_ALLOC(FTRP(bp)));
            ok = 0;
        }
        // 5) 힙 범위 안인가
        if ((void *)bp < mem_heap_lo() || (void *)bp > mem_heap_hi())
        {
            printf("[mm_check] 힙 범위 오류: bp=%p\n", bp);
            ok = 0;
        }
        // 6) 연속된 빈 블록 (coalesce 빠뜨림)
        if (prev_free && !GET_ALLOC(HDRP(bp)))
        {
            printf("[mm_check] 연속된 빈 블록: bp=%p\n", bp);
            ok = 0;
        }
        prev_free = !GET_ALLOC(HDRP(bp));

        // 8) 겹침: 다음 블록은 반드시 나보다 뒤
        if (NEXT_BLKP(bp) <= bp)
        {
            printf("[mm_check] 겹치는 블록: bp=%p, 다음=%p\n", bp, NEXT_BLKP(bp));
            ok = 0;
        }

        // 빈 블록 개수 세기 (14번에서 리스트 개수와 비교)
        if (!GET_ALLOC(HDRP(bp)))
            heap_free++;
    }

    // 7) 에필로그: 루프가 끝난 bp = 에필로그, 크기 0 / 할당 1
    if (GET_SIZE(HDRP(bp)) != 0 || !GET_ALLOC(HDRP(bp)))
    {
        printf("[mm_check] 에필로그 오류: bp=%p, 헤더=%u/%u\n",
               bp, GET_SIZE(HDRP(bp)), GET_ALLOC(HDRP(bp)));
        ok = 0;
    }

    /* 리스트 순회: 빈 블록만 SUCC로 따라감*/
    for (bp = free_listp; bp != NULL; bp = SUCC(bp))
    {
        list_free++;

        // 9) 리스트에 있는 블록은 free여야 함 (place의 remove 빠뜨림)
        if (GET_ALLOC(HDRP(bp)))
        {
            printf("[mm_check] 리스트에 할당 블록: bp=%p\n", bp);
            ok = 0;
        }

        // 10) SUCC가 NULL이 아니면 힙 안을 가리켜야 함
        if (SUCC(bp) != NULL &&
            ((void *)SUCC(bp) < mem_heap_lo() || (void *)SUCC(bp) > mem_heap_hi()))
        {
            printf("[mm_check] SUCC가 힙 밖: bp=%p, SUCC=%p\n", bp, SUCC(bp));
            ok = 0;
            break; // 쓰레기 주소를 더 따라가면 세그폴트 → 멈춤
        }

        // 11) 앞뒤 연결: 내 뒤 블록의 PRED는 나여야 함
        if (SUCC(bp) != NULL && PRED(SUCC(bp)) != bp)
        {
            printf("[mm_check] 연결 오류: bp=%p, SUCC=%p, SUCC의 PRED=%p\n",
                   bp, SUCC(bp), PRED(SUCC(bp)));
            ok = 0;
        }

        // 12) 고리 방지: 빈 블록 수보다 많이 셌으면 같은 블록을 또 지난 것
        if (list_free > heap_free)
        {
            printf("[mm_check] 리스트에 고리가 있음 (list=%d, heap=%d)\n", list_free, heap_free);
            ok = 0;
            break; // 무한 루프 방지
        }
    }

    // 13) 맨 앞 블록의 PRED는 NULL
    if (free_listp != NULL && PRED(free_listp) != NULL)
    {
        printf("[mm_check] 맨 앞 블록의 PRED가 NULL이 아님: %p\n", PRED(free_listp));
        ok = 0;
    }

    // 14) 힙의 빈 블록 수 == 리스트 블록 수 (insert/remove 빠뜨림)
    if (heap_free != list_free)
    {
        printf("[mm_check] 빈 블록 수 불일치: 힙=%d, 리스트=%d\n", heap_free, list_free);
        ok = 0;
    }

    return ok;
}