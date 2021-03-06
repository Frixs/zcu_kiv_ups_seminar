//
// Created by frixs on 10/14/18.
//

#ifndef SERVER_MEMORY_H
#define SERVER_MEMORY_H

/// Custom malloc function for better debug memory allocation.
/// \param size     Allocate memory of that size.
/// \return         Pointer to the location the memory.
void *memory_malloc(size_t size, int c);

/// Custom free function for better debug memory allocation.
/// \param ptr      Free the pointer memory.
void memory_free(void *ptr, int c);

/// Print status of the memory.
void memory_print_status();

#endif //SERVER_MEMORY_H
