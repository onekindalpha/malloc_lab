/*
 * mm.c - Segregated Free List (분리 가용 리스트) 기반 동적 메모리 할당기
 *
 * =================================================================================
 * 1. 블록 구조 (Block Structure)
 * =================================================================================
 * - 모든 블록은 8바이트 정렬(Alignment) 조건을 만족해야 합니다.
 * - 블록 헤더와 풋터의 하위 3비트는 플래그로 사용하며, bit 0은 할당 여부(1: 할당, 0: 가용)를 나타냅니다.
 *
 * [할당 블록 (Allocated Block)]
 *   +-------------------+------------------------------------+-------------------+
 *   |  Header (4Bytes)  |          Payload & Padding         |  Footer (4Bytes)  |
 *   |  size | alloc(1)  |                                    |  size | alloc(1)  |
 *   +-------------------+------------------------------------+-------------------+
 *   * bp (Block Pointer)는 Header 바로 다음(Payload 시작점)을 가리킵니다.
 *
 * [가용 블록 (Free Block)]
 *   +-------------------+-----------------+------------------+---------+-------------------+
 *   |  Header (4Bytes)  |   PRED (8Bytes) |   SUCC (8Bytes)  |  (빈칸)  |  Footer (4Bytes)  |
 *   |  size | alloc(0)  |  이전 가용 포인터 |  다음 가용 포인터  |         |  size | alloc(0)  |
 *   +-------------------+-----------------+------------------+---------+-------------------+
 *   * PRED/SUCC 포인터는 가용 블록의 Payload 영역 내에 저장됩니다. (구조체 free_block_t 활용)
 *   * 최소 블록 크기 MINBLOCK = Header(4B) + PRED(8B) + SUCC(8B) + Footer(4B) = 24Bytes
 *
 * =================================================================================
 * 2. 힙 구조 (Heap Layout)
 * =================================================================================
 * [seg_listp]
 *   │
 *   ├── HEAD(0)  ~ HEAD(11) : 크기 클래스별 가용 리스트 헤더 포인터 배열 (12 * 8B = 96B)
 *   │
 * [heap_listp] (seg_listp + 96B)
 *   │
 *   ├── Alignment Padding  (4Bytes) : 8바이트 정렬을 맞추기 위한 패딩 (값: 0)
 *   ├── Prologue Header    (4Bytes) : 힙 시작 경계 표시 (크기: 8B, alloc: 1)
 *   ├── Prologue Footer    (4Bytes) : 힙 시작 경계 표시 (크기: 8B, alloc: 1)
 *   ├── Epilogue Header    (4Bytes) : 힙 끝 경계 표시   (크기: 0B, alloc: 1)
 *   │
 *   └── [실제 할당/가용 데이터 블록들이 위치하는 공간]
 *
 * =================================================================================
 * 3. 가용 리스트 구성 및 관리 방식 (Segregated Free List)
 * =================================================================================
 * - 크기 범주(Class)별로 독립된 12개의 명시적 이중 연결 리스트(Explicit Doubly Linked List)를 운영합니다.
 * - get_class(size) 함수를 통해 블록 크기에 해당하는 클래스 인덱스(0 ~ 11)를 결정합니다.
 *   (예: <=32B: 0, <=64B: 1, <=128B: 2, ... , >32768B: 11)
 *
 * [가용 리스트 조작 방식]
 *  ① 삽입 (insert_block):
 *     - LIFO (Last-In-First-Out) 정책 적용.
 *     - 가용 블록을 해당 크기 클래스 리스트의 맨 앞(HEAD)에 연결합니다.
 *
 *  ② 삭제 (remove_block):
 *     - 블록이 할당되거나 병합(coalesce)될 때, PRED 및 SUCC 포인터 연결을 재구성하여
 *       해당 리스트에서 가용 블록을 즉시 제거합니다.
 *
 *  ③ 탐색 및 배치 (find_fit & place):
 *     - 요청된 크기(asize)의 클래스 인덱스부터 시작하여 상위 클래스 방향으로 탐색합니다.
 *     - 핏 정책: FIRST_FIT, NEXT_FIT, BEST_FIT 선택 가능 (기본값: FIRST_FIT).
 *     - 할당 시 분할(Splitting):
 *       - 잔여 공간이 MINBLOCK(24B) 이상이면 블록을 분할합니다.
 *       - 단편화 최적화: 요청 크기가 PLACE_THRESHOLD(64B) 미만이면 앞쪽 할당/뒤쪽 가용,
 *         이상이면 앞쪽 가용/뒤쪽 할당 방식으로 배치합니다.
 *
 *  ④ 병합 (coalesce):
 *     - mm_free 또는 extend_heap 시 인접한 앞/뒤 블록의 가용 여부(GET_ALLOC)를 확인합니다.
 *     - 인접한 가용 블록이 존재하면 remove_block으로 기존 가용 블록을 리스트에서 제거한 후,
 *       하나의 큰 블록으로 합쳐 새로운 가용 블록을 생성하고 insert_block으로 재삽입합니다.
 *
 *  ⑤ 재할당 (mm_realloc):
 *     - 현재 블록으로 크기 충족 시: 그대로 반환
 *     - 다음 인접 블록이 가용 블록이고 합쳐서 크기가 충족되는 경우: 제자리 확장 (In-place expansion)
 *     - 다음 블록이 에필로그인 경우: 힙을 필요한 만큼만 확장하여 제자리 확장
 *     - 위 조건 불가 시: mm_malloc -> memcpy -> mm_free 수행
 * =================================================================================
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

/* 보기 좋게 구조체 형태의 캐스팅 활용*/
typedef struct free_block
{
    // header 공간은 bp 바로 앞에 위치
    struct free_block *pred;
    struct free_block *succ;
} free_block_t;

