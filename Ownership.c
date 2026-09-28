#include "Ownership.h"
#include "vm.h"
#include <stdlib.h>
#include <string.h>

/* VM object allocation, cloning, and deterministic destruction. */
#define GET_TAGS_PTR(raw) ((uint8_t*)((raw)->data) + (raw)->data_bytes)

static uint32_t hash_bytes(const char* chars, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i) { hash ^= (uint8_t)chars[i]; hash *= 16777619u; }
    return hash;
}

static int unlink_object(VMContext* vm, VMObject* target) {
    VMObject** link = &vm->objects;
    while (*link && *link != target) link = &(*link)->next;
    if (*link != target) return 0;
    *link = target->next;
    return 1;
}

static void remove_interned_string(VMContext* vm, ObjString* string) {
    if (!vm->strings) return;
    for (uint32_t i = 0; i < vm->string_capacity; ++i) {
        if (vm->strings[i] == string) {
            vm->strings[i] = NULL;
            if (vm->string_count) --vm->string_count;
            return;
        }
    }
}

VMObject* ownership_allocate(VMContext* vm, size_t size, TypeIDEnum type) {
    VMObject* object = (VMObject*)calloc(1, size);
    if (!object) return NULL;
    object->type = type;
    object->allocation_capacity = size;
    object->unclaimed = 1;
    object->next = vm->objects;
    vm->objects = object;
    vm->object_bytes_allocated += size;
    return object;
}

ObjString* intern_string(VMContext* vm, const char* chars, size_t length) {
    const uint32_t hash = hash_bytes(chars, length);
    for (uint32_t i = 0; i < vm->string_capacity; ++i) {
        ObjString* existing = vm->strings ? vm->strings[i] : NULL;
        if (existing && existing->hash == hash && existing->length == length &&
            memcmp(existing->data, chars, length) == 0) return existing;
    }
    if (vm->string_count + 1 > vm->string_capacity) {
        uint32_t old_capacity = vm->string_capacity;
        uint32_t new_capacity = old_capacity ? old_capacity * 2 : 16;
        ObjString** table = (ObjString**)realloc(vm->strings, sizeof(ObjString*) * new_capacity);
        if (!table) return NULL;
        memset(table + old_capacity, 0, sizeof(ObjString*) * (new_capacity - old_capacity));
        vm->strings = table;
        vm->string_capacity = new_capacity;
    }
    ObjString* string = (ObjString*)ownership_allocate(vm, sizeof(ObjString) + length + 1, T_STRING);
    if (!string) return NULL;
    string->header.unclaimed = 0;
    string->length = length;
    string->hash = hash;
    memcpy(string->data, chars, length);
    string->data[length] = '\0';
    for (uint32_t i = 0; i < vm->string_capacity; ++i) {
        if (!vm->strings[i]) { vm->strings[i] = string; ++vm->string_count; break; }
    }
    return string;
}

VMObject* ownership_clone(VMContext* vm, const VMObject* object) {
    if (!vm || !object) return NULL;
    if (object->type == T_STRING) {
        const ObjString* string = (const ObjString*)object;
        return (VMObject*)intern_string(vm, string->data, string->length);
    }
    if (object->type != T_SHARED_PTR) return NULL;

    const ObjRaw* source = (const ObjRaw*)object;
    if (source->kind == OBJ_NATIVE_HANDLE) return NULL;
    size_t trailing = source->kind == OBJ_INSTANCE ? source->slot_count : 0;
    ObjRaw* copy = (ObjRaw*)ownership_allocate(vm,
        sizeof(ObjRaw) + source->data_bytes + trailing, source->header.type);
    if (!copy) return NULL;
    copy->kind = source->kind;
    copy->slot_count = source->slot_count;
    copy->data_bytes = source->data_bytes;
    copy->element_size = source->element_size;
    copy->cleanup = NULL;
    memset(copy->data, 0, source->data_bytes + trailing);

    if (source->kind == OBJ_INSTANCE) {
        const uint64_t* source_slots = (const uint64_t*)source->data;
        const uint8_t* source_tags = GET_TAGS_PTR(source);
        uint64_t* copy_slots = (uint64_t*)copy->data;
        uint8_t* copy_tags = GET_TAGS_PTR(copy);
        for (size_t i = 0; i < source->slot_count; ++i) {
            copy_tags[i] = source_tags[i];
            if (source_tags[i]) {
                VMObject* child = ownership_clone(vm,
                    (const VMObject*)(uintptr_t)source_slots[i]);
                if (source_slots[i] && !child) {
                    ownership_destroy(vm, (VMObject*)copy);
                    return NULL;
                }
                if (child) child->unclaimed = 0;
                copy_slots[i] = (uint64_t)(uintptr_t)child;
            } else copy_slots[i] = source_slots[i];
        }
    } else if (source->kind == OBJ_POINTERS) {
        VMObject* const* source_slots = (VMObject* const*)source->data;
        VMObject** copy_slots = (VMObject**)copy->data;
        for (size_t i = 0; i < source->slot_count; ++i) {
            copy_slots[i] = ownership_clone(vm, source_slots[i]);
            if (source_slots[i] && !copy_slots[i]) {
                ownership_destroy(vm, (VMObject*)copy);
                return NULL;
            }
            if (copy_slots[i]) copy_slots[i]->unclaimed = 0;
        }
    } else {
        memcpy(copy->data, source->data, source->data_bytes);
    }
    return (VMObject*)copy;
}

