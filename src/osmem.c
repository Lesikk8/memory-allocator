// SPDX-License-Identifier: BSD-3-Clause

/*
 * Мінімальний алокатор пам'яті: os_malloc, os_calloc, os_realloc, os_free.
 *
 * Ідея:
 *  - Кожен блок = заголовок struct block_meta + корисні дані (payload).
 *  - Малі блоки беремо з купи (heap) через sbrk()/brk().
 *  - Великі блоки беремо окремо через mmap() і віддаємо через munmap().
 *  - Блоки купи зберігаються у двозв'язному списку (heap_head ... heap_tail)
 *    у тому порядку, в якому лежать у пам'яті.
 *  - Звільнені блоки купи не повертаються ОС, а позначаються STATUS_FREE
 *    і використовуються повторно (split, coalesce, find best).
 */

#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "osmem.h"
#include "block_meta.h"

/* Вирівнювання на 8 байтів */
#define ALIGNMENT		8
#define ALIGN(size)		(((size) + (ALIGNMENT - 1)) & ~((size_t)ALIGNMENT - 1))

/* Розмір заголовка блока (вже вирівняний) */
#define META_SIZE		ALIGN(sizeof(struct block_meta))

/* Поріг, після якого malloc/realloc використовують mmap */
#define MMAP_THRESHOLD		(128 * 1024)

/* Розмір попереднього виділення купи (heap preallocation) */
#define HEAP_PREALLOC		(128 * 1024)

/* Голова і хвіст списку блоків купи */
static struct block_meta *heap_head;
static struct block_meta *heap_tail;

/* ---------------------------------------------------------------------- */
/* Допоміжні функції                                                     */
/* ---------------------------------------------------------------------- */

/* Адреса даних (payload) за адресою заголовка */
static inline void *block_payload(struct block_meta *block)
{
	return (char *)block + META_SIZE;
}

/* Адреса заголовка за адресою даних */
static inline struct block_meta *payload_block(void *ptr)
{
	return (struct block_meta *)((char *)ptr - META_SIZE);
}

/* Обгортка над sbrk() з перевіркою помилки */
static void *heap_grow(size_t increment)
{
	void *old_brk = sbrk(increment);

	DIE(old_brk == (void *)-1, "sbrk");
	return old_brk;
}

/* Додає новий блок у кінець списку купи */
static void heap_append(struct block_meta *block)
{
	block->prev = heap_tail;
	block->next = NULL;

	if (heap_tail)
		heap_tail->next = block;
	else
		heap_head = block;

	heap_tail = block;
}

/*
 * Перший виклик, що працює з купою: виділяємо одразу 128 KB одним блоком.
 * Це зменшує кількість майбутніх викликів brk().
 */
static void heap_preallocate(void)
{
	struct block_meta *block = heap_grow(HEAP_PREALLOC);

	block->size = HEAP_PREALLOC - META_SIZE;
	block->status = STATUS_FREE;
	heap_append(block);
}

/*
 * Розбиває блок, якщо після виділення size байтів залишається місце
 * хоча б для заголовка і 1 (тобто 8 після вирівнювання) байта даних.
 * size вже вирівняний.
 */
static void split_block(struct block_meta *block, size_t size)
{
	struct block_meta *new_block;

	if (block->size < size + META_SIZE + ALIGN(1))
		return;

	new_block = (struct block_meta *)((char *)block + META_SIZE + size);
	new_block->size = block->size - size - META_SIZE;
	new_block->status = STATUS_FREE;

	new_block->prev = block;
	new_block->next = block->next;
	if (block->next)
		block->next->prev = new_block;
	else
		heap_tail = new_block;
	block->next = new_block;

	block->size = size;
}

/* Приєднує до block наступний блок (вважаємо, що він вільний) */
static void merge_with_next(struct block_meta *block)
{
	struct block_meta *next = block->next;

	block->size += META_SIZE + next->size;
	block->next = next->next;

	if (next->next)
		next->next->prev = block;
	else
		heap_tail = block;
}

/* Об'єднує всі сусідні вільні блоки купи (coalesce) */
static void coalesce_all(void)
{
	struct block_meta *block = heap_head;

	while (block) {
		if (block->status == STATUS_FREE) {
			while (block->next && block->next->status == STATUS_FREE)
				merge_with_next(block);
		}
		block = block->next;
	}
}

/* Find best: найменший вільний блок, у який вміщається size байтів */
static struct block_meta *find_best(size_t size)
{
	struct block_meta *best = NULL;

	for (struct block_meta *block = heap_head; block; block = block->next) {
		if (block->status != STATUS_FREE || block->size < size)
			continue;
		if (!best || block->size < best->size)
			best = block;
	}

