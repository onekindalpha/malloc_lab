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
 *   ├── HEAD(0)  ~ HEAD(15) : 크기 클래스별 가용 리스트의 시작 주소 칸 (16 * 8B = 128B)
 *   │                         전역 배열이 금지라서 배열 변수가 아니라 힙 맨 앞의 칸으로 둔다.
 *   │
 * [heap_listp] (seg_listp + 128B)
 *   │
 *   ├── Alignment Padding  (4Bytes) : 8바이트 정렬을 맞추기 위한 패딩 (값: 0)
 *   ├── Prologue Header    (4Bytes) : 힙 시작 경계 표시 (크기: 8B, alloc: 1)
 *   ├── Prologue Footer    (4Bytes) : 힙 시작 경계 표시 (크기: 8B, alloc: 1)
 *   ├── Epilogue Header    (4Bytes) : 힙 끝 경계 표시   (크기: 0B, alloc: 1)
 *   │
 *   └── [실제 할당/가용 데이터 블록들이 위치하는 공간]
 *
 * - seg_listp, heap_listp는 함수 밖의 static 전역 포인터라 데이터 영역에 있다.
 *   (배열이 아닌 포인터 변수 하나라 과제 규칙상 허용)
 *
 * =================================================================================
 * 3. 가용 리스트 구성 및 관리 방식 (Segregated Free List)
 * =================================================================================
 * - 크기 범주(Class)별로 독립된 16개의 명시적 이중 연결 리스트(Explicit Doubly Linked List)를 운영합니다.
 * - get_class(size) 함수를 통해 블록 크기에 해당하는 클래스 인덱스(0 ~ 15)를 결정합니다.
 *   (예: <=24B: 0, <=32B: 1, <=48B: 2, <=64B: 3, <=96B: 4, <=128B: 5, ... , >32768B: 15)
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
 *     - 핏 정책: FIRST_FIT, BEST_FIT 선택 가능 (기본값: FIRST_FIT).
 *       NEXT_FIT은 실험해 보았으나 크기별 리스트에서 이점이 없어 제외했습니다.
 *     - 할당 시 분할(Splitting):
 *       - 잔여 공간이 MINBLOCK(24B) 이상이면 블록을 분할합니다.
 *       - 단편화 최적화: 요청 크기가 PLACE_THRESHOLD(64B) 미만이면 앞쪽 할당/뒤쪽 가용,
 *         이상이면 앞쪽 가용/뒤쪽 할당 방식으로 배치합니다.
 *         (남는 빈 조각을 앞에 두어, 큰 블록끼리 힙 끝 쪽에 모이게 한다 → 외부 단편화 감소)
 *
 *  ④ 병합 (coalesce):
 *     - mm_free 또는 extend_heap 시 인접한 앞/뒤 블록의 가용 여부(GET_ALLOC)를 확인합니다.
 *     - 인접한 가용 블록이 존재하면 remove_block으로 기존 가용 블록을 리스트에서 제거한 후,
 *       하나의 큰 블록으로 합쳐 새로운 가용 블록을 생성하고 insert_block으로 재삽입합니다.
 *
 *  ⑤ 할당 크기와 힙 확장 (mm_malloc):
 *     - 요청 크기가 256B 이상이고 2^k의 7/8 이상이면 2^k로 올려 받는다.
 *       (나중에 조금 더 큰 요청이 와도 그 자리를 재사용할 수 있게)
 *     - 작은 요청(64B 미만) 때문에 힙을 늘릴 때는 4KB를 한 번에 받아,
 *       작은 블록끼리 모이고 큰 블록 사이에 끼지 않게 한다.
 *
 *  ⑥ 재할당 (mm_realloc):
 *     - 현재 블록으로 크기 충족 시: 그대로 반환
 *     - 다음 블록이 에필로그인 경우: 힙을 필요한 만큼만 확장하여 제자리 확장
 *     - 다음 인접 블록이 가용 블록이고 합쳐서 크기가 충족되는 경우: 분할 없이 통째로 흡수 (In-place expansion)
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
    // NEXT_FIT,
    BEST_FIT
} fit_type_t;
/* 실행할 핏 정책 선택 */
static fit_type_t current_fit_mode = BEST_FIT;
/* 리스트 개수 - 크기별 리스트를 16개 두겠다. */
#define LISTNUM 8
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
/* place에서 앞/뒤 배치를 가르는 기준 (이 크기 미만이면 앞쪽에 할당) */
#define PLACE_THRESHOLD 170
/* 작은 요청 때문에 힙을 늘릴 때 한 번에 받을 크기 */
#define SMALL_CHUNKSIZE (1 << 12)
/* 크기 클래스별로 리스트 헤드가 여러 개 필요함. */
/* 가용 블록의 주소가 저장된 곳으로 가서 그 값을 꺼내라는 말. */
#define HEAD(i) (*(free_block_t **)(seg_listp + (i) * DSIZE))
/* 전역 포인터 두 개 (함수 밖 static → 데이터 영역, 배열이 아니라 허용) */
/* 실제 힙 블록들의 시작 주소/ 프롤로그 블록 */
static char *heap_listp;
/* 16개 리스트의 시작 주소 칸(HEAD)을 힙 맨 앞에 일렬로 모아둔 시작 위치 */
static char *seg_listp;