void ownership_destroy(VMContext* vm, VMObject* object) {
    if (!vm || !object || !unlink_object(vm, object)) return;
    if (object->type == T_STRING) {
        remove_interned_string(vm, (ObjString*)object);
    } else if (object->type == T_SHARED_PTR) {
        ObjRaw* raw = (ObjRaw*)object;
        if (raw->kind == OBJ_INSTANCE) {
            uint64_t* slots = (uint64_t*)raw->data;
            uint8_t* tags = GET_TAGS_PTR(raw);
            for (size_t i = raw->slot_count; i > 0; --i) {
                size_t slot = i - 1;
                if (tags[slot]) {
                    VMObject* child = (VMObject*)(uintptr_t)slots[slot];
                    slots[slot] = 0; tags[slot] = 0;
                    if (!child || child->type != T_STRING)
                        ownership_destroy(vm, child);
                }
            }
        } else if (raw->kind == OBJ_POINTERS) {
            VMObject** slots = (VMObject**)raw->data;
            for (size_t i = raw->slot_count; i > 0; --i) {
                VMObject* child = slots[i - 1];
                slots[i - 1] = NULL;
                if (!child || child->type != T_STRING)
                    ownership_destroy(vm, child);
            }
        }
        if (raw->cleanup) raw->cleanup(raw->data);
    }
    if (vm->object_bytes_allocated >= object->allocation_capacity)
        vm->object_bytes_allocated -= object->allocation_capacity;
    else vm->object_bytes_allocated = 0;
    free(object);
}

void ownership_destroy_all(VMContext* vm) {
    if (!vm) return;
    while (vm->objects) ownership_destroy(vm, vm->objects);
    free(vm->strings);
    vm->strings = NULL;
    vm->string_capacity = 0;
    vm->string_count = 0;
}

void ownership_claim(VMContext* vm, VMObject* object) {
    (void)vm;
    if (object && object->type != T_STRING) object->unclaimed = 0;
}

void ownership_unclaim(VMContext* vm, VMObject* object) {
    (void)vm;
    if (object && object->type != T_STRING) object->unclaimed = 1;
}

void ownership_destroy_unclaimed(VMContext* vm) {
    ownership_destroy_unclaimed_except(vm, NULL);
}

void ownership_destroy_unclaimed_except(VMContext* vm, const VMObject* keep) {
    if (!vm) return;
    for (;;) {
        VMObject* unclaimed = NULL;
        for (VMObject* object = vm->objects; object; object = object->next) {
            if (object != keep && object->unclaimed && object->type != T_STRING) {
                unclaimed = object;
                break;
            }
        }
        if (!unclaimed) break;
        ownership_destroy(vm, unclaimed);
    }
}

size_t ownership_live_objects(const VMContext* vm) {
    size_t count = 0;
    for (const VMObject* object = vm ? vm->objects : NULL; object; object = object->next) ++count;
    return count;
}
