/*
 * Created by Ivo Georgiev on 2/9/16.
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h> // for perror()

#include <memory.h>// for memcpy()
#include "mem_pool.h"

/*************/
/*           */
/* Constants */
/*           */
/*************/
static const float      MEM_FILL_FACTOR                 = 0.75;
static const unsigned   MEM_EXPAND_FACTOR               = 2;

static const unsigned   MEM_POOL_STORE_INIT_CAPACITY    = 20;
static const float      MEM_POOL_STORE_FILL_FACTOR      = 0.75;
static const unsigned   MEM_POOL_STORE_EXPAND_FACTOR    = 2;

static const unsigned   MEM_NODE_HEAP_INIT_CAPACITY     = 40;
static const float      MEM_NODE_HEAP_FILL_FACTOR       = 0.75;
static const unsigned   MEM_NODE_HEAP_EXPAND_FACTOR     = 2;

static const unsigned   MEM_GAP_IX_INIT_CAPACITY        = 40;
static const float      MEM_GAP_IX_FILL_FACTOR          = 0.75;
static const unsigned   MEM_GAP_IX_EXPAND_FACTOR        = 2;



/*********************/
/*                   */
/* Type declarations */
/*                   */
/*********************/
typedef struct _alloc {
    char *mem;
    size_t size;
} alloc_t, *alloc_pt;

typedef struct _node {
    alloc_t alloc_record;
    unsigned used;
    unsigned allocated;
    struct _node *next, *prev; // doubly-linked list for gap deletion
} node_t, *node_pt;

typedef struct _gap {
    size_t size;
    node_pt node;
} gap_t, *gap_pt;

typedef struct _pool_mgr {
    pool_t pool;
    node_pt node_heap;
    unsigned total_nodes;
    unsigned used_nodes;
    gap_pt gap_ix;
    unsigned gap_ix_capacity;
} pool_mgr_t, *pool_mgr_pt;



/***************************/
/*                         */
/* Static global variables */
/*                         */
/***************************/
static pool_mgr_pt *pool_store = NULL; // an array of pointers, only expand
static unsigned pool_store_size = 0;
static unsigned pool_store_capacity = 0;



/********************************************/
/*                                          */
/* Forward declarations of static functions */
/*                                          */
/********************************************/
static alloc_status _mem_resize_pool_store();
static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr);
static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr);
static alloc_status
        _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                           size_t size,
                           node_pt node);
static alloc_status
        _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                size_t size,
                                node_pt node);
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr);
static alloc_status _mem_invalidate_gap_ix(pool_mgr_pt pool_mgr);



/****************************************/
/*                                      */
/* Definitions of user-facing functions */
/*                                      */
/****************************************/
alloc_status mem_init() {
    // ensure that it's called only once until mem_free
    // note: holds pointers only, other functions to allocate/deallocate
    if (pool_store == NULL) {
        // allocate the pool store with initial capacity
        pool_store = (pool_mgr_pt*) calloc(MEM_POOL_STORE_INIT_CAPACITY, sizeof(pool_mgr_pt));
        pool_store_capacity = MEM_POOL_STORE_INIT_CAPACITY;
        return ALLOC_OK; // memory has (hopefully) been allocated
    }
    else {
        // if pool_store != NULL, mem_init() was called again before mem_free (bad)
        return ALLOC_CALLED_AGAIN;
    }
}

alloc_status mem_free() {
    // ensure that it's called only once for each mem_init
    // if pool_store == NULL then we called mem_free when already freed
    if (pool_store == NULL) {
        return ALLOC_CALLED_AGAIN;
    }
    // make sure all pool managers have been deallocated
    // if an entry in the pool store is not null, not freed
    for (int i = 0; i < pool_store_size; ++i) {
        if (pool_store[i] != NULL) {
            return ALLOC_NOT_FREED;
        }
    }
    // can free the pool store array
    free(pool_store);
    // update static variables, zero out and nullify pool_store
    pool_store_capacity = 0;
    pool_store_size = 0;
    pool_store = NULL;

    return ALLOC_OK; //everything might have worked..
}

