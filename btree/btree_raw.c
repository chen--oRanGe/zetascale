/************************************************************************
 * 
 *  btree_raw.c  Jan. 21, 2013   Brian O'Krafka
 * 
 * xxxzzz NOTES:
 *     - check all uses of "ret"
 *     - make sure that all btree updates are logged!
 *     - add doxygen comments for all functions
 *     - make asserts a compile time option
 *     - make sure that left/right node pointers are properly maintained
 *     - check insert_ptr arithmetic
 *     - optimize key search within a node?
 *     - how is logical_id_counter persisted?
 *     - where is rightmost updated?
 *     - what if multiple matches for a particular syndrome?  must be
 *       be able to return multiple matches--NO--MUST JUST CHECK MULTIPLE
 *       MATCHES INTERNALLY TO FIND EXACT MATCH
 *     - is there a need to support non-uniqueue keys?
 *       if not, must enforce uniqueness somehow
 *     - add upsert flag and support
 *     - add check that keylen/datalen are correct when using 
 *       size fixed keys/data
 *     - if btree_raw provides returned data/key buffer, a special btree_raw_free_buffer() call
 *       should be used to free the buffer(s).  This will allow optimized buffer management
 *       allocation/deallocation methpnode (that avoid malloc) in the future.
 *     - modularize l1cache stuff
 *     - add free buffer callback
 *     - add get buffer callback
 *     - optimize updates to manipulate btree node in a single operation
 *     - if updates decrease node size below a threshold, must coalesce nodes!!!
 *     - stash overflow key in overflow node(s)!
 *     - use "right-sized" FDF objects for overflow objects, without chaining fixed
 *       sized nodes!
 *     - add 'leftmost' pointers for use with leaf nodes for reverse scans and
 *       simplified update of 'rightmost' pointers
 *     - where is max key length enforced?
 *     - add stats
 *     - add upsert (set) function to btree_raw_write()
 *     - change chunk size in DRAM cache code to match btree node size!
 *     - improve object packing in b-tree nodes
 * 
 * Flavors of b-tree:
 *     - Syndrome search + variable sized keys with variable sized data (primary index)
 *       ((btree->flags & SYNDROME_INDEX) == 0)
 *         - non-leaf nodes: fixed length syndrom, no data
 *         - leaf nodes: fixed length syndrom + variable length key + variable length data
 *     - Variable sized keys with variable sized data (secondary indices)
 *       ((btree->flags & SECONDARY_INDEX) == 0)
 *         - non-leaf nodes: fixed length key, no data
 *         - leaf nodes: fixed length syndrom + variable length key + variable length data
 * 
 ************************************************************************/

//  This instantiates the stats string array
#define _INSTANTIATE_BTSTATS_STRINGS

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <inttypes.h>
#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include "btree_hash.h"
#include "btree_raw.h"
#include "btree_map.h"
#include "btree_pmap.h"
#include "btree_raw_internal.h"
#include "trxcmd.h"
#include <api/fdf.h>

//  Define this to include detailed debugging code
//#define DEBUG_STUFF
//#define BTREE_RAW_CHECK

#define W_UPDATE  1
#define W_CREATE  2
#define W_SET     3

#define MODIFY_TREE 1
#define META_COUNTER_SAVE_INTERVAL 100000

//  used to count depth of btree traversal for writes/deletes
__thread int _pathcnt;

//  used to hold key values during delete or write operations
__thread char      *_keybuf      = NULL;
__thread uint32_t   _keybuf_size = 0;

#define MAX_BTREE_HEIGHT 6400
__thread btree_raw_mem_node_t* modified_nodes[MAX_BTREE_HEIGHT];
__thread btree_raw_mem_node_t* referenced_nodes[MAX_BTREE_HEIGHT];
__thread btree_raw_mem_node_t* deleted_nodes[MAX_BTREE_HEIGHT];
__thread uint64_t modified_nodes_count=0, referenced_nodes_count=0, deleted_nodes_count=0;

__thread uint64_t dbg_referenced = 0;
uint64_t no_restart = 0, restart_cnt = 0, sets_cnt = 0, splits_cnt = 0, restart_rdlocked = 0;

static int Verbose = 0;