typedef enum
{
    FIRST_FIT,
    NEXT_FIT,
    BEST_FIT
} fit_type_t;
/* 실행할 핏 정책 선택 */
static fit_type_t current_fit_mode = FIRST_FIT;
/* 리스트 개수 추가 - 크기별 리스트를 12개 두겠다. */
#define LISTNUM 16
/* 넥스트 핏을 위해 마지막으로 할당한 위치를 클래스별로 저장하는 배열 */
static free_block_t *rover[LISTNUM];
/* single word (4) or double word (8) alignment */
/* 정렬 기준을 8바이트로 정함.  */
#define ALIGNMENT 8
/* rounds up to the nearest multiple of ALIGNMENT */
/* 마지막 3비트를 0으로 만든다 -> 크기를 8의 배수로 맞춘다. 올린다.  */
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~0x7)
/* 기본 상수 */
#define WSIZE 4            // Word and header/footer size (bytes)
#define DSIZE 8            // Double word size (bytes)
#define CHUNKSIZE (1 << 8) // 힙 확장 기본 크기
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define PACK(size, alloc) ((size) | (alloc))                            // Pack a size and allocated bit into a word
#define GET(p) (*(unsigned int *)(p))                                   // Read a word at address p
#define PUT(p, val) (*(unsigned int *)(p) = (val))                      // Write a word at address p
#define GET_SIZE(p) (GET(p) & ~0x7)                                     // Read the size from address p
#define GET_ALLOC(p) (GET(p) & 0x1)                                     // Read the allocated bit from address p
#define HDRP(bp) ((char *)(bp) - WSIZE)                                 // Given block ptr bp, compute address of its header
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)            // Given block ptr bp, compute address of its footer
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE))) // Given block ptr bp, compute address of next block
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE))) // Given block ptr bp, compute address of previous block
/* 최소 블록 크기 24 */
#define MINBLOCK 24
#define PLACE_THRESHOLD 64
/* 크기 클래스별로 리스트 헤드가 여러 개 필요함. */
/* 가용 블록의 주소가 저장된 곳으로 가서 그 값을 꺼내라는 말. */
#define HEAD(i) (*(free_block_t **)(seg_listp + (i) * DSIZE))
/* 전역변수를 많이 쓰지 말라고 함. 아래 정도면 괜찮겠지 */
/* 실제 힙 블록들의 시작 주소/ 프롤로그 블록 */
static char *heap_listp;
/* 12개 리스트의 1등(Head) 주소록을 힙 맨 앞에 일렬로 모아둔 시작 위치 */
static char *seg_listp;

