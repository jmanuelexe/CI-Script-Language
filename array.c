#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "array.h"

/* Define a macro for the initial capacity of the array */
#define INIT_CAPACITY 16

/* Define a macro for the growth factor of the array */
#define GROWTH_FACTOR 2

/* Define a macro for the shrink factor of the array */
#define SHRINK_FACTOR 4

/* Define a macro for getting the pointer to an element at a given index */
#define ARRAY_GET_PTR(arr, index) ((char *)(arr)->data + (index) * (arr)->elem_size)

void array_init(array* arr) {
	arr->data = NULL;
	arr->count = 0;
	arr->capacity = 0;
	arr->elem_size = 0;
}

/* Create a new dynamic array with a given element count */
array array_create(int elem_size) {
    /* Allocate memory for the array classNode */
    array arr;
    
    /* Allocate memory for the array data */
    arr.data = malloc(INIT_CAPACITY * elem_size);
    if (arr.data == NULL) {
        fprintf(stderr, "array_create() error: memory allocation failed.\n");
        exit(EXIT_FAILURE);
    }

    /* Initialize the count, capacity and elem_size fields */
    arr.count = 0;
    arr.capacity = INIT_CAPACITY;
    arr.elem_size = elem_size;
    /* Return the pointer to the array classNode */
    return arr;
}

/* Destroy a dynamic array and free its memory */
void array_destroy(array * arr) {
    /* Check if the pointer is not NULL */
    if (arr != NULL && arr->data) {
        /* Free the array data */
        free(arr->data);
        /* Free the array classNode */
        //free(arr);
        array_init(arr); // Reset the array to a safe state
    }
}

/* Resize a dynamic array to a new capacity */
void array_resize(array * arr, int new_capacity) {
    /* Check if the pointer is not NULL */
    if (arr != NULL) {
        /* Check if the new capacity is valid */
        if (new_capacity > 0) {
            /* Allocate memory for the new array data */
            void* new_data = realloc(arr->data, new_capacity * arr->elem_size);
            if (new_data == NULL) {
                fprintf(stderr, "array_resize() error: memory allocation failed.\n");
                exit(EXIT_FAILURE);
            }
            /* Update the pointer, capacity and count fields */
            arr->data = new_data;
            arr->capacity = new_capacity;
            if (arr->count > new_capacity) {
                arr->count = new_capacity;
            }
        }
        else {
            fprintf(stderr, "array_resize() error: invalid capacity.\n");
        }
    }
}

int array_size(array* arr) {
	/* Check if the pointer is not NULL */
	if (arr != NULL) {
		/* Return the count field */
		return arr->count;
	}
	else {
		fprintf(stderr, "array_size() error: null pointer.\n");
		return -1; /* return an invalid count */
	}
}

/* Append an element to the end of a dynamic array */
void array_append(array * arr, void* value) {
    /* Check if the pointer is not NULL */
    if (arr != NULL) {
        /* Check if the array is full */
        if (arr->count == arr->capacity) {
            /* Double the capacity of the array using bitwise left shift operation */
            array_resize(arr, arr->capacity << 1);
        }
        /* Copy the value at the end of the array using memcpy and pointer arithmetic */
        memcpy(ARRAY_GET_PTR(arr, arr->count), value, arr->elem_size);
        /* Increment the count field using bitwise increment operation */
        ++(arr->count);
    }
}

/* Insert an element at a given position in a dynamic array */
void array_insert(array * arr, int index, void* value) {
    /* Check if the pointer is not NULL */
    if (arr != NULL) {
        /* Check if the index is valid */
        if (index >= 0 && index <= arr->count) {
            /* Check if the array is full */
            if (arr->count == arr->capacity) {
                /* Double the capacity of the array using bitwise left shift operation */
                array_resize(arr, arr->capacity << 1);
            }
            /* Shift the elements from index to count-1 to the right by one position using memmove and pointer arithmetic */
            memmove(ARRAY_GET_PTR(arr, index + 1),
                ARRAY_GET_PTR(arr, index),
                (arr->count - index) * arr->elem_size);
            /* Copy the value at the given index using memcpy and pointer arithmetic */
            memcpy(ARRAY_GET_PTR(arr, index), value, arr->elem_size);
            /* Increment the count field using bitwise increment operation */
            ++(arr->count);
        }
        else {
            fprintf(stderr, "array_insert() error: invalid index.\n");
        }
    }
}

/* Remove an element at a given position in a dynamic array */
void array_remove(array * arr, int index) {
    /* Check if the pointer is not NULL */
    if (arr != NULL) {
        /* Check if the index is valid */
        if (index >= 0 && index < arr->count) {
            /* Shift the elements from index+1 to count-1 to the left by one position using memmove and pointer arithmetic */
            memmove(ARRAY_GET_PTR(arr, index),
                ARRAY_GET_PTR(arr, index + 1),
                (arr->count - index - 1) * arr->elem_size);
            /* Decrement the count field using bitwise decrement operation */
            --(arr->count);
            /* Check if the array is less than a quarter full using bitwise right shift operation */
            if (arr->count < (arr->capacity >> 2)) {
                /* Halve the capacity of the array using bitwise right shift operation */
                array_resize(arr, arr->capacity >> 1);
            }
        }
        else {
            fprintf(stderr, "array_remove() error: invalid index.\n");
        }
    }
}

/* Get the element at a given position in a dynamic array */
void* array_get(array * arr, int index) {
    /* Check if the pointer is not NULL */
    if (arr != NULL) {
        /* Check if the index is valid */
        if (index >= 0 && index < arr->count) {
            /* Return the pointer to the value at the given index using pointer arithmetic */
            return ARRAY_GET_PTR(arr, index);
        }
        else {
            fprintf(stderr, "array_get() error: invalid index.\n");
            return NULL; /* return a null pointer */
        }
    }
    else {
        fprintf(stderr, "array_get() error: null pointer.\n");
        return NULL; /* return a null pointer */
    }
}

/* Set the element at a given position in a dynamic array */
void array_set(array * arr, int index, void* value) {
    /* Check if the pointer is not NULL */
    if (arr != NULL) {
        /* Check if the index is valid */
        if (index >= 0 && index < arr->count) {
            /* Copy the value at the given index using memcpy and pointer arithmetic */
            memcpy(ARRAY_GET_PTR(arr, index), value, arr->elem_size);
        }
        else {
            fprintf(stderr, "array_set() error: invalid index.\n");
        }
    }
}