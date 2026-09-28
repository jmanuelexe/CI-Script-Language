#ifndef CI_OWNERSHIP_H
#define CI_OWNERSHIP_H
#include "types.h"

struct VMContext;

typedef enum {
    // 1. Array of Pointers
    // Created by: OP_ALLOCATE_ARRAY_PTR
    // Data: void* pointers[]
    // Owned elements are destroyed with their containing array.
    OBJ_POINTERS = 0,

    // 2. Native Handle
    // Created by: create_native_handle()
    // Data: void* (Raw C pointer, e.g., Mesh*, FILE*)
    // Opaque C pointer; cleanup runs when its owning handle is destroyed.
    OBJ_NATIVE_HANDLE,

    // 3. Byte Buffer (Blob)
    // Created by: OP_ALLOCATE (with generic bytes)
    // Data: uint8_t[]
    // Raw bytes contain no VM object references.
    OBJ_BLOB,

    // 4. Class instance with per-slot ownership tags.
    OBJ_INSTANCE,
} ObjKind;

typedef struct VMObject VMObject;

struct VMObject {
    struct VMObject* next;
    size_t allocation_capacity;
    TypeIDEnum type;
    uint8_t unclaimed; // Fresh allocation not yet committed to an owner.
};


// String Object
typedef struct {
    VMObject header;
    size_t length;
    uint32_t hash;
    char data[]; // Flexible array
} ObjString;

typedef void (*CDtor)(void* data_ptr);

// Generic Raw Object (for Classes/Allocations)
typedef struct {
    VMObject header;   // MUST be first
    ObjKind kind;
    size_t slot_count; // Logical number of elements/fields.
    size_t data_bytes; // Payload size in bytes; legacy objects use slot_count * 8.
    size_t element_size; // Array element width; legacy objects use 8.
    CDtor cleanup;
    uint8_t data[]; // Payload
} ObjRaw;



VMObject* ownership_allocate(struct VMContext* vm, size_t size, TypeIDEnum type);
ObjString* intern_string(struct VMContext* vm, const char* chars, size_t length);
void ownership_destroy(struct VMContext* vm, VMObject* object);
VMObject* ownership_clone(struct VMContext* vm, const VMObject* object);
void ownership_destroy_all(struct VMContext* vm);
void ownership_claim(struct VMContext* vm, VMObject* object);
void ownership_unclaim(struct VMContext* vm, VMObject* object);
void ownership_destroy_unclaimed(struct VMContext* vm);
void ownership_destroy_unclaimed_except(struct VMContext* vm, const VMObject* keep);
size_t ownership_live_objects(const struct VMContext* vm);

#endif // CI_OWNERSHIP_H