#define bt_err(msg, args...) \
    (bt->msg_cb)(0, 0, __FILE__, __LINE__, msg, ##args)
#define bt_warn(msg, args...) \
    (bt->msg_cb)(1, 0, __FILE__, __LINE__, msg, ##args)

#define zmemcpy(to_in, from_in, n_in)  \
{\
    uint64_t zi;\
    uint64_t zn = (n_in);\
    char  *zto = ((char *) to_in);\
    char  *zfrom = ((char *) from_in);\
    for (zi=0; zi<zn; zi++) {\
        *zto++ = *zfrom++;\
    }\
}

#define zmemmove(to_in, from_in, n_in)  \
{\
    uint64_t zi;\
    uint64_t zn = (n_in);\
    char  *zto = ((char *) to_in);\
    char  *zfrom = ((char *) from_in);\
    if (zto < zfrom) {\
	for (zi=0; zi<zn; zi++) {\
	    *zto++ = *zfrom++;\
	}\
    } else {\
        zto   += zn;\
        zfrom += zn;\
	for (zi=0; zi<zn; zi++) {\
	    *(--zto) = *(--zfrom);\
	}\
    }\
}

#define add_node_stats(bt, pn, s, c) \
{ \
    if (pn->flags & OVERFLOW_NODE) \
        __sync_add_and_fetch(&(bt->stats.stat[BTSTAT_OVERFLOW_##s]),c); \
    else if (pn->flags & LEAF_NODE) \
        __sync_add_and_fetch(&(bt->stats.stat[BTSTAT_LEAF_##s]),c); \
    else \
        __sync_add_and_fetch(&(bt->stats.stat[BTSTAT_NONLEAF_##s]),c); \
}

#define sub_node_stats(bt, pn, s, c) \
{ \
    if (pn->flags & OVERFLOW_NODE) \
        __sync_sub_and_fetch(&(bt->stats.stat[BTSTAT_OVERFLOW_##s]),c); \
    else if (pn->flags & LEAF_NODE) \
        __sync_sub_and_fetch(&(bt->stats.stat[BTSTAT_LEAF_##s]),c); \
    else \
        __sync_sub_and_fetch(&(bt->stats.stat[BTSTAT_NONLEAF_##s]),c); \
}

//#define DBG_PRINT
#ifdef DBG_PRINT
#define dbg_print_key(key, keylen, msg, ...) do { print_key_func(stderr, __FUNCTION__, __LINE__, key, keylen, msg, ##__VA_ARGS__); } while(0)
#define dbg_print(msg, ...) do { fprintf(stderr, "%x %s:%d " msg, (int)pthread_self(), __FUNCTION__, __LINE__, ##__VA_ARGS__); } while(0)
static void print_key_func(FILE *f, const char* func, int line, char* key, int keylen, char *msg, ...);
#else
#define dbg_print(msg, ...)
#define dbg_print_key(key, keylen, msg, ...)
#endif

//#define DEBUG_STUFF
#ifdef DEBUG_STUFF
static void dump_node(btree_raw_t *bt, FILE *f, btree_raw_node_t *n, char *key, uint32_t keylen);
static char *dump_key(char *key, uint32_t keylen);
#endif

#define vlnode_bytes_free(x) ((x)->insert_ptr - sizeof(btree_raw_node_t) - (x)->nkeys * sizeof(node_vlkey_t))
#define vnode_bytes_free(x) ((x)->insert_ptr - sizeof(btree_raw_node_t) - (x)->nkeys * sizeof(node_vkey_t))

static uint64_t get_syndrome(btree_raw_t *bt, char *key, uint32_t keylen);

static btree_status_t savepersistent( btree_raw_t *bt, int create);
static btree_status_t loadpersistent( btree_raw_t *);
static char *get_buffer(btree_raw_t *btree, uint64_t nbytes);
static void free_buffer(btree_raw_t *btree, char *buf);
static void free_node(btree_status_t *ret, btree_raw_t *btree, btree_raw_mem_node_t *n);

static btree_raw_mem_node_t *create_new_node(btree_raw_t *btree, uint64_t logical_id);
static btree_raw_mem_node_t *get_new_node(btree_status_t *ret, btree_raw_t *btree, uint32_t leaf_flags);
//static btree_raw_mem_node_t *get_new_node_low(btree_status_t *ret, btree_raw_t *btree, uint32_t leaf_flags, int ref);
btree_raw_mem_node_t *get_existing_node_low(btree_status_t *ret, btree_raw_t *btree, uint64_t logical_id, int ref);

static int init_l1cache(btree_raw_t *btree, uint32_t n_l1cache_buckets);
static void destroy_l1cache(btree_raw_t *bt);

static btree_status_t deref_l1cache(btree_raw_t *btree);
static void ref_l1cache(btree_raw_t *btree, btree_raw_mem_node_t *n);
static void delete_l1cache(btree_raw_t *btree, btree_raw_mem_node_t *n);
static void modify_l1cache_node(btree_raw_t *btree, btree_raw_mem_node_t *n);

static void lock_modified_nodes_func(btree_raw_t *btree, int lock);
#define lock_modified_nodes(btree) lock_modified_nodes_func(btree, 1)
#define unlock_modified_nodes(btree) lock_modified_nodes_func(btree, 0)

static void update_keypos(btree_raw_t *btree, btree_raw_node_t *n, uint32_t n_key_start);

typedef struct deldata {
    btree_raw_mem_node_t   *balance_node;
} deldata_t;

static void delete_key(btree_status_t *ret, btree_raw_t *btree, btree_raw_mem_node_t *x, char *key, uint32_t keylen, btree_metadata_t *meta, uint64_t syndrome);
static void delete_key_by_pkrec(btree_status_t *ret, btree_raw_t *btree, btree_raw_mem_node_t *x, node_key_t *pk_delete);
static btree_status_t btree_raw_write(struct btree_raw *btree, char *key, uint32_t keylen, char *data, uint64_t datalen, btree_metadata_t *meta, int write_type);

static int find_rebalance(btree_status_t *ret, btree_raw_t *btree, uint64_t this_id, uint64_t left_id, uint64_t right_id, uint64_t l_anchor_id, key_stuff_t *l_anchor_stuff, uint64_t r_anchor_id, key_stuff_t *r_anchor_stuff, int l_this_parent_in, int r_this_parent_in, char *key, uint32_t keylen, btree_metadata_t *meta, uint64_t syndrome);
static void collapse_root(btree_status_t *ret, btree_raw_t *btree, btree_raw_mem_node_t *old_root_node);
static int rebalance(btree_status_t *ret, btree_raw_t *btree, btree_raw_mem_node_t *this_node, uint64_t left_id, uint64_t right_id, uint64_t l_anchor_id, key_stuff_t *l_anchor_stuff, uint64_t r_anchor_id, key_stuff_t *r_anchor_stuff, int l_this_parent, int r_this_parent, btree_metadata_t *meta);

static int check_per_thread_keybuf(btree_raw_t *btree);
static void btree_raw_init_stats(struct btree_raw *btree, btree_stats_t *stats);
#ifdef DEBUG_STUFF
static void btree_raw_dump(FILE *f, struct btree_raw *btree);
#endif

#ifdef BTREE_RAW_CHECK
static void btree_raw_check(struct btree_raw *btree, char* func, char* key);
#endif

static void default_msg_cb(int level, void *msg_data, char *filename, int lineno, char *msg, ...)
{
    char     stmp[512];
    va_list  args;
    char    *prefix;
    int      quit = 0;

    va_start(args, msg);

    vsprintf(stmp, msg, args);
    strcat(stmp, "\n");

    va_end(args);

    switch (level) {
        case 0:  prefix = "ERROR";                quit = 1; break;
        case 1:  prefix = "WARNING";              quit = 0; break;
        case 2:  prefix = "INFO";                 quit = 0; break;
        case 3:  prefix = "DEBUG";                quit = 0; break;
        default: prefix = "PROBLEM WITH MSG_CB!"; quit = 1; break;
	    break;
    } 

    (void) fprintf(stderr, "%s: %s", prefix, stmp);
    if (quit) {
        exit(1);
    }
}

static int default_cmp_cb(void *data, char *key1, uint32_t keylen1, char *key2, uint32_t keylen2)
{
    if (keylen1 < keylen2) {
        return(-1);
    } else if (keylen1 > keylen2) {
        return(1);
    } else if (keylen1 == keylen2) {
        return(memcmp(key1, key2, keylen1));
    }
    assert(0);
    return(0);
}

//======================   INIT  =========================================

static void
l1cache_replace(void *callback_data, char *key, uint32_t keylen, char *pdata, uint64_t datalen)
{
    btree_raw_mem_node_t *n = (btree_raw_mem_node_t*)pdata;

    btree_raw_t *bt = callback_data;
    bt->trx_cmd_cb( TRX_CACHE_DEL, bt->write_node_cb_data, key);

    free_buffer((btree_raw_t *) callback_data, (void*)n->pnode);

    plat_rwlock_destroy(&n->lock);
    free(n);
}

btree_raw_t *
btree_raw_init(uint32_t flags, uint32_t n_partition, uint32_t n_partitions, uint32_t max_key_size, uint32_t min_keys_per_node, uint32_t nodesize, uint32_t n_l1cache_buckets, create_node_cb_t *create_node_cb, void *create_node_data, read_node_cb_t *read_node_cb, void *read_node_cb_data, write_node_cb_t *write_node_cb, void *write_node_cb_data, flush_node_cb_t *flush_node_cb, void *flush_node_cb_data, freebuf_cb_t *freebuf_cb, void *freebuf_cb_data, delete_node_cb_t *delete_node_cb, void *delete_node_data, log_cb_t *log_cb, void *log_cb_data, msg_cb_t *msg_cb, void *msg_cb_data, cmp_cb_t *cmp_cb, void * cmp_cb_data, trx_cmd_cb_t *trx_cmd_cb)
{
    btree_raw_t      *bt;
    uint32_t          nbytes_meta;
    btree_status_t    ret = BTREE_SUCCESS;

    dbg_print("start dbg_referenced %ld\n", dbg_referenced);

    bt = (btree_raw_t *) malloc(sizeof(btree_raw_t));
    if (bt == NULL) {
        return(NULL);
    }

    if (init_l1cache(bt, n_l1cache_buckets)) {
        return(NULL);
    }

    if (flags & VERBOSE_DEBUG) {
        Verbose = 1;
    }

    btree_raw_init_stats(bt, &(bt->stats));

    bt->n_partition          = n_partition;
    bt->n_partitions         = n_partitions;
    bt->flags                = flags;
    bt->max_key_size         = max_key_size;
    bt->min_keys_per_node    = min_keys_per_node;;
    bt->nodesize             = nodesize;
    bt->nodesize_less_hdr    = nodesize - sizeof(btree_raw_node_t);
    // bt->big_object_size      = (nodesize - sizeof(btree_raw_mem_node_t))/2; // xxxzzz check this
    bt->big_object_size      = bt->nodesize_less_hdr / 4 - sizeof(node_vlkey_t); // xxxzzz check this
    dbg_print("nodesize_less_hdr=%d nodezie=%d raw_node_size=%ld\n", bt->nodesize_less_hdr, nodesize, sizeof(btree_raw_node_t));
    bt->logical_id_counter   = 1;
    bt->next_logical_id	     = META_COUNTER_SAVE_INTERVAL; 
    bt->create_node_cb       = create_node_cb;
    bt->create_node_cb_data  = create_node_data;
    bt->read_node_cb         = read_node_cb;
    bt->read_node_cb_data    = read_node_cb_data;
    bt->write_node_cb        = write_node_cb;
    bt->write_node_cb_data   = write_node_cb_data;
    bt->flush_node_cb        = flush_node_cb;
    bt->flush_node_cb_data   = flush_node_cb_data;
    bt->freebuf_cb           = freebuf_cb;
    bt->freebuf_cb_data      = freebuf_cb_data;
    bt->delete_node_cb       = delete_node_cb;
    bt->delete_node_cb_data  = delete_node_data;
    bt->log_cb               = log_cb;
    bt->log_cb_data          = log_cb_data;
    bt->msg_cb               = msg_cb;
    bt->msg_cb_data          = msg_cb_data;
    if (msg_cb == NULL) {
	bt->msg_cb           = default_msg_cb;
	bt->msg_cb_data      = NULL;
    }
    bt->cmp_cb               = cmp_cb;
    bt->cmp_cb_data          = cmp_cb_data;
    if (cmp_cb == NULL) {
	bt->cmp_cb           = default_cmp_cb;
	bt->cmp_cb_data      = NULL;
    }

    bt->trx_cmd_cb           = trx_cmd_cb;

    if (min_keys_per_node < 4) {
	bt_err("min_keys_per_node must be >= 4");
        free(bt);
	return(NULL);
    }

    bt->fkeys_per_node = (nodesize - sizeof(btree_raw_node_t))/sizeof(node_fkey_t);

    nbytes_meta = sizeof(node_vkey_t);
    if (nbytes_meta < sizeof(node_vlkey_t)) {
        nbytes_meta = sizeof(node_vlkey_t);
    }
    nbytes_meta += max_key_size;
    nbytes_meta *= min_keys_per_node;
    nbytes_meta += sizeof(btree_raw_node_t);

    if (nodesize < nbytes_meta) {
	bt_err("Node size (%d bytes) must be large enough to hold at least %d max sized keys (%d bytes each).", nodesize, min_keys_per_node, max_key_size);
        free(bt);
	return(NULL);
    }

    if (flags & RELOAD) {
        if (! loadpersistent( bt)) {
            bt_err( "Could not identify root node!");
            free( bt);
            return (NULL);
        }
    }
    else {
        bt->rootid = bt->logical_id_counter * bt->n_partitions + bt->n_partition;
        if (BTREE_SUCCESS != savepersistent( bt, 1 /* create */)) {
            free( bt);
            return (NULL);
        }

        btree_raw_mem_node_t *root_node = get_new_node( &ret, bt, LEAF_NODE);
        if (BTREE_SUCCESS != ret) {
            bt_warn( "Could not allocate root node! %p", root_node);
            free( bt);
            return (NULL);
        }

        if (!(bt->flags & IN_MEMORY)) {
            assert(root_node->pnode->logical_id == bt->rootid);
        }
        lock_modified_nodes(bt);
    }
    if (BTREE_SUCCESS != deref_l1cache(bt)) {
        ret = BTREE_FAILURE;
    }

    #ifdef DEBUG_STUFF
	if (Verbose) {
	    btree_raw_dump(stderr, bt);
	}
    #endif

    plat_rwlock_init(&bt->lock);
    bt->modified = 0;

    dbg_print("dbg_referenced %ld\n", dbg_referenced);
    assert(!dbg_referenced);
    dbg_print("bt %p lock %p n_part %d\n", bt, &bt->lock, n_partition);

    return(bt);
}

void
btree_raw_destroy (struct btree_raw **bt)
{
		destroy_l1cache(*bt);
		free(*bt);
		*bt = NULL;
}


/*
 * save persistent btree metadata
 *
 * Info is stored as a btree node with special logical ID. 
 */
static btree_status_t
savepersistent( btree_raw_t *bt, int create)
{
    btree_raw_mem_node_t* mem_node;
    btree_status_t	ret = BTREE_SUCCESS;

    if (bt->flags & IN_MEMORY)
        return (BTREE_FAILURE);

    if(create)
        mem_node = create_new_node(bt,
                META_LOGICAL_ID+bt->n_partition);
    else
        mem_node = get_existing_node_low(&ret, bt,
                META_LOGICAL_ID+bt->n_partition, 1);

    if(mem_node)
    {
        btree_raw_persist_t *r = (btree_raw_persist_t*)mem_node->pnode;

        dbg_print("ret=%d create=%d nodeid=%lx lic=%ld rootid=%ld save=%d\n", ret, create, META_LOGICAL_ID+bt->n_partition, bt->logical_id_counter, bt->rootid, r->rootid != bt->rootid || !(bt->logical_id_counter % META_COUNTER_SAVE_INTERVAL));

        if (!create && (r->rootid != bt->rootid || (bt->logical_id_counter >= r->next_logical_id))) {

	   	/* If META_COUNTER_SAVE_INTERVAL limit is hit during current operation we need to update
		 the next limit. These limits are useful to assign the unique id after restart */
	    	if (bt->logical_id_counter >= r->next_logical_id) {
		 	bt->next_logical_id = r->next_logical_id + META_COUNTER_SAVE_INTERVAL;
			r->next_logical_id =  bt->next_logical_id;
		}
		modify_l1cache_node(bt, mem_node);
	}

        r->logical_id_counter = bt->logical_id_counter;
        r->rootid = bt->rootid;
    }
    else
        ret = BTREE_FAILURE;

    if (BTREE_SUCCESS != ret)
        bt_warn( "Could not persist btree!");

    return ret;
}

/*
 * load persistent btree metadata
 *
 * Info is stored as a btree node with special logical ID. 
 */
static btree_status_t
loadpersistent( btree_raw_t *bt)
{
    btree_raw_mem_node_t *mem_node;
    btree_status_t ret = BTREE_SUCCESS;

    mem_node = get_existing_node(&ret, bt,
            META_LOGICAL_ID + bt->n_partition);

    if (ret)
        return (BTREE_SUCCESS);

    btree_raw_persist_t *r = (btree_raw_persist_t*)mem_node->pnode;

    dbg_print("ret=%d nodeid=%lx r->lic %ld r->rootid %ld bt->logical_id_counter %ld\n", ret, META_LOGICAL_ID + bt->n_partition, r->logical_id_counter, r->rootid, r->logical_id_counter + META_COUNTER_SAVE_INTERVAL);

    bt->logical_id_counter = r->next_logical_id;	//next_logical_id is stored before the restart and is used to determine the logical_id_counter value after restart.
    bt->rootid = r->rootid;

    return (BTREE_FAILURE);
}

int btree_raw_free_buffer(btree_raw_t *btree, char *buf)
{
    free_buffer(btree, buf);
    return(0);
}

//======================   GET  =========================================

inline static
int is_overflow(btree_raw_t *btree, btree_raw_node_t *node) { return node->flags & OVERFLOW_NODE; }

int is_leaf(btree_raw_t *btree, btree_raw_node_t *node) { return node->flags & LEAF_NODE; }

inline static
int is_root(btree_raw_t *btree, btree_raw_node_t *node) { return btree->rootid == node->logical_id; }

int get_key_stuff(btree_raw_t *bt, btree_raw_node_t *n, uint32_t nkey, key_stuff_t *pks)
{
    node_vkey_t   *pvk;
    node_vlkey_t  *pvlk;
    node_fkey_t   *pfk;
    int            leaf = 0;

    pks->nkey = nkey;
    if (bt->flags & SECONDARY_INDEX) {
        pks->fixed = 0;
        //  used for secondary indices
	if (n->flags & LEAF_NODE) {
	    leaf               = 1;
	    pvlk               = ((node_vlkey_t *) n->keys) + nkey;
	    pks->ptr           = pvlk->ptr;
	    pks->offset        = sizeof(node_vlkey_t);
	    pks->pkey_struct   = (void *) pvlk;
	    pks->pkey_val      = (char *) n + pvlk->keypos;
	    pks->keylen        = pvlk->keylen;
	    pks->datalen       = pvlk->datalen;
	    pks->fkeys_per_node = 0;
	    pks->seqno         = pvlk->seqno;
	    pks->syndrome      = pvlk->syndrome;
	} else {
	    pvk                = ((node_vkey_t *) n->keys) + nkey;
	    pks->ptr           = pvk->ptr;
	    pks->offset        = sizeof(node_vkey_t);
	    pks->pkey_struct   = (void *) pvk;
	    pks->pkey_val      = (char *) n + pvk->keypos;
	    pks->keylen        = pvk->keylen;
	    pks->datalen       = sizeof(uint64_t);
	    pks->fkeys_per_node = 0;
	    pks->seqno         = pvk->seqno;
	    pks->syndrome      = 0;
	}
    } else if (bt->flags & SYNDROME_INDEX) {
        //  used for primary indices
	if (n->flags & LEAF_NODE) {
	    leaf               = 1;
	    pvlk               = ((node_vlkey_t *) n->keys) + nkey;
	    pks->fixed         = 0;
	    pks->ptr           = pvlk->ptr;
	    pks->offset        = sizeof(node_vlkey_t);
	    pks->pkey_struct   = (void *) pvlk;
	    pks->pkey_val      = (char *) n + pvlk->keypos;
	    pks->keylen        = pvlk->keylen;
	    pks->datalen       = pvlk->datalen;
	    pks->fkeys_per_node = 0;
	    pks->seqno         = pvlk->seqno;
	    pks->syndrome      = pvlk->syndrome;
	} else {
	    pfk                = ((node_fkey_t *) n->keys) + nkey;
	    pks->fixed         = 1;
	    pks->ptr           = pfk->ptr;
	    pks->offset        = sizeof(node_fkey_t);
	    pks->pkey_struct   = (void *) pfk;
	    pks->pkey_val      = (char *) (pfk->key);
	    pks->keylen        = sizeof(uint64_t);
	    pks->datalen       = sizeof(uint64_t);
	    pks->fkeys_per_node = bt->fkeys_per_node;
	    pks->seqno         = pfk->seqno;
	    pks->syndrome      = pfk->key;
	}
    } else {
        assert(0);
    }
    pks->leaf = leaf;
    return(leaf);
}

/*
 *  Returns: key structure which matches 'key', if one is found; NULL otherwise
 *           'pk_insert' returns a pointer to the key struct that would FOLLOW 'key' on 
 *           an insertion into this node. If 'pk_insert' is NULL, 'key' must be
 *           inserted at end of node, or key is already in node.
 *
 *  If node is NOT a leaf node, these 3 values are returned:
 *    child_id:        id of child node that may contain this key
 *    child_id_before: id of child sibling before 'child_id'
 *    child_id_after:  id of child sibling after 'child_id'
 *
 *    If the before or after siblings don't exist, BAD_CHILD is returned.
 *
 *    nkey_child returns the index into the key array of the key entry corresponding
 *    to 'child_id' (for a non-leaf), or for the matching record (for a leaf).  
 *    If 'child_id' corresponds to the rightmost pointer, nkey_child is
 *    set to n->nkeys.  If there is no valid child_id (nonleaf) or matching record (leaf),
 *    nkey_child is set to -1.
 */

static node_key_t *find_key(btree_raw_t *bt, btree_raw_node_t *n, char *key_in, uint32_t keylen_in, uint64_t *child_id, uint64_t *child_id_before, uint64_t *child_id_after, node_key_t **pk_insert, btree_metadata_t *meta, uint64_t syndrome, int32_t *nkey_child)
{
    int            i_start, i_end, i_check, x;
    int            i_check_old;
    node_key_t    *pk = NULL;
    uint64_t       id_child;
    key_stuff_t    ks;
    int            key_found = 0;

    if (n->nkeys == 0) {
        if (n->rightmost == 0) {
	    *child_id        = BAD_CHILD;
	    *nkey_child      = -1;
	    *child_id_before = BAD_CHILD;
	    *child_id_after  = BAD_CHILD;
	    *pk_insert       = NULL;
	} else {
	    // YES, this is really possible!
	    // For example, when the root is a leaf and overflows on an insert.
	    *child_id        = n->rightmost;
	    *nkey_child      = 0;
	    *child_id_before = BAD_CHILD;
	    *child_id_after  = BAD_CHILD;
	    *pk_insert       = NULL;
	}
        assert(!is_leaf(bt, n) || *child_id == BAD_CHILD);
        return(NULL);
    }

    i_start     = 0;
    i_end       = n->nkeys - 1;
    i_check     = (i_start + i_end)/2;
    i_check_old = i_check;
    while (1) {

        (void) get_key_stuff(bt, n, i_check, &ks);
	pk = ks.pkey_struct;
	id_child = ks.ptr;

        if (ks.fixed) {
	    if (syndrome < ks.syndrome) {
		x = -1;
	    } else if (syndrome > ks.syndrome) {
		x = 1;
	    } else {
		x = 0;
	    }
	} else {
	    if (bt->flags & SYNDROME_INDEX) {
		if (syndrome < ks.syndrome) {
		    x = -1;
		} else if (syndrome > ks.syndrome) {
		    x = 1;
		} else {
		    x = 0;
		}
	    } else {
		x = bt->cmp_cb(bt->cmp_cb_data, key_in, keylen_in, ks.pkey_val, ks.keylen);
	    }
	}

        if ((x == 0) &&
            ((meta->flags & READ_SEQNO_LE) || 
             (meta->flags & READ_SEQNO_GT_LE)))
        {
            //  Must take sequence numbers into account

            if (meta->flags & READ_SEQNO_LE) {
                if (ks.seqno > meta->seqno_le) {
                    x = -1; // higher sequence numbers go BEFORE lower ones!
                }
            } else if (meta->flags & READ_SEQNO_LE) {
                if (ks.seqno > meta->seqno_le) {
                    x = -1; // higher sequence numbers go BEFORE lower ones!
                } else if (ks.seqno <= meta->seqno_gt) {
                    x = 1; // lower sequence numbers go AFTER lower ones!
                }
            } else {
                assert(0);
            }
        }

        if (x > 0) {
            //  key > pvk->key
            if (i_check == (n->nkeys-1)) {
                // key might be in rightmost child
                if(is_leaf(bt, n))
                    *child_id        = BAD_CHILD;
                else
                    *child_id        = n->rightmost;
		*nkey_child      = n->nkeys;
		*child_id_before = id_child;
                *child_id_after  = BAD_CHILD;
                *pk_insert       = NULL;
                assert(!is_leaf(bt, n) || *child_id == BAD_CHILD);
                return(NULL);
            }
            i_start     = i_check + 1;
        } else if (x < 0) {
            //  key < pvk->key
            if (i_check == 0) {
                // key might be in leftmost child for non-leaf nodes
                if (is_leaf(bt, n)) {
                    *child_id        = BAD_CHILD;
                    *nkey_child      = -1;
                } else {
                    *child_id        = id_child;
                    *nkey_child      = i_check;
                }
                *child_id_before = BAD_CHILD;
		if (i_check == (n->nkeys-1)) {
		    *child_id_after = n->rightmost;
		} else {
		    (void) get_key_stuff(bt, n, i_check+1, &ks);
		    *child_id_after = ks.ptr;
		}
                *pk_insert      = (node_key_t *) n->keys;
                assert(!is_leaf(bt, n) || *child_id == BAD_CHILD);
                return(NULL);
            }
            i_end       = i_check;
        } else {
            //  key == pvk->key
	    key_found = 1;
        }

	i_check_old = i_check;
	i_check     = (i_start + i_end)/2;

	if (key_found || 
	    (i_check_old == i_check)) 
	{
	    //  this is the end of the search

	    *child_id   = id_child;
	    *nkey_child = i_check;
	    *pk_insert  = pk;

	    if (i_check == 0) {
		*child_id_before = BAD_CHILD;
	    } else {
		get_key_stuff(bt, n, i_check-1, &ks);
		*child_id_before = ks.ptr;
	    }

	    if (i_check >= (n->nkeys-1)) {
		if (x > 0) {
		    *child_id        = n->rightmost;
		    *child_id_after  = BAD_CHILD;
		    *nkey_child      = n->nkeys;
		    *pk_insert       = NULL;
		} else {
		    *child_id_after  = n->rightmost;
		}
	    } else {
		get_key_stuff(bt, n, i_check+1, &ks);
		*child_id_after = ks.ptr;
	    }

            if (n->flags & LEAF_NODE) {
                *child_id_before = BAD_CHILD;
                *child_id_after  = BAD_CHILD;
                *child_id  = BAD_CHILD;
	    }

            if (!key_found) {
		pk = NULL;
	    }

            assert(!is_leaf(bt, n) || *child_id == BAD_CHILD);
	    return(pk);
	}
    }
    assert(0);  // we should never get here!
}

static node_key_t *bsearch_key(btree_raw_t *bt, btree_raw_node_t *n, char *key_in, uint32_t keylen_in, uint64_t *child_id, btree_metadata_t *meta, uint64_t syndrome)
{
    node_key_t       *pk_insert;
    uint64_t          child_id_before, child_id_after;
    int32_t           nkey_child;

    return find_key(bt, n, key_in, keylen_in, child_id, &child_id_before, &child_id_after, &pk_insert, meta, syndrome, &nkey_child);
}

static char *get_buffer(btree_raw_t *btree, uint64_t nbytes)
{
    char  *p;
    // pid_t  tid = syscall(SYS_gettid);

    p = malloc(nbytes);
    // xxxzzz SEGFAULT
    // fprintf(stderr, "SEGFAULT get_buffer: %p [tid=%d]\n", p, tid);
    return(p);
}

static void free_buffer(btree_raw_t *btree, char *buf)
{
    // pid_t  tid = syscall(SYS_gettid);

    if (btree->freebuf_cb != NULL) {
        if (btree->freebuf_cb(btree->freebuf_cb_data, buf)) {
	    	assert(0); // xxxzzz remove this!
		}
    } else {
		// xxxzzz SEGFAULT
		// fprintf(stderr, "SEGFAULT free_buffer: %p [tid=%d]\n", buf, tid);
		free(buf);
    }
}

btree_status_t get_leaf_data(btree_raw_t *bt, btree_raw_node_t *n, void *pkey, char **data, uint64_t *datalen, uint32_t meta_flags, int ref)
{
    node_vlkey_t       *pvlk;
    btree_raw_node_t   *z;
    btree_status_t      ret=BTREE_SUCCESS;
    char               *buf;
    char               *p;
    int                 buf_alloced=0;
    uint64_t            nbytes;
    uint64_t            copybytes;
    uint64_t            z_next;

    pvlk = (node_vlkey_t *) pkey;

    if (meta_flags & BUFFER_PROVIDED) {
        if (*datalen < pvlk->datalen) {
	    ret = BTREE_BUFFER_TOO_SMALL;
	    if (meta_flags & ALLOC_IF_TOO_SMALL) {
		buf = get_buffer(bt, pvlk->datalen);
		if (buf == NULL) {
		    bt_err("Failed to allocate a buffer of size %lld in get_leaf_data!", pvlk->datalen);
		    return(BTREE_FAILURE);
		}
		buf_alloced = 1;
	    } else {
	        return(BTREE_BUFFER_TOO_SMALL);
	    }
	} else {
	    buf = *data;
	}
    } else {
        buf = get_buffer(bt, pvlk->datalen);
	if (buf == NULL) {
	    bt_err("Failed to allocate a buffer of size %lld in get_leaf_data!", pvlk->datalen);
	    return(BTREE_FAILURE);
	}
	buf_alloced = 1;
    }

    if ((pvlk->keylen + pvlk->datalen) < bt->big_object_size) {
        //  key and data are in this btree node
	memcpy(buf, (char *) n + pvlk->keypos + pvlk->keylen, pvlk->datalen);
    } else {
        //  data is in overflow btree nodes

        if (pvlk->datalen > 0) {
	    nbytes = pvlk->datalen;
	    p      = buf;
	    z_next = pvlk->ptr;
	    while(nbytes > 0 && z_next)
	    {
		btree_raw_mem_node_t *node = get_existing_node_low(&ret, bt, z_next, ref);
		if(!node)
		    break;
		z = node->pnode;
                copybytes = nbytes >= bt->nodesize_less_hdr ? bt->nodesize_less_hdr : nbytes;
		memcpy(p, ((char *) z + sizeof(btree_raw_node_t)), copybytes);
		nbytes -= copybytes;
		p      += copybytes;
		z_next  = z->next;
		if(!ref)
		    deref_l1cache_node(bt, node);
	    }
	    if (nbytes) {
		bt_err("Failed to get overflow node (logical_id=%lld)(nbytes=%ld) in get_leaf_data!", z_next, nbytes);
		if (buf_alloced) {
		    free_buffer(bt, buf);
		}
		return(BTREE_FAILURE);
	    }
	    assert(z_next == 0);
	}
    }
    *datalen = pvlk->datalen;
    *data    = buf;
    return(ret);
}

btree_status_t 
get_leaf_key(btree_raw_t *bt, btree_raw_node_t *n, void *pkey, char **key, 
             uint32_t *keylen, uint32_t meta_flags)
{
	node_vlkey_t       *pvlk;
	btree_status_t     ret = BTREE_SUCCESS;
	char               *buf;

	pvlk = (node_vlkey_t *) pkey;

	if (meta_flags & BUFFER_PROVIDED) {
		if (*keylen < pvlk->keylen) {
			ret = BTREE_BUFFER_TOO_SMALL;
			if (!(meta_flags & ALLOC_IF_TOO_SMALL)) {
				return ret;
			}

			buf = get_buffer(bt, pvlk->keylen);
			if (buf == NULL) {
				bt_err("Failed to allocate a buffer of size %lld "
				       "in get_leaf_key!", pvlk->keylen);
				return(BTREE_FAILURE);
			}
		} else {
			buf = *key;
		}
	} else {
		buf = get_buffer(bt, pvlk->keylen);
		if (buf == NULL) {
			bt_err("Failed to allocate a buffer of size %lld "
			       "in get_leaf_key!", pvlk->keylen);
			return(BTREE_FAILURE);
		}
	}

	memcpy(buf, (char *) n + pvlk->keypos, pvlk->keylen);
	*keylen = pvlk->keylen;
	*key    = buf;

	return(ret);
}

static void delete_overflow_data(btree_status_t *ret, btree_raw_t *bt, uint64_t ptr_in, uint64_t datalen)
{
    uint64_t            ptr;
    uint64_t            ptr_next;
    btree_raw_mem_node_t   *n;

    if (*ret) { return; }

    for (ptr = ptr_in; ptr != 0; ptr = ptr_next) {
	n = get_existing_node(ret, bt, ptr);
	if (BTREE_SUCCESS != *ret) {
	    bt_err("Failed to find an existing overflow node in delete_overflow_data!");
	    return;
	}

	ptr_next = n->pnode->next;
	free_node(ret, bt, n);
	if (BTREE_SUCCESS != *ret) {
	    bt_err("Failed to free an existing overflow node in delete_overflow_data!");
	}
    }
    __sync_sub_and_fetch(&(bt->stats.stat[BTSTAT_OVERFLOW_BYTES]), datalen);
}

static uint64_t allocate_overflow_data(btree_raw_t *bt, uint64_t datalen, char *data, btree_metadata_t *meta)
{
    uint64_t            n_nodes;
    btree_raw_mem_node_t   *n, *n_first = NULL, *n_last = NULL;
    btree_status_t      ret = BTREE_SUCCESS;
    char               *p = data;;
    uint64_t            nbytes = datalen;

    dbg_print("datalen %ld nodesize_less_hdr: %d bt %p\n", datalen, bt->nodesize_less_hdr, bt);

    if (!datalen)
        return(BTREE_SUCCESS);

    n_nodes = (datalen + bt->nodesize_less_hdr - 1) / bt->nodesize_less_hdr;

    n_first = n = get_new_node(&ret, bt, OVERFLOW_NODE);
    while(nbytes > 0 && !ret) {
	n->next = 0;

	if (n_last != NULL)
	    n_last->pnode->next = n->pnode->logical_id;

	int b = nbytes < bt->nodesize_less_hdr ? nbytes : bt->nodesize_less_hdr;

	memcpy(((char *) n->pnode + sizeof(btree_raw_node_t)), p, b);

	p += b;
	nbytes -= b;
	n_last = n;

        __sync_add_and_fetch(&(bt->stats.stat[BTSTAT_OVERFLOW_BYTES]), 
                               b + sizeof(btree_raw_node_t));
	if(nbytes)
	    n = get_new_node(&ret, bt, OVERFLOW_NODE);
    }

    if(BTREE_SUCCESS == ret) 
	return n_first->pnode->logical_id;

    /* Error. Delete partially allocated data */
    ret = BTREE_SUCCESS;

    if(n_first)
        delete_overflow_data(&ret, bt, n_first->pnode->logical_id, datalen);

    return(ret);
}

static uint64_t get_syndrome(btree_raw_t *bt, char *key, uint32_t keylen)
{
    uint64_t   syndrome;

    syndrome = btree_hash((const unsigned char *) key, keylen, 0);
    return(syndrome);
}

/* Caller is responsible for leaf_lock unlock and node dereferencing */
node_key_t* btree_raw_find(struct btree_raw *btree, char *key, uint32_t keylen, uint64_t syndrome, btree_metadata_t *meta, btree_raw_mem_node_t** node, int write_lock, int* pathcnt)
{
    btree_raw_mem_node_t *parent;
    btree_status_t    ret = BTREE_SUCCESS;
    uint64_t          child_id;

restart:
    child_id = btree->rootid;

    *node = get_existing_node_low(&ret, btree, child_id, 0);
    assert(*node); //FIME add ret checking here

    if(is_leaf(btree, (*node)->pnode) && write_lock)
        plat_rwlock_wrlock(&(*node)->lock);
    else
        plat_rwlock_rdlock(&(*node)->lock);

    if(child_id != btree->rootid)
    {
        plat_rwlock_unlock(&(*node)->lock);
        deref_l1cache_node(btree, *node);
        goto restart;
    }

    while(!is_leaf(btree, (*node)->pnode)) {
        (void)bsearch_key(btree, (*node)->pnode, key, keylen, &child_id, meta, syndrome);
        assert(child_id != BAD_CHILD);

        parent = *node;

        *node = get_existing_node_low(&ret, btree, child_id, 0);
        assert(BTREE_SUCCESS == ret && *node); //FIXME add correct error checking here

        if(is_leaf(btree, (*node)->pnode) && write_lock)
            plat_rwlock_wrlock(&(*node)->lock);
        else
            plat_rwlock_rdlock(&(*node)->lock);

        plat_rwlock_unlock(&parent->lock);
        deref_l1cache_node(btree, parent);

        (*pathcnt)++;
    }

    return bsearch_key(btree, (*node)->pnode, key, keylen, &child_id, meta, syndrome);
}

extern __thread long long locked;

btree_status_t btree_raw_get(struct btree_raw *btree, char *key, uint32_t keylen, char **data, uint64_t *datalen, btree_metadata_t *meta)
{
    btree_status_t    ret = BTREE_KEY_NOT_FOUND;
    int               pathcnt = 1;
    btree_raw_mem_node_t *node;
    node_key_t       *keyrec;
    uint64_t          syndrome = get_syndrome(btree, key, keylen);

    dbg_print_key(key, keylen, "before ret=%d lic=%ld", ret, btree->logical_id_counter);


    plat_rwlock_rdlock(&btree->lock);

    keyrec = btree_raw_find(btree, key, keylen, syndrome, meta, &node, 0 /* shared */, &pathcnt);

    plat_rwlock_unlock(&btree->lock);

    if(keyrec) {
	ret = get_leaf_data(btree, node->pnode, keyrec, data, datalen, meta->flags, 0);
	assert(BTREE_SUCCESS == ret);
    }

    dbg_print_key(key, keylen, "after ret=%d lic=%ld keyrec %p", ret, btree->logical_id_counter, keyrec);

    plat_rwlock_unlock(&node->lock);
    deref_l1cache_node(btree, node);

    __sync_add_and_fetch(&(btree->stats.stat[BTSTAT_GET_CNT]), 1);
    __sync_add_and_fetch(&(btree->stats.stat[BTSTAT_GET_PATH]), pathcnt);


    return(ret);
}

//======================   INSERT/UPDATE/UPSERT  =========================================

//  return 0 if success, 1 otherwise
static int
init_l1cache(btree_raw_t *bt, uint32_t n_l1cache_buckets)
{
	int n = 0;
    bt->n_l1cache_buckets = n_l1cache_buckets;

    char *p = getenv("N_L1CACHE_PARTITIONS");
    if(p)
        n = atoi(p);
    if(n <=0 || n > 10000000)
        n = 256;

    bt->l1cache = PMapInit(n, n_l1cache_buckets / n + 1, 16 * (n_l1cache_buckets / n + 1), 1, l1cache_replace, (void *) bt);
    if (bt->l1cache == NULL) {
        return(1);
    }
    return(0);
}

void
destroy_l1cache(btree_raw_t *bt)
{
		PMapDestroy(&(bt->l1cache));
}

void
deref_l1cache_node(btree_raw_t* btree, btree_raw_mem_node_t *node)
{
    if (btree->flags & IN_MEMORY)
        return;

    dbg_print("node %p id %ld root: %d leaf: %d refcnt %d dbg_referenced: %lx mpnode %ld refs %ld\n", node, node->pnode->logical_id, is_root(btree, node->pnode), is_leaf(btree, node->pnode), PMapGetRefcnt(btree->l1cache, (char *) &node->pnode->logical_id, sizeof(uint64_t)), dbg_referenced, modified_nodes_count, referenced_nodes_count);

    if (!PMapRelease(btree->l1cache, (char *) &node->pnode->logical_id, sizeof(node->pnode->logical_id)))
        assert(0);

    assert(dbg_referenced);
    dbg_referenced--;
}

/*
 * Flush the modified and deleted nodes, unlock those nodes, cleare the reference
 * for such nodes.
 */
static btree_status_t deref_l1cache(btree_raw_t *btree)
{
    uint64_t i, j;
    btree_raw_mem_node_t *n;
    btree_status_t        ret = BTREE_SUCCESS;
    btree_status_t        txnret = BTREE_SUCCESS;

    for(i = 0; i < modified_nodes_count; i++)
    {
        n = modified_nodes[i];

        dbg_print("write_node_cb key=%ld data=%p datalen=%d\n", n->pnode->logical_id, n, btree->nodesize);

        j = 0;
        while(j < i && modified_nodes[j] != modified_nodes[i])
            j++;

        if(j >= i)
	btree->write_node_cb(&ret, btree->write_node_cb_data, n->pnode->logical_id, (char*)n->pnode, btree->nodesize);

        n->dirty = 0;
        add_node_stats(btree, n->pnode, L1WRITES, 1);
    }

    for(i = 0; i < deleted_nodes_count; i++)
    {
        n = deleted_nodes[i];

        dbg_print("delete_node_cb key=%ld data=%p datalen=%d\n", n->pnode->logical_id, n, btree->nodesize);

        ret = btree->delete_node_cb(btree->create_node_cb_data, n->pnode->logical_id);
        add_node_stats(btree, n->pnode, L1WRITES, 1);
    }

    //TODO
    //if(ret || txnret)
    //    invalidate_l1cache(btree);

    unlock_modified_nodes(btree);

    //  clear reference bits
    for(i = 0; i < referenced_nodes_count; i++)
    {
        n = referenced_nodes[i];
        deref_l1cache_node(btree, n);
    }

    for(i = 0; i < deleted_nodes_count; i++)
    {
        n = deleted_nodes[i];
        delete_l1cache(btree, n);
    }

    modified_nodes_count = 0;
    referenced_nodes_count = 0;
    deleted_nodes_count = 0;

//    assert(PMapNEntries(btree->l1cache) <= 16 * (btree->n_l1cache_buckets / 1000 + 1) * 1000 + 1);

    return  BTREE_SUCCESS == ret ? txnret : ret;
}

static btree_raw_mem_node_t* add_l1cache(btree_raw_t *btree, btree_raw_node_t *n)
{
    btree_raw_mem_node_t *node;

    node = malloc(sizeof(btree_raw_mem_node_t));
    assert(node);
    assert(n);

    node->pnode = n;
    node->modified = 0;
#ifdef DEBUG_STUFF
    node->last_dump_modified = 0;
#endif
    plat_rwlock_init(&node->lock);

    dbg_print("%p id %ld lock %p root: %d leaf: %d over: %d\n", n, n->logical_id, &node->lock, is_root(btree, n), is_leaf(btree, n), is_overflow(btree, n));

    if(!PMapCreate(btree->l1cache, (char *) &(n->logical_id), sizeof(uint64_t), (char *) node, sizeof(uint64_t)))
    {
        plat_rwlock_destroy(&node->lock);
        free(node);
        return NULL;
    }
    btree->trx_cmd_cb( TRX_CACHE_ADD, btree->write_node_cb_data, (void *)n->logical_id);

    dbg_referenced++;

    btree->stats.stat[BTSTAT_L1ENTRIES] = PMapNEntries(btree->l1cache);

    return node;
}

static void ref_l1cache(btree_raw_t *btree, btree_raw_mem_node_t *n)
{
    assert(referenced_nodes_count < MAX_BTREE_HEIGHT);
    assert(n);
    dbg_print("%p id %ld root: %d leaf: %d over: %d dbg_referenced %lx\n", n, n->pnode->logical_id, is_root(btree, n->pnode), is_leaf(btree, n->pnode), is_overflow(btree, n->pnode), dbg_referenced);
    referenced_nodes[referenced_nodes_count++] = n;
}

static btree_raw_mem_node_t *get_l1cache(btree_raw_t *btree, uint64_t logical_id)
{
    btree_raw_mem_node_t *n;
    uint64_t datalen;

    if (PMapGet(btree->l1cache, (char *) &logical_id, sizeof(uint64_t), (char **) &n, &datalen) == NULL)
        return NULL;
    if (btree->trx_cmd_cb( TRX_CACHE_QUERY, btree->write_node_cb_data, (void *)logical_id) == 0) {
		PMapDelete( btree->l1cache, (char *)&logical_id, sizeof logical_id);
		return (NULL);
    }

    dbg_referenced++;

    dbg_print("n %p node %p id %ld(%ld) lock %p root: %d leaf: %d over %d refcnt %d\n", n, n->pnode, n->pnode->logical_id, logical_id, &n->lock, is_root(btree, n->pnode), is_leaf(btree, n->pnode), is_overflow(btree, n->pnode), PMapGetRefcnt(btree->l1cache, (char *) &logical_id, sizeof(uint64_t)));

    return n;
}

static void delete_l1cache(btree_raw_t *btree, btree_raw_mem_node_t *n)
{
    dbg_print("node %p root: %d leaf: %d refcnt %d\n", n, is_root(btree, n->pnode), is_leaf(btree, n->pnode), PMapGetRefcnt(btree->l1cache, (char *) &n->pnode->logical_id, sizeof(uint64_t)));

    btree->trx_cmd_cb( TRX_CACHE_DEL, btree->write_node_cb_data, (void *)n->pnode->logical_id);
    (void) PMapDelete(btree->l1cache, (char *) &(n->pnode->logical_id), sizeof(uint64_t));

    btree->stats.stat[BTSTAT_L1ENTRIES] = PMapNEntries(btree->l1cache);
}

static void modify_l1cache_node(btree_raw_t *btree, btree_raw_mem_node_t *node)
{
    dbg_print("node %p id %ld root: %d leaf: %d refcnt %d dbg_referenced: %lx mpnode %ld refs %ld\n", node, node->pnode->logical_id, is_root(btree, node->pnode), is_leaf(btree, node->pnode), PMapGetRefcnt(btree->l1cache, (char *) &node->pnode->logical_id, sizeof(uint64_t)), dbg_referenced, modified_nodes_count, referenced_nodes_count);
    assert(modified_nodes_count < MAX_BTREE_HEIGHT);
    node->modified++;
    node->dirty=1;
    modified_nodes[modified_nodes_count++] = node;
}

inline static
void lock_nodes_list(btree_raw_t *btree, int lock, btree_raw_mem_node_t** list, int count)
{
    int i, j;
    btree_raw_mem_node_t     *node;

    for(i = 0; i < count; i++)
    {
        node = get_l1cache(btree, list[i]->pnode->logical_id);
        assert(node); // the node is in the cache, hence, get_l1cache cannot fail

        j = 0;
        while(j < i && list[j] != list[i])
            j++;

        if(j >= i && !is_overflow(btree, node->pnode) && node->pnode->logical_id != META_LOGICAL_ID+btree->n_partition) {
        dbg_print("list[%d]->logical_id=%ld lock=%p lock=%d\n", i, list[i]->pnode->logical_id, &node->lock, lock);

            if(lock)
                plat_rwlock_wrlock(&node->lock);
            else
                plat_rwlock_unlock(&node->lock);
        }

        deref_l1cache_node(btree, node);
    }
}

static void lock_modified_nodes_func(btree_raw_t *btree, int lock)
{
    dbg_print("lock %d start\n", lock);
    lock_nodes_list(btree, lock, modified_nodes, modified_nodes_count);
    lock_nodes_list(btree, lock, deleted_nodes, deleted_nodes_count);
    dbg_print("lock %d finish\n", lock);
}

btree_raw_mem_node_t *get_existing_node_low(btree_status_t *ret, btree_raw_t *btree, uint64_t logical_id, int ref)
{
    btree_raw_node_t  *pnode;
    btree_raw_mem_node_t  *n = NULL;

    if (*ret) { return(NULL); }

    *ret = BTREE_SUCCESS;

    if (btree->flags & IN_MEMORY) {
        n = (btree_raw_mem_node_t*)logical_id;
        dbg_print("n=%p flags=%d\n", n, n->pnode->flags);
    } else {
retry:
        //  check l1cache first
        n = get_l1cache(btree, logical_id);
        if (n != NULL) {
            add_node_stats(btree, n->pnode, L1HITS, 1);
	} else {
	    //  look for the node the hard way
	    //  If we don't look at the ret code, why does read_node_cb need one?
	    pnode = (btree_raw_node_t*)btree->read_node_cb(ret, btree->read_node_cb_data, logical_id);
	    if (pnode == NULL) {
		*ret = BTREE_FAILURE;
		return(NULL);
	    }
            add_node_stats(btree, pnode, L1MISSES, 1);

            // already in the cache retry get
	    n = add_l1cache(btree, pnode);
	    if(!n)
            {
                free(pnode);
                goto retry;
            }
	}
        if(ref)
            ref_l1cache(btree, n);
    }
    if (n == NULL) {
        *ret = BTREE_FAILURE;
	return(NULL);
    }
    
    return(n);
}

btree_raw_mem_node_t *get_existing_node(btree_status_t *ret, btree_raw_t *btree, uint64_t logical_id)
{
    return get_existing_node_low(ret, btree, logical_id, 1);
}

static
btree_raw_mem_node_t *create_new_node(btree_raw_t *btree, uint64_t logical_id)
{
    btree_raw_mem_node_t *n = NULL;
    btree_raw_node_t *pnode = (btree_raw_node_t *) malloc(btree->nodesize);
    // n = btree->create_node_cb(ret, btree->create_node_cb_data, logical_id);
    //  Just malloc the node here.  It will be written
    //  out at the end of the request by deref_l1cache().
    if (pnode != NULL) {
        pnode->logical_id = logical_id;
        n = add_l1cache(btree, pnode);
        assert(n); /* the tree is exclusively locked */
        ref_l1cache(btree, n);
        modify_l1cache_node(btree, n);
    }

    return n;
}

static btree_raw_mem_node_t *get_new_node(btree_status_t *ret, btree_raw_t *btree, uint32_t leaf_flags)
{
    btree_raw_node_t  *n;
    btree_raw_mem_node_t  *node;
    uint64_t           logical_id;
    // pid_t  tid = syscall(SYS_gettid);

    if (*ret) { return(NULL); }

    if (btree->flags & IN_MEMORY) {
        node = malloc(sizeof(btree_raw_mem_node_t) + btree->nodesize);
        node->pnode = (btree_raw_node_t*) ((void*)node + sizeof(btree_raw_mem_node_t));
        n = node->pnode;
        plat_rwlock_init(&node->lock);
	logical_id = (uint64_t) node;
	if (n != NULL) {
	    n->logical_id = logical_id;
	}
#ifdef DEBUG_STUFF
	if(Verbose)
        fprintf(stderr, "%x %s n=%p node=%p flags=%d\n", (int)pthread_self(), __FUNCTION__, n, node, leaf_flags);
#endif
    } else {
        logical_id = __sync_fetch_and_add(&btree->logical_id_counter, 1)*btree->n_partitions + btree->n_partition;
        if (BTREE_SUCCESS != savepersistent( btree, 0)) {
            *ret = BTREE_FAILURE;
            return (NULL);
        }
	node = create_new_node(btree, logical_id);
	n = node->pnode;
    }
    if (n == NULL) {
        *ret = BTREE_FAILURE;
	return(NULL);
    }

    n->flags      = leaf_flags;
    n->lsn        = 0;
    n->checksum   = 0;
    n->insert_ptr = btree->nodesize;
    n->nkeys      = 0;
    n->prev       = 0; // used for chaining nodes for large objects
    n->next       = 0; // used for chaining nodes for large objects
    n->rightmost  = BAD_CHILD;

    /* Update relevent node types, total count and bytes used in node */
    add_node_stats(btree, n, NODES, 1);
    add_node_stats(btree, n, BYTES, sizeof(btree_raw_node_t));

    return node;
}

static void free_node(btree_status_t *ret, btree_raw_t *btree, btree_raw_mem_node_t *n)
{
    // pid_t  tid = syscall(SYS_gettid);

    if (*ret) { return; }

    sub_node_stats(btree, n->pnode, NODES, 1);
    sub_node_stats(btree, n->pnode, BYTES, sizeof(btree_raw_node_t));

    if (btree->flags & IN_MEMORY) {
	    // xxxzzz SEGFAULT
	    // fprintf(stderr, "SEGFAULT free_node: %p [tid=%d]\n", n, tid);
       // fprintf(stderr, "%x %s n=%p node=%p flags=%d", (int)pthread_self(), __FUNCTION__, n, (void*)n - sizeof(btree_raw_mem_node_t), n->flags);
	    free((void*)n - sizeof(btree_raw_mem_node_t));
    } else {
        //delete_l1cache(btree, n);
        assert(deleted_nodes_count < MAX_BTREE_HEIGHT);
        deleted_nodes[deleted_nodes_count++] = n;
        //*ret = btree->delete_node_cb(n, btree->create_node_cb_data, n->logical_id);
    }
}

/*   Split the 'from' node across 'from' and 'to'.
 *
 *   Returns: pointer to the key at which the split was done
 *            (all keys < key must go in node 'to')
 *
 *   THIS FUNCTION DOES NOT SET THE RETURN CODE...DOES ANY LOOK AT IT???
 */
static void split_copy(btree_status_t *ret, btree_raw_t *btree, btree_raw_node_t *from, btree_raw_node_t *to, char **key_out, uint32_t *keylen_out, uint64_t *split_syndrome_out)
{
    node_fkey_t   *pfk;
    uint32_t       i, threshold, nbytes_to, nbytes_from, nkeys_to, nkeys_from;
    uint32_t       nbytes = 0;
    uint32_t       nbytes_split_nonleaf = 0;
    uint32_t       nbytes_fixed;
    key_stuff_t    ks;
    uint64_t       n_right     = 0;
    uint64_t       old_n_right = 0;
    uint64_t       split_syndrome = 0;
    char          *key = NULL;
    uint32_t       keylen = 0;

    if (*ret) { return; }

    (void) get_key_stuff(btree, from, 0, &ks);

    if (ks.fixed) {
        nkeys_to     = (ks.fkeys_per_node/2);
        nbytes_to    = nkeys_to*ks.offset;

        nkeys_from   = (from->nkeys - nkeys_to);
        nbytes_from  = nkeys_from*ks.offset;
	    nbytes_fixed = ks.offset;

	//  last key in 'to' node gets inserted into parent
	//  For lack of a better place, we stash the split key
	//  in an unused key slot in the 'to' node.
	//  This temporary copy is only used by the caller to
	//  split_copy to insert the split key into the parent.
 
 	pfk            = (node_fkey_t *) to->keys + nkeys_to;
	pfk->ptr       = ((node_fkey_t *) from->keys + nkeys_to)->ptr;
	key            = (char *) &(pfk->ptr);
	keylen         = sizeof(uint64_t);
	split_syndrome = ((node_fkey_t *) from->keys + nkeys_to - 1)->key;
	n_right        = ((node_fkey_t *) from->keys + nkeys_to - 1)->ptr;

    } else {
        threshold = (btree->nodesize - sizeof(btree_raw_node_t))/2;
	nbytes_to = 0;
	for (i=0; i<from->nkeys; i++) {
	    (void) get_key_stuff(btree, from, i, &ks);
	    nbytes     = ks.keylen;
	    if (ks.leaf) {
		if ((ks.keylen + ks.datalen) < btree->big_object_size) { // xxxzzz check this!
		    nbytes += ks.datalen;
		}
	    }
	    nbytes_to += nbytes;
	    old_n_right = n_right;
	    n_right     = ks.ptr;
	    if ((nbytes_to + (i+1)*ks.offset) > threshold) {
	        break;
	    }
	    //  last key in 'to' node gets inserted into parent
	    key            = ks.pkey_val; // This should be unchanged in the from node, 
		   		          // even though it is no longer used!
	    keylen               = ks.keylen;
	    split_syndrome       = ks.syndrome;
	    nbytes_split_nonleaf = ks.keylen;
	}
	assert(i < from->nkeys); // xxxzzz remove me!
	assert(i != 0);          // xxxzzz remove me!

	nkeys_to   = i; // xxxzzz check this
	nbytes_to -= nbytes; // xxxzzz check this
	n_right    = old_n_right;

	nkeys_from   = from->nkeys - nkeys_to;
	nbytes_from  = btree->nodesize - from->insert_ptr - nbytes_to;
	nbytes_fixed = ks.offset;
    }
    *key_out            = key;
    *keylen_out         = keylen;
    *split_syndrome_out = split_syndrome;

    // copy the fixed size portion of the keys

    memcpy(to->keys, from->keys, nkeys_to*nbytes_fixed);
    to->nkeys = ks.leaf ? nkeys_to : nkeys_to - 1;

    memmove(from->keys, (char *) from->keys + nkeys_to*nbytes_fixed, nkeys_from*nbytes_fixed);
    from->nkeys = nkeys_from;

    if (ks.fixed) {
	to->insert_ptr   = 0;
	from->insert_ptr = 0;
    } else {
	// for variable sized keys, copy the variable sized portion
	//  For leaf nodes, copy the data too

        if (ks.leaf) {
	    memcpy(((char *) to) + btree->nodesize - nbytes_to, 
		   ((char *) from) + from->insert_ptr,
		   nbytes_to);

	    to->insert_ptr   = btree->nodesize - nbytes_to;
	    from->insert_ptr = btree->nodesize - nbytes_from;
	} else {
	    //  for non-leaves, skip split key
	    memcpy(((char *) to) + btree->nodesize - nbytes_to + nbytes_split_nonleaf, 
		   ((char *) from) + from->insert_ptr,
		   nbytes_to - nbytes_split_nonleaf);

	    to->insert_ptr   = btree->nodesize - nbytes_to + nbytes_split_nonleaf;
	    from->insert_ptr = btree->nodesize - nbytes_from;
	}
	
	//  update the keypos pointers
        update_keypos(btree, to, 0);
        update_keypos(btree, from, 0);
    }

    // update the rightmost pointers of the 'to' node
    if (ks.leaf) {
	to->rightmost = from->logical_id;
	// xxxzzz from->leftmost = to->logical_id;
	// xxxzzz continue from here:fix rightmost pointer of left sibling!!
	// xxxzzz continue from here:fix leftmost pointer of right sibling!!
    } else {
	to->rightmost = n_right;
    }

    #ifdef DEBUG_STUFF
	if (Verbose) {
	    fprintf(stderr, "********  After split_copy for key '%s' [syn=%lu], rightmost %lx B-Tree BEGIN:  *******\n", dump_key(key, keylen), split_syndrome, to->rightmost);
	    btree_raw_dump(stderr, btree);
	    fprintf(stderr, "********  After split_copy for key '%s' [syn=%lu], To-Node:  *******\n", dump_key(key, keylen), split_syndrome);
            dump_node(btree, stderr, to, key, keylen);
	    fprintf(stderr, "********  After split_copy for key '%s' [syn=%lu], B-Tree END:  *******\n", dump_key(key, keylen), split_syndrome);
	}
    #endif

    return;
}

static int has_fixed_keys(btree_raw_t *btree, btree_raw_node_t *n)
{
    return((btree->flags & SYNDROME_INDEX) && !is_leaf(btree, n));
}

static void update_keypos(btree_raw_t *btree, btree_raw_node_t *n, uint32_t n_key_start)
{
    int            i;
    uint32_t       keypos;
    node_vkey_t   *pvk;
    node_vlkey_t  *pvlk;

    if (has_fixed_keys(btree, n)) {
        return;
    }

    keypos = n->insert_ptr;
    if (n->flags & LEAF_NODE) {
	for (i=n_key_start; i<n->nkeys; i++) {
	    pvlk = (node_vlkey_t *) (((char *) n->keys) + i*sizeof(node_vlkey_t));
	    pvlk->keypos = keypos;
	    keypos += pvlk->keylen;
	    if ((pvlk->keylen + pvlk->datalen) < btree->big_object_size) {
	        //  data is NOT overflowed!
		keypos += pvlk->datalen;
	    }
	}
    } else {
	for (i=n_key_start; i<n->nkeys; i++) {
	    pvk = (node_vkey_t *) (((char *) n->keys) + i*sizeof(node_vkey_t));
	    pvk->keypos = keypos;
	    keypos += pvk->keylen;
	}
    }
}

/*   Insert a new key into a node (and possibly its data if this is a leaf)
 *   This assumes that we have enough space!
 *   It is the responsibility of the caller to check!
 */
static void insert_key_low(btree_status_t *ret, btree_raw_t *btree, btree_raw_mem_node_t *node, char *key, uint32_t keylen, uint64_t seqno, uint64_t datalen, char *data, btree_metadata_t *meta, uint64_t syndrome, node_key_t* pkrec, node_key_t* pk_insert)
{
    btree_raw_node_t* x = node->pnode;
    node_vkey_t   *pvk;
    node_vlkey_t  *pvlk;
    node_fkey_t   *pfk;
    uint32_t       nkeys_to = 0, nkeys_from = 0;
    uint32_t       fixed_bytes;
    uint64_t       child_id, child_id_before, child_id_after;
    node_vkey_t   *pvk_insert;
    node_vlkey_t  *pvlk_insert;
    uint32_t       pos_new_key = 0;
    key_stuff_t    ks;
    uint32_t       vbytes_this_node = 0;
    uint64_t       ptr_overflow = 0;
    uint32_t       pos_split = 0;
    uint32_t       nbytes_split = 0;
    uint32_t       nbytes_free;
    int32_t        nkey_child;
    uint64_t       nbytes_stats;

    dbg_print("node: %p id %ld\n", x, x->logical_id);

    if (*ret) { return; }

    nbytes_stats = keylen;

    if (pkrec != NULL) {
        // delete existing key first
	delete_key_by_pkrec(ret, btree, node, pkrec);
	assert((*ret) == BTREE_SUCCESS);
	pkrec = find_key(btree, x, key, keylen, &child_id, &child_id_before, &child_id_after, &pk_insert, meta, syndrome, &nkey_child);
	assert(pkrec == NULL);
    } else {
        modify_l1cache_node(btree, node);
    }

    (void) get_key_stuff(btree, x, 0, &ks);

    if (pk_insert == NULL) {
	nkeys_to     = x->nkeys;
	pos_split    = btree->nodesize;
	nbytes_split = btree->nodesize - x->insert_ptr;
    }

    if (!ks.fixed) {
        if (x->flags & LEAF_NODE) {
	    pvlk_insert = (node_vlkey_t *) pk_insert;
	    if (pvlk_insert != NULL) {
		nkeys_to     = (((char *) pk_insert) - ((char *) x->keys))/ks.offset;
		pos_split    = pvlk_insert->keypos;
		nbytes_split = pvlk_insert->keypos - x->insert_ptr;
	    }
            nbytes_stats += sizeof(node_vlkey_t);
	} else {
	    pvk_insert  = (node_vkey_t *) pk_insert;
	    if (pvk_insert != NULL) {
		nkeys_to     = (((char *) pk_insert) - ((char *) x->keys))/ks.offset;
		pos_split    = pvk_insert->keypos;
		nbytes_split = pvk_insert->keypos - x->insert_ptr;
	    }
            nbytes_stats += sizeof(node_vkey_t);
	}
        fixed_bytes = ks.offset;
    } else {
        fixed_bytes = ks.offset;
	if (pk_insert != NULL) {
	    nkeys_to = (((char *) pk_insert) - ((char *) x->keys))/ks.offset;
	}
        nbytes_stats += sizeof(node_fkey_t);
    }
    nkeys_from = x->nkeys - nkeys_to;

    if ((!ks.fixed) && 
        (x->flags & LEAF_NODE) && 
	((keylen + datalen) >= btree->big_object_size)) // xxxzzz check this!
    { 
	//  Allocate nodes for overflowed objects first, in case
	//  something goes wrong.
        
	ptr_overflow = allocate_overflow_data(btree, datalen, data, meta);
	if ((ptr_overflow == 0) && (datalen != 0)) {
	    // something went wrong with the allocation
	    *ret = BTREE_FAILURE;
	    return;
	}
    }

    if (ks.fixed) {
	// check that there is enough space!
	assert(x->nkeys < (btree->fkeys_per_node));
    } else {
        if (x->flags & LEAF_NODE) {

	    //  insert variable portion of new key (and possibly data) in
	    //  sorted key order at end of variable data stack in node

	    if ((keylen + datalen) >= btree->big_object_size) { // xxxzzz check this!
	        //  put key in this node, data in overflow nodes
		vbytes_this_node = keylen;
	    } else {
	        //  put key and data in this node
		vbytes_this_node = keylen + datalen;
	    }
	    // check that there is enough space!
	    nbytes_free = vlnode_bytes_free(x);
	    assert(nbytes_free >= (sizeof(node_vlkey_t) + vbytes_this_node));

	    //  make space for variable portion of new key/data

	    memmove((char *) x + pos_split - nbytes_split - vbytes_this_node,
		    (char *) x + pos_split - nbytes_split, 
		    nbytes_split);

	    pos_new_key = pos_split - vbytes_this_node;

	    //  insert variable portion of new key

	    memcpy((char *) x + pos_new_key, key, keylen);
	    if (vbytes_this_node > keylen) {
		//  insert data
		memcpy((char *) x + pos_new_key + keylen, data, datalen);
	    }
	} else {
	    vbytes_this_node = keylen;

	    // check that there is enough space!
	    nbytes_free = vnode_bytes_free(x);
	    assert(nbytes_free >= (sizeof(node_vkey_t) + vbytes_this_node));

	    //  make space for variable portion of new key/data

	    memmove((char *) x + pos_split - nbytes_split - vbytes_this_node,
		    (char *) x + pos_split - nbytes_split, 
		    nbytes_split);

	    pos_new_key = pos_split - vbytes_this_node;

	    //  insert variable portion of new key

	    memcpy((char *) x + pos_new_key, key, keylen);
	}
    }

    //  Make space for fixed portion of new key.
    // 
    //  NOTE: This MUST be done after updating the variable part
    //        because the variable part uses key data in its old location!
    //

    if (nkeys_from != 0) {
	memmove((char *) (x->keys) + (nkeys_to + 1)*fixed_bytes, (char *) (x->keys) + nkeys_to*fixed_bytes, nkeys_from*fixed_bytes);
    }

    if (!ks.fixed) {
	x->insert_ptr -= vbytes_this_node;
    } else {
	x->insert_ptr = 0; // xxxzzz this should be redundant!
    }

    //  Do this here because update_keypos() requires it!
    x->nkeys += 1;

    //  insert fixed portion of new key
    if (!ks.fixed) {
        if (x->flags & LEAF_NODE) {
	    pvlk           = (node_vlkey_t *) ((char *) (x->keys) + nkeys_to*fixed_bytes);
	    pvlk->keylen   = keylen;
	    pvlk->keypos   = pos_new_key;
	    pvlk->datalen  = datalen;
	    pvlk->seqno    = seqno;
	    pvlk->syndrome = syndrome;
	    if ((keylen + datalen) >= btree->big_object_size) { // xxxzzz check this!
	        //  data is in overflow nodes
		pvlk->ptr = ptr_overflow;
	    } else {
	        //  data is in this node
		pvlk->ptr = 0;
                nbytes_stats += datalen;
	    }
	} else {
	    pvk          = (node_vkey_t *) ((char *) (x->keys) + nkeys_to*fixed_bytes);
	    pvk->keylen  = keylen;
	    pvk->keypos  = pos_new_key;
	    pvk->seqno   = seqno;
	    assert(datalen == sizeof(uint64_t));
	    pvk->ptr     = *((uint64_t *) data);
	}

	//  update all of the 'keypos' fields in the fixed portion
	update_keypos(btree, x, 0);

    } else {
        pfk            = (node_fkey_t *) ((char *) (x->keys) + nkeys_to*fixed_bytes);
	pfk->key       = syndrome;
	pfk->seqno     = seqno;
	assert(datalen == sizeof(uint64_t));
	pfk->ptr       = *((uint64_t *) data);
    }

    if (x->flags & LEAF_NODE) {
        /* A new object has been inserted. increment the count */
        __sync_add_and_fetch(&(btree->stats.stat[BTSTAT_NUM_OBJS]), 1);
	__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_LEAF_BYTES]), nbytes_stats);
    } else {
	__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_NONLEAF_BYTES]), nbytes_stats);
    }

    #ifdef DEBUG_STUFF
	if (Verbose) {
	    char  stmp[10000];
	    int   len;
	    if ((btree->flags & SYNDROME_INDEX) && !(x->flags & LEAF_NODE)) {
	        sprintf(stmp, "%p", key);
		len = strlen(stmp);
	    } else {
		strncpy(stmp, key, keylen);
		len = keylen;
	    }

            uint64_t ddd = dbg_referenced;
	    fprintf(stderr, "%x ********  After insert_key '%s' [syn %lu], datalen=%ld, node %p BEGIN:  *******\n", (int)pthread_self(), dump_key(stmp, len), syndrome, datalen, x);
	    btree_raw_dump(stderr, btree);
	    fprintf(stderr, "%x ********  After insert_key '%s' [syn %lu], datalen=%ld, NODE:  *******\n", (int)pthread_self(), dump_key(stmp, len), syndrome, datalen);
            assert(ddd == dbg_referenced);
	    (void) get_key_stuff(btree, x, 0, &ks);
	    if ((btree->flags & SYNDROME_INDEX) && !(x->flags & LEAF_NODE)) {
	        sprintf(stmp, "%p", ks.pkey_val);
		dump_node(btree, stderr, x, stmp, strlen(stmp));
	    } else {
		dump_node(btree, stderr, x, ks.pkey_val, ks.keylen);
	    }
            assert(ddd == dbg_referenced);
	    fprintf(stderr, "%x ********  After insert_key '%s' [syn %lu], datalen=%ld, node %p END  *******\n", (int)pthread_self(), dump_key(stmp, len), syndrome, datalen, x);
	}
    #endif
}

static void insert_key(btree_status_t *ret, btree_raw_t *btree, btree_raw_mem_node_t *node, char *key, uint32_t keylen, uint64_t seqno, uint64_t datalen, char *data, btree_metadata_t *meta, uint64_t syndrome)
{
    btree_raw_node_t* x = node->pnode;
    uint64_t       child_id, child_id_before, child_id_after;
    int32_t        nkey_child;
    node_key_t    *pk_insert;
    node_key_t* pkrec;

    pkrec = find_key(btree, x, key, keylen, &child_id, &child_id_before, &child_id_after, &pk_insert, meta, syndrome, &nkey_child);

    return insert_key_low(ret, btree, node, key, keylen, seqno, datalen, data, meta, syndrome, pkrec, pk_insert);
}

static void delete_key_by_pkrec(btree_status_t* ret, btree_raw_t *btree, btree_raw_mem_node_t *node, node_key_t *pk_delete)
{
    btree_raw_node_t* x = node->pnode;
    uint32_t       nkeys_to, nkeys_from;
    uint32_t       fixed_bytes;
    uint64_t       datalen = 0;
    uint64_t       keylen = 0;
    uint64_t       nbytes_stats = 0;
    node_vkey_t   *pvk_delete = NULL;
    node_vlkey_t  *pvlk_delete = NULL;
    key_stuff_t    ks;

    assert(pk_delete);

    if(*ret) return;

    (void) get_key_stuff(btree, x, 0, &ks);

    modify_l1cache_node(btree, node);

    if (!ks.fixed) {
        if (x->flags & LEAF_NODE) {
	    pvlk_delete = (node_vlkey_t *) pk_delete;

	    keylen = pvlk_delete->keylen;
	    if ((pvlk_delete->keylen + pvlk_delete->datalen) >= btree->big_object_size) {
	        // data NOT stored in the node
		datalen = 0;
                delete_overflow_data(ret, btree, pvlk_delete->ptr, pvlk_delete->datalen);
	    } else {
	        // data IS stored in the node
		datalen = pvlk_delete->datalen;
	    }
            nbytes_stats = sizeof(node_vlkey_t) + datalen;
	} else {
	    pvk_delete = (node_vkey_t *) pk_delete;
	    keylen = pvk_delete->keylen;
            nbytes_stats = sizeof(node_vkey_t);
	}
	fixed_bytes = ks.offset;
	nkeys_to = (((char *) pk_delete) - ((char *) x->keys))/ks.offset;
        nbytes_stats += keylen;
    } else {
        fixed_bytes = sizeof(node_fkey_t);
	nkeys_to = (((char *) pk_delete) - ((char *) x->keys))/sizeof(node_fkey_t);
        nbytes_stats = sizeof(node_fkey_t);
    }

    if (x->flags & LEAF_NODE) {
        __sync_sub_and_fetch(&(btree->stats.stat[BTSTAT_NUM_OBJS]), 1);
	__sync_sub_and_fetch(&(btree->stats.stat[BTSTAT_LEAF_BYTES]), nbytes_stats);
    } else {
	__sync_sub_and_fetch(&(btree->stats.stat[BTSTAT_NONLEAF_BYTES]), nbytes_stats);
    }

    nkeys_from = x->nkeys - nkeys_to - 1;

    if (!ks.fixed) {
	assert(keylen);
	//  remove variable portion of key
        if (x->flags & LEAF_NODE) {
	    memmove((char *) x + x->insert_ptr + keylen + datalen, 
		    (char *) x + x->insert_ptr, 
		    pvlk_delete->keypos - x->insert_ptr);
	    x->insert_ptr += (keylen + datalen);
	} else {
	    memmove((char *) x + x->insert_ptr + keylen, 
		    (char *) x + x->insert_ptr, 
		    pvk_delete->keypos - x->insert_ptr);
	    x->insert_ptr += keylen;
	}
    }

    //  Remove fixed portion of deleted key.
    // 
    //  NOTE: This MUST be done after deleting the variable part
    //        because the variable part uses key data in its old location!
    //

    memmove((char *) (x->keys) + nkeys_to*fixed_bytes, (char *) (x->keys) + (nkeys_to+1)*fixed_bytes, nkeys_from*fixed_bytes);

    //  Do this here because update_keypos() requires it!
    x->nkeys -= 1;

    //  delete fixed portion of new key
    if (!ks.fixed) {
	//  update all of the 'keypos' fields in the fixed portion
	update_keypos(btree, x, 0);
    } else {
	x->insert_ptr = 0; // xxxzzz this should be redundant!
    }

    #if 0 //def DEBUG_STUFF
	if (Verbose) {
	    char stmp[10000];
	    int  len;
	    if (btree->flags & SYNDROME_INDEX) {
	        sprintf(stmp, "%p", pvlk_delete->key);
		len = strlen(stmp);
	    } else {
		strncpy(stmp, pvlk_delete->key, pvlk_delete->keylen);
		len = pvlk_delete->keylen;
	    }
	    fprintf(stderr, "********  After delete_key '%s' [syn %lu]:  *******\n", dump_key(stmp, len), syndrome);
	    btree_raw_dump(stderr, btree);
	}
    #endif
}

static void delete_key(btree_status_t *ret, btree_raw_t *btree, btree_raw_mem_node_t *node, char *key, uint32_t keylen, btree_metadata_t *meta, uint64_t syndrome)
{
    uint64_t       child_id;
    node_key_t    *pk_delete;

    if (*ret) { return; }

    pk_delete = bsearch_key(btree, node->pnode, key, keylen, &child_id, meta, syndrome);

    if (pk_delete == NULL) {
	*ret = BTREE_KEY_NOT_FOUND; 
	return;
    }

    delete_key_by_pkrec(ret, btree, node, pk_delete);
}

static btree_raw_mem_node_t* btree_split_child(btree_status_t *ret, btree_raw_t *btree, btree_raw_mem_node_t *n_parent, btree_raw_mem_node_t *n_child, uint64_t seqno, btree_metadata_t *meta, uint64_t syndrome)
{
    btree_raw_mem_node_t     *n_new;
    uint32_t              keylen = 0;
    char                 *key = NULL;
    uint64_t              split_syndrome = 0;

    if (*ret) { return NULL; }

    __sync_add_and_fetch(&(btree->stats.stat[BTSTAT_SPLITS]),1);

    n_new = get_new_node(ret, btree, is_leaf(btree, n_child->pnode) ? LEAF_NODE : 0);
    if (BTREE_SUCCESS != *ret)
        return NULL;

    // n_parent will be marked modified by insert_key()
    // n_new was marked in get_new_node()
    dbg_print("n_child=%ld n_parent=%ld\n", n_child->pnode->logical_id, n_parent->pnode->logical_id);

    modify_l1cache_node(btree, n_child);

    split_copy(ret, btree, n_child->pnode, n_new->pnode, &key, &keylen, &split_syndrome);
    
    if (BTREE_SUCCESS == *ret) {
	//  Add the split key in the parent
	insert_key(ret, btree, n_parent, key, keylen, seqno, sizeof(uint64_t), (char *) &(n_new->pnode->logical_id), meta, split_syndrome);

	btree->log_cb(ret, btree->log_cb_data, BTREE_UPDATE_NODE, btree, n_parent);
	btree->log_cb(ret, btree->log_cb_data, BTREE_UPDATE_NODE, btree, n_child);
	btree->log_cb(ret, btree->log_cb_data, BTREE_CREATE_NODE, btree, n_new);
    }

    #ifdef DEBUG_STUFF
	if (Verbose) {
	    fprintf(stderr, "********  After btree_split_child (id_child='%ld'):  *******\n", n_child->pnode->logical_id);
	    btree_raw_dump(stderr, btree);
	}
    #endif

    return n_new;
}

//  Check if a node has enough space for insertion
//  of a totally new item.
static btree_status_t is_full_insert(btree_raw_t *btree, btree_raw_node_t *n, uint32_t keylen, uint64_t datalen)
{
    btree_status_t ret = BTREE_SUCCESS;
    uint32_t   nbytes_free = 0;

    if (n->flags & LEAF_NODE) {
        // vlkey
        nbytes_free = vlnode_bytes_free(n);
	if ((keylen + datalen) >= btree->big_object_size) { // xxxzzz check this!
	    //  don't include datalen because object data is kept
	    //  in overflow btree nodes
	    if (nbytes_free < (sizeof(node_vlkey_t) + keylen)) {
		ret = BTREE_FAILURE;
	    }
	} else {
	    if (nbytes_free < (sizeof(node_vlkey_t) + keylen + datalen)) {
		ret = BTREE_FAILURE;
	    }
	}
    } else if (btree->flags & SECONDARY_INDEX) {
        // vkey
        nbytes_free = vnode_bytes_free(n);
	// if (nbytes_free < (sizeof(node_vkey_t) + keylen)) {
	if (nbytes_free < (sizeof(node_vkey_t) + btree->max_key_size)) {
	    ret = BTREE_FAILURE;
	}
    } else {
        // fkey
	if (n->nkeys > (btree->fkeys_per_node-1)) {
	    ret = BTREE_FAILURE;
	}
    }

    dbg_print("nbytes_free: %d keylen %d datalen %ld nkeys %d vkey_t %ld raw_node_t %ld insert_ptr %d ret %d\n", nbytes_free, keylen, datalen, n->nkeys, sizeof(node_vkey_t), sizeof(btree_raw_mem_node_t), n->insert_ptr, ret);

    return (ret);
}

//  Check if a leaf node has enough space for an update of
//  an existing item.
static btree_status_t is_full_update(btree_raw_t *btree, btree_raw_node_t *n, node_vlkey_t *pvlk, uint32_t keylen, uint64_t datalen)
{
    btree_status_t        ret = BTREE_SUCCESS;
    uint32_t              nbytes_free = 0;
    uint64_t              update_bytes = 0;

    assert(n->flags & LEAF_NODE);  //  xxxzzz remove this

    if ((keylen + datalen) >= btree->big_object_size) { // xxxzzz check this!
        //  updated data will be put in overflow node(s)
        update_bytes = keylen;
    } else {
        //  updated data fits in a node
        update_bytes = keylen + datalen;
    }

    // must be vlkey!
    nbytes_free = vlnode_bytes_free(n);
    if ((pvlk->keylen + pvlk->datalen) >= btree->big_object_size) { // xxxzzz check this!
        //  Data to be overwritten is in overflow node(s).
	if ((nbytes_free + pvlk->keylen) < update_bytes) {
	    ret = BTREE_FAILURE;
	}
    } else {
        //  Data to be overwritten is in this node.
	if ((nbytes_free + pvlk->keylen + pvlk->datalen) < update_bytes) {
	    ret = BTREE_FAILURE;
	}
    }
    dbg_print("nbytes_free: %d pvlk->keylen %d pvlk->datalen %ld keylen %d datalen %ld update_bytes %ld insert_ptr %d nkeys %d ret %d\n", nbytes_free, pvlk->keylen, pvlk->datalen, keylen, datalen, update_bytes, n->insert_ptr, n->nkeys, ret);

    return(ret);
}

static int is_node_full(btree_raw_t *bt, btree_raw_node_t *r, char *key, uint32_t keylen, uint64_t datalen, btree_metadata_t *meta, uint64_t syndrome, int write_type, node_key_t* pkrec)
{
    node_vlkey_t  *pvlk;
    int            full;

    if (is_leaf(bt, r)) {
        pvlk = (node_vlkey_t *) pkrec;
	if (pvlk == NULL) {
	    full = is_full_insert(bt, r, keylen, datalen);
	} else {
	  // key found for update or set
	  full = is_full_update(bt, r, pvlk, keylen, datalen);
	}
    } else {
        //  non-leaf nodes
	if (pkrec == NULL) {
	    full = is_full_insert(bt, r, keylen, datalen);
	} else if (bt->flags & SECONDARY_INDEX) {
	    // must be enough room for max sized key in case child is split!
	    full = is_full_insert(bt, r, keylen, datalen);
	} else {
	    // SYNDROME_INDEX
	    // must be enough room for max sized key in case child is split!
	    full = is_full_insert(bt, r, keylen, datalen);
	}
    }
    return full;
}

/*
 * Given a set of keys and a reference key, find all keys in set less than
 * the reference key.
 */
static inline uint32_t 
get_keys_less_than(btree_raw_t *btree, char *key, uint32_t keylen,
	       btree_mput_obj_t *objs, uint32_t count)

{
	int i_start, i_end, i_center;
	int x;
	int i_largest = 0;
	uint32_t num = 0;

	i_start = 0;
	i_end   = count - 1;
	i_largest = -1;

	num = count;
	while (i_start <= i_end) {
		i_center = (i_start + i_end) / 2;

	        x = btree->cmp_cb(btree->cmp_cb_data, key, keylen,
				 objs[i_center].key, objs[i_center].key_len);
		if (x < 0) {
			/*
			 * We are smaller than i_center,
			 * So the last closest to our key 
			 * and largest seen is i_center so far.
			 */
			i_largest = i_center;
			i_end = i_center - 1;
		} else if (x > 0) {
			i_start = i_center + 1;
		} else {
			/*
			 * Got a match. Our btree stores the matching
			 * key on left node. So this keys is inclusive.
			 */
			i_largest = i_center + 1;
			break;
		}
	}


	if (i_largest >= 0 && i_largest <= (count - 1)) {
		num = i_largest; //count is index + 1
	}


#ifdef DEBUG_BUILD
	/*
	 * check that keys are sorted.
	 */
	for (i = 0; i < count - 1; i++) {
		if (btree->cmp_cb(NULL, objs[i].key, objs[i].key_len,
				  objs[i + 1].key, objs[i + 1].key_len) >= 0) {
			/*
			 * Found key out of place.
			 */
			assert(0);
		}
	}
#endif 

	assert(num);
	return num;
}


/*
 * Return true if there is a key > the given key, false otherwise.
 * The returned key is set in ks.
 */
static inline bool 
find_right_key_in_node(btree_raw_t *bt, btree_raw_node_t *n,
		      char *key, uint32_t keylen, 
		      key_stuff_t *ks, int *index, bool inclusive)
{
	int i_start, i_end, i_center;
	int x;
	int i_largest = 0;

	i_start = 0;
	i_end = n->nkeys - 1;
	i_largest = -1;

	if (index) {
		(*index) = -1;
	}

	while (i_start <= i_end) {
		i_center = (i_start + i_end) / 2;

		(void) get_key_stuff(bt, n, i_center, ks);
		x = bt->cmp_cb(bt->cmp_cb_data, key, keylen,
		                    ks->pkey_val, ks->keylen);

		if (x < 0) {
			i_largest = i_center;
			i_end = i_center - 1;
		} else if (x > 0) {
			i_start = i_center + 1;
		} else {
			/*
			 * Got a match for reference key.
			 * Our right key is same key if asked for
			 * inclusive or it will be next if not asked
			 * for inclusive.
			 */
			if (inclusive) {
				i_largest = i_center;
			} else {
				i_largest = i_center + 1;
			}
			break;
		}
	}

	if (i_largest >= 0 && i_largest <= (n->nkeys - 1)) {
		(void) get_key_stuff(bt, n, i_largest, ks);
		if (index != NULL) {
			(*index) = i_largest;
		}
		return true;
	}

	/*
	 * No key greater than the given key in this node.
	 */
	return false;
}

/*
 * Get number of keys that can be taken to child for mput
 * without voilating btree order.
 */
static inline uint32_t  
get_adjusted_num_objs(btree_raw_t *bt, btree_raw_node_t *n,
		     char *key, uint32_t keylen, 
		     btree_mput_obj_t *objs, uint32_t count)
{
	key_stuff_t ks;
	bool has_right_key = false;
	uint32_t new_count = count;

	if (count <= 1) {
		return count;
	}

	if (is_leaf(bt, n)) {
		return count;
	}
	
	has_right_key = find_right_key_in_node(bt, n, key,
					       keylen, &ks,
					       NULL, true);
	if (has_right_key == true) {
		new_count = get_keys_less_than(bt, ks.pkey_val,
						ks.keylen, objs, count);
	}
	assert(count > 0);
	return new_count;
}

/*
 * Insert a set of kyes to a leaf node. It takes are of finiding proper position of
 * key within leaf and also check for space in leaf.
 * Caller must make sure that this is the correct leat to insert.
 * The caller must also take write lock on leaf.
 *
 * Returns BTREE_SUCCESS if all keys inserted without a problem.
 * If any error while inserting, it will provide number of keys successfully 
 * inserted in 'objs_written' with an error code as return value.
 */
static btree_status_t
btree_insert_keys_leaf(btree_raw_t *btree, btree_metadata_t *meta, uint64_t syndrome, 
		       btree_raw_mem_node_t*mem_node, int write_type, uint64_t seqno,
		       btree_mput_obj_t *objs, uint32_t count, node_key_t *pk_insert,
		       node_key_t *pkrec, uint32_t *objs_written)

{

	btree_status_t ret = BTREE_SUCCESS;
	uint32_t written = 0;
	uint64_t child_id, child_id_before, child_id_after;
	int32_t  nkey_child;

	while ((written < count) && (!is_node_full(btree, mem_node->pnode, objs[written].key,
					   objs[written].key_len, objs[written].data_len,
					   meta, syndrome, write_type, pkrec))) {
		/*
		 * Write how many objects we can accomodate in this leaf
		 */

		if ((write_type != W_UPDATE || pkrec) &&
		    (write_type != W_CREATE || !pkrec)) {

			insert_key_low(&ret, btree, mem_node, objs[written].key,
				       objs[written].key_len, seqno,
				       objs[written].data_len, objs[written].data,
				       meta, syndrome, pkrec, pk_insert);

			written++;
			/*
			 * Find position for next key in this node.
			 */
			if (written < count) {
				pkrec = find_key(btree, mem_node->pnode, objs[written].key,
						 objs[written].key_len, &child_id, &child_id_before,
						 &child_id_after, &pk_insert, meta, syndrome, &nkey_child);
			}

			btree->log_cb(&ret, btree->log_cb_data, BTREE_UPDATE_NODE, btree, mem_node);
		} else {
			// write_type == W_UPDATE && !pkrec) || (write_type == W_CREATE && pkrec))

			/*
			 * key not found for an update! or key was found for an insert!
			 */
			ret = BTREE_KEY_NOT_FOUND;

			/*
			 * We dont try if any one failed.
			 */
			break;
		}
	}

	*objs_written = written;
	return ret;
}

/*
 * Main routine for inserting one or more keys in to a btree. 
 *
 * Single Mput algo:
 * It try to find the leaf where input key can be inserted. While doing the
 * search, it also splits any node in path that is full. Once leaf is found, the
 * key is inserted in to it.
 *
 * MPut algo: 
 * This rouine takes first keys in input set as reference and find the leaf where
 * this key can be inserted. As it goes towards the leaf in tree, it will keep on trimming
 * the input key list to what is actually applicable to the subtree where serach is heading.
 * Once it has that leaf, it will insert all keys that could
 * possibly fit to that leaf.
 */
btree_status_t 
btree_raw_mwrite_low(btree_raw_t *btree, btree_mput_obj_t *objs, uint32_t num_objs,
		     btree_metadata_t *meta, uint64_t syndrome, int write_type,
		     int* pathcnt, uint32_t *objs_written)
{

	int               split_pending = 0, parent_write_locked = 0;
	int32_t           nkey_child;
	uint64_t          child_id, child_id_before, child_id_after;
	node_key_t       *pk_insert, *pkrec = NULL;
	btree_status_t    ret = BTREE_SUCCESS;
	btree_status_t    txnret = BTREE_SUCCESS;
	btree_raw_node_t *node = NULL;
	btree_raw_mem_node_t *mem_node = NULL, *parent = NULL;
	uint32_t count = num_objs;
	uint32_t written = 0;
	uint64_t seqno = meta->seqno;

	*objs_written = 0;
	plat_rwlock_rdlock(&btree->lock);
	assert(referenced_nodes_count == 0);

restart:
	child_id = btree->rootid;

	while(child_id != BAD_CHILD) {
		if(!(mem_node = get_existing_node_low(&ret, btree, child_id, 1))) {
			ret = BTREE_FAILURE;
			goto err_exit;
		}

		node = mem_node->pnode;

mini_restart:
		(*pathcnt)++;

		if(is_leaf(btree, node) || split_pending) {
			plat_rwlock_wrlock(&mem_node->lock);
		} else {
			plat_rwlock_rdlock(&mem_node->lock);
		}

		if(!parent && child_id != btree->rootid) {
			/*
			 * While we reach here it is possible that root got changed.
			 */
			__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_PUT_RESTART_CNT]), 1);
			plat_rwlock_unlock(&mem_node->lock);
			if (BTREE_SUCCESS != deref_l1cache(btree)) {
				assert(0);
			}
			goto restart;
		}

		pkrec = find_key(btree, node, objs[0].key, objs[0].key_len,
				 &child_id, &child_id_before,
				 &child_id_after, &pk_insert,
				 meta, syndrome, &nkey_child);

		if (!is_node_full(btree, node, objs[0].key, objs[0].key_len,
				 objs[0].data_len, meta, syndrome, write_type, pkrec)) {

			if(parent && (!parent_write_locked || !parent->dirty)) {
				plat_rwlock_unlock(&parent->lock);
			}

			/*
			 * Get the set of keys less than child_id_after.
			 */
			count = get_adjusted_num_objs(btree, node, 
						      objs[0].key, objs[0].key_len,
						      objs, count);
			
			parent = mem_node;
			parent_write_locked = is_leaf(btree, node) || split_pending;
			split_pending = 0;
			continue;
		}

		/*
		 * Found a full node on the way, split it first.
		 */
		if(!split_pending && (!is_leaf(btree, node) ||
			    (parent && !parent_write_locked))) {

			plat_rwlock_unlock(&mem_node->lock);

			if(parent) {
				uint64_t save_modified = parent->modified;

				/*
				 * Promote lock of parent from read to write.
				 * parent->modified state used to check if anything
				 * changed during that promotion.
				 */
				plat_rwlock_unlock(&parent->lock);
				plat_rwlock_wrlock(&parent->lock);

				if(parent->modified != save_modified) {
					restart_cnt++;
					__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_PUT_RESTART_CNT]), 1);
					plat_rwlock_unlock(&parent->lock);
					parent = NULL;

					if (BTREE_SUCCESS != deref_l1cache(btree)) {
						assert(0);
					}
					
					/*
					 * Parent got changed while we tried to promote lock to write.
					 * restart from root with hope to get write lock next try.
					 */
					goto restart;
				}
				parent_write_locked = 1;
			}

			no_restart++;
			split_pending = 1;
			/*
			 * We have parent write lock, take write lock on current node.
			 * to do split.
			 */
			goto mini_restart;
		}

		if(is_root(btree, node)) {
			/*
			 * Split of root is special case since it 
			 * needs to allocate two new nodes.
			 */
			parent = get_new_node(&ret, btree, 0 /* flags */);
			if(!parent) {
				ret = BTREE_FAILURE;
				goto err_exit;
			}

			plat_rwlock_wrlock(&parent->lock);
			parent_write_locked = 1;

			dbg_print("root split %ld new root %ld\n",
				  btree->rootid, parent->pnode->logical_id);
			parent->pnode->rightmost  = btree->rootid;
			uint64_t saverootid = btree->rootid;
			btree->rootid = parent->pnode->logical_id;
			if (BTREE_SUCCESS != savepersistent( btree, 0)) {
				assert(0);
				btree->rootid = saverootid;
				ret = BTREE_FAILURE;
				goto err_exit;
			}
		}

		splits_cnt++;

		btree_raw_mem_node_t *new_node = btree_split_child(&ret, btree, parent,
							   mem_node, seqno, meta, syndrome);
		if(BTREE_SUCCESS != ret) {
			ret = BTREE_FAILURE;
			goto err_exit;
		}

		plat_rwlock_wrlock(&new_node->lock);

		split_pending = 0;

		/*
		 * Just split the node, so start again from parent.
		 */
		pkrec = find_key(btree, parent->pnode, objs[0].key, objs[0].key_len,
				 &child_id, &child_id_before, &child_id_after, 
				 &pk_insert, meta, syndrome, &nkey_child);
		assert(child_id != BAD_CHILD);

		/*
		 * Get the set of keys less than child_id_after.
		 */
		count = get_adjusted_num_objs(btree, parent->pnode, 
					      objs[0].key, objs[0].key_len,
					      objs, count);

		if(mem_node->pnode->logical_id != child_id) {
			/*
			 * The current key is part of either new now or current node.
			 * after split.
			 */
			mem_node = new_node;
		}

		node = mem_node->pnode;
		parent = mem_node;

		(*pathcnt)++;

		pkrec = find_key(btree, node, objs[0].key, objs[0].key_len, &child_id,
				 &child_id_before, &child_id_after, 
				 &pk_insert, meta, syndrome, &nkey_child);
		/*
		 * Get the set of keys less than child_id_after.
		 */
		count = get_adjusted_num_objs(btree, node, 
					      objs[0].key, objs[0].key_len,
					      objs, count);

	}

	dbg_print("before modifiing leaf node id %ld is_leaf: %d is_root: %d is_over: %d\n",
		  node->logical_id, is_leaf(btree, node), is_root(btree, node), is_overflow(btree, node));

	assert(is_leaf(btree, node));
	sets_cnt++;
	plat_rwlock_unlock(&btree->lock);

	written = 0;
	ret = btree_insert_keys_leaf(btree, meta, syndrome, mem_node, write_type,
				     seqno, &objs[0], count, pk_insert, pkrec, &written);
	*objs_written = written;	

	/*
	 * If we could not insert in to node , it might be unchanged.
	 * So no point of keeping it locked.
	 */
	if (ret != BTREE_SUCCESS && !mem_node->dirty) {
		plat_rwlock_unlock(&mem_node->lock);
		
	}

	/*
	 * the deref_l1cache will release the references and lock of modified nodes.
	 */
	if (BTREE_SUCCESS != deref_l1cache(btree)) {
		ret = BTREE_FAILURE;
	}

	if (written > 1) {
	    __sync_add_and_fetch(&(btree->stats.stat[BTSTAT_MPUT_IO_SAVED]), written - 1);
	}

	assert(referenced_nodes_count == 0);
	return BTREE_SUCCESS == ret ? txnret : ret;

err_exit:

	plat_rwlock_unlock(&btree->lock);
	assert(referenced_nodes_count == 0);
	return BTREE_SUCCESS == ret ? txnret : ret;
}

/*
 * return 0 if key falls in range
 * returns -1 if range_key is less than key.
 * returns +1 if range_key is greater than key.
 */
static int 
btree_key_in_range(btree_raw_t *bt, 
	     char *range_key, uint32_t range_key_len,
	     char *key, uint32_t keylen)
{
	int x = 0;

	if (keylen < range_key_len) {
		return 1;
	}

	x = bt->cmp_cb(bt->cmp_cb_data, range_key, range_key_len,
		       key, range_key_len); //adjust length

	// TBD: we should have a range check function passed from user.

	return x;
}

static inline bool
find_first_key_in_range(btree_raw_t *bt,
			btree_raw_node_t *n,
			char *range_key,
			uint32_t range_key_len,
			key_stuff_t *ks,
			int *index)
{
	int i_start, i_end, i_center;
	int i_last_match = -1;
	key_stuff_t ks_last_match = {0};
	key_stuff_t ks_tmp;
	int x = 0;

	if (index) {
		(*index) = -1;
	}

	i_start = 0;
	i_end = n->nkeys - 1;

	while (i_start <= i_end) {
		i_center = (i_start + i_end) / 2;

		(void) get_key_stuff(bt, n, i_center, &ks_tmp);
		x = btree_key_in_range(bt, range_key, range_key_len,
				       ks_tmp.pkey_val, ks_tmp.keylen);

		if (x <= 0) {
			/*
			 * Got first greater than or equal to range_key.
			 */	
			i_last_match = i_center;
			i_end = i_center - 1;
			ks_last_match = ks_tmp;
		} else if (x > 0) {
			i_start = i_center + 1;	
		}
	}

	if (i_last_match >= 0 && i_last_match <= (n->nkeys - 1)) {
		*ks = ks_last_match;
		(*index) = i_last_match;
		assert(!ks->fixed); //Not supprted for fixed keys yet
		return true;
	} else if (!is_leaf(bt, n)) {
		/*
		 * Range is greater than the last key in non leaf node.
		 * Only path we can follow is the rightmost child.
		 */
		ks->ptr = n->rightmost;
		(*index) = n->nkeys - 1;
		assert(i_start > (n->nkeys - 1));
		return true;

	}
	return false;
}

/*
 * Search the new key that applies to the given range.
 *
 * If Marker is set to NULL, this is the first key search, so search
 * first key that falls in the range. If marker is not null, search the next
 * key that falls in the range according to marker.
 *
 * Returns: if non-leaf node, the child_id is set to child that we must
 * 	    traverse to find the next range key to update.
 *	    If leaf node, the ks is set to next key that falls in range.
 */
static bool
find_next_rupdate_key(btree_raw_t *bt, btree_raw_node_t *n, char *range_key,
			   uint32_t range_key_len, key_stuff_t *ks,
			   uint64_t *child_id, btree_rupdate_marker_t **marker)
{
	bool res = false;
	int index = -1;

	*child_id = BAD_CHILD;

	if ((*marker)->set) {
		/*
		 * Get next key from the marker.
		 */
		res = find_right_key_in_node(bt, n,
					     (*marker)->last_key, (*marker)->last_key_len,
					     ks, &index, false);

		assert(res == false || bt->cmp_cb(bt->cmp_cb_data, ks->pkey_val, ks->keylen,
					     (*marker)->last_key, (*marker)->last_key_len) == 1);
				     	
		/*
		 * Search end at end of the node, consider righmost key as well
		 */
		if ((res == false) && !is_leaf(bt, n)) {
			ks->ptr = n->rightmost;
			res = true;
		}
	} else {
		/*
		 * First key in the range.
		 */
		res = find_first_key_in_range(bt, n, range_key,
					      range_key_len, ks,
					      &index);
	}

	if (res == true) {
		/*
		 * Init or update the marker.
		 * Marker is across calls, need to do a deep copy.
		 */
		if (is_leaf(bt, n)) {
			/*
			 * Marker get updated only for leaf nodes.
			 */
			if (btree_key_in_range(bt, 
					       range_key, range_key_len,
					       ks->pkey_val, ks->keylen) != 0) {
				/*
				 * Got a key out of range key in leaf.
				 */
				(*marker)->set = false;
				res = false;
			} else {
				memcpy((*marker)->last_key, ks->pkey_val, ks->keylen);
				(*marker)->last_key[ks->keylen] = 0;
				(*marker)->last_key_len = ks->keylen;
				(*marker)->index = index;
				(*marker)->set = true;
			}
		}

		/*
		 * Set the child id as well.
		 */
		*child_id = ks->ptr;
	}

	return res;
}

/*
 * Given a range and a leaf node, update all keys falling in that range.
 * Caller must hold lock and ref on this leaf.
 */
static btree_status_t
btree_rupdate_raw_leaf(
		struct btree_raw *btree, 
		btree_raw_mem_node_t *node,
	        char *range_key,
	        uint32_t range_key_len,
		btree_metadata_t *meta,
	        btree_rupdate_cb_t callback_func,
	        void * callback_args,	
	        uint32_t *objs_updated,
	        btree_rupdate_marker_t **marker)
{
	btree_status_t ret = BTREE_SUCCESS;
	key_stuff_t ks;
	node_vlkey_t  *pvlk;
	btree_rupdate_cb_t cb_func = 
			(btree_rupdate_cb_t) callback_func;
	char *new_data = NULL;
	uint64_t datalen = 0;
	uint64_t new_data_len = 0;
	int count = 0;
	uint32_t objs_done = 0;
	char **bufs = NULL;
	int i = 0;
	bool no_modify = true;
	uint64_t child_id = BAD_CHILD;
	uint64_t seqno = meta->seqno;
	char *key_local = NULL;
	uint32_t key_local_len = 0;;

	assert(is_leaf(btree, node->pnode));

	(*objs_updated) = 0;

	bufs = (char **) malloc(sizeof(char *) * node->pnode->nkeys);
	if (bufs == NULL) {
		ret = BTREE_FAILURE;
		goto exit;
	}
	
	while (find_next_rupdate_key(btree, node->pnode, range_key,
				         range_key_len, &ks, &child_id, marker) == true) {

		pvlk = (node_vlkey_t *) ks.pkey_struct;
		ret = get_leaf_data(btree, node->pnode, pvlk,
				    &bufs[count], &datalen, 0, 0);
		if (ret != BTREE_SUCCESS) {
			goto done;
		}

		new_data_len = 0;
		new_data = NULL;

		if (cb_func != NULL) {
			if ((*cb_func) (ks.pkey_val, ks.keylen,
					bufs[count], datalen,
					callback_args, &new_data, &new_data_len) == false) {
				/*
				 * object data not changed, no need to update.
				 */
				count++;
				continue;
			}
		}

		if (new_data_len != 0) {
			/*
			 * The callback has set new data in new_data.
			 */
			free(bufs[count]);		
			bufs[count] = new_data;
			datalen = new_data_len;
		}

		/*
		 * Copy the key to local structure.
		 */
		key_local = (char *) malloc(ks.keylen + 1);
		if (key_local == NULL) {
			count++;
			goto done;
		}
		memcpy(key_local, ks.pkey_val, ks.keylen);
		key_local_len = ks.keylen;
		
		if (is_full_update(btree, node->pnode, pvlk, ks.keylen, datalen)) {
			/*
			 * Node does not have space for new data.
			 */
			ret = BTREE_RANGE_UPDATE_NEEDS_SPACE;

			/*
			 * Set this key and data in marker to retry single key update
			 * for this key.
			 */
			(*marker)->retry_key = key_local;
			(*marker)->retry_keylen = key_local_len;
				
			(*marker)->retry_data = bufs[count];
			(*marker)->retry_datalen = datalen;
			goto done;
		}


		/*
		 * Update the key.
		 */
		insert_key(&ret, btree, node, key_local, key_local_len,
			   seqno, datalen, bufs[count], meta, 0);

		no_modify = false;

		free(key_local);
		key_local = NULL;
		key_local_len = 0;

		count++;
		objs_done++;
	}

done:
	if (count == 0) {
		/*
		 * Got the end, set marker to invalid
		 */
		(*marker)->set = false;
	}

	/*
	 * Flush this node to disk.
	 */
	(*objs_updated) = objs_done;

exit:
	/*
	 * Could not modify anything in node,
	 * so release the lock explicitly.
	 */
	if (no_modify) {
		plat_rwlock_unlock(&node->lock);
	}

	/*
	 * the deref_l1cache will release the lock of modified nodes.
         * Also the references of looked up nodes.
	 */
	if (BTREE_SUCCESS != deref_l1cache(btree)) {
		ret = BTREE_FAILURE;
	}

	/*
	 * Free the temporary buffers.
	 */
	if (bufs) {
		for (i = 0 ; i < count; i++) {
			free(bufs[i]);
		}
		free(bufs);
	}

	assert(referenced_nodes_count == 0);
	return ret;
}

static btree_status_t
btree_raw_rupdate_low(
		struct btree_raw *btree, 
		uint64_t node_id,
		btree_metadata_t *meta,
	        char *range_key,
	        uint32_t range_key_len,
	        btree_rupdate_cb_t callback_func,
	        void * callback_args,	
	        uint32_t *objs_updated,
	        btree_rupdate_marker_t **marker,
		btree_raw_mem_node_t *parent);

static btree_status_t
btree_rupdate_raw_non_leaf(
		struct btree_raw *btree, 
		btree_raw_mem_node_t *mem_node,
	        char *range_key,
	        uint32_t range_key_len,
		btree_metadata_t *meta,
	        btree_rupdate_cb_t callback_func,
	        void * callback_args,	
	        uint32_t *objs_updated,
	        btree_rupdate_marker_t **marker)
{
	key_stuff_t ks;
	uint64_t child_id = BAD_CHILD;
	btree_status_t ret = BTREE_SUCCESS;
	bool res = false;

	assert(!is_leaf(btree, mem_node->pnode));

	/*
	 * Not at leaf yet, keep on seraching down the tree.
	 */
	res = find_next_rupdate_key(btree, mem_node->pnode, 
					 range_key, range_key_len, 
					 &ks, &child_id, marker);

	/*
	 * Search cannot end at non-leaf.
	 */
	assert(res);
	if (res == true) {
		ret = btree_raw_rupdate_low(btree, child_id, meta, 
						 range_key, range_key_len, callback_func,
						 callback_args, objs_updated,
						 marker, mem_node);
	}

	return ret;
}

/*
 * Do range update for a subtree starting with other than root node.
 */
static btree_status_t
btree_raw_rupdate_low(
		struct btree_raw *btree, 
		uint64_t node_id,
		btree_metadata_t *meta,
	        char *range_key,
	        uint32_t range_key_len,
	        btree_rupdate_cb_t callback_func,
	        void * callback_args,	
	        uint32_t *objs_updated,
	        btree_rupdate_marker_t **marker,
		btree_raw_mem_node_t *parent)
{
	btree_raw_mem_node_t *mem_node = NULL;
	btree_status_t ret = BTREE_SUCCESS;

	mem_node = get_existing_node_low(&ret, btree, node_id, 1);
	if (ret != BTREE_SUCCESS) {
		plat_rwlock_unlock(&parent->lock);
		return ret;
	}

	/*
	 * Take write lock on leaf nodes and read on other nodes.
	 */
	if (is_leaf(btree, mem_node->pnode)) {
		plat_rwlock_wrlock(&mem_node->lock);
	} else {
		plat_rwlock_rdlock(&mem_node->lock);
	}

	plat_rwlock_unlock(&parent->lock);

	if (!is_leaf(btree, mem_node->pnode)) {
		/*
		 * Not at leaf yet, keep on seraching down the tree.
		 */
		ret = btree_rupdate_raw_non_leaf(btree, mem_node, range_key, range_key_len,
					  	      meta, callback_func, callback_args,
						      objs_updated, marker);

	}  else {

		/*
		 * Found the leaf, update the keys in range.
		 */
		ret = btree_rupdate_raw_leaf(btree, mem_node, range_key, range_key_len,
						  meta, callback_func, callback_args,
						  objs_updated, marker);	
	}

	return ret;
}

/*
 * Do range update for Btree tree.
 */
static btree_status_t
btree_raw_rupdate_low_root(
		struct btree_raw *btree, 
		btree_metadata_t *meta,
	        char *range_key,
	        uint32_t range_key_len,
	        btree_rupdate_cb_t callback_func,
	        void * callback_args,	
	        uint32_t *objs_updated,
	        btree_rupdate_marker_t **marker)
{
	btree_raw_mem_node_t *mem_node = NULL;
	btree_status_t ret = BTREE_SUCCESS;
	uint64_t node_id;

	plat_rwlock_rdlock(&btree->lock);

restart:
	node_id = btree->rootid;

	mem_node = get_existing_node_low(&ret, btree, node_id, 1);
	if (ret != BTREE_SUCCESS) {
		goto out;	
	}

	/*
	 * Take write lock on leaf nodes and read on other nodes.
	 */
	if (is_leaf(btree, mem_node->pnode)) {
		plat_rwlock_wrlock(&mem_node->lock);
	} else {
		plat_rwlock_rdlock(&mem_node->lock);
	}


	if (btree->rootid != node_id) {
		/*
		 * By the time we take lock on root node, tree
		 * got split and created another root node.
		 */
		plat_rwlock_unlock(&mem_node->lock);
		goto restart;
	}


	if (!is_leaf(btree, mem_node->pnode)) {
		/*
		 * Not at leaf yet, keep on seraching down the tree.
		 */
		ret = btree_rupdate_raw_non_leaf(btree, mem_node, range_key, range_key_len,
					  	      meta, callback_func, callback_args,
						      objs_updated, marker);
	}  else {

		/*
		 * Found the leaf, update the keys in range.
		 */
		ret = btree_rupdate_raw_leaf(btree, mem_node, range_key, range_key_len,
						  meta, callback_func, callback_args,
						  objs_updated, marker);	
	}

out:
	plat_rwlock_unlock(&btree->lock);
	return ret;
}

btree_status_t
btree_raw_rupdate(
		struct btree_raw *btree, 
		btree_metadata_t *meta,
	        char *range_key,
	        uint32_t range_key_len,
	        btree_rupdate_cb_t callback_func,
	        void * callback_args,	
	        uint32_t *objs_updated,
	        btree_rupdate_marker_t **marker)
{

	btree_status_t ret = BTREE_SUCCESS;

	ret = btree_raw_rupdate_low_root(btree, meta,
					 range_key, range_key_len, 
					 callback_func, callback_args,
					 objs_updated, marker);
	return ret;
}

static btree_status_t 
btree_raw_write(struct btree_raw *btree, char *key, uint32_t keylen,
		char *data, uint64_t datalen, btree_metadata_t *meta, int write_type)
{
    btree_status_t      ret = BTREE_SUCCESS;
    int                 pathcnt = 0;
    uint64_t            syndrome = get_syndrome(btree, key, keylen);
    btree_mput_obj_t objs; 
    uint32_t objs_done = 0;

    objs.key = key;
    objs.key_len = keylen;
    objs.data = data;
    objs.data_len = datalen;


    dbg_print_key(key, keylen, "write_type=%d ret=%d lic=%ld", write_type, ret, btree->logical_id_counter);

    dbg_print(" before dbg_referenced %ld\n", dbg_referenced);

    ret = btree_raw_mwrite_low(btree, &objs, 1, meta,
			       syndrome, write_type, &pathcnt, &objs_done);

    dbg_print("after dbg_referenced %ld\n", dbg_referenced);
    assert(!dbg_referenced);

    dbg_print_key(key, keylen, "write_type=%d ret=%d lic=%ld", write_type, ret, btree->logical_id_counter);

    //TODO change to atomics
    if (BTREE_SUCCESS == ret) {
        switch (write_type) {
	    case W_CREATE:
		__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_CREATE_CNT]),1);
		__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_CREATE_PATH]),pathcnt);
		break;
	    case W_SET:
		__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_SET_CNT]),1);
		__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_SET_PATH]), pathcnt);
		break;
	    case W_UPDATE:
		__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_UPDATE_CNT]),1);
		__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_UPDATE_PATH]), pathcnt);
		break;
	    default:
	        assert(0);
		break;
	}
    }