/* 함수 프로토타입 */
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
//     이 줄을 주석 처리하면 → DEBUG가 없는 상태

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
 *
 * [힙 초기화 메모리 배치 구조]
 * [0번지] ─── seg_listp 가 가리키는 곳
 *   │
 *   ├── HEAD(0)  (8B) ┐
 *   ├── HEAD(1)  (8B) │  <-- 12개 가용 리스트의 시작 포인터 보관함
 *   │   ...           │      (총 96바이트 = LISTNUM * DSIZE)
 *   └── HEAD(11) (8B) ┘
 *   │
 * [96번지] ── heap_listp 가 가리키는 곳 (seg_listp + 96)
 *   │
 *   ├── Alignment Padding (4B)  ┐
 *   ├── Prologue Header   (4B)  ├─ 힙 시작 및 끝 경계 표시용 기초 블록
 *   ├── Prologue Footer   (4B)  │  (총 16바이트 = 4 * WSIZE)
 *   └── Epilogue Header   (4B)  ┘
 *   │
 * [112번지] ── 실제 malloc으로 나눠줄 사용자 블록들이 생성되는 공간 시작!
 */
int mm_init(void)
{
    /* 1. 크기별 가용 리스트 헤더 배열(96B) + 힙 기초 블록(16B) 총 112B를 한 번에 할당 */
    if ((seg_listp = mem_sbrk(LISTNUM * DSIZE + 4 * WSIZE)) == (void *)-1)
        return -1;
    /* 2. 12개 리스트 헤더 초기화 (모두 빈 상태인 NULL로 설정) */
    for (int i = 0; i < LISTNUM; i++)
    {
        /* 12개 리스트 헤더를 널로 초기화한다. */
        HEAD(i) = NULL;
    }
    /* 3. heap_listp를 가용 리스트 배열 바로 뒤(96바이트 오프셋)로 지정 */
    heap_listp = seg_listp + (LISTNUM * DSIZE);
    /* 4. 기초 블록 값 채우기 (패딩, 프롤로그 헤더/풋터, 에필로그 헤더) */
    PUT(heap_listp, 0);                            // Alignment padding (96~99)
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1)); // Prologue header   (100~103)
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); // Prologue footer   (104~107)
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));     // Epilogue header   (108~111)
    /* 5. heap_listp 포인터를 프롤로그 블록 뒤(104번지)로 이동하여 기준점 설정 */
    heap_listp += (2 * WSIZE);
    /* 6. CHUNKSIZE만큼 힙을 확장하여 첫 번째 가용 블록 생성 */
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
    /* [정렬 상태 유지] 워드 개수를 짝수로 맞추어야 8의 배수 바이트가 된다. */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;
    /* system call (mem_sbrk) 호출 */
    /* 힙을 size만큼 늘린다. 실패하면 -1이 오므로 정수로 바꿔 비교 */
    if ((long)(bp = mem_sbrk(size)) == -1)
        return NULL;
    /* Free block header - 새 가용 블록 헤더 (옛 에필로그 자리)*/
    PUT(HDRP(bp), PACK(size, 0));
    /* Free block footer - 새 가용 블록 풋터 */
    PUT(FTRP(bp), PACK(size, 0));
    /* New epilogue header - 에필로그 헤더*/
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));
    /* 이전 가용a 블록과 즉시 연결하여 통합함 */
    return coalesce(bp);
}
/* 크기 클래스를 사용한 seglist를 이용하는 방식임.  */
/* get_class 함수 블록 크기가 속하는 크기 리스트 번호를 반환.*/
static int get_class(size_t size)
{
    if (size <= 24)
        return 0;
    if (size <= 32)
        return 1;
    if (size <= 48)
        return 2;
    if (size <= 64)
        return 3;
    if (size <= 96)
        return 4;
    if (size <= 128)
        return 5;
    if (size <= 192)
        return 6;
    if (size <= 256)
        return 7;
    if (size <= 512)
        return 8;
    if (size <= 1024)
        return 9;
    if (size <= 2048)
        return 10;
    if (size <= 4096)
        return 11;
    if (size <= 8192)
        return 12;
    if (size <= 16384)
        return 13;
    if (size <= 32768)
        return 14;
    return 15;
}
/* static find_first_fit */
static void *find_first_fit(size_t asize)
{
    /* First Fit Search - 조정된 요청크기 이상인 첫번째로 맞는 가용블록을 반환함. */
    for (int i = get_class(asize); i < LISTNUM; i++)
        /* 현재 헤드를 가리키고 있는 블록포인터로 전체 탐색을 진행한다. */
        for (free_block_t *fp = HEAD(i); fp != NULL; fp = fp->succ)
        {
            /* 조정된 요청 크기 이상의 블록포인터를 찾아서 반환한다. */
            if (asize <= GET_SIZE(HDRP(fp)))
            {
                /* 블록포인터를 반환한다. */
                return (void *)fp;
            }
        }
    /* 추가로 여기서 널을 반환하면 Mm_malloc에서 검사를 해서 알맞은 공간이 없으면 extend_heap을 한다. */
    return NULL;
}