/* 함수 프로토타입 */
static void *extend_heap(size_t words);
static void *find_fit(size_t asize);
static void *coalesce(void *bp);
static void *place(void *bp, size_t asize);
static void remove_block(void *bp);
static void insert_block(void *bp);
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
 * mm_init - 힙을 처음 만들고 HEAD 칸, 프롤로그, 에필로그를 세운 뒤 첫 가용 블록을 만든다.
 *
 * [힙 초기화 메모리 배치 구조]
 * [0번지] ─── seg_listp 가 가리키는 곳
 *   │
 *   ├── HEAD(0)  (8B) ┐
 *   ├── HEAD(1)  (8B) │  <-- 16개 가용 리스트의 시작 포인터 보관함
 *   │   ...           │      (총 128바이트 = LISTNUM * DSIZE)
 *   └── HEAD(15) (8B) ┘
 *   │
 * [128번지] ── heap_listp 가 처음 가리키는 곳 (seg_listp + 128)
 *   │
 *   ├── Alignment Padding (4B)  ┐
 *   ├── Prologue Header   (4B)  ├─ 힙 시작 및 끝 경계 표시용 기초 블록
 *   ├── Prologue Footer   (4B)  │  (총 16바이트 = 4 * WSIZE)
 *   └── Epilogue Header   (4B)  ┘
 *   │
 * [144번지] ── 실제 malloc으로 나눠줄 사용자 블록들이 생성되는 공간 시작!
 */
int mm_init(void)
{
    /* 1. 크기별 가용 리스트 HEAD 칸(128B) + 힙 기초 블록(16B) 총 144B를 한 번에 할당 */
    if ((seg_listp = mem_sbrk(LISTNUM * DSIZE + 4 * WSIZE)) == (void *)-1)
        return -1;
    /* 2. 16개 리스트 HEAD 초기화 (모두 빈 상태인 NULL로 설정)
       extend_heap 안에서 insert_block이 불리므로 반드시 그 전에 해야 한다. */
    for (int i = 0; i < LISTNUM; i++)
    {
        HEAD(i) = NULL;
    }
    /* 3. heap_listp를 HEAD 칸 바로 뒤(128바이트 오프셋)로 지정 */
    heap_listp = seg_listp + (LISTNUM * DSIZE);
    /* 4. 기초 블록 값 채우기 (패딩, 프롤로그 헤더/풋터, 에필로그 헤더) */
    PUT(heap_listp, 0);                            // Alignment padding (128~131)
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1)); // Prologue header   (132~135)
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); // Prologue footer   (136~139)
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));     // Epilogue header   (140~143)
    /* 5. heap_listp 포인터를 프롤로그 헤더 뒤(136번지)로 이동하여 기준점 설정 */
    heap_listp += (2 * WSIZE);
    /* 6. CHUNKSIZE만큼 힙을 확장하여 첫 번째 가용 블록 생성 */
    if (extend_heap(48 / WSIZE) == NULL)
        return -1;
    CHECKHEAP();
    return 0;
}

