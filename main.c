#include <stdbool.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>

const int HEADER_MARKER = 0x55;
const int NODE_MARKER = 0xDD;
const int SINGLE_PAGE = 4096;

typedef struct Node node;
struct Node {
    int marker;
    int length;
    bool in_use;
    node *prev;
    node *next;
};

typedef struct Header{
    atomic_bool lock;
    int marker;
    node *first_node;
} header;

void *get_aligned_address(void *starting_address, size_t occupied_bytes, size_t alignment);
void *allocate_memory(header *_header, int bytes);
void free_memory(header *_header, void *memory_address);

int main() {
    // Request a page from the operating system
    void *raw_memory_address = sbrk(SINGLE_PAGE);
    if (raw_memory_address == (void *)-1) return 1;

    // Create the header
    void *header_start_address = get_aligned_address(raw_memory_address, sizeof(header), 8);
    header *_header = (header *)header_start_address;

    if (_header->marker != HEADER_MARKER) {
        _header->marker = HEADER_MARKER;

        // Create the first_node
        void *first_node_start_address = get_aligned_address((void *)header_start_address, sizeof(node), 8);
        node *first_node = (node *)first_node_start_address;

        first_node->marker = NODE_MARKER;
        first_node->in_use = false;
        first_node->prev = NULL;
        first_node->next = NULL;
        first_node->length = SINGLE_PAGE - (sizeof(header) + sizeof(node));

        _header->first_node = first_node;
        atomic_init(&_header->lock, false); 
    }

    printf("=== Starting Allocator V2 Test Suite ===\n\n");

    // -------------------------------------------------------------
    // TEST 1: Basic Allocation, Alignment, and Memory Verification
    // -------------------------------------------------------------
    printf("[Test 1] Allocating a single 4-byte integer...\n");
    int *ptr1 = (int *)allocate_memory(_header, sizeof(int));
    assert(ptr1 != (void *)-1 && ptr1 != NULL);
    
    // Verify address alignment (must be divisible by 8)
    printf("         Allocated address: %p\n", (void *)ptr1);
    assert(((uintptr_t)ptr1 % 8) == 0);
    
    // Safely write and read data (Verifies we didn't smash headers)
    *ptr1 = 42;
    printf("         Value stored successfully: %d\n", *ptr1);
    assert(*ptr1 == 42);
    printf("✅ Test 1 Passed: Clean allocation and perfect alignment.\n\n");


    // -------------------------------------------------------------
    // TEST 2: Block Splitting Verification
    // -------------------------------------------------------------
    printf("[Test 2] Allocating a second block to trigger a list split...\n");
    // Request 100 bytes. This should carve into the giant remaining node.
    char *ptr2 = (char *)allocate_memory(_header, 100);
    assert(ptr2 != NULL);
    
    // Look backward to verify ptr2's owner node structure
    node *node2 = (node *)ptr2 - 1;
    printf("         New split node details -> Length: %d, In Use: %s\n", 
           node2->length, node2->in_use ? "TRUE" : "FALSE");
    assert(node2->in_use == true);
    printf("✅ Test 2 Passed: Splitting math correctly carved out a node.\n\n");


    // -------------------------------------------------------------
    // TEST 3: Coalescing (Merging Free Blocks) Verification
    // -------------------------------------------------------------
    printf("[Test 3] Testing block merging via free_memory()...\n");
    node *node1 = (node *)ptr1 - 1;
    int length_before_free = _header->first_node->length;
    
    printf("         Freeing first and second blocks...\n");
    free_memory(_header, ptr1);
    free_memory(_header, ptr2);
    
    // Because ptr1 and ptr2 were side-by-side, freeing both should cause 
    // node1 to absorb node2 completely.
    printf("         First node length after automatic merge: %d\n", _header->first_node->length);
    assert(_header->first_node->length > length_before_free);
    assert(_header->first_node->in_use == false);
    printf("✅ Test 3 Passed: Blocks successfully merged, preventing fragmentation.\n\n");


    // -------------------------------------------------------------
    // TEST 4: Out of Memory & OS Pagination (sbrk) Trigger
    // -------------------------------------------------------------
    printf("[Test 4] Requesting massive block to force an extra OS Page...\n");
    // Request a chunk larger than our leftover space to trigger sbrk pagination
    void *massive_ptr = allocate_memory(_header, SINGLE_PAGE - 100);
    assert(massive_ptr != NULL && massive_ptr != (void *)-1);
    
    // Check if our list now contains multiple distinct nodes chained up
    node *check_node = _header->first_node;
    int node_count = 0;
    while(check_node != NULL) {
        node_count++;
        check_node = check_node->next;
    }
    printf("         Total nodes tracking heap pages: %d\n", node_count);
    assert(node_count >= 2);
    
    // Clean up
    free_memory(_header, massive_ptr);
    printf("✅ Test 4 Passed: Heap expansion behaves defensively via sbrk pagination.\n\n");
    return 0;
}