/* find_best_fit 함수*/
static void *find_best_fit(size_t asize)
{
    /* best_bp는 널로 초기화한다. */
    char *best_bp = NULL;
    /* 무한대를 표현하는 방법은 양수 타입에서 1을 뺌.*/
    size_t min_extra = (size_t)-1;
    /* best Fit Search - 가용리스트 전체를 탐색하여 가장 작은 가용 블록을 선택한다. */
    for (int i = get_class(asize); i < LISTNUM; i++)
        /* 리스트를 탐색한다.*/
        for (free_block_t *fp = HEAD(i); fp != NULL; fp = fp->succ)
        {
            /* 조정된 요청 크기보다 블록 크기가 큰 가용 블록을 찾음 */
            if (asize <= GET_SIZE(HDRP(fp)))
            {
                /* 남는 크기 = 현재 블록 크기 - 조정된 요청 크기를 빼서 계산한다. */
                size_t remain = (GET_SIZE(HDRP(fp)) - asize);
                /* 남는 크기가 0이 되면 즉시 현재 블록포인터를 반환한다.
                 */
                if (remain == 0)
                    /* 즉시 현재 블록포인터를 반환한다. */
                    return (void *)fp;
                /* 가장 작은 크기의 블록을 찾기 위한 분기 */
                else if (remain < min_extra)
                {
                    /* 남는 크기를 작은 것으로 갱신한다. */
                    min_extra = remain;
                    /* 베스트 블록포인터의 주소를 갱신한다 */
                    best_bp = (void *)fp;
                }
            }
        }
    /* 가용리스트 순회를 다 도는 동안 즉시 반환은 못했지만,
    베스트 블록포인터가 NULL이 아니면 반환한다.*/
    if (best_bp != NULL)
        return best_bp;
    return NULL;
}

static void *find_fit(size_t asize)
{
    switch (current_fit_mode)
    {
    case FIRST_FIT:
        return find_first_fit(asize);
    // case NEXT_FIT:
    //     return find_next_fit(asize);
    case BEST_FIT:
        return find_best_fit(asize);
    default:
        return find_first_fit(asize);
    }
}

/* static seglist의 remove block */
/* 가용리스트에서 현재 블록을 제외한 가용리스트를 만든다. */
/* 내 앞 블록을 뒤에 연결하기 (앞블록이 없으면 가용리스트의 헤드) */
/* 내 뒤 블록을 앞에 연결하기 (뒤 블록이 없는 경우는 없으니까) */
static void remove_block(void *bp)
{
    /* 이 블록이 몇 번 리스트에 들어있는지 크기로 알아내겠다 */
    int i = get_class(GET_SIZE(HDRP(bp)));
    /* 1) void *bp를 구조체 포인터로 변환 */
    free_block_t *fp = (free_block_t *)bp;
    /* 2-1) 내 앞 블록이 있으면, 앞 블록의 뒤를 내 뒤 블록으로 연결 */
    if (fp->pred != NULL)
        fp->pred->succ = fp->succ;
    else
        /* 2-2) 내 앞 블록이 없으면, 가용리스트의 헤드를 내 뒤 블록으로 연결 */
        HEAD(i) = fp->succ;
    /* 3) 내 뒤 블록이 있으면, 내 뒤 블록의 이전을 내 앞 블록으로 연결 */
    if (fp->succ != NULL)
        fp->succ->pred = fp->pred;
}

/* static seglist의 insert block*/
/* 가용 리스트 맨 앞(HEAD)에 새로 찾은 가용 블록(bp)을 연결한다. */
static void insert_block(void *bp)
{
    /*이 블록의 헤더 크기를 보고 몇 번 리스트에 넣을 지 정한다. */
    int i = get_class(GET_SIZE(HDRP(bp)));
    /* void *bp를 구조체 포인터로 변환 */
    free_block_t *fp = (free_block_t *)bp;
    /* 현재 블록의 뒤를 가용리스트의 헤드라고 한다.  */
    fp->succ = HEAD(i);
    /* 현재 블록의 이전을 NULL이라고 한다. */
    fp->pred = NULL;
    /* 가용리스트의 헤드가 NULL이 아니면 */
    if (HEAD(i) != NULL)
    {
        /* 가용리스트의 헤드 이전을 현재 블록으로 연결한다. */
        HEAD(i)->pred = fp;
    }
    /* 가용리스트의 헤드를 현재 블록으로 한다. */
    HEAD(i) = fp;
}

