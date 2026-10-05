/*
 * mm.c - Explicit free list 기반 malloc 패키지
 *
 * [블록 구조]
 *  할당 블록:
 *   +--------+---------------------------+--------+
 *   | header |         payload           | footer |
 *   | size|1 |                           | size|1 |
 *   +--------+---------------------------+--------+
 *     4B                                    4B
 *
 *  가용 블록:
 *   +--------+--------+--------+---------+--------+
 *   | header |  PRED  |  SUCC  |  (빈칸) | footer |
 *   | size|0 |  8B    |  8B    |         | size|0 |
 *   +--------+--------+--------+---------+--------+
 *   -> 최소 블록 크기 MINBLOCK = 4 + 8 + 8 + 4 = 24B
 *
 *  - header/footer 하위 비트: bit0 = 할당 여부
 *  - 모든 블록은 8바이트 정렬
 *
 * [힙 구조]
 *  | pad | prologue(8/1) | prologue(8/1) | 블록들 ... | epilogue(0/1) |
 *
 * [가용 리스트 관리]
 *  - 이중 연결 리스트 (PRED/SUCC 포인터를 가용 블록 payload에 저장)
 *  - 삽입: LIFO (free_listp 맨 앞에 삽입)
 *  - 탐색: first fit (free_listp부터 SUCC를 따라감)
 *  - free 시 coalesce로 앞뒤 가용 블록과 즉시 병합
 *
 * [배치 정책]
 *  - 남는 공간 >= MINBLOCK 이면 분할
 *  - 요청 크기 < PLACE_THRESHOLD(96): 블록 앞쪽에 배치
 *    요청 크기 >= PLACE_THRESHOLD   : 블록 뒤쪽에 배치 (단편화 감소)
 *
 * [realloc]
 *  - 새 크기가 현재 블록에 들어가면 그대로 반환
 *  - 다음 블록이 epilogue이면 힙을 확장해서 제자리 확장
 *  - 다음 블록이 가용이고 합쳐서 충분하면 제자리 확장
 *  - 그 외: malloc → memcpy → free
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
#define WSIZE 4            // Word and header/footer size (bytes)
#define DSIZE 8            // Double word size (bytes)
#define CHUNKSIZE (1 << 8) // Extend heap by this amount (bytes

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
#define PLACE_THRESHOLD 64

/* 리스트 개수 추가 - 크기별 리스트를 12개 두겠다. */
#define LISTNUM 12
/* 추가: i번 리스트 Head읽기 쓰기 매크로
seg_listp + i * 8 위치에 있는 8바이트 포인터.
*/
#define HEAD(i) (*(char **)(seg_listp + (i) * DSIZE))
/* 전역변수를 많이 쓰지 말라고 함. 아래 정도면 괜찮겠지 */

static char *heap_listp; // Pointer to first block
// static char *free_listp; //
/* head 배열(힙 안)의 시작 주소*/
static char *seg_listp;

static void *extend_heap(size_t words);
static void *find_fit(size_t asize);
static void *coalesce(void *bp);
static void *place(void *bp, size_t asize);
static void remove_block(void *bp);
static void insert_block(void *bp);
/*추가: Get_class 프로토타입*/
static int get_class(size_t size);

int mm_check(void);

// #define DEBUG // "DEBUG라는 이름이 존재한다"고 표시만 함 (값은 없음)
//   이 줄을 주석 처리하면 → DEBUG가 없는 상태

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
    /* head 12칸 (12*8바이트)과 기존 4워드 한번에 받아서 그 시작 주소를 seg_listp에 저장하겠다. */
    if ((seg_listp = mem_sbrk(LISTNUM * DSIZE + 4 * WSIZE)) == (void *)-1)
        return -1;
    /* 12개 리스트가 모두 비어있도록 head를 전부 NULL로 만들겠다.
    extend_heap안에서 insert_blocK이 불리기 때문에
    반드시 그 전에 해야 한다.
    */
    for (int i = 0; i < LISTNUM; i++)
    {
        HEAD(i) = NULL;
    }
    /*기존 힙 구조는 Head칸 바로 뒤에서 시작하게 하겠다. */
    heap_listp = seg_listp + (LISTNUM * DSIZE);

    PUT(heap_listp, 0);                            //
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1)); // Prologue header
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); // Prologue footer
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));     // Epilogue header
    heap_listp += (2 * WSIZE);

    // free_listp = NULL; // for seglist
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