#ifdef BTREE_RAW_CHECK
    btree_raw_check(btree, (char *) __FUNCTION__, dump_key(key, keylen));
#endif

    return(ret);
}


static btree_status_t btree_raw_flush_low(btree_raw_t *btree, char *key, uint32_t keylen, uint64_t syndrome)
{
	btree_status_t    ret = BTREE_SUCCESS;
	node_key_t       *pkrec = NULL;
	btree_raw_mem_node_t *node = NULL;
	int				  pathcnt = 0;
	btree_metadata_t  meta;

	meta.flags = 0;

    plat_rwlock_rdlock(&btree->lock);

    pkrec = btree_raw_find(btree, key, keylen, syndrome, &meta, &node, 1 /* EX */, &pathcnt);

    plat_rwlock_unlock(&btree->lock);

    if (pkrec)
      btree->flush_node_cb(&ret, btree->flush_node_cb_data, (uint64_t) node->pnode->logical_id);

    deref_l1cache_node(btree, node);
    plat_rwlock_unlock(&node->lock);

    return ret;
}

//======================   FLUSH   =========================================
btree_status_t btree_raw_flush(struct btree_raw *btree, char *key, uint32_t keylen)
{
    btree_status_t      ret = BTREE_FAILURE;
    uint64_t            syndrome = get_syndrome(btree, key, keylen);

    dbg_print_key(key, keylen, "before ret=%d lic=%ld", ret, btree->logical_id_counter);

    ret = btree_raw_flush_low(btree, key, keylen, syndrome);

    dbg_print_key(key, keylen, "after ret=%d lic=%ld", ret, btree->logical_id_counter);

    //TODO change to atomics
    if (BTREE_SUCCESS == ret) 
        btree->stats.stat[BTSTAT_FLUSH_CNT]++;

#ifdef BTREE_RAW_CHECK
    btree_raw_check(btree, (char *) __FUNCTION__, dump_key(key, keylen));
#endif

    assert(!dbg_referenced);

    return(ret);
}