pool_pt mem_pool_open(size_t size, alloc_policy policy) {
    // make sure there the pool store is allocated
    if (pool_store == NULL) { // no pool_store has yet been allocated
        return NULL;
    }
    // expand the pool store, if necessary

     alloc_status ret_status = _mem_resize_pool_store();
     assert(ret_status == ALLOC_OK); //end program if alloc NOT ok
     if (ret_status != ALLOC_OK) {
         return NULL; //need to expand the pool store
     }

    // allocate a new mem pool mgr
    pool_mgr_pt new_pmgr = (pool_mgr_pt) calloc(1, sizeof(pool_mgr_t));
    // check success, on error return null
    assert(new_pmgr);
    if (new_pmgr == NULL) {
        return NULL;
    }
    // allocate a new memory pool
    void * new_mem = malloc(size);
    // allocate mem, set all parameters
    new_pmgr->pool.mem = new_mem;   //mem holds size bytes
    new_pmgr->pool.policy = policy;
    new_pmgr->pool.total_size = size;
    new_pmgr->pool.num_allocs = 0;  // no nodes have been allocated
    new_pmgr->pool.num_gaps = 1;    // the entire thing is a gap
    new_pmgr->pool.alloc_size = 0;  // pool has nothing allocated
    // check success, on error deallocate mgr and return null
    assert(new_pmgr->pool.mem);
    // some error occurred, the pool was not allocated
    if (new_pmgr->pool.mem == NULL) {
        free(new_pmgr);
        new_pmgr = NULL;
        return NULL;
    }
    // allocate a new node heap
    node_pt new_nheap = (node_pt) calloc(MEM_NODE_HEAP_INIT_CAPACITY, sizeof(node_t));
    // check success, on error deallocate mgr/pool and return null
    assert(new_nheap);
    if (new_nheap == NULL) {
        free(new_pmgr);
        new_pmgr = NULL;
        free(new_mem);
        new_mem = NULL;
        return NULL;
    }
    // allocate a new gap index
    gap_pt new_gapix = (gap_pt) calloc(MEM_GAP_IX_INIT_CAPACITY, sizeof(gap_t));
    // check success, on error deallocate mgr/pool/heap and return null
    //assert(new_gapix);
    if (new_gapix == NULL) {
        free(new_pmgr);
        new_pmgr = NULL;
        free(new_mem);
        new_mem = NULL;
        free(new_nheap);
        new_nheap = NULL;
        return NULL;
    }
    // assign all the pointers and update meta data:
    new_pmgr->node_heap = new_nheap;
    new_pmgr->total_nodes = MEM_NODE_HEAP_INIT_CAPACITY;
    new_pmgr->used_nodes = 1;     //just the 1 gap
    new_pmgr->gap_ix = new_gapix;
    new_pmgr->gap_ix_capacity = MEM_GAP_IX_INIT_CAPACITY;

    //   initialize top node of node heap
    new_pmgr->node_heap[0].alloc_record.size = size;
    new_pmgr->node_heap[0].alloc_record.mem = new_mem;
    new_pmgr->node_heap[0].used = 1;
    new_pmgr->node_heap[0].allocated = 0;
    new_pmgr->node_heap[0].next = NULL;
    new_pmgr->node_heap[0].prev = NULL;
    //   initialize top node of gap index
    new_pmgr->gap_ix[0].size = size;
    new_pmgr->gap_ix[0].node = new_pmgr->node_heap;

    //   initialize pool mgr
    //   link pool mgr to pool store
    // find the first empty position in the pool_store
    // and link the new pool mgr to that location
    for(int i = 0; i < pool_store_size; ++i) {
        if (pool_store[i] == NULL) {
            pool_store[i] = new_pmgr;
            ++pool_store_size;
            break;
        }
    }
    // return the address of the mgr, cast to (pool_pt)
    return (pool_pt)new_pmgr;
}

