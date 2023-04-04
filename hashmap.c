// This is an implementation of the open-addressing hash table.

#include "dyibicc.h"

// Initial hash bucket size
#define INIT_SIZE 16

// Rehash if the usage exceeds 70%.
#define HIGH_WATERMARK 70

// We'll keep the usage below 50% after rehashing.
#define LOW_WATERMARK 50

// Represents a deleted hash entry
#define TOMBSTONE ((void*)-1)

static uint64_t fnv_hash(char* s, int len) {
  uint64_t hash = 0xcbf29ce484222325;
  for (int i = 0; i < len; i++) {
    hash *= 0x100000001b3;
    hash ^= (unsigned char)s[i];
  }
  return hash;
}

// Make room for new entires in a given hashmap by removing
// tombstones and possibly extending the bucket size.
static void rehash(HashMap* map) {
  // Compute the size of the new hashmap.
  int nkeys = 0;
  for (int i = 0; i < map->capacity; i++)
    if (map->buckets[i].key && map->buckets[i].key != TOMBSTONE)
      nkeys++;

  int cap = map->capacity;
  while ((nkeys * 100) / cap >= LOW_WATERMARK)
    cap = cap * 2;
  assert(cap > 0);

  // Create a new hashmap and copy all key-values.
  HashMap map2 = {0};
  map2.buckets = bumpcalloc(cap, sizeof(HashEntry), map->alloc_lifetime);
  map2.capacity = cap;
  map2.alloc_lifetime = map->alloc_lifetime;

  for (int i = 0; i < map->capacity; i++) {
    HashEntry* ent = &map->buckets[i];
    if (ent->key && ent->key != TOMBSTONE) {
      hashmap_put2(&map2, ent->key, ent->keylen, ent->val);
    }
  }

  assert(map2.used == nkeys);
  if (map->alloc_lifetime == AL_Manual) {
    alloc_free(map->buckets, map->alloc_lifetime);
  }
  *map = map2;
}

static bool match(HashEntry* ent, char* key, int keylen) {
  return ent->key && ent->key != TOMBSTONE && ent->keylen == keylen &&
         memcmp(ent->key, key, keylen) == 0;
}

static HashEntry* get_entry(HashMap* map, char* key, int keylen) {
  if (!map->buckets)
    return NULL;

  uint64_t hash = fnv_hash(key, keylen);

  for (int i = 0; i < map->capacity; i++) {
    HashEntry* ent = &map->buckets[(hash + i) % map->capacity];
    if (match(ent, key, keylen))
      return ent;
    if (ent->key == NULL)
      return NULL;
  }
  unreachable();
}

static HashEntry* get_or_insert_entry(HashMap* map, char* key, int keylen) {
  if (!map->buckets) {
    map->buckets = bumpcalloc(INIT_SIZE, sizeof(HashEntry), map->alloc_lifetime);
    map->capacity = INIT_SIZE;
  } else if ((map->used * 100) / map->capacity >= HIGH_WATERMARK) {
    rehash(map);
  }

  uint64_t hash = fnv_hash(key, keylen);

  for (int i = 0; i < map->capacity; i++) {
    HashEntry* ent = &map->buckets[(hash + i) % map->capacity];

    if (match(ent, key, keylen))
      return ent;

    if (ent->key == TOMBSTONE) {
      ent->key = key;
      ent->keylen = keylen;
      return ent;
    }

    if (ent->key == NULL) {
      ent->key = key;
      ent->keylen = keylen;
      map->used++;
      return ent;
    }
  }
  unreachable();
}

void* hashmap_get(HashMap* map, char* key) {
  return hashmap_get2(map, key, (int)strlen(key));
}

void* hashmap_get2(HashMap* map, char* key, int keylen) {
  HashEntry* ent = get_entry(map, key, keylen);
  return ent ? ent->val : NULL;
}

void hashmap_put(HashMap* map, char* key, void* val) {
  hashmap_put2(map, key, (int)strlen(key), val);
}

void hashmap_put2(HashMap* map, char* key, int keylen, void* val) {
  HashEntry* ent = get_or_insert_entry(map, key, keylen);
  ent->val = val;
}

void hashmap_delete(HashMap* map, char* key) {
  hashmap_delete2(map, key, (int)strlen(key));
}

void hashmap_delete2(HashMap* map, char* key, int keylen) {
  HashEntry* ent = get_entry(map, key, keylen);
  if (ent)
    ent->key = TOMBSTONE;
}

#if 0
void hashmap_test(void) {
  HashMap *map = bumpcalloc(1, sizeof(HashMap));

  for (int i = 0; i < 5000; i++)
    hashmap_put(map, format("key %d", i), (void *)(size_t)i);
  for (int i = 1000; i < 2000; i++)
    hashmap_delete(map, format("key %d", i));
  for (int i = 1500; i < 1600; i++)
    hashmap_put(map, format("key %d", i), (void *)(size_t)i);
  for (int i = 6000; i < 7000; i++)
    hashmap_put(map, format("key %d", i), (void *)(size_t)i);

  for (int i = 0; i < 1000; i++)
    assert((size_t)hashmap_get(map, format("key %d", i)) == i);
  for (int i = 1000; i < 1500; i++)
    assert(hashmap_get(map, "no such key") == NULL);
  for (int i = 1500; i < 1600; i++)
    assert((size_t)hashmap_get(map, format("key %d", i)) == i);
  for (int i = 1600; i < 2000; i++)
    assert(hashmap_get(map, "no such key") == NULL);
  for (int i = 2000; i < 5000; i++)
    assert((size_t)hashmap_get(map, format("key %d", i)) == i);
  for (int i = 5000; i < 6000; i++)
    assert(hashmap_get(map, "no such key") == NULL);
  for (int i = 6000; i < 7000; i++)
    hashmap_put(map, format("key %d", i), (void *)(size_t)i);

  assert(hashmap_get(map, "no such key") == NULL);
  printf("OK\n");
}
#endif