/*get_class 블록 크기가 속하는 리스트 번호를 반환.
 */
static int get_class(size_t size)
{
    int i = 0;
    size_t limit = 32;
    // 크기 상한을 사용하면 됨.
    /* 크기가 지금 상한보다 크고 아직 마지막 리스트가 아니면,
    다음 리스트로 넘어가면서 상한을 2배로 늘리겠다. */
    while (size > limit && i < LISTNUM - 1)
    {
        i++;
        limit = limit * 2;
    }
    return i;
}
/* static find_fit */
static void *find_fit(size_t asize)
{
    /* First Fit Search*/
    void *bp;
    /* asize(요청 크기)가 속한 리스트부터 시작해서, 거기서 못 찾으면 더 큰 리스트로 넘어가며
    찾겠습니다. 그보다 작은 리스트에는 요청보다 작은 블록만 있으니 볼 필요가 없음*/
    for (int i = get_class(asize); i < LISTNUM; i++)
        /* 현재 헤더에서 읽힌 크기가 0보다 클 동안 다음 블록으로 이동한다. */
        for (bp = HEAD(i); bp != NULL; bp = SUCC(bp))
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
    /* 이 블록이 몇 번 리스트에 들어있는지 크기로 알아내겠다 */
    int i = get_class(GET_SIZE(HDRP(bp)));
    /* 일단 세개의 케이스를 나눠서 지운다. */
    /* 일단 내 앞이 있으면 */
    if (PRED(bp) != NULL)
        SUCC(PRED(bp)) = SUCC(bp);
    /* 맨 앞이 나일때*/
    else
        HEAD(i) = SUCC(bp);
    /* 일단 내 뒤가 있으면 */
    if (SUCC(bp) != NULL)
        PRED(SUCC(bp)) = PRED(bp);
}

/* static seglist의 insert block*/
static void insert_block(void *bp)
{
    /*이 블록의 헤더 크기를 보고 몇 번 리스트에 넣을 지 정한다. */
    int i = get_class(GET_SIZE(HDRP(bp)));
    SUCC(bp) = HEAD(i);
    PRED(bp) = NULL;
    if (HEAD(i) != NULL)
    {
        PRED(HEAD(i)) = bp;
    }
    HEAD(i) = bp;
}