/* static seglist의 place */
/* 가용 블록을 분할하거나 분할하지 않고 최종 배치해서 할당 주소를 리턴함. */
static void *place(void *bp, size_t asize)
{
    /* 현재 블록의 크기 */
    size_t csize = GET_SIZE(HDRP(bp));
    /* free요청을 받으면 -> 새 프리리스트 반환 전에 -> place와 coalesce를 하는 거니까 */
    /* place에서는 프리리스트에 insert를 하기 전에, 현재 프리할 블록을 가용 리스트에서 제외한다. */
    remove_block(bp);
    /* 현재 블록 크기에서 조정한 요청 크기를 뺐을때 최소블록 이상인 경우*/
    if ((csize - asize) >= (MINBLOCK))
    {
        /* 조정한 요청 크기가 임계치보다 작은 경우 */
        if (asize < PLACE_THRESHOLD)
        {
            /* [방식 A] 작은 요청: 앞쪽에 조정된 요청 크기만큼 할당, 뒤쪽 가용 조각 */
            /* 할당여부를 기록한다. */
            PUT(HDRP(bp), PACK(asize, 1));
            PUT(FTRP(bp), PACK(asize, 1));
            /* 포인터를 뒤쪽으로 옮긴다. */
            void *next_bp = NEXT_BLKP(bp);
            /* 뒤쪽 가용 블록 포인터를 통해 뒤쪽 가용 블록 조각에 대한
            헤더와 풋터를 설정한다. */
            PUT(HDRP(next_bp), PACK(csize - asize, 0));
            PUT(FTRP(next_bp), PACK(csize - asize, 0));
            /* 뒤쪽 가용 블록 포인터를 통해 프리리스트의 맨 앞에 연결한다.  */
            insert_block(next_bp);
            /* 앞쪽 할당 블록포인터를 반환한다. */
            return bp;
        }
        else
        {
            /* [방식 B] 큰 요청: 앞쪽 가용 조각, 뒤쪽 할당 */
            /* 현재 블록 크기에서 조정된 요청 크기를 뺀 것을 block pointer에 더하여, 할당 블록 포인터를 만든다.*/
            void *alloc_bp = (char *)bp + (csize - asize);
            /* 1. 앞쪽 가용 블록에 헤더와 풋터를 설정한다.*/
            /* 여기서 csize에서 csize - asize로 바뀌니까 위에서 미리 저장해둠. */
            PUT(HDRP(bp), PACK(csize - asize, 0));
            PUT(FTRP(bp), PACK(csize - asize, 0));
            /* 2. 뒤쪽 할당 블록에 헤더와 풋터를 설정한다.  */
            /* 할당 여부를 기록한다. */
            PUT(HDRP(alloc_bp), PACK(asize, 1));
            PUT(FTRP(alloc_bp), PACK(asize, 1));
            /* 앞쪽 가용 블록을 프리리스트 맨 처음에 연결한다.  */
            insert_block(bp);
            /* 뒤쪽 할당 블록 포인터를 반환한다. */
            return alloc_bp;
        }
    }
    /* 최소 블록 크기보다 작아 분할 안하는 경우에는 현재 블록 크기를 할당함.*/
    else
    {
        /* 현재 블록 크기를 헤더와 풋터에 설정한다. */
        /* 할당 여부를 기록한다. */
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
        /* 할당 블록 포인터를 반환한다. */
        return bp;
    }
}
/* 조정된 요청 크기 만드는 부분을 헬퍼함수로 만듦. */
static size_t adjust_size(size_t size)
{
    size_t asize;
    /* 사이즈가 8바이트 이하이면 */
    if (size <= DSIZE)
        /* 조정된 요청 크기를 24바이트로 한다. */
        return MINBLOCK;
    /* 조정된 요청 크기를 계산한다. */
    return DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);
}
/*
 * mm_malloc - Allocate a block by incrementing the brk pointer.
 *     Always allocate a block whose size is a multiple of the alignment.
 */
