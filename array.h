#pragma once

/* Define a classNode to represent a dynamic array */
typedef struct {
    void* data; /* pointer to the array data */
    int count; /* current number of elements in the array */
    int capacity; /* maximum number of elements the array can hold */
    int elem_size; /* count of each element in bytes */
} array;

void array_init(array* arr);
array array_create(int elem_size);
void array_destroy(array* arr);
void array_resize(array* arr, int new_capacity);
int array_size(array* arr);
void array_append(array* arr, void* value);
void array_insert(array* arr, int index, void* value);
void array_remove(array* arr, int index);
void* array_get(array* arr, int index);
void array_set(array* arr, int index, void* value);