/* static seglist의 place */
static void *place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp)); // 현재 블록의 크기
    /* 지금 프리리스트로 빼는 애를 remove를 먼저 한다. */
    remove_block(bp);
    /* 분할을 하는 경우 */
    if ((csize - asize) >= (MINBLOCK))
    {
        // 작은 요청: 앞쪽 할당, 뒤쪽 빈 조각 //
        if (asize < PLACE_THRESHOLD)
        {
            // 작은 요청: 할당한 것이 앞에 오는 경우.
            void *alloc = bp;
            // 현재 블록 크기에서 요청한 크기를 뺀 것이 최소 블록 이상이면
            PUT(HDRP(bp), PACK(asize, 1)); // 현재 블록 헤더를 요청한 크기로 바꾼다.
            PUT(FTRP(bp), PACK(asize, 1)); // 현재 블록 풋터를 요청한 크기로 바꾼다.
            // 다음 블록으로 이동한다.
            bp = NEXT_BLKP(bp);
            PUT(HDRP(bp), PACK(csize - asize, 0)); // 다음 블록 헤더를 남는 크기로 바꾼다.
            PUT(FTRP(bp), PACK(csize - asize, 0)); // 다음 블록 풋터를 남는 크기로 바꾼다.
            // 뒤쪽 조각을 리스트에 넣기
            insert_block(bp); // insert_block을 진행을 한다.
            return alloc;
        }
        else
        {
            // 새 방식: 앞쪽 빈 조각, 뒤쪽 할당 //
            // 앞쪽 헤더 풋터
            PUT(HDRP(bp), PACK(csize - asize, 0));
            PUT(FTRP(bp), PACK(csize - asize, 0));
            // 앞쪽 조각을 리스트에 넣기
            insert_block(bp);
            // bp를 뒤쪽으로 이동
            bp = NEXT_BLKP(bp);
            // 뒤쪽 헤더, 풋터 정의하기
            PUT(HDRP(bp), PACK(asize, 1));
            PUT(FTRP(bp), PACK(asize, 1));
            // 뒤쪽 블록이 할당 블록
            return bp;
        }
    }
    // 분할 안하는 경우 //
    else
    {
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
        return bp;
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
        bp = place(bp, asize);
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
    bp = place(bp, asize);
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
/*
 * coalesce - bp 앞뒤 블록이 가용이면 병합한다.
 *   case 1: 앞(할당) 뒤(할당) -> 병합 없음
 *   case 2: 앞(할당) 뒤(가용) -> 뒤와 병합
 *   case 3: 앞(가용) 뒤(할당) -> 앞과 병합
 *   case 4: 앞(가용) 뒤(가용) -> 셋 다 병합
 *   병합 전 이웃은 remove_block으로 리스트에서 빼고,
 *   결과 블록을 insert_block으로 리스트 맨 앞에 넣는다.
 *   반환: 병합된 블록의 bp
 */
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
 * mm_check - 힙과 가용 리스트의 일관성 검사. 문제가 있으면 0 반환.
 *
 * [힙 검사]
 *  1. prologue 헤더가 DSIZE/할당 상태인가
 *  2. 모든 블록 bp가 8바이트 정렬인가
 *  3. header와 footer가 일치하는가
 *  4. 블록 크기 >= MINBLOCK 인가
 *  5. 연속된 가용 블록이 없는가 (coalesce 누락 탐지)
 *  6. 블록이 힙 범위 안에 있는가
 *  7. 블록끼리 겹치지 않는가
 *  8. epilogue가 0/1 인가
 * [리스트 검사]
 *  9.  리스트의 모든 블록이 가용 상태인가
 *  10. 리스트 포인터가 힙 범위 안을 가리키는가
 *  11. PRED/SUCC 양방향 연결이 일치하는가
 *  12. 첫 노드의 PRED가 NULL인가
 *  13. 힙의 가용 블록 수 == 리스트 노드 수 인가
 *  14. 리스트에 사이클이 없는가
 *
 *  사용법: #define DEBUG 켜면 CHECKHEAP()이 매 연산 후 호출됨
 */
/*
 * mm_check - 힙과 가용 리스트의 일관성 검사. 문제가 있으면 0 반환.
 *
 * [힙 검사] - 힙을 주소 순서대로 한 번 훑는다
 *  1. prologue 헤더가 DSIZE/할당 상태인가
 *  2. 모든 블록 bp가 8바이트 정렬인가
 *  3. 블록 크기가 8의 배수이고 MINBLOCK 이상인가
 *  4. header와 footer가 일치하는가
 *  5. 블록이 힙 범위 안에 있는가
 *  6. 연속된 가용 블록이 없는가 (coalesce 누락 탐지)
 *  7. epilogue가 0/1 인가
 *  8. 블록끼리 겹치지 않는가
 * [리스트 검사] - 12개 크기별 리스트를 각각 훑는다
 *  9.  리스트의 모든 블록이 가용 상태인가
 *  10. SUCC가 힙 범위 안을 가리키는가
 *  11. PRED/SUCC 양방향 연결이 일치하는가
 *  12. 리스트에 고리(사이클)가 없는가
 *  13. 각 리스트 첫 블록의 PRED가 NULL인가
 *  14. 힙의 가용 블록 수 == 전체 리스트 블록 수 인가
 *  15. 각 블록이 자기 크기에 맞는 리스트에 있는가
 *
 *  사용법: #define DEBUG 켜면 CHECKHEAP()이 매 연산 후 호출됨
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

    /* ===== ① 힙 순회: 모든 블록을 주소 순서대로 한 번 ===== */
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

    /* ===== ② 리스트 순회: 0번부터 11번 리스트까지 하나씩 ===== */
    for (int i = 0; i < LISTNUM; i++)
    {
        // 13) 각 리스트의 맨 앞 블록은 PRED가 NULL이어야 한다
        if (HEAD(i) != NULL && PRED(HEAD(i)) != NULL)
        {
            printf("[mm_check] %d번 리스트 맨 앞의 PRED가 NULL이 아님: %p\n", i, PRED(HEAD(i)));
            ok = 0;
        }

        /* i번 리스트 안의 블록을 SUCC로 하나씩 따라가며 검사 */
        for (bp = HEAD(i); bp != NULL; bp = SUCC(bp))
        {
            list_free++;

            // 9) 리스트에 있는 블록은 가용이어야 한다 (place의 remove 빠뜨림)
            if (GET_ALLOC(HDRP(bp)))
            {
                printf("[mm_check] 리스트에 할당 블록: bp=%p\n", bp);
                ok = 0;
            }

            // 10) SUCC가 NULL이 아니면 힙 안을 가리켜야 한다
            if (SUCC(bp) != NULL &&
                ((void *)SUCC(bp) < mem_heap_lo() || (void *)SUCC(bp) > mem_heap_hi()))
            {
                printf("[mm_check] SUCC가 힙 밖: bp=%p, SUCC=%p\n", bp, SUCC(bp));
                ok = 0;
                break; // 쓰레기 주소를 더 따라가면 세그폴트 → 이 리스트 검사 중단
            }

            // 11) 내 뒤 블록의 PRED는 나여야 한다
            if (SUCC(bp) != NULL && PRED(SUCC(bp)) != bp)
            {
                printf("[mm_check] 연결 오류: bp=%p, SUCC=%p, SUCC의 PRED=%p\n",
                       bp, SUCC(bp), PRED(SUCC(bp)));
                ok = 0;
            }

            // 12) 고리 방지: 힙의 빈 블록 수보다 많이 셌으면 같은 블록을 또 지난 것
            if (list_free > heap_free)
            {
                printf("[mm_check] 리스트에 고리가 있음 (list=%d, heap=%d)\n", list_free, heap_free);
                ok = 0;
                break; // 무한 루프 방지
            }

            // 15) 블록 크기로 구한 리스트 번호가 지금 리스트(i)와 같아야 한다
            if (get_class(GET_SIZE(HDRP(bp))) != i)
            {
                printf("[mm_check] 잘못된 리스트: bp=%p, 크기=%u, 있는 곳=%d, 있어야 할 곳=%d\n",
                       bp, GET_SIZE(HDRP(bp)), i, get_class(GET_SIZE(HDRP(bp))));
                ok = 0;
            }
        } // 안쪽 for 끝: i번 리스트 다 봄
    } // 바깥 for 끝: 12개 리스트 다 봄

    /* ===== ③ 마무리: 모든 for 밖 ===== */
    // 14) 힙에서 센 빈 블록 수 == 12개 리스트에서 센 블록 수
    if (heap_free != list_free)
    {
        printf("[mm_check] 빈 블록 수 불일치: 힙=%d, 리스트=%d\n", heap_free, list_free);
        ok = 0;
    }

    return ok;
}