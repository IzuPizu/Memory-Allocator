// SPDX-License-Identifier: BSD-3-Clause
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "osmem.h"
#include "block_meta.h"


#define MMAP_THRESHOLD (128 * 1024)
#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define PAGE_SIZE getpagesize()

// Head of the linked list of memory blocks
struct block_meta *head;
// Heap was already preallocated -> [0(false), 1(true)]
int preallocHeap;
// Os_calloc was called -> [0(false), 1(true)]
int callocType;
// Os_realloc was called -> [0(false), 1(true)]
int reallocType;

// Function prototypes
void deleteBlock(struct block_meta **head, struct block_meta *delBlock);
struct block_meta *splitRealloc(struct block_meta *callocBlock, size_t size);
struct block_meta *extendRealloc(struct block_meta **head, struct block_meta *memBlock, size_t size);
struct block_meta *extendHeap(struct block_meta **head, size_t size, int reallocSet);
struct block_meta *searchAndSplit(struct block_meta **head, size_t size);
void mergeFreeBlocks(struct block_meta *head, int reallocType);
void insertInList(struct block_meta **head, struct block_meta *memBlock);
struct block_meta *allocateMmap(size_t size);
struct block_meta *tryAll(struct block_meta *head, size_t size);
struct block_meta *allocateSbrk(size_t size);

void *os_malloc(size_t size)
{
	// !We retain the size of PAYLOAD + PADDING [ = ALIGN(size) ] in memBlock->size
	struct block_meta *memBlock;

	// ALIGN the memory requested + metadata
	size_t payloadAndMeta = ALIGN(size) + ALIGN(sizeof(struct block_meta));

	if (size <= 0)
		return NULL;

	// If memory is allocated using calloc => check size of memory chunk
	// mmap -> larger memory chunk than page_size
	// sbrk -> smaller memory chunk than page_size
	if (callocType == 1) {
		if (payloadAndMeta >= (size_t)PAGE_SIZE)
			memBlock = allocateMmap(payloadAndMeta);

		if (payloadAndMeta < (size_t)PAGE_SIZE) {
			if (preallocHeap == 0) {
				preallocHeap = 1;
				memBlock = allocateSbrk(MMAP_THRESHOLD);
			} else {
				// Try to find already existing free block (reuse block)
				memBlock = tryAll(head, payloadAndMeta);
				if (memBlock == NULL) {
					// Could not find free block after merge & split & extending heap
					// => allocate memory with sbrk
					memBlock = allocateSbrk(payloadAndMeta);
				}
			}
		}
		return (void *)(memBlock + 1);
	}

	// Check size of allocation
	// mmap -> larger memory chunk than MMAP_THRESHOLD
	// sbrk -> smaller memory chunk than MMAP_THRESHOLD
	if (payloadAndMeta >= MMAP_THRESHOLD)
		memBlock = allocateMmap(payloadAndMeta);

	if (payloadAndMeta < MMAP_THRESHOLD) {
		// Prealloc heap memory if its used for the first time
		if (preallocHeap == 0) {
			preallocHeap = 1;
			memBlock = allocateSbrk(MMAP_THRESHOLD);
		} else {
			// Try to find already existing free block (reuse block)
			memBlock = tryAll(head, payloadAndMeta);
			if (memBlock == NULL) {
				// Could not find free block after merge & split & extending heap
				// => allocate memory with sbrk
				memBlock = allocateSbrk(payloadAndMeta);
			}
		}
	}

	return (void *)(memBlock + 1);
}

