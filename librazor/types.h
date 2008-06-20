#ifndef _RAZOR_TYPES_H_
#define _RAZOR_TYPES_H_

#include <stdint.h>

struct array {
	void *data;
	int size, alloc;
};

void array_init(struct array *array);
void array_release(struct array *array);
void *array_add(struct array *array, int size);


struct list_head {
	uint list_ptr : 24;
	uint flags    : 8;
};

struct list {
	uint data  : 24;
	uint flags : 8;
};

void list_set_empty(struct list_head *head);
void list_set_ptr(struct list_head *head, uint32_t ptr);
void list_set_array(struct list_head *head, struct array *pool, struct array *items, int force_indirect);

struct list *list_first(struct list_head *head, struct array *pool);
struct list *list_next(struct list *list);

void list_remap_pool(struct array *pool, uint32_t *map);
void list_remap_head(struct list_head *list, uint32_t *map);


struct hashtable {
	struct array buckets;
	struct array *pool;
};

void hashtable_init(struct hashtable *table, struct array *pool);
void hashtable_release(struct hashtable *table);
uint32_t hashtable_insert(struct hashtable *table, const char *key);
uint32_t hashtable_lookup(struct hashtable *table, const char *key);
uint32_t hashtable_tokenize(struct hashtable *table, const char *string);


struct bitarray {
	uint32_t *bits;
};

void bitarray_init(struct bitarray *bitarray, int size, int intial_value);
void bitarray_release(struct bitarray *bitarray);
void bitarray_set(struct bitarray *bitarray, int bit, int value);
int bitarray_get(struct bitarray *bitarray, int bit);


#endif /* _RAZOR_TYPES_H_ */