//======================   INSERT  =========================================

btree_status_t btree_raw_insert(struct btree_raw *btree, char *key, uint32_t keylen, char *data, uint64_t datalen, btree_metadata_t *meta)
{
    return(btree_raw_write(btree, key, keylen, data, datalen, meta, W_CREATE));
}

//======================   UPDATE  =========================================

btree_status_t btree_raw_update(struct btree_raw *btree, char *key, uint32_t keylen, char *data, uint64_t datalen, btree_metadata_t *meta)
{
    return(btree_raw_write(btree, key, keylen, data, datalen, meta, W_UPDATE));
}

//======================   UPSERT (SET)  =========================================

btree_status_t btree_raw_set(struct btree_raw *btree, char *key, uint32_t keylen, char *data, uint64_t datalen, btree_metadata_t *meta)
{
    return(btree_raw_write(btree, key, keylen, data, datalen, meta, W_SET));
}

btree_status_t
btree_raw_mput(struct btree_raw *btree, btree_mput_obj_t *objs, uint32_t num_objs,
	       uint32_t flags, btree_metadata_t *meta, uint32_t *objs_written)
{
	btree_status_t      ret = BTREE_SUCCESS;
	int                 pathcnt = 0;
	uint64_t            syndrome = 0;  //no use of syndrome in variable keys
	int write_type = 0;

	if (flags & FDF_WRITE_MUST_NOT_EXIST) {
		write_type = W_CREATE;
	} else if (flags & FDF_WRITE_MUST_EXIST) {
		write_type = W_UPDATE;
	} else {
		write_type = W_SET;
	}

	ret = btree_raw_mwrite_low(btree, objs, num_objs, meta, syndrome, 
				   write_type, &pathcnt, objs_written);
	if (BTREE_SUCCESS == ret) {
		switch (write_type) {
		case W_CREATE:
			__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_CREATE_CNT]),*objs_written);
			__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_CREATE_PATH]),pathcnt);
			break;
		case W_SET:
			__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_SET_CNT]),*objs_written);
			__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_SET_PATH]), pathcnt);
			break;
		case W_UPDATE:
			__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_UPDATE_CNT]),*objs_written);
			__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_UPDATE_PATH]), pathcnt);
			break;
		default:
			assert(0);
		}
	}
	return ret;
}