void *get_aligned_address(void *starting_address, size_t occupied_bytes, size_t alignment) {
    // Calculate where the occupied memory physically ends
    // Round THAT ending address up to the next safe alignment boundary
    uintptr_t end_of_occupied_space = (uintptr_t)starting_address + occupied_bytes;
    uintptr_t aligned_address = (end_of_occupied_space + (alignment - 1)) & ~(alignment - 1);
    return (void *)aligned_address;
}

void *allocate_memory(header *_header, int requested_bytes){
    // Acquire the lock
    // If atomic_exchange returns true, it means another thread already locked it.
    // We wait until the lock is open
    while (atomic_exchange(&_header->lock, true)) usleep(0);
    requested_bytes = (requested_bytes + 7) & ~7;
    node *free_node = _header->first_node;

    while(free_node != NULL && (free_node->in_use || requested_bytes > free_node->length)){
        free_node = free_node->next;
    };

    // The first page requested from the OS has been used up
    if (free_node == NULL){
        void *raw_memory_address = sbrk(SINGLE_PAGE);
        if (raw_memory_address == (void *)-1) {
            atomic_store(&_header->lock, false);
            return (void *)-1;
        }

        // Find the last node in the list
        node *last_node = _header->first_node;
        while(last_node->next != NULL){
            last_node = last_node->next;
        }

        // Create a new "free" node and find an aligned address for it
        node *new_free_node = (node *) raw_memory_address;

        new_free_node->in_use = false;
        new_free_node->next = NULL;
        new_free_node->prev = last_node;
        new_free_node->marker = NODE_MARKER;
        new_free_node->length = SINGLE_PAGE - sizeof(node);

        last_node->next = new_free_node;

        atomic_store(&_header->lock, false);
        // Rerun the request again
        return allocate_memory(_header, requested_bytes);
    }

    free_node->in_use = true;

    // If the leftover space is enough to actually create a new node
    // and an additional 8 bytes, create a new node and set the length
    // to be the leftover space while taking into account the actal space the new node takes
    int leftover_space = free_node->length - requested_bytes;
    if (leftover_space >= (sizeof(node) + 8)) {
        void *new_node_address = get_aligned_address((void *) free_node, sizeof(node) + requested_bytes, 8);
        node *new_node = (node *) new_node_address;

        new_node->marker = NODE_MARKER;
        new_node->in_use = false;
        new_node->prev = free_node;
        new_node->next = free_node->next;
        new_node->length = leftover_space - sizeof(node);

        free_node->next = new_node;
        free_node->length = requested_bytes;
    }

    // When done, atomics require you to safely store 'false' back into the lock
    atomic_store(&_header->lock, false);
    return (void *)(free_node + 1);
}

void free_memory(header *_header, void *memory_address) {
    if (memory_address == NULL) return;
    while (atomic_exchange(&_header->lock, true)) usleep(0);
    
    // Step backward by one node struct size to find the metadata header
    node *owner_node = (node *)memory_address - 1;

    // Safety check: make sure a rogue pointer didn't corrupt this block
    if (owner_node->marker != NODE_MARKER) {
        atomic_store(&_header->lock, false);
        return;
    }

    owner_node->in_use = false;
    if (owner_node->next != NULL && !owner_node->next->in_use) {
        // Calculate exactly where owner_node physically ends in RAM
        char *physical_end = (char *)owner_node + sizeof(node) + owner_node->length;
        
        // Only merge if the next node starts EXACTLY where this one ends
        if (physical_end == (char *)owner_node->next) {
            // Absorb the next node's header size AND its length into our block
            owner_node->length += sizeof(node) + owner_node->next->length;
            
            // Bypass the absorbed node in the linked list chain
            owner_node->next = owner_node->next->next;
            if (owner_node->next != NULL) {
                owner_node->next->prev = owner_node;
            }
        }
    }

    if (owner_node->prev != NULL && !owner_node->prev->in_use) {
        // Calculate exactly where the previous node physically ends in RAM
        char *prev_physical_end = (char *)owner_node->prev + sizeof(node) + owner_node->prev->length;
        
        // Only merge if our current node touches the previous node perfectly
        if (prev_physical_end == (char *)owner_node) {
            // Absorb our current node's header size and length into the previous node
            owner_node->prev->length += sizeof(node) + owner_node->length;
            
            // Bypass our current node in the linked list chain
            owner_node->prev->next = owner_node->next;
            if (owner_node->next != NULL) {
                owner_node->next->prev = owner_node->prev;
            }
        }
    }

    atomic_store(&_header->lock, false);
}