#if 1
void *mm_malloc(size_t size)
{
    /* 만약 조정된 요청 크기 블록 사이즈에 맞는 가용 블록을 찾지 못햇을 경우 익스텐드 힙을 한다. */
    size_t extendsize;
    /* 선언만 하고 뒤에서 초기화할 수 있음. */
    char *bp;
    /* size가 0일때 NULL을 반환한다.(이상한 요청) */
    if (size == 0)
        return NULL;
    size_t asize = adjust_size(size);
    /* find_fit함수로 조정된 요청 크기에 맞는 가용 블록의 시작주소를 반환하는 조건문 */
    if ((bp = find_fit(asize)) != NULL)
    {
        /* 조정된 요청 크기에 맞는 가용 블록의 시작주소에 배치를 진행한다. 분할을 하거나 하지 않고 배치를 하여 최종 확정된 가용 블록 주소를 반환한다. */
        bp = place(bp, asize);
        CHECKHEAP();
        /* 최종 배치된 가용 블록의 시작 주소를 반환한다. */
        return bp;
    }
    /* 만일 find_fit함수로 조정된 요청 크기에 맞는 가용 블록을 찾지 못한다면 Extendheap을 진행하기 전,
    extendsize를 계산한다 (조정된 요청 크기와 청크사이즈 둘중에 최대값으로 한다) */
    extendsize = MAX(asize, CHUNKSIZE);
    /* extendsize를 워드사이즈로 바꾸어, extend_heap을 진행하여 블록포인터의 시작주소가 NULL이 되면 NULL을 반환한다. */
    bp = extend_heap(extendsize / WSIZE);
    if (bp == NULL)
        return NULL;
    /* 새 힙공간의 bp 위치에 asize만큼 할당 및 분할 처리  */
    bp = place(bp, asize);
    CHECKHEAP();
    /* 할당된 메모리 주소 반환 */
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
    /* 현재 블록포인터의 헤드를 통해 사이즈를 찾는다. */
    size_t size = GET_SIZE(HDRP(ptr));
    /* 헤더에 사이즈와 할당여부를 0으로 기록한다. */
    PUT(HDRP(ptr), PACK(size, 0));
    PUT(FTRP(ptr), PACK(size, 0));
    /* 방금 프리된 블록을 이웃 가용 블록과 병합하고, 최종 확정된 블록을 가용 리스트에 추가한다. */
    coalesce(ptr);
    CHECKHEAP();
}

/* 헬퍼 함수 */
/* end 블록의 풋터 부터 start 블록의 헤더까지 바이트 크기를 계산하고
할당여부도 기록을 하여 시작 블록 포인터를 반환하는 함수 */
static void *combines_blocks(void *start_ptr, void *end_ptr, int alloc_flag)
{
    /* 1. 마지막 블록 포인터의 풋터 끝점부터 시작 블록 포인터의 헤더까지 전체 바이트 크기 계산 */
    size_t total_size = (char *)FTRP(end_ptr) + WSIZE - (char *)HDRP(start_ptr);
    /* 2. 시작 블록 포인터와 마지막 블록 포인터를 기록 */
    PUT(HDRP(start_ptr), PACK(total_size, alloc_flag));
    PUT(FTRP(end_ptr), PACK(total_size, alloc_flag));
    /* 3. 시작 블록 포인터를 반환한다. */
    return (void *)start_ptr;
}

/*
 * coalesce - bp 앞뒤 블록이 가용이면 병합한다.
 *   case 1: 앞(할당) 뒤(할당) -> 병합 없음
 *   case 2: 앞(할당) 뒤(가용) -> 뒤와 병합
 *   case 3: 앞(가용) 뒤(할당) -> 앞과 병합
 *   case 4: 앞(가용) 뒤(가용) -> 셋 다 병합
 *   coalesce(병합 내지 연결)을 하기 전에 미리 remove_block으로 기존 블록에 대해 가용리스트에서 제거를 미리한다.
 *   합쳐진 가용 블록은 insert_block으로 가용 리스트 맨 처음에 넣는다.
 */