//======================   DELETE   =========================================

static int is_leaf_minimal_after_delete(btree_raw_t *btree, btree_raw_node_t *n, node_vlkey_t* pk)
{
    assert(n->flags & LEAF_NODE);
    uint32_t datalen = ((pk->keylen + pk->datalen) < btree->big_object_size) ? pk->datalen : 0;
    uint32_t nbytes_used = (btree->nodesize - n->insert_ptr - pk->keylen - datalen) + (n->nkeys - 1) * sizeof(node_vlkey_t);
    return 2 * nbytes_used < btree->nodesize - sizeof(btree_raw_node_t);
}

static int is_minimal(btree_raw_t *btree, btree_raw_node_t *n, uint32_t l_balance_keylen, uint32_t r_balance_keylen)
{
    uint32_t   nbytes_used;
    int        ret = 0;
    uint32_t   max_balance_keylen;

    if (n->logical_id == btree->rootid) {
        // root
	if (!(n->flags & LEAF_NODE) && (n->nkeys == 0)) {
	    ret = 1;
	} else {
	    ret = 0;
	}
    } else {
        // non-root
	if (n->flags & LEAF_NODE) {
	    nbytes_used = (btree->nodesize - n->insert_ptr) + n->nkeys*sizeof(node_vlkey_t);
	} else if (btree->flags & SYNDROME_INDEX) {
	    //  The '+1' here is to allow for conversion of a rightmost pointer to
	    //  a key value during a merge!
	    nbytes_used = (n->nkeys + 1)*sizeof(node_fkey_t);
	} else {
	    max_balance_keylen = (l_balance_keylen > r_balance_keylen) ? l_balance_keylen : r_balance_keylen;
	    nbytes_used  = (btree->nodesize - n->insert_ptr) + n->nkeys*sizeof(node_vkey_t);
	    //  This allows for conversion of the rightmost 
	    //  pointer to a normal key, using the anchor key value.
	    nbytes_used  += max_balance_keylen + sizeof(node_vkey_t);
	}
	if ((2*nbytes_used) < (btree->nodesize - sizeof(btree_raw_node_t))) {
	    ret = 1;
	} else {
	    ret = 0;
	}
    }
    return(ret);
}