alloc_status mem_pool_close(pool_pt pool) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    // possible because pool is at the top of the pool_mgr_t structure
    pool_mgr_pt new_pmgr = (pool_mgr_pt) pool;
    
    // check if this pool is allocated
    // check if it has zero allocations
    // check if pool has only one gap
    if (
       (new_pmgr == NULL) ||
       (pool->num_gaps > 1) ||
       (pool->num_gaps == 0) ||
       pool->num_allocs >= 1) {
        return ALLOC_NOT_FREED;
    }
    // free memory pool
    free(new_pmgr->pool.mem);
    new_pmgr->pool.mem = NULL;

    // free node heap
    free(new_pmgr->node_heap);
    new_pmgr->node_heap = NULL;

    // free gap index
    free(new_pmgr->gap_ix);
    new_pmgr->gap_ix = NULL;

    // find mgr in pool store and set to null
    for(int i = 0; i < pool_store_size; ++i) {
        if (pool_store[i] == new_pmgr) {
            pool_store[i] = NULL;
            break;
        }
    }
    // note: don't decrement pool_store_size, because it only grows
    // free mgr
    free(new_pmgr);
    new_pmgr = NULL;
    return ALLOC_OK;
}

void * mem_new_alloc(pool_pt pool, size_t size) {
    
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt new_pmgr = (pool_mgr_pt) (pool);
    
    // check if any gaps, return null if none
    if (new_pmgr->pool.num_gaps == 0) {
        return NULL;
    }
    // expand heap node, if necessary, quit on error
    //TODO see if this works
    alloc_status result =_mem_resize_node_heap(new_pmgr);

    assert(result == ALLOC_OK);
    // check used nodes fewer than total nodes, quit on error
    if (new_pmgr->used_nodes > new_pmgr->total_nodes) {
        return NULL;
    }
    
    node_pt new_alloc = NULL;
    if (pool->policy == FIRST_FIT) {
        
        // assumes node_heap[0] is the head ptr
        new_alloc = new_pmgr->node_heap;
        for (int i = 0; i < new_pmgr->total_nodes; ++i) {
            
            // Used: 1, Allocated: 0 indicates a gap
            // looking for gap who's size is > than our needed size
            if (new_alloc->used == 1 && new_alloc->allocated == 0 // BREAK
                && size <= new_alloc->alloc_record.size) { // found gap
                // use new_alloc->allocated to signal success below
                new_alloc->allocated = 1;
                break;
            }
            
            new_alloc = new_alloc->next;
            if (new_alloc == NULL) {
                return NULL;
            }
        }
        
    } else if (pool->policy == BEST_FIT) {
        
        // gaps will be sorted according to size available
        for (int i = 0; i < pool->num_gaps; ++i) {
            if (size <= new_pmgr->gap_ix[i].size) { // found gap
                new_alloc = new_pmgr->gap_ix[i].node;
                // use new_alloc->allocated to signal success below
                new_alloc->allocated = 1;
                break;
            }
        }
    }
    
    // TODO assert
    // if we exit all the way without finding an unused node, we will need a bigger node
    // heap. that should be taken care of above, so I should also add an assert here
    
    if (new_alloc == NULL || !new_alloc->allocated) { //the node was not found
        return NULL;
    }
    
    // At this point, new_alloc is a gap, and
    // new_alloc->alloc_record.size; is the size of the gap
    
    // update metadata (num_allocs, alloc_size)
    pool->num_allocs += 1;
    pool->alloc_size += size;
    
    size_t remaining_gap = new_alloc->alloc_record.size - size;
    _mem_remove_from_gap_ix(new_pmgr, new_alloc->alloc_record.size, new_alloc);
    
    // TODO clarify what to do to the node here
    // convert gap_node to an allocation node of given size
    new_alloc->alloc_record.size = size;
    
    if (remaining_gap) {
        
        node_pt new_gap = NULL;
        for (int i = 0; i < new_pmgr->total_nodes; ++i) {
            
            if (!new_pmgr->node_heap[i].used) {
                
                // found an unused node in the node heap
                new_gap = &new_pmgr->node_heap[i];
                new_gap->used = 1;
                new_gap->alloc_record.mem = NULL;
                new_gap->alloc_record.size = remaining_gap;
                new_gap->allocated = 0;
                
                if (new_alloc->next != NULL) {
                    new_alloc->next->prev = new_gap;
                }
                new_gap->next = new_alloc->next;
                new_alloc->next = new_gap;
                new_gap->prev = new_alloc;
                new_pmgr->used_nodes += 1;
                break;
            }
        }
        
        alloc_status status = _mem_add_to_gap_ix(new_pmgr, remaining_gap, new_gap);
        
        if (status == ALLOC_FAIL) {
            return NULL;
        }
    }
    
    return (alloc_pt) new_alloc;
}