/*
 * extend_heap - 힙을 words 워드만큼 늘려 새 가용 블록을 만든다.
 * 옛 에필로그 자리에 새 블록 헤더를 쓰고, 끝에 에필로그를 다시 세운 뒤
 * 앞 블록이 가용이면 합친다. 반환값: 새(또는 합쳐진) 가용 블록의 bp
 */
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;
    /* [정렬 상태 유지] 워드 개수를 짝수로 맞추어야 8의 배수 바이트가 된다. */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;
    /* 힙을 size만큼 늘린다. 실패하면 -1이 오므로 정수로 바꿔 비교 */
    if ((long)(bp = mem_sbrk(size)) == -1)
        return NULL;
    /* Free block header - 새 가용 블록 헤더 (옛 에필로그 자리)*/
    PUT(HDRP(bp), PACK(size, 0));
    /* Free block footer - 새 가용 블록 풋터 */
    PUT(FTRP(bp), PACK(size, 0));
    /* New epilogue header - 에필로그 헤더*/
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));
    /* 이전 가용 블록과 즉시 연결하여 통합함 */
    return coalesce(bp);
}

/*
 * get_class - 블록 크기가 속하는 크기 리스트 번호(0~15)를 반환한다.
 * 작은 크기는 촘촘하게(24, 32, 48, 64 ...), 큰 크기는 2배씩 나눈다.
 */
static int get_class_raw(size_t size);
/* get_class - 크기 클래스 번호. 256B보다 큰 블록은 마지막 리스트(7번) 하나에 모은다. */
static int get_class(size_t size)
{
    int c = get_class_raw(size);
    return c < LISTNUM ? c : LISTNUM - 1;
}
static int get_class_raw(size_t size)
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

/*
 * find_first_fit - asize가 속한 클래스부터 위로 올라가며,
 * 각 리스트에서 처음 만나는 충분히 큰 가용 블록을 반환한다. 없으면 NULL.
 */
static void *find_first_fit(size_t asize)
{
    for (int i = get_class(asize); i < LISTNUM; i++)
        /* 현재 HEAD부터 SUCC를 따라 리스트를 탐색한다. */
        for (free_block_t *fp = HEAD(i); fp != NULL; fp = fp->succ)
        {
            /* 조정된 요청 크기 이상의 블록포인터를 찾아서 반환한다. */
            if (asize <= GET_SIZE(HDRP(fp)))
            {
                return (void *)fp;
            }
        }
    /* 여기서 NULL을 반환하면 mm_malloc에서 extend_heap을 한다. */
    return NULL;
}

/*
 * find_best_fit - asize가 속한 클래스부터 끝까지 훑어, 남는 크기가 가장 작은 블록을 반환한다.
 * 딱 맞는 블록(남는 크기 0)을 만나면 바로 반환한다. 없으면 NULL.
 */
static void *find_best_fit(size_t asize)
{
    /* best_bp는 널로 초기화한다. */
    char *best_bp = NULL;
    /* 무한대를 표현하는 방법은 양수 타입에서 1을 뺌.*/
    size_t min_extra = (size_t)-1;
    for (int i = get_class(asize); i < LISTNUM; i++)
        for (free_block_t *fp = HEAD(i); fp != NULL; fp = fp->succ)
        {
            /* 조정된 요청 크기보다 블록 크기가 큰 가용 블록을 찾음 */
            if (asize <= GET_SIZE(HDRP(fp)))
            {
                /* 남는 크기 = 현재 블록 크기 - 조정된 요청 크기 */
                size_t remain = (GET_SIZE(HDRP(fp)) - asize);
                /* 남는 크기가 0이면 즉시 현재 블록포인터를 반환한다. */
                if (remain == 0)
                    return (void *)fp;
                /* 가장 작은 크기의 블록을 찾기 위한 분기 */
                else if (remain < min_extra)
                {
                    min_extra = remain;
                    best_bp = (void *)fp;
                }
            }
        }
    /* 즉시 반환은 못했지만 후보가 있으면 반환한다. */
    if (best_bp != NULL)
        return best_bp;
    return NULL;
}

/* find_fit - current_fit_mode에 따라 first fit 또는 best fit으로 탐색한다. */
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

/*
 * remove_block - 가용 블록 bp를 자기 클래스 리스트에서 뺀다.
 * 내 앞 블록과 내 뒤 블록을 서로 이어 준다 (앞이 없으면 HEAD를 내 뒤로).
 */
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

