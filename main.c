// 1) Create the global gatekeeper

// This is essentially at the very beginning even before our structure. It acts
// as the entry point before requesting some memory, that way it can prevent potential
// issues that would typically arise in a multi-threaded program. 

// By going through this gatekeeper before requesting some memory, we can implement
// a "lock". If that lock is active it means another thread is already handling
// a request for memory in the strcture and the thread that meets this lock has
// to wait until it's false before allocating some new memory.

// The gatekeeper requires these things:
// A) Marker - A fixed number which is a way to know the beginning of everything. 
//             It proves that the entire memory allocator has been successfully initialized 
//             for the first time when a program is booted up.

// B) Lock - A boolean value that prevents any other threads from executing a memory request
//           until it's set to false.


// 2) Start the doubly linked list structure

// This is what get's created everytime malloc() is called and
// we expect every requested memory to be back to back kind of like
// an array. That would be the case in a perfect world but in this scenario:
// [GateKeeper] | [ Block 1 (In use) ] ---> [ Block 2 (In use) ] --> [ Block 3 (In use) ]

// If a program calls free([address of block 2]) and intends to free up block 2 the chain starts
// dissapearing: 

// [GateKeeper] | [ Block 1 (In use) ] ---> [ Block 2 (FREE) ] --> [ Block 3 (In use) ]

// since each block stores pointers to the next block and to the previous block we'd need
// the connection to remain between block 1 and block 3. If someone then requests memory
// smaller than the space left by the freed Block 2, our allocator needs to somehow do
// some maths and calculate everything for the connection to remain. 


// NOTE: each block contains a header that stores metadata as follows:
// A) Marker - A fixed number that protects individual memory blocks down the chain.
//             If free() is called and everything starts becoming fragmented, it ensures
//              that nobody's stray pointer or bug overwrote the metadata of that exact chunk of RAM.

// B) Previous block pointer - Memory address of the block before it.

// C) Next block pointer - Memory address of the block after it.

// D) Length - How many bytes were requested?

// E) InUse - A boolean that determines if block is free or not

// NOTE: A "Block" dosen't mean the actual block of memory in the explanation. 
// a block means a header (which contains metadata) + bytes requested.

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>

const int GATEKEEPER_MARKER = 0x55;
const int NODE_MARKER = 0xDD;
const int SINGLE_PAGE = 4096;

typedef struct Node node;
struct Node{
    int marker;
    int length;
    bool in_use;
    node *prev_node;
    node *next_node;
};

typedef struct GateKeeper{
    int marker;
    bool lock;
    node *start_node;
} gatekeeper;

gatekeeper *create_gatekeeper(void *heap_start_address);
void *allocate_memory(gatekeeper *gatekeeper_address, int size);
void free_memory(void *first_memory_block_address);

int main (void){
    void *raw_memory_address = sbrk(SINGLE_PAGE);
    void *heap_start_address = NULL;

    if (raw_memory_address == (void*)-1) return 1;
    if (heap_start_address == NULL) heap_start_address = raw_memory_address;

    gatekeeper *gate_keeper = create_gatekeeper(heap_start_address);
    if (gate_keeper == NULL) return 1;

    int *requested_memory_start_address = allocate_memory(gate_keeper, sizeof(int));
    *requested_memory_start_address = 60;
    free_memory(requested_memory_start_address);
    return 0;
}

gatekeeper *create_gatekeeper(void *heap_start_address){
    gatekeeper *gate_keeper = (gatekeeper *)heap_start_address;

    if (gate_keeper->marker != GATEKEEPER_MARKER){

        gate_keeper->marker = GATEKEEPER_MARKER;
        gate_keeper->lock = false;

        node *initial_node_address = (node *)(gate_keeper + 1);;
        initial_node_address->in_use = false;
        initial_node_address->marker = NODE_MARKER;
        initial_node_address->next_node = NULL;
        initial_node_address->prev_node = NULL;

        uint8_t total_used_bytes = sizeof(gatekeeper) + sizeof(node);
        initial_node_address->length = SINGLE_PAGE - total_used_bytes;

        gate_keeper->start_node = initial_node_address;
        return gate_keeper;
    }

    return NULL;
}

void *allocate_memory(gatekeeper *gatekeeper_address, int size){
    gatekeeper_address->lock = true;

    node *start_node_address = gatekeeper_address->start_node;
    node *curr_node_address = start_node_address;

    while(curr_node_address != NULL && (curr_node_address->in_use || curr_node_address->length < size)){
        curr_node_address = curr_node_address->next_node;
    }

    if (curr_node_address == NULL) {
        gatekeeper_address->lock = false;

        void *new_raw_memory_address = sbrk(SINGLE_PAGE);
        if (new_raw_memory_address == (void*)-1) return NULL;

        node *last_existing_node_address = start_node_address;

        while(last_existing_node_address->next_node != NULL){
            last_existing_node_address  = last_existing_node_address->next_node;
        };

        node *new_node = (node *)new_raw_memory_address;
        new_node->in_use = false;
        new_node->marker = NODE_MARKER;
        new_node->next_node = NULL;
        new_node->prev_node = last_existing_node_address;
        new_node->length = SINGLE_PAGE - sizeof(node);

        last_existing_node_address->next_node = new_node;
        return allocate_memory(gatekeeper_address, size);
    };

    curr_node_address->in_use = true;

    if (curr_node_address->next_node == NULL){
        node *new_node = (node *) ((char *)curr_node_address + sizeof(node) + size);
        new_node->in_use = false;
        new_node->marker = NODE_MARKER;
        new_node->next_node = NULL;
        new_node->prev_node = curr_node_address;
        new_node->length = curr_node_address->length - size;

        curr_node_address->length = size;
        curr_node_address->next_node = new_node;
    }

    else {
        int leftover_space = curr_node_address->length - size;
        curr_node_address->length = size;
    
        if (leftover_space > sizeof(node)){
            node *new_node = (node *)((char *)curr_node_address + sizeof(node) + size);
            new_node->marker = NODE_MARKER;
            new_node->in_use = false;
            new_node->next_node = curr_node_address->next_node;
            new_node->prev_node = curr_node_address;
            new_node->length = leftover_space;
    
            curr_node_address->next_node = new_node;
        }
    }

    gatekeeper_address->lock = false;
    return curr_node_address + 1;
}

void free_memory(void *first_memory_block_address){
    node *owner_node_address = (node *) first_memory_block_address - 1;
    if (owner_node_address->marker == NODE_MARKER) owner_node_address->in_use = false;
}