void os_free(void *ptr)
{
	struct block_meta *memBlock = (struct block_meta *)ptr - 1;

	if (ptr == NULL)
		return;

	// Check if the block was allocated with sbrk()
	if (memBlock->status == STATUS_ALLOC) {
		// Mark the block as free
		memBlock->status = STATUS_FREE;
	}

	// Check if the block was mapped
	if (memBlock->status == STATUS_MAPPED) {
		memBlock->status = STATUS_FREE;
		deleteBlock(&head, memBlock);
		int retc = munmap(memBlock, memBlock->size);
		// If munmap was not successful
		DIE(retc == -1, "An error occured at free");
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
		return NULL;

	size_t callocSize = ALIGN(nmemb * size);
	// Set callocType to true because we are using calloc
	callocType = 1;
	void *memBlock = os_malloc(callocSize);

	// Allocation was successful
	if (memBlock != NULL) {
		// Set the memory to 0
		void *callocBlock = memset(memBlock, 0, callocSize);
		// Set callocType back to 0
		callocType = 0;
		return (void *)callocBlock;
	}
	// Allocation failed
	// Set callocType back to 0
	callocType = 0;
	return NULL;
}

void *os_realloc(void *ptr, size_t size)
{
	size_t alignedNewPayload = ALIGN(size);
	struct block_meta *memBlock = (struct block_meta *)ptr - 1;
	struct block_meta *bestFit = NULL;

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	if (ptr == NULL)
		return os_malloc(size);

	// Prevent undefined behavior
	if (memBlock->status == STATUS_FREE)
		return NULL;

	// If new requested size is the same as the previously allocated size => return ptr
	if (alignedNewPayload == memBlock->size)
		return ptr;

	// Ptr(memBlock) does not point to a block on heap => Realloc the block and copy the contents
	if (memBlock->status == STATUS_MAPPED) {
		void *newAddr = os_malloc(size);
		// If new requested size is smaller than previously allocated size =>
		// truncate memory block
		if (alignedNewPayload < memBlock->size)
			memcpy(newAddr, ptr, alignedNewPayload);
		else
			memcpy(newAddr, ptr, memBlock->size);

		os_free(ptr);
		return newAddr;
	}

	// Ptr(memBlock) points to a memory block on the heap
	if (memBlock->status == STATUS_ALLOC) {
		// If new requested size is smaller than the previously allocated size  => truncate memory block
		if (alignedNewPayload < memBlock->size) {
			bestFit = splitRealloc(memBlock, alignedNewPayload);
			return (void *)(bestFit + 1);
		}

		// If new requested size is larger than previously allocated size
		if (alignedNewPayload > memBlock->size) {
			// First try to expand the block.
			if (memBlock->next != NULL) {
				// Try to expand the block
				bestFit = extendRealloc(&head, memBlock, alignedNewPayload);
			} else {
				// If memBlock is last on heap => expand last block on heap
				reallocType = 1;
				bestFit = extendHeap(&memBlock, alignedNewPayload, reallocType);
			}

			// Could expand the block
			if (bestFit != NULL)
				return (void *)(bestFit + 1);

			// Set reallocType back to 0
			reallocType = 0;
			// We could not expand the block => the block will be reallocated and its contents copied.
			void *newAddr = os_malloc(size);

			memcpy(newAddr, ptr, memBlock->size);
			os_free(ptr);
			return newAddr;
		}
	}

	return NULL;
}

struct block_meta *allocateMmap(size_t size)
{
	// Allocate memory using mmap
	void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	// Check if an error occured after the syscall
	DIE(ptr == MAP_FAILED, "An error occured at mmap");
	// Update the metadata for this block and add it in the linked list
	struct block_meta *memBlock = (struct block_meta *)ptr;

	memBlock->next = NULL;
	memBlock->prev = NULL;
	// Retain only the payload + padding size
	memBlock->size = size;
	memBlock->status = STATUS_MAPPED;

	insertInList(&head, memBlock);
	return memBlock;
}

struct block_meta *allocateSbrk(size_t size)
{
	// Allocate memory on the heap using sbrk
	void *ptr = sbrk(size);
	// Check if an error occured after the syscall
	DIE((ptr == (void *)-1), "An error occured at sbrk");
	// Update the metadata for this block and add it in the linked list
	struct block_meta *memBlock = (struct block_meta *)ptr;

	memBlock->next = NULL;
	memBlock->prev = NULL;
	// Retain only the payload + padding size
	memBlock->size = size - ALIGN(sizeof(struct block_meta));
	memBlock->status = STATUS_ALLOC;

	insertInList(&head, memBlock);
	return memBlock;
}

struct block_meta *tryAll(struct block_meta *head, size_t size)
{
	struct block_meta *memBlock = NULL;
	// Try to merge all free blocks (coalesce)
	mergeFreeBlocks(head, 0);
	// Try to split and search for suitable free block (best fit)
	memBlock = searchAndSplit(&head, size);

	if (memBlock != NULL)
		// We have found a suitable free block
		return memBlock;

	// There was no free block to reuse => try to expand last block on the heap
	memBlock = extendHeap(&head, size, 0);
	return memBlock;
}

void mergeFreeBlocks(struct block_meta *head, int reallocType)
{
	struct block_meta *tmp = head;

	if (head == NULL)
		return;

	// If its a realloc
	if (reallocType == 1) {
		// tmp points to the block we want to expand
		if (tmp->next != NULL && tmp->next->status == STATUS_FREE) {
			tmp->size += tmp->next->size + ALIGN(sizeof(struct block_meta));
			tmp->next = tmp->next->next;
		}
	}

	// If is not a realloc
	if (reallocType == 0) {
		// We traverse the linked and try to merge the free blocks
		while (tmp->next != NULL && tmp != NULL) {
			if (tmp->status == STATUS_FREE && tmp->next->status == STATUS_FREE) {
				tmp->size += tmp->next->size + ALIGN(sizeof(struct block_meta));
				tmp->next = tmp->next->next;
			} else {
				tmp = tmp->next;
			}
		}
	}
}

struct block_meta *searchAndSplit(struct block_meta **head, size_t size)
{
	// Try to find a free block we can allocate of the size required
	struct block_meta *tmp = *head;
	struct block_meta *bestFit = NULL;
	size_t requiredSize = size - ALIGN(sizeof(struct block_meta));

	// Find best suitable free block
	while (tmp != NULL) {
		if (tmp->size >= requiredSize && tmp->status == STATUS_FREE) {
			// If it's the first suitable block or smaller than the current best fit
			if (bestFit == NULL || tmp->size < bestFit->size)
				bestFit = tmp;
		}
		// Continue searching
		tmp = tmp->next;
	}

	if (bestFit != NULL) {
		struct block_meta *newBlock = NULL;

		bestFit->status = STATUS_ALLOC;
		// Check if splitting is possible
		if (bestFit->size >= ALIGN(1) + size) {
			// Create a new free block after splitting
			newBlock = (struct block_meta *)((char *)bestFit + size);
			newBlock->size = bestFit->size - size;
			newBlock->status = STATUS_FREE;
			bestFit->size = requiredSize;

			// Link the new block to the original block
			newBlock->next = bestFit->next;
			bestFit->next = newBlock;
			newBlock->prev = bestFit;
		}
	}
	return bestFit;
}

struct block_meta *extendHeap(struct block_meta **head, size_t size, int reallocType)
{
	struct block_meta *tmp = *head;

	// If its a realloc
	if (reallocType == 1) {
		// Try to expand last block on the heap
		if (tmp->next == NULL) {
			void *newAddr = sbrk(size - tmp->size);

			DIE(newAddr == ((void *)-1), "Error at sbrk in last block expand\n");
			tmp->size = size;
			tmp->status = STATUS_ALLOC;
			tmp->next = NULL;
			return tmp;
		}
	}

	// Traverese the linked list until last memory block
	while (tmp->next != NULL)
		tmp = tmp->next;

	// We got to the last block => check if we can expand it (its free)
	if (tmp->status == STATUS_FREE) {
		// Allocate memory on the heap using sbrk
		void *ptr = sbrk(size - ALIGN(sizeof(struct block_meta)) - tmp->size);
		// Check if an error occured after the syscall
		DIE((ptr == (void *)-1), "An error occured at sbrk");
		// Update the metadata
		tmp->size = size - ALIGN(sizeof(struct block_meta));
		tmp->status = STATUS_ALLOC;
		tmp->next = NULL;
		return tmp;
	}
	return NULL;
}

void insertInList(struct block_meta **head, struct block_meta *memBlock)
{
	struct block_meta *temp = *head;
	// Insert memory block at the end of the doubly linked list
	if (*head == NULL) {
		*head = memBlock;
		memBlock->prev = NULL;
		memBlock->next = NULL;
		return;
	}

	while (temp->next != NULL)
		temp = temp->next;

	temp->next = memBlock;
	memBlock->prev = temp;
}

struct block_meta *splitRealloc(struct block_meta *memBlock, size_t size)
{
	struct block_meta *newBlock;

	if (memBlock->size >= ALIGN(1) + (size + ALIGN(sizeof(struct block_meta)))) {
		newBlock = (struct block_meta *)((char *)memBlock + size + ALIGN(sizeof(struct block_meta)));
		newBlock->size = memBlock->size - (size + ALIGN(sizeof(struct block_meta)));
		newBlock->status = STATUS_FREE;

		// Link the new block to the original block
		newBlock->next = memBlock->next;
		memBlock->next = newBlock;
		newBlock->prev = memBlock;
		memBlock->size = size;
	}

	return memBlock;
}

struct block_meta *extendRealloc(struct block_meta **head, struct block_meta *memBlock, size_t size)
{
	struct block_meta *tmp = *head;
	// Find the block to be extended
	while (tmp != NULL && tmp != memBlock)
		tmp = tmp->next;

	// If the block was found => merge(coalesce) blocks to expand the memBlock
	if (tmp != NULL) {
		// Also set reallocType
		reallocType = 1;
		mergeFreeBlocks(tmp, reallocType);

		// Check if the expanded block is suitable
		if (tmp->size >= size) {
			// Split the expanded block
			reallocType = 0;
			return splitRealloc(tmp, size);
		}
		// Set reallocType back to 0
		reallocType = 0;
	}

	return NULL;
}

void deleteBlock(struct block_meta **head, struct block_meta *delBlock)
{
	// Delete memory block from the doubly linked list
	if (*head == NULL || delBlock == NULL)
		return;

	// If delBlock is the head of the linked list, point the head to the next of delBlock
	if (*head == delBlock)
		*head = delBlock->next;

	// If delBlock is not the last block
	if (delBlock->next != NULL)
		delBlock->next->prev = delBlock->prev;

	// If delBlock is not the first block
	if (delBlock->prev != NULL)
		delBlock->prev->next = delBlock->next;
}
