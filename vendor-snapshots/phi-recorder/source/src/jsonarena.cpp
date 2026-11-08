#include "jsonarena.h"

#include <cstdlib>

#include "cJSON.h"

namespace jsonarena {
namespace {

const size_t kChunk = 4u << 20;  // 4 MB per block

struct Chunk {
    Chunk* next;
    size_t used;
    size_t cap;
};

Chunk* g_head = nullptr;
bool g_installed = false;

void* CJSON_CDECL arenaAlloc(size_t size) {
    if (size == 0) size = 1;
    size = (size + 15u) & ~(size_t)15u;
    if (!g_head || g_head->used + size > g_head->cap) {
        const size_t cap = size > kChunk ? size : kChunk;
        Chunk* c = (Chunk*)std::malloc(sizeof(Chunk) + cap);
        if (!c) return std::malloc(size);  // out of luck, but never crash
        c->next = g_head;
        c->used = 0;
        c->cap = cap;
        g_head = c;
    }
    void* p = (char*)(g_head + 1) + g_head->used;
    g_head->used += size;
    return p;
}

// Nothing is released individually; the arena lives until the process exits.
void CJSON_CDECL arenaFree(void*) {}

}  // namespace

void install() {
    if (g_installed) return;
    g_installed = true;
    cJSON_Hooks hooks;
    hooks.malloc_fn = arenaAlloc;
    hooks.free_fn = arenaFree;
    cJSON_InitHooks(&hooks);
}

}  // namespace jsonarena