alloc_status mem_del_alloc(pool_pt pool, void* alloc) {
    
    pool_mgr_pt new_pmgr = (pool_mgr_pt) pool;
    node_pt node_handle = (node_pt) alloc;
    
    // find the node to delete in the node heap
    unsigned del_index = 0;
    while (del_index < new_pmgr->total_nodes) {
        
        if (new_pmgr->node_heap[del_index].alloc_record.mem == node_handle->alloc_record.mem) {
            break;
        }
        ++del_index;
    }
    
    // make sure it's found
    if (del_index == new_pmgr->total_nodes) {
        return ALLOC_FAIL;
    }
    
    // convert to gap node
    // allocated = 0 indicates a gap node
    node_handle->allocated = 0;
    
    // update metadata (num_allocs, alloc_size)
    --pool->num_allocs;
    pool->alloc_size -= node_handle->alloc_record.size;
    
    // if the next node in the list is also a gap, merge into node handle
    if ((node_handle->next != NULL) &&
        (node_handle->next->allocated == 0)) {
        
        _mem_remove_from_gap_ix(new_pmgr,
            node_handle->next->alloc_record.size,
            node_handle->next);
        
        // add the sizes
        // update node as unused
        // update metadata (used nodes)
        node_handle->alloc_record.size += node_handle->next->alloc_record.size;
        node_handle->next->used = 0;
        --new_pmgr->used_nodes;
        
        // update linked list:
        // IF next node has a continuing node, give
        // THAT node a new prev. We are merging the
        // node_handle->next INTO node_handle
        if (node_handle->next->next) {
            node_handle->next->next->prev = node_handle;
        }
        node_pt tmp = node_handle->next;
        node_handle->next = node_handle->next->next;
        tmp->next = NULL;
        tmp->prev = NULL;
        tmp->alloc_record.size = 0;
    }
    
    // if the prev node in the list is also a gap, merge into node handle
    if ((node_handle->prev != NULL) &&
        (node_handle->prev->allocated == 0)) {
        
        alloc_status status = _mem_remove_from_gap_ix(new_pmgr,
            node_handle->prev->alloc_record.size,
            node_handle->prev);
        
        // add the sizes
        // update node as unused
        // update metadata (used nodes)
        node_handle->alloc_record.size += node_handle->prev->alloc_record.size;
        node_handle->prev->used = 0;
        --new_pmgr->used_nodes;
        
        // update linked list:
        // IF prev node has a continuing node, give
        // THAT node a new next. We are merging the
        // node_handle->prev INTO node_handle
        if (node_handle->prev->prev) {
            node_handle->prev->prev->next = node_handle;
        }
        node_pt tmp = node_handle->prev;
        node_handle->prev = node_handle->prev->prev;
        tmp->prev = NULL;
        tmp->next = NULL;
        tmp->alloc_record.size = 0;
    }
    
    alloc_status status = _mem_add_to_gap_ix(new_pmgr, node_handle->alloc_record.size, node_handle);
    
    if (status == ALLOC_FAIL) {
        return ALLOC_FAIL;
    }

    return ALLOC_OK;
}