void *coalesce(void *ptr)
{
    /* 이전 할당 여부 - 경계 태그 방식에서 이전 블록의 할당여부는 풋터에서 가져온다. */
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(ptr)));
    /* 다음 할당 여부 - 다음 힙 리스트 메모리 주소를 가리키는 포인터를 헤더를 가리키게 하여 할당여부를 알아낸다. */
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(ptr)));
    /* Case1 : 이전과 다음 모두 할당 블록이면*/
    if (prev_alloc && next_alloc)
    {
    }
    /* Case 2: 다음 블록만 가용 블록 상태 */
    else if (prev_alloc && !next_alloc)
    {
        /* (병합을 하면 시작 포인터가 바뀐다)
        다음 블록 포인터가 제거된 가용리스트를 만든다.*/
        remove_block(NEXT_BLKP(ptr));
        /* 현재 블록포인터부터 다음 블록포인터까지를 합친 (사이즈와 할당여부) 블록포인터의 시작주소를 반환한다. */
        ptr = combines_blocks(ptr, NEXT_BLKP(ptr), 0);
    }
    /* Case 3: 이전 블록만 가용 블록 상태 */
    else if (!prev_alloc && next_alloc) // Case 3
    {
        /* (병합을 하면 시작 포인터가 바뀐다.)  */
        remove_block(PREV_BLKP(ptr));
        /* 이전 블록포인터부터 현재 블록포인터를 합친 (사이즈와 할당여부) 블록포인터의 시작주로를 반환한다.*/
        ptr = combines_blocks(PREV_BLKP(ptr), ptr, 0);
    }
    /* 이전 블록과 다음 블록 모두 가용 블록인 경우 */
    else
    {
        /* (병합을 하면 시작 포인터가 바뀐다.)
        이전 블록 포인터를 가용리스트에서 제거한다.*/
        remove_block(PREV_BLKP(ptr));
        /* (병합을 하면 시작 포인터가 바뀐다.)
        다음 블록 포인터를 가용리스트에서 제거한다.*/
        remove_block(NEXT_BLKP(ptr));
        /* 이전블록포인터부터 다음 블록포인터까지를 합친 (사이즈와 할당여부) 블록포인터의 시작주소를 반환한다. */
        ptr = combines_blocks(PREV_BLKP(ptr), NEXT_BLKP(ptr), 0);
    }
    /* 합쳐진 가용 블록을 가용 리스트의 맨 앞에 삽입한다. */
    insert_block(ptr);
    /* 블록 포인터의 시작주소를 반환한다. */
    return ptr;
}
/*
 * mm_realloc - Implemented simply in terms of mm_malloc and mm_free
 */