/*
 * insert_block - 가용 블록 bp를 자기 클래스 리스트 맨 앞(HEAD)에 넣는다 (LIFO).
 * 옛 첫 블록 주소를 잃지 않도록 HEAD는 마지막에 바꾼다.
 */
static void insert_block(void *bp)
{
    /* 이 블록의 헤더 크기를 보고 몇 번 리스트에 넣을 지 정한다. */
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

/*
 * place - 가용 블록 bp에 asize 바이트를 배치하고, 실제로 할당된 블록의 주소를 반환한다.
 * 남는 크기가 MINBLOCK 이상이면 분할한다.
 *   - asize < PLACE_THRESHOLD: 앞쪽에 할당하고 뒤 조각은 가용 리스트에 넣는다. (방식 A)
 *   - 그 외: 앞 조각은 가용으로 남기고 뒤쪽에 할당한다. 할당 주소가 bp와 달라진다. (방식 B)
 * 남는 크기가 MINBLOCK보다 작으면 분할하지 않고 블록 전체를 할당한다.
 * 호출자는 bp가 아니라 반환값을 써야 한다.
 */
static void *place(void *bp, size_t asize)
{
    /* 현재 블록의 크기 */
    size_t csize = GET_SIZE(HDRP(bp));
    /* 이 블록은 곧 할당되므로 먼저 가용 리스트에서 뺀다. */
    remove_block(bp);
    /* 현재 블록 크기에서 조정한 요청 크기를 뺐을때 최소블록 이상인 경우*/
    if ((csize - asize) >= (MINBLOCK))
    {
        /* 조정한 요청 크기가 임계치보다 작은 경우 */
        if (asize < PLACE_THRESHOLD)
        {
            /* [방식 A] 작은 요청: 앞쪽에 조정된 요청 크기만큼 할당, 뒤쪽 가용 조각 */
            PUT(HDRP(bp), PACK(asize, 1));
            PUT(FTRP(bp), PACK(asize, 1));
            /* 포인터를 뒤쪽으로 옮긴다. */
            void *next_bp = NEXT_BLKP(bp);
            /* 뒤쪽 가용 조각의 헤더와 풋터를 설정한다. */
            PUT(HDRP(next_bp), PACK(csize - asize, 0));
            PUT(FTRP(next_bp), PACK(csize - asize, 0));
            /* 뒤쪽 가용 조각을 프리리스트의 맨 앞에 연결한다.  */
            insert_block(next_bp);
            /* 앞쪽 할당 블록포인터를 반환한다. */
            return bp;
        }
        else
        {
            /* [방식 B] 큰 요청: 앞쪽 가용 조각, 뒤쪽 할당 */
            /* bp에서 (csize - asize)만큼 뒤가 할당 블록의 시작 */
            void *alloc_bp = (char *)bp + (csize - asize);
            /* 1. 앞쪽 가용 조각에 헤더와 풋터를 설정한다.
               (csize는 위에서 미리 저장해 두었다) */
            PUT(HDRP(bp), PACK(csize - asize, 0));
            PUT(FTRP(bp), PACK(csize - asize, 0));
            /* 2. 뒤쪽 할당 블록에 헤더와 풋터를 설정한다.  */
            PUT(HDRP(alloc_bp), PACK(asize, 1));
            PUT(FTRP(alloc_bp), PACK(asize, 1));
            /* 앞쪽 가용 조각을 프리리스트 맨 처음에 연결한다.  */
            insert_block(bp);
            /* 뒤쪽 할당 블록 포인터를 반환한다. */
            return alloc_bp;
        }
    }
    /* 최소 블록 크기보다 작아 분할 안하는 경우에는 현재 블록 크기를 할당함.*/
    else
    {
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
        return bp;
    }
}

/*
 * adjust_size - 사용자 요청 크기를 블록 크기(asize)로 바꾼다.
 * 헤더·풋터 8B를 더하고 8의 배수로 올린다. 최소 MINBLOCK(24B).
 */
static size_t adjust_size(size_t size)
{
    if (size <= DSIZE)
        return MINBLOCK;
    return DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);
}