/*   delete a key
 *
 *   returns BTREE_SUCCESS
 *   returns BTREE_FAILURE
 *   returns BTREE_NOT_FOUND
 *
 *   Reference: "Implementing Deletion in B+-trees", Jan Jannink, SIGMOD RECORD,
 *              Vol. 24, No. 1, March 1995
 */
btree_status_t btree_raw_delete(struct btree_raw *btree, char *key, uint32_t keylen, btree_metadata_t *meta)
{
    btree_status_t        ret = BTREE_SUCCESS;
    btree_status_t        txnret = BTREE_SUCCESS;
    int                   pathcnt = 0, opt;
    btree_raw_mem_node_t     *node;
    node_key_t           *keyrec;
    uint64_t              syndrome = get_syndrome(btree, key, keylen);

    assert(locked == 1);

    plat_rwlock_rdlock(&btree->lock);

    keyrec = btree_raw_find(btree, key, keylen, syndrome, meta, &node, 1 /* EX */, &pathcnt);

    /* Check if delete without restructure is possible */
    opt = keyrec && _keybuf && !is_leaf_minimal_after_delete(btree, node->pnode, (node_vlkey_t*)keyrec);

    if(opt) {
        ref_l1cache(btree, node);
	delete_key_by_pkrec(&ret, btree, node, keyrec);
	__sync_add_and_fetch(&(btree->stats.stat[BTSTAT_DELETE_OPT_CNT]),1);
    }
    else
    {
        deref_l1cache_node(btree, node);
        plat_rwlock_unlock(&node->lock);
    }

    plat_rwlock_unlock(&btree->lock);

    if (opt && BTREE_SUCCESS != deref_l1cache(btree))
        ret = BTREE_FAILURE;

    dbg_print_key(key, keylen, "ret=%d keyrec=%d, opt=%d", ret, keyrec, opt);

    if(!keyrec)
        return BTREE_KEY_NOT_FOUND; // key not found

    if(BTREE_SUCCESS != ret || BTREE_SUCCESS != txnret)
        return BTREE_SUCCESS == ret ? txnret : ret;

    if(opt) {
#ifdef BTREE_RAW_CHECK
    btree_raw_check(btree, (char *) __FUNCTION__, dump_key(key, keylen));
#endif
	return BTREE_SUCCESS; // optimistic delete succeeded
    }

    dbg_print("dbg_referenced %ld\n", dbg_referenced);
    assert(locked == 1);
    assert(!dbg_referenced);

    /* Need tree restructure. Write lock whole tree and retry */
    plat_rwlock_wrlock(&btree->lock);

    // make sure that the temporary key buffer has been allocated
    if (check_per_thread_keybuf(btree)) {
        plat_rwlock_unlock(&btree->lock);

	return(BTREE_FAILURE); // xxxzzz is this the best I can do?
    }

    (void) find_rebalance(&ret, btree, btree->rootid, BAD_CHILD, BAD_CHILD, BAD_CHILD, NULL, BAD_CHILD, NULL, 0, 0, key, keylen, meta, syndrome);

    lock_modified_nodes(btree);

    plat_rwlock_unlock(&btree->lock);

    if (BTREE_SUCCESS != deref_l1cache(btree)) {
        ret = BTREE_FAILURE;
    }

    __sync_add_and_fetch(&(btree->stats.stat[BTSTAT_DELETE_CNT]),1);
    __sync_add_and_fetch(&(btree->stats.stat[BTSTAT_DELETE_PATH]), pathcnt);

    dbg_print("dbg_referenced %ld\n", dbg_referenced);
    assert(locked == 1);
    assert(!dbg_referenced);

#ifdef BTREE_RAW_CHECK
    btree_raw_check(btree, (char *) __FUNCTION__, dump_key(key, keylen));
#endif

    return(ret);
}

/*   recursive deletion/rebalancing routine
 *
 *   ret = 0: don't rebalance this level
 *   ret = 1: rebalance this level if necessary
 *
 */
static int find_rebalance(btree_status_t *ret, btree_raw_t *btree, uint64_t this_id, uint64_t left_id, uint64_t right_id, uint64_t l_anchor_id, key_stuff_t *l_anchor_stuff, uint64_t r_anchor_id, key_stuff_t *r_anchor_stuff, int l_this_parent_in, int r_this_parent_in, char *key, uint32_t keylen, btree_metadata_t *meta, uint64_t syndrome)
{
    node_key_t         *keyrec;
    node_key_t         *pk_insert;
    btree_raw_mem_node_t   *this_mem_node, *left_mem_node, *right_mem_node;
    btree_raw_node_t   *this_node, *left_node, *right_node;
    uint64_t            next_node, next_left, next_right, next_l_anchor, next_r_anchor;
    uint64_t            child_id, child_id_before, child_id_after;
    int                 l_this_parent, r_this_parent;
    key_stuff_t         ks, ks_l, ks_r;
    key_stuff_t        *next_l_anchor_stuff;
    key_stuff_t        *next_r_anchor_stuff;
    int32_t             nkey_child;
    int                 do_rebalance = 1;
    uint32_t            l_balance_keylen = 0;
    uint32_t            r_balance_keylen = 0;

    if (*ret) { return(0); }

    this_mem_node = get_existing_node(ret, btree, this_id);
    this_node = this_mem_node->pnode;
    assert(this_node != NULL); // xxxzzz remove this
    _pathcnt++;

    //  PART 1: recursive descent from root to leaf node

        //  find path in this node for key
    keyrec = find_key(btree, this_node, key, keylen, &child_id, &child_id_before, &child_id_after, &pk_insert, meta, syndrome, &nkey_child);

    next_node = child_id;

    if (is_leaf(btree, this_node)) {
        if (keyrec) {
	    // key found at leaf
            // remove entry from a leaf node
            delete_key(ret, btree, this_mem_node, key, keylen, meta, syndrome);
            btree->log_cb(ret, btree->log_cb_data, BTREE_UPDATE_NODE, btree, this_mem_node);
	} else {
	    // key NOT found at leaf
	    *ret = 1;
	}
    } else {
        //   this node is internal

	    // calculate neighbor and anchor nodes
	if (child_id_before == BAD_CHILD) {
	    // next_node is least entry in this_node
	    if (left_id != BAD_CHILD) {
		left_mem_node = get_existing_node(ret, btree, left_id);
                left_node = left_mem_node->pnode;
		next_left = left_node->rightmost;
	    } else {
		next_left = BAD_CHILD;
	    }
	    next_l_anchor       = l_anchor_id;
	    next_l_anchor_stuff = l_anchor_stuff;
	    l_this_parent       = 0;
	    if (l_anchor_stuff == NULL) {
	        l_balance_keylen = 0;
	    } else {
	        l_balance_keylen = l_anchor_stuff->keylen;
	    }
	} else {
	    next_left           = child_id_before;
	    next_l_anchor       = this_node->logical_id;
	    (void) get_key_stuff(btree, this_node, nkey_child - 1, &ks_l);
	    next_l_anchor_stuff = &ks_l;
	    l_this_parent       = 1;
	    l_balance_keylen    = ks_l.keylen;
	}

	if (child_id_after == BAD_CHILD) {
	    // next_node is greatest entry in this_node
	    if (right_id != BAD_CHILD) {
		right_mem_node = get_existing_node(ret, btree, right_id);
                right_node = right_mem_node->pnode;
		assert(right_node); // xxxzzz fix this!
		(void) get_key_stuff(btree, right_node, 0, &ks);
		next_right = ks.ptr;
	    } else {
		next_right = BAD_CHILD;
	    }
	    next_r_anchor       = r_anchor_id;
	    next_r_anchor_stuff = r_anchor_stuff;
	    r_this_parent       = 0;
	    if (r_anchor_stuff == NULL) {
	        r_balance_keylen = 0;
	    } else {
	        r_balance_keylen = r_anchor_stuff->keylen;
	    }
	} else {
	    next_right          = child_id_after;
	    next_r_anchor       = this_node->logical_id;
	    (void) get_key_stuff(btree, this_node, nkey_child, &ks_r);
	    next_r_anchor_stuff = &ks_r;
	    r_this_parent       = 1;
	    r_balance_keylen    = ks_r.keylen;
	}

	    // recursive call
	do_rebalance = find_rebalance(ret, btree, next_node, next_left, next_right, next_l_anchor, next_l_anchor_stuff, next_r_anchor, next_r_anchor_stuff, l_this_parent, r_this_parent, key, keylen, meta, syndrome);
    }

	//  does this node need to be rebalanced?
    if ((!do_rebalance) || (!is_minimal(btree, this_node, l_balance_keylen, r_balance_keylen)))
	return 0;

    if (this_id == btree->rootid) {
        collapse_root(ret, btree, this_mem_node);
	return 0;
    }

    return rebalance(ret, btree, this_mem_node, left_id, right_id, l_anchor_id, l_anchor_stuff, r_anchor_id, r_anchor_stuff, l_this_parent_in, r_this_parent_in, meta);
}

static void collapse_root(btree_status_t *ret, btree_raw_t *btree, btree_raw_mem_node_t *old_root_mem_node)
{
    btree_raw_node_t* old_root_node = old_root_mem_node->pnode;

    if (*ret) { return; }

    if (is_leaf(btree, old_root_node)) {
	//  just keep old empty root node
        if (old_root_node->nkeys != 0) {
	    *ret = 1; // this should never happen!
	}
    } else {
	assert(old_root_node->nkeys == 0);
	assert(old_root_node->rightmost != BAD_CHILD);
	btree->rootid = old_root_node->rightmost;
        if (BTREE_SUCCESS != savepersistent( btree, 0))
                assert( 0);
	free_node(ret, btree, old_root_mem_node);
    }
    return;
}

static void update_ptr(btree_raw_t *btree, btree_raw_node_t *n, uint32_t nkey, uint64_t ptr)
{
    node_vlkey_t  *pvlk;
    node_vkey_t   *pvk;
    node_fkey_t   *pfk;

    if (is_leaf(btree, n)) {
	pvlk = ((node_vlkey_t *) n->keys) + nkey;
	pvlk->ptr = ptr;
    } else if (btree->flags & SECONDARY_INDEX) {
	pvk      = ((node_vkey_t *) n->keys) + nkey;
	pvk->ptr = ptr;
    } else {
	pfk      = ((node_fkey_t *) n->keys) + nkey;
	pfk->ptr = ptr;
    }
}

/*   Equalize keys between 'from' node and 'to' node, given that 'to' is to right of 'from'.
 */
static void shift_right(btree_raw_t *btree, btree_raw_node_t *anchor, btree_raw_node_t *from, btree_raw_node_t *to, char *s_key, uint32_t s_keylen, uint64_t s_syndrome, uint64_t s_seqno, char **r_key_out, uint32_t *r_keylen_out, uint64_t *r_syndrome_out, uint64_t *r_seqno_out)
{
    int            i;
    uint32_t       threshold;
    node_fkey_t   *pfk;
    node_vkey_t   *pvk;
    node_vlkey_t  *pvlk;
    uint32_t       nbytes_fixed;
    uint32_t       nbytes_free;
    uint32_t       nbytes_needed;
    uint32_t       nkeys_shift;
    uint32_t       nbytes_shift;
    uint32_t       nbytes_shift_old;
    key_stuff_t    ks;
    uint32_t       nbytes_f;
    uint32_t       nbytes_t;
    uint32_t       nbytes;

    char          *r_key;
    uint32_t       r_keylen;
    uint64_t       r_syndrome;
    uint64_t       r_seqno;
    uint64_t       r_ptr;

    __sync_add_and_fetch(&(btree->stats.stat[BTSTAT_RSHIFTS]),1);

    (void) get_key_stuff(btree, from, 0, &ks);
    nbytes_fixed = ks.offset;

    if (ks.fixed) {
        if (from->nkeys <= to->nkeys) {
	    *r_key_out = NULL;
	    return;
	}
	// xxxzzz should the following takes into account the inclusion of the anchor separator key?
        nkeys_shift = (from->nkeys - to->nkeys)/2;
	if (nkeys_shift == 0) {
	    nkeys_shift = 1;
	}
        nbytes_shift = nkeys_shift*ks.offset;
	pfk = (node_fkey_t *) ((char *) from->keys + (from->nkeys - nkeys_shift)*nbytes_fixed);
	r_key      = (char *) pfk->key;
	r_keylen   = sizeof(uint64_t);
	r_syndrome = pfk->key;
	r_seqno    = pfk->seqno;
	r_ptr      = pfk->ptr;

    } else {

        nkeys_shift  = 0;
	nbytes_shift = 0;
	nbytes_f     = (btree->nodesize - from->insert_ptr) + from->nkeys*nbytes_fixed;
	nbytes_t     = (btree->nodesize - to->insert_ptr)   + to->nkeys*nbytes_fixed;
	if ((nbytes_f <= nbytes_t) || (from->nkeys <= 1)) {
	    *r_key_out = NULL;
	    return;
	}
        threshold    = (nbytes_f - nbytes_t)/2;

        nbytes_shift_old = 0;
	for (i=0; i<from->nkeys; i++) {
	    (void) get_key_stuff(btree, from, from->nkeys - 1 - i, &ks);
	    nbytes = ks.keylen;
	    if (ks.leaf) {
		if ((ks.keylen + ks.datalen) < btree->big_object_size) { // xxxzzz check this!
		    nbytes += ks.datalen;
		}
	    }
	    nbytes_shift_old = nbytes_shift;
	    nbytes_shift += nbytes;
	    nkeys_shift++;
	    if (ks.leaf) {
		if ((nbytes_shift + nkeys_shift*nbytes_fixed) >= threshold) {
		    break;
		}
	    } else {
		// the following takes into account the inclusion of the anchor separator key!
		if ((nbytes_shift + nkeys_shift*nbytes_fixed + (s_keylen - ks.keylen)) >= threshold) {
		    break;
		}
	    }
	}
	assert(i < from->nkeys); // xxxzzz remove this!

	if (nkeys_shift >= from->nkeys) {
	    nkeys_shift--;
	    nbytes_shift = nbytes_shift_old;
	}

	if (ks.leaf) {
	    pvlk = (node_vlkey_t *) ((char *) from->keys + (from->nkeys - nkeys_shift - 1)*nbytes_fixed);
	    // copy the key into the non-volatile per-thread buffer
	    assert(_keybuf);
	    memcpy(_keybuf, (char *) from + pvlk->keypos, pvlk->keylen);
	    r_key      = _keybuf;
	    r_keylen   = pvlk->keylen;
	    r_syndrome = pvlk->syndrome;
	    r_seqno    = pvlk->seqno;
	    r_ptr      = pvlk->ptr;
	} else {
	    pvk = (node_vkey_t *) ((char *) from->keys + (from->nkeys - nkeys_shift)*nbytes_fixed);
	    // copy the key into the non-volatile per-thread buffer
	    assert(_keybuf);
	    memcpy(_keybuf, (char *) from + pvk->keypos, pvk->keylen);
	    r_key      = _keybuf;
	    r_keylen   = pvk->keylen;
	    r_syndrome = 0;
	    r_seqno    = pvk->seqno;
	    r_ptr      = pvk->ptr;
	}
    }

    if (ks.leaf) {
        nbytes_free    = vlnode_bytes_free(to);
	nbytes_needed  = nbytes_shift + nkeys_shift*sizeof(node_vlkey_t);
	assert(nbytes_free >= nbytes_needed); // xxxzzz remove this!
	// make room for the lower fixed keys
	memmove((char *) to->keys + nkeys_shift*nbytes_fixed, (char *) to->keys, to->nkeys*nbytes_fixed);

	// copy the fixed size portion of the keys
	memcpy(to->keys, (char *) from->keys + (from->nkeys - nkeys_shift)*nbytes_fixed, nkeys_shift*nbytes_fixed);
	to->nkeys   = to->nkeys + nkeys_shift;
	from->nkeys = from->nkeys - nkeys_shift;

    } else {
	if (ks.fixed) {
	    assert((to->nkeys + nkeys_shift) <= btree->fkeys_per_node); // xxxzzz remove this!
	} else {
            nbytes_free    = vnode_bytes_free(to);
	    nbytes_needed  = (nbytes_shift - r_keylen + s_keylen) + nkeys_shift*sizeof(node_vkey_t);
	    assert(nbytes_free >= nbytes_needed); // xxxzzz remove this!
	}

	// make room for the lower fixed keys, plus the separator from the anchor
	memmove((char *) to->keys + nkeys_shift*nbytes_fixed, (char *) to->keys, to->nkeys*nbytes_fixed);
	// copy the fixed size portion of the keys
	memcpy(to->keys, (char *) from->keys + (from->nkeys - nkeys_shift + 1)*nbytes_fixed, (nkeys_shift - 1)*nbytes_fixed);
	to->nkeys   = to->nkeys + nkeys_shift;
	from->nkeys = from->nkeys - nkeys_shift; // convert last key to rightmost pointer

	// copy 'from' rightmost pointer
	if (ks.fixed) {
	    pfk = (node_fkey_t *) ((char *) to->keys + (nkeys_shift - 1)*nbytes_fixed);
	    pfk->key   = (uint64_t) s_key;
	    pfk->ptr   = from->rightmost;
	    pfk->seqno = s_seqno;
	} else {
	    pvk = (node_vkey_t *) ((char *) to->keys + (nkeys_shift - 1)*nbytes_fixed);
	    pvk->keylen = s_keylen;
	    pvk->keypos = 0; // will be set in update_keypos below
	    pvk->ptr    = from->rightmost;
	    pvk->seqno  = s_seqno;
	}
    }

    // copy variable sized stuff

    if (ks.fixed) {
	to->insert_ptr = 0;
    } else {
	// for variable sized keys, copy the variable sized portion
	//  For leaf nodes, copy the data too

        if (ks.leaf) {
	    memcpy(((char *) to) + to->insert_ptr - nbytes_shift, 
		   ((char *) from) + btree->nodesize - nbytes_shift,
		   nbytes_shift);
	    // clean up 'from' variable stuff
	    memmove(((char *) from) + from->insert_ptr + nbytes_shift, 
		    ((char *) from) + from->insert_ptr,
		    (btree->nodesize - from->insert_ptr) - nbytes_shift);

	    to->insert_ptr   = to->insert_ptr   - nbytes_shift;
	    from->insert_ptr = from->insert_ptr + nbytes_shift;
	} else {
	    //  for non-leaves, include the 'right' pointer from the 'from' node

	    memcpy(((char *) to) + to->insert_ptr - (nbytes_shift - r_keylen) - s_keylen, 
		   ((char *) from) + btree->nodesize - (nbytes_shift - r_keylen),
		   nbytes_shift - r_keylen);
	    memcpy(((char *) to) + to->insert_ptr - s_keylen, s_key, s_keylen);

	    // clean up 'from' variable stuff
	    memmove(((char *) from) + from->insert_ptr + nbytes_shift, 
		    ((char *) from) + from->insert_ptr,
		    btree->nodesize - from->insert_ptr - nbytes_shift);

	    to->insert_ptr   = to->insert_ptr   - (nbytes_shift - r_keylen) - s_keylen;
	    from->insert_ptr = from->insert_ptr + (nbytes_shift - r_keylen) + r_keylen;
	}
	
	//  update the keypos pointers
        update_keypos(btree, to,   0);
        update_keypos(btree, from, 0);
    }

    // update the rightmost pointer of the 'from' node
    from->rightmost = r_ptr;

    *r_key_out      = r_key;
    *r_keylen_out   = r_keylen;
    *r_syndrome_out = r_syndrome;
    *r_seqno_out    = r_seqno;

    #ifdef DEBUG_STUFF
	if (Verbose) {
	    char stmp[10000];
	    int  len;
	    if (btree->flags & SYNDROME_INDEX) {
	        sprintf(stmp, "%p", s_key);
		len = strlen(stmp);
	    } else {
		strncpy(stmp, s_key, s_keylen);
		len = s_keylen;
	    }
	    fprintf(stderr, "********  After shift_right for key '%s' [syn=%lu] (from=%p, to=%p) B-Tree:  *******\n", dump_key(stmp, len), s_syndrome, from, to);
	    btree_raw_dump(stderr, btree);
	}
    #endif

    return;
}

