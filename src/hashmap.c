#include "quarzum.h"

#define INIT_SIZE 16
#define HIGH_WATERMARK 70
#define LOW_WATERMARK 50
#define TOMBSTONE ((void *)-1)

static uint64_t fnv_hash(char* s, int len) {
    uint64_t hash = 0xcbf29ce484222325;
    for (int i = 0; i < len; i++) {
        hash *= 0x100000001b3;
        hash ^= (unsigned char)s[i];
    }
    return hash;
}

static void rehash(HashMap* map) {
    int nkeys = 0;
    for (int i = 0; i < map->capacity; i++)
        if (map->buckets[i].key && map->buckets[i].key != TOMBSTONE)
        nkeys++;

    int cap = map->capacity;
    while ((nkeys * 100) / cap >= LOW_WATERMARK)
        cap = cap * 2;
    assert(cap > 0);

    HashMap map2 = {};
    map2.buckets = calloc(cap, sizeof(HashEntry));
    map2.capacity = cap;

    for (int i = 0; i < map->capacity; i++) {
        HashEntry *ent = &map->buckets[i];
        if (ent->key && ent->key != TOMBSTONE)
        hashmap_put(&map2, ent->key, ent->val);
    }

    assert(map2.used == nkeys);
    *map = map2;
}

static bool match(HashEntry* ent, char* key, int keylen) {
    if (!ent->key || ent->key == TOMBSTONE)
        return false;
    int stored_len = strlen(ent->key);
    return stored_len == keylen && memcmp(ent->key, key, keylen) == 0;
}

static HashEntry *get_entry(HashMap* map, char* key, int keylen) {
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

static HashEntry *get_or_insert_entry(HashMap* map, char* key, int keylen) {
    if (!map->buckets) {
        map->buckets = calloc(INIT_SIZE, sizeof(HashEntry));
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
        return ent;
        }

        if (ent->key == NULL) {
        ent->key = key;
        map->used++;
        return ent;
        }
    }
    unreachable();
}


void* hashmap_get(HashMap* map, char* key) {
    HashEntry *ent = get_entry(map, key, strlen(key));
    return ent ? ent->val : NULL;
}


void hashmap_put(HashMap* map, char* key, void* val) {
    HashEntry *ent = get_or_insert_entry(map, key, strlen(key));
    ent->val = val;
}

void hashmap_delete(HashMap* map, char* key) {
    HashEntry *ent = get_entry(map, key, strlen(key));
    if (ent)
        ent->key = TOMBSTONE;
}