void mem_inspect_pool(pool_pt pool,
                      pool_segment_pt *segments,
                      unsigned *num_segments) {
    // get the mgr from the pool
    pool_mgr_pt new_pmgr = (pool_mgr_pt) pool;

    // allocate the segments array with size == used_nodes
    pool_segment_pt new_seg_array = calloc(new_pmgr->used_nodes, sizeof(pool_segment_t));
    if (new_seg_array == NULL) {
        return;
    }
    
    node_pt it = NULL;
    
    // Find first used node in node heap.
    for (int i = 0; i < new_pmgr->total_nodes; ++i) {
        if (new_pmgr->node_heap[i].used) {
            it = &new_pmgr->node_heap[i];
            break;
        }
    }
    
    // Traverse to the beginning of the heap
    assert(it != NULL);
    while (it->prev != NULL) {
        it = it->prev;
    }
    
    for (int i = 0; i < new_pmgr->total_nodes; ++i) {
        new_seg_array[i].size = it->alloc_record.size;
        new_seg_array[i].allocated = it->allocated;
        it = it->next;
        if (it == NULL) {
            break;
        }
    }
    
    // "return" the values
    *segments = new_seg_array;
    *num_segments = new_pmgr->used_nodes;
}



/***********************************/
/*                                 */
/* Definitions of static functions */
/*                                 */
/***********************************/
static alloc_status _mem_resize_pool_store() {
    // check if necessary
    // cast to float for accurate math with float const MEM_POOL_STORE_FILL_FACTOR
    // using a cast to size_t, otherwise the float cannot fit in size_t
    if (((float) pool_store_size / pool_store_capacity) >= MEM_POOL_STORE_FILL_FACTOR) {
        pool_store = realloc(pool_store, (size_t) (pool_store_capacity *
                MEM_POOL_STORE_FILL_FACTOR * sizeof(pool_mgr_pt)));
        pool_store_capacity = pool_store_capacity * MEM_POOL_STORE_EXPAND_FACTOR;
        printf("Resizing pool store\n");
    }
    return ALLOC_OK;
}

static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr) {
    // see above
    if (((float) pool_mgr->used_nodes / pool_mgr->total_nodes) >=
            MEM_NODE_HEAP_FILL_FACTOR) {
        /* first we have to clear the gap index
         * this makes sure we don't have a bunch of invalid pointers
         * after moving the nodes to a new array.
         */
        printf("Resizing node heap\n");
        _mem_invalidate_gap_ix(pool_mgr);
        
        // allocate a new, expanded node heap
        node_pt new_heap = calloc(pool_mgr->total_nodes *
            MEM_NODE_HEAP_EXPAND_FACTOR, sizeof(node_t));
        
        // allocate a work node, use this to traverse each next pointer in the list
        node_pt work_node = pool_mgr->node_heap;
        
        // allocate the head pointer to easily copy over at the end
        node_pt head_node = new_heap;
        
        /*
         * Now we traverse the new and old node_heaps and
         * copy over each node one at a time using memcpy.
         * If a node is a gap (allocated = 0) we add it to the gap index
         */
        // TODO last left off here
        
        while (work_node != NULL) {
            memcpy(new_heap, work_node, sizeof(node_t));
            
            // if it's a gap add it to the gap index
            if (work_node->allocated == 0) {
                _mem_add_to_gap_ix(pool_mgr, new_heap->alloc_record.size, new_heap);
            }
            
            // move on to the next in the list
            work_node = work_node->next;
            new_heap = new_heap->next;
        }

        // update the capacity of the node heap and the head node.
        pool_mgr->total_nodes = pool_mgr->total_nodes * MEM_NODE_HEAP_EXPAND_FACTOR;
        pool_mgr->node_heap = head_node;
    }
    return ALLOC_OK;
}

static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr) {
    if (((float) pool_mgr->gap_ix->size / pool_mgr->gap_ix_capacity) >=
            MEM_GAP_IX_FILL_FACTOR) {
        pool_mgr->gap_ix = realloc(pool_mgr->gap_ix, (size_t) (pool_mgr->gap_ix_capacity *
                                                               MEM_GAP_IX_EXPAND_FACTOR * sizeof(pool_mgr_pt)));
        pool_mgr->gap_ix_capacity = pool_mgr->gap_ix_capacity * MEM_GAP_IX_EXPAND_FACTOR;
        printf("Resizing gap index\n");
    }
    return ALLOC_OK;
}