/*
 * mm_malloc - size 바이트를 담을 블록을 할당하고 payload 주소를 반환한다.
 * 1) 큰 요청은 2^k 근처면 2^k로 올린다. 2) asize를 계산한다.
 * 3) find_fit으로 가용 블록을 찾으면 place로 배치한다.
 * 4) 없으면 힙을 늘린다 (작은 요청은 4KB를 한 번에) → place.
 */
void *mm_malloc(size_t size)
{
    size_t extendsize;
    char *bp;
    /* size가 0일때 NULL을 반환한다.(이상한 요청) */
    if (size == 0)
        return NULL;
    /* 요청 크기가 256B 이상이고 2^k의 7/8 이상이면 2^k로 올려 받는다. */
    {
        size_t p = 8;
        while (p < size)
            p <<= 1;
        if (size >= 100 && size < 1024 && size * 8 >= p * 7)
            size = p;
    }
    size_t asize = adjust_size(size);
    /* 조정된 요청 크기에 맞는 가용 블록을 찾으면 그 자리에 배치한다. */
    if ((bp = find_fit(asize)) != NULL)
    {
        /* 분할을 하거나 하지 않고 배치하여 최종 할당 주소를 받는다. */
        bp = place(bp, asize);
        CHECKHEAP();
        return bp;
    }
    /* 맞는 블록이 없으면 힙을 늘린다.
       작은 요청은 4KB를 한 번에 받아 작은 블록끼리 모이게 하고 (큰 블록 사이에 끼지 않도록),
       큰 요청은 asize와 CHUNKSIZE 중 큰 쪽만큼 받는다. */
    extendsize = (asize < 80) ? MAX(asize, SMALL_CHUNKSIZE)
                              : MAX(asize, CHUNKSIZE);
    bp = extend_heap(extendsize / WSIZE);
    if (bp == NULL)
        return NULL;
    /* 새 힙공간의 bp 위치에 asize만큼 할당 및 분할 처리  */
    bp = place(bp, asize);
    CHECKHEAP();
    return bp;
}

/*
 * mm_free - 블록을 가용으로 표시하고, 이웃 가용 블록과 병합한 뒤 리스트에 넣는다.
 */
void mm_free(void *ptr)
{
    /* 현재 블록포인터의 헤더를 통해 사이즈를 찾는다. */
    size_t size = GET_SIZE(HDRP(ptr));
    /* 헤더와 풋터의 할당여부를 0으로 기록한다. */
    PUT(HDRP(ptr), PACK(size, 0));
    PUT(FTRP(ptr), PACK(size, 0));
    /* 이웃 가용 블록과 병합하고, 최종 블록을 가용 리스트에 추가한다. */
    coalesce(ptr);
    CHECKHEAP();
}

/*
 * combines_blocks - start 블록 헤더부터 end 블록 풋터까지를 하나의 블록으로 기록한다.
 * 바깥 헤더(start)와 바깥 풋터(end) 두 곳에만 합친 크기를 쓰고 start를 반환한다.
 */
static void *combines_blocks(void *start_ptr, void *end_ptr, int alloc_flag)
{
    /* 1. end 블록 풋터 끝부터 start 블록 헤더까지 전체 바이트 크기 계산 */
    size_t total_size = (char *)FTRP(end_ptr) + WSIZE - (char *)HDRP(start_ptr);
    /* 2. 바깥 헤더와 바깥 풋터에 기록 */
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
 *   병합 전에 remove_block으로 이웃 가용 블록을 리스트에서 먼저 뺀다.
 *   합쳐진 가용 블록은 insert_block으로 가용 리스트 맨 처음에 넣는다.
 */
void *coalesce(void *ptr)
{
    /* 이전 블록의 할당여부는 이전 블록의 풋터에서 가져온다 (경계 태그). */
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(ptr)));
    /* 다음 블록의 할당여부는 다음 블록의 헤더에서 가져온다. */
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(ptr)));
    /* Case1 : 이전과 다음 모두 할당 블록이면*/
    if (prev_alloc && next_alloc)
    {
    }
    /* Case 2: 다음 블록만 가용 블록 상태 */
    else if (prev_alloc && !next_alloc)
    {
        remove_block(NEXT_BLKP(ptr));
        ptr = combines_blocks(ptr, NEXT_BLKP(ptr), 0);
    }
    /* Case 3: 이전 블록만 가용 블록 상태 */
    else if (!prev_alloc && next_alloc)
    {
        remove_block(PREV_BLKP(ptr));
        ptr = combines_blocks(PREV_BLKP(ptr), ptr, 0);
    }
    /* Case 4: 이전 블록과 다음 블록 모두 가용 블록인 경우 */
    else
    {
        remove_block(PREV_BLKP(ptr));
        remove_block(NEXT_BLKP(ptr));
        ptr = combines_blocks(PREV_BLKP(ptr), NEXT_BLKP(ptr), 0);
    }
    /* 합쳐진 가용 블록을 가용 리스트의 맨 앞에 삽입한다. */
    insert_block(ptr);
    return ptr;
}