/*   Equalize keys between 'from' node and 'to' node, given that 'to' is to the left of 'from'.
 */
static void shift_left(btree_raw_t *btree, btree_raw_node_t *anchor, btree_raw_node_t *from, btree_raw_node_t *to, char *s_key, uint32_t s_keylen, uint64_t s_syndrome, uint64_t s_seqno, char **r_key_out, uint32_t *r_keylen_out, uint64_t *r_syndrome_out, uint64_t *r_seqno_out)
{
    int            i;
    uint32_t       threshold;
    node_fkey_t   *pfk;
    node_vkey_t   *pvk;
    node_vlkey_t  *pvlk;
    uint32_t       nbytes_fixed;
    uint32_t       nbytes_free;
    uint32_t       nbytes_needed;
    uint32_t       nkeys_shift;
    uint32_t       nbytes_shift;
    uint32_t       nbytes_shift_old;
    key_stuff_t    ks;
    uint32_t       nbytes_f;
    uint32_t       nbytes_t;
    uint32_t       nbytes_to;
    uint32_t       nbytes;

    char          *r_key;
    uint32_t       r_keylen;
    uint64_t       r_syndrome;
    uint64_t       r_seqno;
    uint64_t       r_ptr;

    __sync_add_and_fetch(&(btree->stats.stat[BTSTAT_LSHIFTS]),1);

    (void) get_key_stuff(btree, from, 0, &ks);
    nbytes_fixed = ks.offset;

    if (ks.fixed) {
        if (from->nkeys <= to->nkeys) {
	    *r_key_out = NULL;
	    return;
	}
	// xxxzzz should the following takes into account the inclusion of the anchor separator key?
        nkeys_shift = (from->nkeys - to->nkeys)/2;
	if (nkeys_shift == 0) {
	    nkeys_shift = 1; // always shift at least one key!
	}
        nbytes_shift = nkeys_shift*ks.offset;
	pfk = (node_fkey_t *) ((char *) from->keys + (nkeys_shift - 1)*nbytes_fixed);
	r_key      = (char *) pfk->key;
	r_keylen   = sizeof(uint64_t);
	r_syndrome = pfk->key;
	r_seqno    = pfk->seqno;
	r_ptr      = pfk->ptr;

    } else {

        nkeys_shift  = 0;
	nbytes_shift = 0;
	nbytes_f     = (btree->nodesize - from->insert_ptr) + from->nkeys*nbytes_fixed;
	nbytes_t     = (btree->nodesize - to->insert_ptr)   + to->nkeys*nbytes_fixed;
	if ((nbytes_f <= nbytes_t) || (from->nkeys <= 1)) {
	    *r_key_out = NULL;
	    return;
	}
        threshold    = (nbytes_f - nbytes_t)/2;

        nbytes_shift_old = 0;
	for (i=0; i<from->nkeys; i++) {
	    (void) get_key_stuff(btree, from, i, &ks);
	    nbytes = ks.keylen;
	    if (ks.leaf) {
		if ((ks.keylen + ks.datalen) < btree->big_object_size) { // xxxzzz check this!
		    nbytes += ks.datalen;
		}
	    }
	    nbytes_shift_old = nbytes_shift;
	    nbytes_shift    += nbytes;
	    nkeys_shift++;
	    if (ks.leaf) {
		if ((nbytes_shift + nkeys_shift*nbytes_fixed) >= threshold) {
		    break;
		}
	    } else {
		// the following takes into account the inclusion of the anchor separator key!
		if ((nbytes_shift + nkeys_shift*nbytes_fixed + (s_keylen - ks.keylen)) >= threshold) {
		    break;
		}
	    }
	}
	assert(i < from->nkeys); // xxxzzz remove this!
	if (nkeys_shift >= from->nkeys) {
	    nkeys_shift--;
	    nbytes_shift = nbytes_shift_old;
	}

	if (ks.leaf) {
	    pvlk = (node_vlkey_t *) ((char *) from->keys + (nkeys_shift - 1)*nbytes_fixed);
	    // copy the key into the non-volatile per-thread buffer
	    assert(_keybuf);
	    memcpy(_keybuf, (char *) from + pvlk->keypos, pvlk->keylen);
	    r_key      = _keybuf;
	    r_keylen   = pvlk->keylen;
	    r_syndrome = pvlk->syndrome;
	    r_seqno    = pvlk->seqno;
	    r_ptr      = pvlk->ptr;
	} else {
	    pvk = (node_vkey_t *) ((char *) from->keys + (nkeys_shift - 1)*nbytes_fixed);
	    // copy the key into the non-volatile per-thread buffer
	    assert(_keybuf);
	    memcpy(_keybuf, (char *) from + pvk->keypos, pvk->keylen);
	    r_key      = _keybuf;
	    r_keylen   = pvk->keylen;
	    r_syndrome = 0;
	    r_seqno    = pvk->seqno;
	    r_ptr      = pvk->ptr;
	}
    }

    if (ks.leaf) {
        nbytes_free    = vlnode_bytes_free(to);
	nbytes_needed  = nbytes_shift + nkeys_shift*sizeof(node_vlkey_t);
	assert(nbytes_free >= nbytes_needed); // xxxzzz remove this!

	// copy the fixed size portion of the keys
	memcpy((char *) to->keys + to->nkeys*nbytes_fixed, from->keys, nkeys_shift*nbytes_fixed);

	// remove keys from 'from' node
	memmove(from->keys, (char *) from->keys + nkeys_shift*nbytes_fixed, (from->nkeys - nkeys_shift)*nbytes_fixed);

	to->nkeys   = to->nkeys + nkeys_shift;
	from->nkeys = from->nkeys - nkeys_shift;

    } else {
	if (ks.fixed) {
	    //  this allows for the conversion of the 'to' right ptr to a regular key:
	    assert((to->nkeys + nkeys_shift) <= btree->fkeys_per_node); // xxxzzz remove this!
	} else {
            nbytes_free    = vnode_bytes_free(to);
	    nbytes_needed  = (nbytes_shift - r_keylen + s_keylen) + nkeys_shift*sizeof(node_vkey_t);
	    assert(nbytes_free >= nbytes_needed); // xxxzzz remove this!
	}

	// copy the fixed size portion of the keys
	    //  this allows for the conversion of the 'to' right ptr to a regular key:
	memcpy((char *) to->keys + (to->nkeys + 1)*nbytes_fixed, from->keys, (nkeys_shift - 1)*nbytes_fixed);

	// remove keys from 'from' node
	memmove(from->keys, (char *) from->keys + nkeys_shift*nbytes_fixed, (from->nkeys - nkeys_shift)*nbytes_fixed);

	// convert 'to' rightmost pointer into a regular key
	if (ks.fixed) {
	    pfk = (node_fkey_t *) ((char *) to->keys + to->nkeys*nbytes_fixed);
	    pfk->key   = (uint64_t) s_key;
	    pfk->ptr   = to->rightmost;
	    pfk->seqno = s_seqno;
	} else {
	    pvk = (node_vkey_t *) ((char *) to->keys + to->nkeys*nbytes_fixed);
	    pvk->keylen = s_keylen;
	    pvk->keypos = 0; // will be set in update_keypos below
	    pvk->ptr    = to->rightmost;
	    pvk->seqno  = s_seqno;
	}
	
	//  Update nkeys AFTER converting rightmost pointer so that nkeys math
	//  above is correct!

	to->nkeys   = to->nkeys + nkeys_shift;
	from->nkeys = from->nkeys - nkeys_shift; // convert last key to rightmost pointer
    }

    // copy variable sized stuff

    if (ks.fixed) {
	to->insert_ptr = 0;
    } else {
	// for variable sized keys, copy the variable sized portion
	//  For leaf nodes, copy the data too

        if (ks.leaf) {
	    //  Move existing 'to' stuff to make room for 'from' stuff.
	    nbytes_to = btree->nodesize - to->insert_ptr;
	    memmove(((char *) to) + to->insert_ptr - nbytes_shift, 
		   ((char *) to) + to->insert_ptr,
		   nbytes_to);

            //  Copy over the 'from' stuff.
	    memcpy(((char *) to) + to->insert_ptr - nbytes_shift + nbytes_to, 
		   ((char *) from) + from->insert_ptr,
		   nbytes_shift);

	    to->insert_ptr   = to->insert_ptr   - nbytes_shift;
	    from->insert_ptr = from->insert_ptr + nbytes_shift;

	} else {

	    //  Move existing 'to' stuff to make room for 'from' stuff.
	    //  For non-leaves, include the 'right' pointer from the 'to' node.
	    nbytes_to = btree->nodesize - to->insert_ptr;
	    memmove(((char *) to) + to->insert_ptr - (nbytes_shift - r_keylen + s_keylen), 
		   ((char *) to) + to->insert_ptr,
		   nbytes_to);

            //  Copy key that converts 'right' pointer of 'to' node to a regular key.
	    memcpy(((char *) to) + to->insert_ptr - (nbytes_shift - r_keylen + s_keylen) + nbytes_to, s_key, s_keylen);

            //  Copy over the 'from' stuff.
	    memcpy(((char *) to) + to->insert_ptr - (nbytes_shift - r_keylen + s_keylen) + nbytes_to + s_keylen, 
		   ((char *) from) + from->insert_ptr,
		   nbytes_shift - r_keylen);

	    to->insert_ptr   = to->insert_ptr   - (nbytes_shift - r_keylen + s_keylen);
	    from->insert_ptr = from->insert_ptr + nbytes_shift;
	}
	
	//  update the keypos pointers
        update_keypos(btree, to,   0);
        update_keypos(btree, from, 0);
    }

    // update the rightmost pointer of the 'to' node
    to->rightmost = r_ptr;

    *r_key_out      = r_key;
    *r_keylen_out   = r_keylen;
    *r_syndrome_out = r_syndrome;
    *r_seqno_out    = r_seqno;

    #ifdef DEBUG_STUFF
	if (Verbose) {
	    char stmp[10000];
	    int  len;
	    if (btree->flags & SYNDROME_INDEX) {
	        sprintf(stmp, "%p", s_key);
		len = strlen(stmp);
	    } else {
		strncpy(stmp, s_key, s_keylen);
		len = s_keylen;
	    }
	    fprintf(stderr, "********  After shift_left for key '%s' [syn=%lu], B-Tree:  *******\n", dump_key(stmp, len), s_syndrome);
	    btree_raw_dump(stderr, btree);
	}
    #endif

    return;
}

/*   Copy keys from 'from' node to 'to' node, given that 'to' is to left of 'from'.
 */
static void merge_left(btree_raw_t *btree, btree_raw_node_t *anchor, btree_raw_node_t *from, btree_raw_node_t *to, char *s_key, uint32_t s_keylen, uint64_t s_syndrome, uint64_t s_seqno)
{
    node_fkey_t   *pfk;
    node_vkey_t   *pvk;
    uint32_t       nbytes_from;
    uint32_t       nbytes_to;
    uint32_t       nbytes_fixed;
    uint32_t       nbytes_free;
    uint32_t       nbytes_needed;
    key_stuff_t    ks;

    __sync_add_and_fetch(&(btree->stats.stat[BTSTAT_LMERGES]),1);

    (void) get_key_stuff(btree, from, 0, &ks);

    nbytes_fixed = ks.offset;
    if (ks.fixed) {
        nbytes_from = from->nkeys*ks.offset;
    } else {
	nbytes_from = btree->nodesize - from->insert_ptr;;
    }

    if (ks.leaf) {
        nbytes_free    = vlnode_bytes_free(to);
	nbytes_needed  = btree->nodesize - from->insert_ptr + from->nkeys*sizeof(node_vlkey_t);
	assert(nbytes_free >= nbytes_needed); // xxxzzz remove this!

	// copy the fixed size portion of the keys
	memcpy((char *) to->keys + to->nkeys*nbytes_fixed, from->keys, from->nkeys*nbytes_fixed);
	to->nkeys = to->nkeys + from->nkeys;

    } else {
	if (ks.fixed) {
	    assert((to->nkeys + from->nkeys + 1) <= btree->fkeys_per_node); // xxxzzz remove this!
	} else {
            nbytes_free    = vnode_bytes_free(to);
	    nbytes_needed  = btree->nodesize - from->insert_ptr + from->nkeys*sizeof(node_vkey_t);
	    nbytes_needed += (s_keylen + sizeof(node_vkey_t));
	    assert(nbytes_free >= nbytes_needed); // xxxzzz remove this!
	}

	//  Copy the fixed size portion of the keys, leaving space for the
	//  converting the 'to' right pointer to a regular key.
	memcpy((char *) to->keys + (to->nkeys + 1)*nbytes_fixed, from->keys, from->nkeys*nbytes_fixed);

	// convert 'to' rightmost pointer to a regular key
	if (ks.fixed) {
	    pfk = (node_fkey_t *) ((char *) to->keys + to->nkeys*nbytes_fixed);
	    pfk->key   = (uint64_t) s_key;
	    pfk->ptr   = to->rightmost;
	    pfk->seqno = s_seqno;
	} else {
	    pvk = (node_vkey_t *) ((char *) to->keys + to->nkeys*nbytes_fixed);
	    pvk->keylen = s_keylen;
	    pvk->keypos = 0; // will be set in update_keypos below
	    pvk->ptr    = to->rightmost;
	    pvk->seqno  = s_seqno;
	}
	to->nkeys = to->nkeys + from->nkeys + 1;
    }

    // copy variable sized stuff

    if (ks.fixed) {
	to->insert_ptr = 0;
    } else {
	// for variable sized keys, copy the variable sized portion
	//  For leaf nodes, copy the data too

        if (ks.leaf) {
	    //  Move existing 'to' stuff to make room for 'from' stuff.
	    nbytes_to = btree->nodesize - to->insert_ptr;
	    memmove(((char *) to) + to->insert_ptr - nbytes_from, 
		   ((char *) to) + to->insert_ptr,
		   nbytes_to);

            //  Copy over the 'from' stuff.
	    memcpy(((char *) to) + to->insert_ptr - nbytes_from + nbytes_to, 
		   ((char *) from) + from->insert_ptr,
		   nbytes_from);

	    to->insert_ptr = to->insert_ptr - nbytes_from;
	} else {
	    //  Move existing 'to' stuff to make room for 'from' stuff.
	    //  For non-leaves, include the 'right' pointer from the 'to' node.
	    nbytes_to = btree->nodesize - to->insert_ptr;
	    memmove(((char *) to) + to->insert_ptr - nbytes_from - s_keylen, 
		   ((char *) to) + to->insert_ptr,
		   nbytes_to);

            //  Copy over the 'from' stuff.
	    memcpy(((char *) to) + to->insert_ptr - nbytes_from - s_keylen + nbytes_to + s_keylen, 
		   ((char *) from) + from->insert_ptr,
		   nbytes_from);

            //  Copy key that converts 'right' pointer of 'to' node to a regular key.
	    memcpy(((char *) to) + to->insert_ptr - nbytes_from - s_keylen + nbytes_to, s_key, s_keylen);

	    to->insert_ptr   = to->insert_ptr- nbytes_from - s_keylen;
	}
	
	//  update the keypos pointers
        update_keypos(btree, to, 0);
    }

    // adjust the 'right' pointer of the merged node
    to->rightmost = from->rightmost;

    #ifdef DEBUG_STUFF
	if (Verbose) {
	    char stmp[10000];
	    int  len;
	    if (btree->flags & SYNDROME_INDEX) {
	        sprintf(stmp, "%p", s_key);
		len = strlen(stmp);
	    } else {
		strncpy(stmp, s_key, s_keylen);
		len = s_keylen;
	    }
	    fprintf(stderr, "********  After merge_left for key '%s' [syn=%lu], B-Tree:  *******\n", dump_key(stmp, len), s_syndrome);
	    btree_raw_dump(stderr, btree);
	}
    #endif

    return;
}

/*   Copy keys from 'from' node to 'to' node, given that 'to' is to right of 'from'.
 */
static void merge_right(btree_raw_t *btree, btree_raw_node_t *anchor, btree_raw_node_t *from, btree_raw_node_t *to, char *s_key, uint32_t s_keylen, uint64_t s_syndrome, uint64_t s_seqno)
{
    node_fkey_t   *pfk;
    node_vkey_t   *pvk;
    uint32_t       nbytes_from;
    uint32_t       nbytes_fixed;
    uint32_t       nbytes_free;
    uint32_t       nbytes_needed;
    key_stuff_t    ks;

    __sync_add_and_fetch(&(btree->stats.stat[BTSTAT_RMERGES]),1);

    (void) get_key_stuff(btree, from, 0, &ks);

    nbytes_fixed = ks.offset;
    if (ks.fixed) {
        nbytes_from = from->nkeys*ks.offset;
    } else {
	nbytes_from = btree->nodesize - from->insert_ptr;;
    }

    if (ks.leaf) {
        nbytes_free    = vlnode_bytes_free(to);
	nbytes_needed  = btree->nodesize - from->insert_ptr + from->nkeys*sizeof(node_vlkey_t);
	assert(nbytes_free >= nbytes_needed); // xxxzzz remove this!
	// make room for the lower fixed keys
	memmove((char *) to->keys + from->nkeys*nbytes_fixed, (char *) to->keys, to->nkeys*nbytes_fixed);

	// copy the fixed size portion of the keys
	memcpy(to->keys, from->keys, from->nkeys*nbytes_fixed);
	to->nkeys = to->nkeys + from->nkeys;

    } else {
	if (ks.fixed) {
	    assert((to->nkeys + from->nkeys + 1) <= btree->fkeys_per_node); // xxxzzz remove this!
	} else {
            nbytes_free    = vnode_bytes_free(to);
	    nbytes_needed  = btree->nodesize - from->insert_ptr + from->nkeys*sizeof(node_vkey_t);
	    nbytes_needed += (s_keylen + sizeof(node_vkey_t));
	    assert(nbytes_free >= nbytes_needed); // xxxzzz remove this!
	}

	// make room for the lower fixed keys, plus the separator from the anchor
	memmove((char *) to->keys + (from->nkeys + 1)*nbytes_fixed, (char *) to->keys, to->nkeys*nbytes_fixed);
	// copy the fixed size portion of the keys
	memcpy(to->keys, from->keys, from->nkeys*nbytes_fixed);
	to->nkeys = to->nkeys + from->nkeys + 1;

	// copy 'from' rightmost pointer
	if (ks.fixed) {
	    pfk = (node_fkey_t *) ((char *) to->keys + from->nkeys*nbytes_fixed);
	    pfk->key   = (uint64_t) s_key;
	    pfk->ptr   = from->rightmost;
	    pfk->seqno = s_seqno;
	} else {
	    pvk = (node_vkey_t *) ((char *) to->keys + from->nkeys*nbytes_fixed);
	    pvk->keylen = s_keylen;
	    pvk->keypos = 0; // will be set in update_keypos below
	    pvk->ptr    = from->rightmost;
	    pvk->seqno  = s_seqno;
	}
    }

    // copy variable sized stuff

    if (ks.fixed) {
	to->insert_ptr = 0;
    } else {
	// for variable sized keys, copy the variable sized portion
	//  For leaf nodes, copy the data too

        if (ks.leaf) {
	    memcpy(((char *) to) + to->insert_ptr - nbytes_from, 
		   ((char *) from) + from->insert_ptr,
		   nbytes_from);

	    to->insert_ptr = to->insert_ptr - nbytes_from;
	} else {
	    //  for non-leaves, include the 'right' pointer from the 'from' node
	    memcpy(((char *) to) + to->insert_ptr - nbytes_from - s_keylen, 
		   ((char *) from) + from->insert_ptr,
		   nbytes_from);

	    memcpy(((char *) to) + to->insert_ptr - s_keylen, s_key, s_keylen);

	    to->insert_ptr   = to->insert_ptr- nbytes_from - s_keylen;
	}
	
	//  update the keypos pointers
        update_keypos(btree, to, 0);
    }

    #ifdef DEBUG_STUFF
	if (Verbose) {
	    char stmp[10000];
	    int  len;
	    if (btree->flags & SYNDROME_INDEX) {
	        sprintf(stmp, "%p", s_key);
		len = strlen(stmp);
	    } else {
		strncpy(stmp, s_key, s_keylen);
		len = s_keylen;
	    }
	    fprintf(stderr, "********  After merge_right for key '%s' [syn=%lu], B-Tree:  *******\n", dump_key(stmp, len), s_syndrome);
	    btree_raw_dump(stderr, btree);
	}
    #endif

    return;
}