void *mm_realloc(void *ptr, size_t size)
{
    /* 만약 기존 포인터 주소가 NULL이면 말록을 새로 할당한다. */
    if (ptr == NULL)
        return mm_malloc(size);
    /* 사이즈가 0인 잘못된 요청이 있으면 */
    if (size == 0)
    {
        /* 해당 포인터에 대해 프리를 한다. */
        mm_free(ptr);
        /* NULL로 해제 후 무효화를 한다.  */
        return NULL;
    }
    /* 조정된 요청 크기를 구한다. */
    size_t asize = adjust_size(size);
    /* 지금 블록 전체 크기를 구한다. */
    size_t oldblock = GET_SIZE(HDRP(ptr));
    /* 다음 포인터 변수 */
    void *next = NEXT_BLKP(ptr);
    /* 1. 현재 블록의 전체 크기가 조정된 요청 크기 이상인 경우 현재 포인터를 반환한다. */
    if (oldblock >= asize)
        return ptr;
    /* [다음이 에필로그인 경우] */
    if (GET_SIZE(HDRP(next)) == 0)
    {
        /* 조정된 요청 크기에서 지금 블록의 전체 크기를 뺀 것을 필요 크기라고 한다.*/
        size_t need = asize - oldblock;
        /* 필요 크기가 최소 요청 크기보다 작은 경우 필요크기를 최소 블록크기로 한다. */
        if (need < MINBLOCK)
            need = MINBLOCK;
        /* 필요 크기를 워드사이즈로 변환하여 익스텐드힙을 하여 힙을 할당하지 못한 경우 널을 반환한다. */
        if (extend_heap(need / WSIZE) == NULL)
            return NULL;
        /* 새로 생긴 빈 블록 공간에 대해 포인터를 갱신한다. */
        next = NEXT_BLKP(ptr);
    }
    /* 다음 블록 포인터가 가리키는 헤더를 통해 nextblock을 구한다. */
    size_t nextblock = GET_SIZE(HDRP(next));
    size_t totalsize = oldblock + nextblock;
    size_t remainsize = totalsize - asize;
    /* [제자리 확장을 할 수 있는 경우를 확인] */
    if (!GET_ALLOC(HDRP(next)) && totalsize >= asize)
    {
        /* 뒤 블록을 리스트에서 뺀다  */
        remove_block(next);
        /* 뒤 블록을 통째로 흡수한다.*/
        PUT(HDRP(ptr), PACK(totalsize, 1));
        PUT(FTRP(ptr), PACK(totalsize, 1));
        CHECKHEAP();
        return ptr;
    }
    /* [제자리 확장으로 안 되는 경우: 새 블록 + 복사 + 반납] */
    void *newptr = mm_malloc(size);
    /* 말록을 통해 새로운 포인터를 반환받지 못한 경우 NULL을 반환한다.  */
    if (newptr == NULL)
        return NULL;
    /* 지금 블록 크기에서 8(헤더와 풋터)를 제외한 사이즈를 구한다.*/
    size_t oldsize = oldblock - DSIZE;
    /* 복사 사이즈는 사용자 요청 크기보다 지금 블록 크기가 큰지를 보고, 지금 블록 크기가 더 큰 경우 사용자 요청 크기를 카피 사이즈로 하고, 사용자 요청 크기가 더 큰 경우 지금 블록 크기를 복사 사이즈로 한다. */
    size_t copysize = (size < oldsize) ? size : oldsize;
    /* 기존 포인터에서 새로운 포인터로 복사 사이즈를 통해 복사시킨다. */
    memcpy(newptr, ptr, copysize);
    /* 기존 포인터에 대해 프리를 한다. */
    mm_free(ptr);
    CHECKHEAP();
    /* 새로운 포인터를 반환한다. */
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

    /* 힙 순회: 모든 블록을 주소 순서대로 한 번  */
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

    /* 리스트 순회: 0번부터 11번 리스트까지 하나씩 */
    for (int i = 0; i < LISTNUM; i++)
    {
        // 13) 각 리스트의 맨 앞 블록은 PRED가 NULL이어야 한다
        if (HEAD(i) != NULL && HEAD(i)->pred != NULL)
        {
            printf("[mm_check] %d번 리스트 맨 앞의 PRED가 NULL이 아님: %p\n", i, HEAD(i)->pred);
            ok = 0;
        }
        /* i번 리스트 안의 블록을 SUCC로 하나씩 따라가며 검사 */
        for (free_block_t *fp = HEAD(i); fp != NULL; fp = fp->succ)
        {
            list_free++;
            // 9) 리스트에 있는 블록은 가용이어야 한다 (place의 remove 빠뜨림)
            if (GET_ALLOC(HDRP(fp)))
            {
                printf("[mm_check] 리스트에 할당 블록: bp=%p\n", fp);
                ok = 0;
            }

            // 10) SUCC가 NULL이 아니면 힙 안을 가리켜야 한다
            if (fp->succ != NULL &&
                ((void *)fp->succ < mem_heap_lo() || (void *)fp->succ > mem_heap_hi()))
            {
                printf("[mm_check] SUCC가 힙 밖: bp=%p, SUCC=%p\n", fp, fp->succ);
                ok = 0;
                break; // 쓰레기 주소를 더 따라가면 세그폴트 → 이 리스트 검사 중단
            }

            // 11) 내 뒤 블록의 PRED는 나여야 한다
            if (fp->succ != NULL && fp->succ->pred != fp)
            {
                printf("[mm_check] 연결 오류: bp=%p, SUCC=%p, SUCC의 PRED=%p\n",
                       fp, fp->succ, fp->succ->pred);
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
            if (get_class(GET_SIZE(HDRP(fp))) != i)
            {
                printf("[mm_check] 잘못된 리스트: bp=%p, 크기=%u, 있는 곳=%d, 있어야 할 곳=%d\n",
                       fp, GET_SIZE(HDRP(fp)), i, get_class(GET_SIZE(HDRP(fp))));
                ok = 0;
            }
        } // 안쪽 for 끝: i번 리스트 다 봄
    } // 바깥 for 끝: 12개 리스트 다 봄

    /*  마무리: 모든 for 밖 */
    // 14) 힙에서 센 빈 블록 수 == 12개 리스트에서 센 블록 수
    if (heap_free != list_free)
    {
        printf("[mm_check] 빈 블록 수 불일치: 힙=%d, 리스트=%d\n", heap_free, list_free);
        ok = 0;
    }
    return ok;
}