static alloc_status _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                                       size_t size,
                                       node_pt node) {
    // let the result be negative to start
    alloc_status result = ALLOC_FAIL;
    
    // This is the boundary edge. Note: it seems the previous expectation
    // was that _mem_resize_gap_ix could be called and that function would
    // perform whatever check necessary instead of doing this boundary check
    // here. I'm not sure of that though. (see the comment below)
    if (pool_mgr->pool.num_gaps == pool_mgr->gap_ix_capacity) {
        // expand the gap index, if necessary (call the function)
        result = _mem_resize_gap_ix(pool_mgr);
        // assert(result == ALLOC_OK);
        if (result != ALLOC_OK) {
            return ALLOC_FAIL;
        }
    }
    
    // add the entry at the end
    // clarity: gap_ix[pool_mgr->pool.num_gaps] should refer to
    // the index just behind the last gap in the pool.
    // Set size and pointer to the node of this gap node
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].size = size;
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].node = node;
    // update metadata (num_gaps)
    ++pool_mgr->pool.num_gaps;
    
    // sort the gap index (call the function)
    result = _mem_sort_gap_ix(pool_mgr);
    //assert(result == ALLOC_OK);
    // check success
    if (result != ALLOC_OK) {
        return ALLOC_FAIL;
    }
    return result;
}

static alloc_status _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                            size_t size,
                                            node_pt node) {
    assert(pool_mgr->pool.num_gaps != 0);
    // find the position of the node in the gap index, track that index
    unsigned i;
    for (i = 0; i < pool_mgr->pool.num_gaps; ++i) {
        if (pool_mgr->gap_ix[i].node == node) {
            break;
        }
    }
    // loop from there to the end of the array:
    //     pull the entries (i.e. copy over) one position up
    //     this effectively deletes the chosen node
    while (i < pool_mgr->pool.num_gaps - 1) {
        pool_mgr->gap_ix[i].size = pool_mgr->gap_ix[i+1].size;
        pool_mgr->gap_ix[i].node = pool_mgr->gap_ix[i+1].node;
        ++i;
    }
    // update metadata (num_gaps)
    --pool_mgr->pool.num_gaps;
    
    // zero out the element at position num_gaps!
    pool_mgr->gap_ix[i].size = 0;
    pool_mgr->gap_ix[i].node = NULL;
    
    // is there an ALLOC_FAIL case?
    return ALLOC_OK;
}

// note: only called by _mem_add_to_gap_ix, which appends a single entry
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr) {
    // the new entry is at the end, so "bubble it up"
    // loop from num_gaps - 1 until but not including 0:
    for (int i = pool_mgr->pool.num_gaps - 1; i > 0; --i) {
        /* if the size of the current entry is less than the previous (u - 1)
         * or if the sizes are the same but the current entry points to a
         * node with a lower address of pool allocation address (node)
         * swap them (by copying) (remember to use a temporary variable)
         */
        if ((pool_mgr->gap_ix[i].size < pool_mgr->gap_ix[i-1].size)
                || ((pool_mgr->gap_ix[i].size == pool_mgr->gap_ix[i-1].size)
                && (pool_mgr->gap_ix[i].node <
                pool_mgr->gap_ix[i-1].node))) {
            gap_t tmp_gap = pool_mgr->gap_ix[i];
            pool_mgr->gap_ix[i] = pool_mgr->gap_ix[i-1];
            pool_mgr->gap_ix[i-1] = tmp_gap;
        }
    }
    return ALLOC_OK;
}

static alloc_status _mem_invalidate_gap_ix(pool_mgr_pt pool_mgr) {
    //create a gap_ix to work from, pointing to the gap_ix we're clearing
    gap_pt new_gap = pool_mgr->gap_ix;

    //iterate through gaps and clear the data and pointer
    for (int i = 0; i < pool_mgr->pool.num_gaps; ++i) {
        new_gap[i].node = NULL;
        new_gap[i].size = 0;
    }
    // make sure we got every gap (should be impossible not to)
    for (int i = 0; i < pool_mgr->pool.num_gaps; ++i) {
        if (new_gap[i].node != NULL || new_gap[i].size != 0) {
            return ALLOC_FAIL;
        }
    }
    printf("invalidating gap index\n");
    return ALLOC_OK;
}