static int rebalance(btree_status_t *ret, btree_raw_t *btree, btree_raw_mem_node_t *this_mem_node, uint64_t left_id, uint64_t right_id, uint64_t l_anchor_id, key_stuff_t *l_anchor_stuff, uint64_t r_anchor_id, key_stuff_t *r_anchor_stuff, int l_this_parent, int r_this_parent, btree_metadata_t *meta)
{
    btree_raw_node_t     *this_node = this_mem_node->pnode, *left_node, *right_node, *balance_node, *anchor_node, *merge_node;
    btree_raw_mem_node_t *left_mem_node, *right_mem_node, *balance_mem_node, *anchor_mem_node;
    char                 *s_key;
    uint32_t              s_keylen;
    uint64_t              s_syndrome;
    uint64_t              s_seqno;
    uint64_t              s_ptr;
    char                 *r_key = NULL;
    uint32_t              r_keylen = 0;
    uint64_t              r_syndrome = 0;
    uint64_t              r_seqno = 0;
    uint32_t              balance_keylen;
    key_stuff_t           ks;
    int                   balance_node_is_sibling;
    int                   next_do_rebalance = 0;

    if (*ret) { return(0); }

    if (left_id == BAD_CHILD) {
        left_node = NULL;
        left_mem_node = NULL;
    } else {
	left_mem_node = get_existing_node(ret, btree, left_id);
        left_node = left_mem_node->pnode;
    }

    if (right_id == BAD_CHILD) {
        right_node = NULL;
        right_mem_node = NULL;
    } else {
	right_mem_node = get_existing_node(ret, btree, right_id);
        right_node = right_mem_node->pnode;
    }

    if (left_node == NULL) {
        balance_node   = right_node;
        balance_mem_node   = right_mem_node;
	balance_keylen = r_anchor_stuff->keylen;
    } else if (right_node == NULL) {
        balance_node   = left_node;
        balance_mem_node   = left_mem_node;
	balance_keylen = l_anchor_stuff->keylen;
    } else {
        // give siblings preference
	if (l_this_parent && (!r_this_parent)) {
	    balance_node   = left_node;
            balance_mem_node   = left_mem_node;
	    balance_keylen = l_anchor_stuff->keylen;
	} else if (r_this_parent && (!l_this_parent)) {
	    balance_node   = right_node;
            balance_mem_node   = right_mem_node;
	    balance_keylen = r_anchor_stuff->keylen;
        } else if (left_node->insert_ptr > right_node->insert_ptr) {
	    balance_node   = right_node;
            balance_mem_node   = right_mem_node;
	    balance_keylen = r_anchor_stuff->keylen;
	} else {
	    balance_node   = left_node;
            balance_mem_node   = left_mem_node;
	    balance_keylen = l_anchor_stuff->keylen;
	}
    }

    balance_node_is_sibling = balance_node == left_node ? l_this_parent : r_this_parent;

    assert(balance_node != NULL);

    if ((!is_minimal(btree, balance_node, balance_keylen, 0)) ||
        (!balance_node_is_sibling))
    {
        next_do_rebalance = 0;
        if (balance_node == left_node) {
	    anchor_mem_node    = get_existing_node(ret, btree, l_anchor_id);
            anchor_node = anchor_mem_node->pnode;

	    s_key      = l_anchor_stuff->pkey_val;
	    s_keylen   = l_anchor_stuff->keylen;
	    s_syndrome = l_anchor_stuff->syndrome;
	    s_seqno    = l_anchor_stuff->seqno;
	    s_ptr      = l_anchor_stuff->ptr;

	    shift_right(btree, anchor_node, balance_node, this_node, s_key, s_keylen, s_syndrome, s_seqno, &r_key, &r_keylen, &r_syndrome, &r_seqno);

            if (r_key != NULL) {
		// update keyrec in anchor
		delete_key(ret, btree, anchor_mem_node, s_key, s_keylen, meta, s_syndrome);
		insert_key(ret, btree, anchor_mem_node, r_key, r_keylen, r_seqno, sizeof(uint64_t), (char *) &s_ptr, meta, r_syndrome);
	    }
	} else {
	    anchor_mem_node = get_existing_node(ret, btree, r_anchor_id);
            anchor_node = anchor_mem_node->pnode;

	    s_key      = r_anchor_stuff->pkey_val;
	    s_keylen   = r_anchor_stuff->keylen;
	    s_syndrome = r_anchor_stuff->syndrome;
	    s_seqno    = r_anchor_stuff->seqno;
	    s_ptr      = r_anchor_stuff->ptr;

	    shift_left(btree, anchor_node, balance_node, this_node, s_key, s_keylen, s_syndrome, s_seqno, &r_key, &r_keylen, &r_syndrome, &r_seqno);

            if (r_key != NULL) {
		// update keyrec in anchor
		delete_key(ret, btree, anchor_mem_node, s_key, s_keylen, meta, s_syndrome);
		insert_key(ret, btree, anchor_mem_node, r_key, r_keylen, r_seqno, sizeof(uint64_t), (char *) &s_ptr, meta, r_syndrome);
	    }
	}

    } else {
        next_do_rebalance = 1;
        if (balance_node == left_node) {
	    //  left anchor is parent of this_node
	    anchor_mem_node    = get_existing_node(ret, btree, l_anchor_id);
            anchor_node = anchor_mem_node->pnode;
	    merge_node     = left_node;

	    s_key      = l_anchor_stuff->pkey_val;
	    s_keylen   = l_anchor_stuff->keylen;
	    s_syndrome = l_anchor_stuff->syndrome;
	    s_seqno    = l_anchor_stuff->seqno;

	    merge_left(btree, anchor_node, this_node, merge_node, s_key, s_keylen, s_syndrome, s_seqno);

	    //  update the anchor
	    //  cases:
	    //       1) this_node is the rightmost pointer
	    //       2) this_node is NOT a rightmost pointer
	    //

            if (this_node->logical_id == anchor_node->rightmost) {
		//  child is the rightmost pointer
		// 
	        //  Make the 'rightmost' point to the merge_node,
		//  then delete the key for the merge_node.
		anchor_node->rightmost = l_anchor_stuff->ptr;
	    } else {
	        //  Make the key for 'this_node' point to the merge_node,
		//  then delete the key for the merge_node.
		//  Note that this_node corresponds to l_anchor_stuff->nkey+1!
		// 
	        update_ptr(btree, anchor_node, l_anchor_stuff->nkey+1, l_anchor_stuff->ptr);
	    }
	    delete_key(ret, btree, anchor_mem_node, l_anchor_stuff->pkey_val, l_anchor_stuff->keylen, meta, l_anchor_stuff->syndrome);
	    btree->log_cb(ret, btree->log_cb_data, BTREE_UPDATE_NODE, btree, anchor_mem_node);

	    // free this_node
	    if (!(*ret)) {
		free_node(ret, btree, this_mem_node);
	    }

	} else {
	    //  Since the left anchor is not the parent of this_node,
	    //  the right anchor MUST be parent of this_node.
	    //  Also, this_node must be key number 0.

	    assert(r_this_parent);
	    anchor_mem_node    = get_existing_node(ret, btree, r_anchor_id);
            anchor_node = anchor_mem_node->pnode;
	    merge_node     = right_node;

	    s_key      = r_anchor_stuff->pkey_val;
	    s_keylen   = r_anchor_stuff->keylen;
	    s_syndrome = r_anchor_stuff->syndrome;
	    s_seqno    = r_anchor_stuff->seqno;

	    merge_right(btree, anchor_node, this_node, merge_node, s_key, s_keylen, s_syndrome, s_seqno);

	    //  update the anchor
	    // 
	    //  Just delete this_node.  
	    //  Whether or not the merge_node is the rightmost pointer,
	    //  the separator for the merge key is still valid after the
	    //  'this_key' is deleted.
	    //  

	    //  If anchor is 'rightmost', r_anchor_stuff holds data for 'this_node'.
	    //  Otherwise, r_anchor_stuff holds data for the node to the
	    //  immediate right of 'this_node'.

            //  Get data for 'this_node'.
            if (r_anchor_stuff->ptr == this_node->logical_id) {
	        //  Anchor is 'rightmost' node.
		//  Delete key for 'this_node'.
		delete_key(ret, btree, anchor_mem_node, r_anchor_stuff->pkey_val, r_anchor_stuff->keylen, meta, r_anchor_stuff->syndrome);
	    } else {
	        //  Anchor is NOT 'rightmost' node.
		//  Delete key for 'this_node'.
		(void) get_key_stuff(btree, anchor_node, r_anchor_stuff->nkey-1, &ks);
		delete_key(ret, btree, anchor_mem_node, ks.pkey_val, ks.keylen, meta, ks.syndrome);
	    }
	    btree->log_cb(ret, btree->log_cb_data, BTREE_UPDATE_NODE, btree, anchor_mem_node);

	    // free this_node
	    if (!(*ret)) {
		free_node(ret, btree, this_mem_node);
	    }
	}
    }

    return(next_do_rebalance);
}

static int check_per_thread_keybuf(btree_raw_t *btree)
{
    //  Make sure that the per-thread key buffer has been allocated,
    //  and that it is big enough!
    if (_keybuf_size < btree->nodesize) {
	if (_keybuf != NULL) {
	    free(_keybuf);
	    _keybuf_size = 0;
	}
	_keybuf = malloc(btree->nodesize);
	if (_keybuf == NULL) {
	    return(1);
	}
	_keybuf_size = btree->nodesize;
    }
    return(0);
}


//======================   FAST_BUILD  =========================================

int btree_raw_fast_build(btree_raw_t *btree)
{
    // TBD xxxzzz
    return(0);
}

//======================   DUMP  =========================================
#ifdef DEBUG_STUFF
static char *dump_key(char *key, uint32_t keylen)
{
    static char  stmp[200];

    stmp[0] = '\0';
    if (keylen > 100) {
	strncat(stmp, key, 100);
	strcat(stmp, "...");
    } else {
	strncat(stmp, key, keylen);
    }
    return(stmp);
}

static void dump_line(FILE *f, char *key, uint32_t keylen)
{
    if (key != NULL) {
	fprintf(f, "----------- Key='%s' -----------\n", dump_key(key, keylen));
    } else {
	fprintf(f, "-----------------------------------------------------------------------------------\n");
    }
}

static void dump_node(btree_raw_t *bt, FILE *f, btree_raw_node_t *n, char *key, uint32_t keylen)
{
    int             i;
    char           *sflags;
    int             nfreebytes;
    int             nkey_bytes;
    node_fkey_t    *pfk;
    node_vkey_t    *pvk;
    node_vlkey_t   *pvlk;
    key_stuff_t     ks;
    btree_raw_mem_node_t   *n_child;

    dump_line(f, key, keylen);

    if (n == NULL) {
        fprintf(f, "***********   BAD NODE!!!!   **********\n");
        abort();
	return;
    }

    if (is_leaf(bt, n)) {
        sflags = "LEAF";
	nkey_bytes = sizeof(node_vlkey_t);
    } else {
        sflags = "";
	if (bt->flags & SYNDROME_INDEX) {
	    nkey_bytes = sizeof(node_fkey_t);
	} else {
	    nkey_bytes = sizeof(node_vkey_t);
	}
	assert(n->rightmost != 0);
    }

    if ((bt->flags & SYNDROME_INDEX) && !(n->flags & LEAF_NODE)) {
	nfreebytes = bt->nodesize - sizeof(btree_raw_node_t) - n->nkeys*nkey_bytes;
    } else {
	nfreebytes = n->insert_ptr - sizeof(btree_raw_node_t) - n->nkeys*nkey_bytes;
    }
    assert(nfreebytes >= 0);

    fprintf(f, "Node [%ld][%p]: %d keys, ins_ptr=%d, %d free bytes, flags:%s%s, right=[%ld]\n", n->logical_id, n, n->nkeys, n->insert_ptr, nfreebytes, sflags, is_root(bt, n) ? ":ROOT" : "", n->rightmost);

    for (i=0; i<n->nkeys; i++) {

	if (n->flags & LEAF_NODE) {
	    pvlk = ((node_vlkey_t *) n->keys) + i;
	    fprintf(f, "   syn=%lu, Key='%s': ", pvlk->syndrome, dump_key((char *) n + pvlk->keypos, pvlk->keylen));
	    fprintf(f, "keylen=%d, keypos=%d, datalen=%ld, ptr=%ld, seqno=%ld", pvlk->keylen, pvlk->keypos, pvlk->datalen, pvlk->ptr, pvlk->seqno);
	    if ((pvlk->keylen + pvlk->datalen) >= bt->big_object_size) {
		//  overflow object
		fprintf(f, " [OVERFLOW!]\n");
	    } else {
		fprintf(f, "\n");
	    }
	} else if (bt->flags & SECONDARY_INDEX) {
	    pvk  = ((node_vkey_t *) n->keys) + i;
	    fprintf(f, "   Key='%s': ", dump_key((char *) n + pvk->keypos, pvk->keylen));
	    fprintf(f, "keylen=%d, keypos=%d, ptr=%ld, seqno=%ld\n", pvk->keylen, pvk->keypos, pvk->ptr, pvk->seqno);
	} else if (bt->flags & SYNDROME_INDEX) {
	    pfk  = ((node_fkey_t *) n->keys) + i;
	    fprintf(f, "   syn=%lu: ", pfk->key);
	    fprintf(f, "ptr=%ld, seqno=%ld\n", pfk->ptr, pfk->seqno);
	} else {
	    assert(0);
	}
    }

    if (!(n->flags & LEAF_NODE)) {
	btree_status_t ret = BTREE_SUCCESS;
	char  stmp[100];

	// non-leaf
	for (i=0; i<n->nkeys; i++) {
	    assert(!get_key_stuff(bt, n, i, &ks));
	    n_child = get_existing_node_low(&ret, bt, ks.ptr, 0); 
            if(n_child->modified != n_child->last_dump_modified)
            {
	    if (bt->flags & SYNDROME_INDEX) {
	        sprintf(stmp, "%p", ks.pkey_val);
		dump_node(bt, f, n_child->pnode, stmp, strlen(stmp));
	    } else {
		dump_node(bt, f, n_child->pnode, ks.pkey_val, ks.keylen);
	    }
            n_child->last_dump_modified = n_child->modified;
            }
            deref_l1cache_node(bt, n_child);
	}
	if (n->rightmost != 0) {
	    n_child = get_existing_node_low(&ret, bt, n->rightmost, 0); 
            if(n_child->modified != n_child->last_dump_modified)
            {
	    dump_node(bt, f, n_child->pnode, "==RIGHT==", 9);
            n_child->last_dump_modified = n_child->modified;
            }
            deref_l1cache_node(bt, n_child);
	}
    }
}

static
void btree_raw_dump(FILE *f, btree_raw_t *bt)
{
    btree_status_t     ret = BTREE_SUCCESS;
    btree_raw_mem_node_t  *n;
    char               sflags[1000];

    sflags[0] = '\0';
    if (bt->flags & SYNDROME_INDEX) {
        strcat(sflags, "SYN ");
    }
    if (bt->flags & SECONDARY_INDEX) {
        strcat(sflags, "SEC ");
    }
    if (bt->flags & IN_MEMORY) {
        strcat(sflags, "MEM");
    }

    dump_line(f, NULL, 0);

    fprintf(f, "B-Tree: flags:(%s), node:%dB, maxkey:%dB, minkeys:%d, bigobj:%dB\n", sflags, bt->nodesize, bt->max_key_size, bt->min_keys_per_node, bt->big_object_size);

    n = get_existing_node_low(&ret, bt, bt->rootid, 0); 
    if (BTREE_SUCCESS != ret || (n == NULL)) {
	fprintf(f, "*********************************************\n");
	fprintf(f, "    *****  Could not get root node!!!!  *****\n");
	fprintf(f, "*********************************************\n");
    }
    
    if(n->modified != n->last_dump_modified)
    {
        dump_node(bt, f, n->pnode, "===ROOT===", 10);
        n->last_dump_modified = n->modified;
    }

    dump_line(f, NULL, 0);
    deref_l1cache_node(bt, n);
//    deref_l1cache(bt);
}
#endif

//======================   CHECK   =========================================
#ifdef DBG_PRINT
static void print_key_func(FILE *f, const char* func, int line, char* key, int keylen, char *msg, ...)
{
	int i;
    char     stmp[1024];
    char     stmp1[1024];
    va_list  args;

    va_start(args, msg);

    vsprintf(stmp, msg, args);

    va_end(args);

	assert(keylen + 1 < sizeof(stmp1));
	for(i=0;i<keylen;i++)
		stmp1[i] = key[i] < 32 ? '^' : key[i];
	stmp1[i] = 0;
    (void) fprintf(stderr, "%x %s:%d %s key=[%s]\n", (int)pthread_self(), func, line,  stmp, stmp1);
}
#endif

static void check_err(FILE *f, char *msg, ...)
{
    char     stmp[1024];
    va_list  args;

    va_start(args, msg);

    vsprintf(stmp, msg, args);
    strcat(stmp, "\n");

    va_end(args);

    (void) fprintf(stderr, "%x %s", (int)pthread_self(), stmp);
    abort();
}

static void check_node(btree_raw_t *bt, FILE *f, btree_raw_mem_node_t *node, char *key_in_left, uint32_t keylen_in_left, char *key_in, uint32_t keylen_in, char *key_in_right, uint32_t keylen_in_right, int rightmost_flag)
{
    btree_raw_node_t *n = node->pnode;
    int                 i;
    int                 nfreebytes;
    int                 nkey_bytes;
    node_fkey_t        *pfk;
    node_vkey_t        *pvk;
    node_vlkey_t       *pvlk;
    key_stuff_t         ks;
    key_stuff_t         ks_left;
    key_stuff_t         ks_right;
    btree_raw_mem_node_t   *n_child;
    int                 x;

    if (n == NULL) {
        fprintf(f, "***********   ERROR: check_node: BAD NODE!!!!   **********\n");
	return;
    }
#ifdef DEBUG_STUFF
#if 0
    fprintf(stderr, "%x %s node=%p\n", (int)pthread_self(), __FUNCTION__, n);
    if(key_in_left)
    fprintf(stderr, "%x %s left [%.*s]\n", (int)pthread_self(), __FUNCTION__, keylen_in_left, key_in_left);
    if(key_in_right)
    fprintf(stderr, "%x %s right [%.*s]\n", (int)pthread_self(), __FUNCTION__, keylen_in_right, key_in_right);
    if(key_in)
    fprintf(stderr, "%x %s in [%.*s]\n", (int)pthread_self(), __FUNCTION__, keylen_in, key_in);
#endif
#endif
    if (n->flags & LEAF_NODE) {
	nkey_bytes = sizeof(node_vlkey_t);
    } else {
	if (bt->flags & SYNDROME_INDEX) {
	    nkey_bytes = sizeof(node_fkey_t);
	} else {
	    nkey_bytes = sizeof(node_vkey_t);
	}
	assert(n->rightmost != 0);
    }
    if ((bt->flags & SYNDROME_INDEX) && !(n->flags & LEAF_NODE)) {
	nfreebytes = bt->nodesize - sizeof(btree_raw_node_t) - n->nkeys*nkey_bytes;
    } else {
	nfreebytes = n->insert_ptr - sizeof(btree_raw_node_t) - n->nkeys*nkey_bytes;
    }
    assert(nfreebytes >= 0);

    for (i=0; i<n->nkeys; i++) {
        if (n->flags & LEAF_NODE) {
	    assert(get_key_stuff(bt, n, i, &ks));
	} else {
	    assert(!get_key_stuff(bt, n, i, &ks));
	}
	if (key_in_left != NULL) {
	    x = bt->cmp_cb(bt->cmp_cb_data, key_in_left, keylen_in_left, ks.pkey_val, ks.keylen);
	    if (rightmost_flag) {
		if (x != -1) {
		    check_err(f, "***********   ERROR: check_node left (right): node %p key %d out of order!!!!   **********\n", n, i);
		}
	    } else {
		if (x != -1) {
		    check_err(f, "***********   ERROR: check_node left: node %p key %d out of order!!!!   **********\n", n, i);
		}
	    }
	}

	if (key_in != NULL) {
	    x = bt->cmp_cb(bt->cmp_cb_data, key_in, keylen_in, ks.pkey_val, ks.keylen);
	    if (rightmost_flag) {
		if (x == -1) {
		    check_err(f, "***********   ERROR: check_node (right): node %p key %d out of order!!!!   **********\n", n, i);
		}
	    } else {
		if (x == -1) {
		    check_err(f, "***********   ERROR: check_node: node %p key %d out of order!!!!   **********\n", n, i);
		}
	    }
	}

	if (key_in_right != NULL) {
	    x = bt->cmp_cb(bt->cmp_cb_data, key_in_right, keylen_in_right, ks.pkey_val, ks.keylen);
	    if (rightmost_flag) {
		if (x == -1) {
		    check_err(f, "***********   ERROR: check_node right (right): node %p key %d out of order!!!!   **********\n", n, i);
		}
	    } else {
		if (x == -1) {
		    check_err(f, "***********   ERROR: check_node right: node %p key %d out of order!!!!   **********\n", n, i);
		}
	    }
	}

	if (i > 0) {
	    // make sure the keys within this node are sorted! 
	    (void) get_key_stuff(bt, n, i, &ks_left);
	    x = bt->cmp_cb(bt->cmp_cb_data, ks_left.pkey_val, ks_left.keylen, ks.pkey_val, ks.keylen);
	    if (x == -1) {
		check_err(f, "***********   ERROR: check_node internal: node %p key %d out of order!!!!   **********\n", n, i);
	    }
	}

	if (n->flags & LEAF_NODE) {
	    pvlk = ((node_vlkey_t *) n->keys) + i;
	    // purposefully empty 
	} else if (bt->flags & SECONDARY_INDEX) {
	    pvk  = ((node_vkey_t *) n->keys) + i;
	    // purposefully empty 
	} else if (bt->flags & SYNDROME_INDEX) {
	    pfk  = ((node_fkey_t *) n->keys) + i;
	    // purposefully empty 
	} else {
	    assert(0);
	}
    }

    if (!(n->flags & LEAF_NODE)) {
	btree_status_t ret = BTREE_SUCCESS;
	char  stmp[100];
	char  stmp_left[100];
	char  stmp_right[100];

	// non-leaf
	for (i=0; i<n->nkeys; i++) {
	    (void) get_key_stuff(bt, n, i, &ks);
	    n_child = get_existing_node(&ret, bt, ks.ptr); 
	    if (bt->flags & SYNDROME_INDEX) {
	        sprintf(stmp, "%p", ks.pkey_val);
		if (i == 0) {
		    if (key_in_left == NULL) {
			strcpy(stmp_left, "");
		    } else {
			strcpy(stmp_left, key_in_left);
		    }
		} else {
		    (void) get_key_stuff(bt, n, i-1, &ks_left);
		    sprintf(stmp_left, "%p", ks_left.pkey_val);
		}
		if (i == (n->nkeys-1)) {
		    if (key_in_right == NULL) {
			strcpy(stmp_right, "");
		    } else {
			strcpy(stmp_right, key_in_right);
		    }
		} else {
		    (void) get_key_stuff(bt, n, i+1, &ks_right);
		    sprintf(stmp_right, "%p", ks_right.pkey_val);
		}
		check_node(bt, f, n_child, stmp_left, strlen(stmp_left), stmp, strlen(stmp), stmp_right, strlen(stmp_right), 0 /* right */);
	    } else {
		if (i == 0) {
		    if (key_in_left != NULL) {
			ks_left.pkey_val = key_in_left;
			ks_left.keylen   = keylen_in_left;
		    } else {
			ks_left.pkey_val = NULL;
			ks_left.keylen   = 0;
		    }
		} else {
		    (void) get_key_stuff(bt, n, i-1, &ks_left);
		}
		if (i == (n->nkeys-1)) {
		    ks_right.pkey_val = key_in_right;
		    ks_right.keylen   = keylen_in_right;
		} else {
		    (void) get_key_stuff(bt, n, i+1, &ks_right);
		}
		check_node(bt, f, n_child, ks_left.pkey_val, ks_left.keylen, ks.pkey_val, ks.keylen, ks_right.pkey_val, ks_right.keylen, 0 /* right */);
	    }
	}
	if (n->rightmost != 0) {
	    n_child = get_existing_node(&ret, bt, n->rightmost); 
	    if (n->nkeys == 0) {
	        //  this can only happen for the root!
	        assert(n->logical_id == bt->rootid);
		check_node(bt, f, n_child, NULL, 0, NULL, 0, NULL, 0, 1 /* right */);
	    } else {
		(void) get_key_stuff(bt, n, n->nkeys-1, &ks_left);
		check_node(bt, f, n_child, ks_left.pkey_val, ks_left.keylen, key_in_right, keylen_in_right, NULL, 0, 1 /* right */);
	    }
	}
    }
}

#ifdef BTREE_RAW_CHECK
static
void btree_raw_check(btree_raw_t *bt, char* func, char* key)
{
    btree_status_t     ret = BTREE_SUCCESS;
    btree_raw_mem_node_t  *n;

    plat_rwlock_wrlock(&bt->lock);

#ifdef DEBUG_STUFF
	fprintf(stderr, "BTREE_CHECK %x %s btree %p key %s lock %p BEGIN\n", (int)pthread_self(), func, bt, key, &bt->lock);
#endif
    n = get_existing_node(&ret, bt, bt->rootid); 
    if (BTREE_SUCCESS != ret || (n == NULL)) {
	check_err(stderr, "*****  ERROR: btree_raw_check: Could not get root node!!!!  *****\n");
    }
    
    check_node(bt, stderr, n, NULL, 0, NULL, 0, NULL, 0, 0);

    (void)deref_l1cache(bt);

#ifdef DEBUG_STUFF
    fprintf(stderr, "BTREE_CHECK %x %s btree %p key %s lock %p END\n", (int)pthread_self(), func, bt, key, &bt->lock);
#endif

    plat_rwlock_unlock(&bt->lock);
}
#endif

//======================   TEST  =========================================

void btree_raw_test(btree_raw_t *btree)
{
    // TBD xxxzzz
}

//======================   SNAPSHOTS   =========================================

extern int btree_raw_snapshot(struct btree_raw *btree, uint64_t *seqno)
{
    // TBD xxxzzz
    return(0);
}

extern int btree_raw_delete_snapshot(struct btree_raw *btree, uint64_t seqno)
{
    // TBD xxxzzz
    return(0);
}

extern int btree_raw_get_snapshots(struct btree_raw *btree, uint32_t *n_snapshots, uint64_t *seqnos)
{
    // TBD xxxzzz
    return(0);
}

//======================   STATS   =========================================

static void btree_raw_init_stats(struct btree_raw *btree, btree_stats_t *stats)
{
    memset(stats, 0, sizeof(btree_stats_t));
}

void btree_raw_get_stats(struct btree_raw *btree, btree_stats_t *stats)
{
    memcpy(stats, &(btree->stats), sizeof(btree_stats_t));

    btree->stats.stat[BTSTAT_MPUT_IO_SAVED] = 0;
}

char *btree_stat_name(btree_stat_t stat_type)
{
    return(btree_stats_strings[stat_type]);
}

void btree_dump_stats(FILE *f, btree_stats_t *stats)
{
    int j;

    fprintf(stderr, "==============================================================\n");
    for (j=0; j<N_BTSTATS; j++) {
        fprintf(stderr, "%-23s = %"PRIu64"\n", btree_stat_name(j), stats->stat[j]);
    }
    fprintf(stderr, "==============================================================\n");
}