/*
 * mm_realloc - 블록 크기를 size로 바꾼다. 이사(새 할당 + 복사)를 최대한 피한다.
 * 1) 지금 블록이 충분하면 그대로 반환
 * 2) 뒤가 에필로그면 모자란 만큼만 힙을 늘려 뒤에 빈 블록을 만든다
 * 3) 뒤가 가용이고 합쳐서 충분하면 분할 없이 통째로 흡수 (곧 다시 커질 여유분)
 * 4) 안 되면 mm_malloc → memcpy(작은 쪽 크기만큼) → mm_free
 */
void *mm_realloc(void *ptr, size_t size)
{
    /* ptr이 NULL이면 malloc과 같다. */
    if (ptr == NULL)
        return mm_malloc(size);
    /* size가 0이면 free 후 NULL */
    if (size == 0)
    {
        mm_free(ptr);
        return NULL;
    }
    /* 조정된 요청 크기를 구한다. */
    size_t asize = adjust_size(size);
    /* 지금 블록 전체 크기를 구한다. */
    size_t oldblock = GET_SIZE(HDRP(ptr));
    /* 다음 블록 포인터 */
    void *next = NEXT_BLKP(ptr);
    /* 1. 지금 블록으로 충분하면 그대로 반환 */
    if (oldblock >= asize)
        return ptr;
    /* 2. 다음이 에필로그면 모자란 만큼만 힙을 늘려 그 자리에서 바로 키운다.
       (빈 블록을 만들지 않으므로 최소 블록 24B 제한 없이 8B 단위로 정확히 늘린다) */
    if (GET_SIZE(HDRP(next)) == 0)
    {
        size_t need = asize - oldblock;
        if (mem_sbrk(need) == (void *)-1)
            return NULL;
        PUT(HDRP(ptr), PACK(asize, 1));
        PUT(FTRP(ptr), PACK(asize, 1));
        PUT(HDRP(NEXT_BLKP(ptr)), PACK(0, 1)); /* 새 에필로그 */
        CHECKHEAP();
        return ptr;
    }
    size_t nextblock = GET_SIZE(HDRP(next));
    size_t totalsize = oldblock + nextblock;
    /* 3. 뒤가 가용이고 합쳐서 충분하면 제자리 확장 */
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
    /* 4. 제자리 확장이 안 되면: 새 블록 + 복사 + 반납 */
    void *newptr = mm_malloc(size);
    if (newptr == NULL)
        return NULL;
    /* 옛 payload 크기 = 블록 크기 - 헤더·풋터 8B */
    size_t oldsize = oldblock - DSIZE;
    /* 새 크기와 옛 payload 중 작은 쪽만큼 복사한다. */
    size_t copysize = (size < oldsize) ? size : oldsize;
    memcpy(newptr, ptr, copysize);
    mm_free(ptr);
    CHECKHEAP();
    return newptr;
}

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
 * [리스트 검사] - 16개 크기별 리스트를 각각 훑는다
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

    /* 리스트 순회: 0번부터 15번 리스트까지 하나씩 */
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
    } // 바깥 for 끝: 16개 리스트 다 봄

    /*  마무리: 모든 for 밖 */
    // 14) 힙에서 센 빈 블록 수 == 16개 리스트에서 센 블록 수
    if (heap_free != list_free)
    {
        printf("[mm_check] 빈 블록 수 불일치: 힙=%d, 리스트=%d\n", heap_free, list_free);
        ok = 0;
    }
    return ok;
}