	return best;
}

/* Виділення великого блока через mmap(); size вже вирівняний */
static void *alloc_mapped(size_t size)
{
	struct block_meta *block;

	block = mmap(NULL, size + META_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	DIE(block == MAP_FAILED, "mmap");

	block->size = size;
	block->status = STATUS_MAPPED;
	block->prev = NULL;
	block->next = NULL;

	return block_payload(block);
}

/* Виділення блока на купі; size вже вирівняний */
static void *alloc_heap(size_t size)
{
	struct block_meta *block;

	if (!heap_head)
		heap_preallocate();

	/* Спершу об'єднуємо вільні блоки, потім шукаємо найкращий */
	coalesce_all();
	block = find_best(size);
	if (block) {
		split_block(block, size);
		block->status = STATUS_ALLOC;
		return block_payload(block);
	}

	/* Вільного блока немає: якщо останній блок вільний — розширюємо його */
	if (heap_tail->status == STATUS_FREE) {
		heap_grow(size - heap_tail->size);
		heap_tail->size = size;
		heap_tail->status = STATUS_ALLOC;
		return block_payload(heap_tail);
	}

	/* Інакше додаємо новий блок у кінець купи */
	block = heap_grow(META_SIZE + size);
	block->size = size;
	block->status = STATUS_ALLOC;
	heap_append(block);

	return block_payload(block);
}

/* Спільна логіка для malloc і calloc: threshold — поріг для mmap */
static void *alloc_block(size_t size, size_t threshold)
{
	size_t aligned = ALIGN(size);

	if (META_SIZE + aligned >= threshold)
		return alloc_mapped(aligned);

	return alloc_heap(aligned);
}

/* ---------------------------------------------------------------------- */
/* Публічний інтерфейс                                                    */
/* ---------------------------------------------------------------------- */

void *os_malloc(size_t size)
{
	if (size == 0)
		return NULL;

	return alloc_block(size, MMAP_THRESHOLD);
}

void os_free(void *ptr)
{
	struct block_meta *block;
	int ret;

	if (!ptr)
		return;

	block = payload_block(ptr);

	if (block->status == STATUS_MAPPED) {
		/* Пам'ять, отримана через mmap, повертається ОС */
		ret = munmap(block, block->size + META_SIZE);
		DIE(ret == -1, "munmap");
	} else if (block->status == STATUS_ALLOC) {
		/* Блок купи лише позначаємо вільним — brk() не викликаємо */
		block->status = STATUS_FREE;
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	size_t total;
	void *ptr;

	if (nmemb == 0 || size == 0)
		return NULL;

	/* Захист від переповнення множення */
	if (nmemb > (size_t)-1 / size)
		return NULL;

	total = nmemb * size;

	/* Для calloc поріг mmap — розмір сторінки */
	ptr = alloc_block(total, (size_t)getpagesize());
	memset(ptr, 0, total);

	return ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	struct block_meta *block;
	size_t aligned;
	void *new_ptr;

	if (!ptr)
		return os_malloc(size);

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	block = payload_block(ptr);

	/* realloc на звільненому блоці — некоректне використання */
	if (block->status == STATUS_FREE)
		return NULL;

	aligned = ALIGN(size);

	/*
	 * Блоки з mmap або запит, що перевищує поріг: завжди виділяємо
	 * новий блок, копіюємо дані і звільняємо старий.
	 */
	if (block->status == STATUS_MAPPED || META_SIZE + aligned >= MMAP_THRESHOLD) {
		new_ptr = os_malloc(size);
		memcpy(new_ptr, ptr, block->size < aligned ? block->size : aligned);
		os_free(ptr);
		return new_ptr;
	}

	/* 1. Зменшення (або той самий розмір): обрізаємо блок на місці */
	if (aligned <= block->size) {
		split_block(block, aligned);
		return ptr;
	}

	/* 2. Блок останній на купі: просто розширюємо купу */
	if (block == heap_tail) {
		heap_grow(aligned - block->size);
		block->size = aligned;
		return ptr;
	}

	/*
	 * 3. Пробуємо приєднувати наступні вільні блоки по одному,
	 *    перевіряючи після кожного, чи вже вистачає місця.
	 *    Об'єднані блоки залишаються об'єднаними навіть якщо не вистачило.
	 */
	while (block->next && block->next->status == STATUS_FREE) {
		merge_with_next(block);
		if (block->size >= aligned) {
			split_block(block, aligned);
			return ptr;
		}
	}

	/* 4. Не вдалося розширити: виділяємо новий блок і переносимо дані */
	new_ptr = os_malloc(size);
	memcpy(new_ptr, ptr, block->size);
	os_free(ptr);

	return new_ptr;
}
