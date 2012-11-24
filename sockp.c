/**
 *Kernel socket pool
 *Author Zhigang Zhang <zzgang_2008@gmail.com>
 */
#include <linux/string.h>
#include <linux/jiffies.h>
#include <net/sock.h>
#include "sys_call.h"
#include "connp.h"
#include "util.h"

#if LOCK_TYPE_MUTEX 
#include <linux/mutex.h>
#else
#include <linux/spinlock.h> 
#endif


#if LOCK_TYPE_MUTEX 

#define SOCKP_LOCK_INIT()  mutex_init(&ht.ht_lock)
#define SOCKP_LOCK() mutex_lock(&ht.ht_lock)
#define SOCKP_UNLOCK() mutex_unlock(&ht.ht_lock)
#define SOCKP_LOCK_DESTROY() mutex_destroy(&ht.ht_lock)

#else //spin_lock

#define SOCKP_LOCK_INIT()  spin_lock_init(&ht.ht_lock)
#define SOCKP_LOCK() spin_lock(&ht.ht_lock)
#define SOCKP_UNLOCK() spin_unlock(&ht.ht_lock)
#define SOCKP_LOCK_DESTROY()

#endif

#define HASH(address_ptr) ht.hash_table[_hashfn((struct sockaddr_in *)(address_ptr))]
#define SHASH(address_ptr, s) ht.shash_table[_shashfn((struct sockaddr_in *)(address_ptr), (struct socket *)s)]

#define KEY_MATCH_CAST(address_ptr1, address_ptr2) ((address_ptr1)->sin_port == (address_ptr2)->sin_port && (address_ptr1)->sin_addr.s_addr == (address_ptr2)->sin_addr.s_addr)
#define KEY_MATCH(address_ptr1, address_ptr2) KEY_MATCH_CAST((struct sockaddr_in *)address_ptr1, (struct sockaddr_in *)address_ptr2)
#define SKEY_MATCH(address_ptr1, sock_ptr1, address_ptr2, sock_ptr2) (KEY_MATCH(address_ptr1, address_ptr2) && (sock_ptr1 == sock_ptr2))

#define PUT_SB(sb_ptr) ((sb_ptr)->sb_in_use = 0) 

#define INSERT_TO_HLIST(head, bucket) \
    do {                      \
        (bucket)->sb_prev = NULL; \
        (bucket)->sb_next = (head); \
        if ((head))     \
        (head)->sb_prev = (bucket); \
        (head) = (bucket);  \
    } while(0)

#define REMOVE_FROM_HLIST(head, bucket) \
    do {  \
        if ((bucket)->sb_prev)                  \
        (bucket)->sb_prev->sb_next = (bucket)->sb_next; \
        if ((bucket)->sb_next)              \
        (bucket)->sb_next->sb_prev = (bucket)->sb_prev; \
        if ((head) == (bucket)) \
        (head) = (bucket)->sb_next;     \
    } while(0)

#define INSERT_TO_SHLIST(head, bucket) \
    do {                               \
        (bucket)->sb_sprev = NULL; \
        (bucket)->sb_snext = (head); \
        if ((head))     \
        (head)->sb_sprev = (bucket); \
        (head) = (bucket);\
    } while(0)

#define REMOVE_FROM_SHLIST(head, bucket) \
    do {    \
        if ((bucket)->sb_sprev)                  \
        (bucket)->sb_sprev->sb_snext = (bucket)->sb_snext; \
        if ((bucket)->sb_snext)              \
        (bucket)->sb_snext->sb_sprev = (bucket)->sb_sprev; \
        if ((head) == (bucket)) \
        (head) = (bucket)->sb_snext;    \
    } while(0)

#define INSERT_TO_TLIST(bucket) \
    do {    \
        (bucket)->sb_trav_next = NULL; \
        (bucket)->sb_trav_prev = ht.sb_trav_tail;  \
        if (!ht.sb_trav_head)              \
        ht.sb_trav_head = (bucket);    \
        if (ht.sb_trav_tail)               \
        ht.sb_trav_tail->sb_trav_next = (bucket);  \
        ht.sb_trav_tail = (bucket); \
    } while(0)


#define REMOVE_FROM_TLIST(bucket) \
    do {    \
        if ((bucket)->sb_trav_next)  \
        (bucket)->sb_trav_next->sb_trav_prev = (bucket)->sb_trav_prev; \
        if ((bucket)->sb_trav_prev) \
        (bucket)->sb_trav_prev->sb_trav_next = (bucket)->sb_trav_next; \
        if ((bucket) == ht.sb_trav_head)  \
        ht.sb_trav_head = (bucket)->sb_trav_next; \
        if ((bucket) == ht.sb_trav_tail)   \
        ht.sb_trav_tail = (bucket)->sb_trav_prev; \
    } while(0)


#define SOCKADDR_COPY(sockaddr_dest, sockaddr_src) memcpy((void *)sockaddr_dest, (void *)sockaddr_src, sizeof(struct sockaddr))

#if LRU
static struct socket_bucket * get_empty_slot(struct sockaddr *);
#else
static struct socket_bucket * get_empty_slot(void);
#endif

static inline unsigned int _hashfn(struct sockaddr_in *);
static inline unsigned int _shashfn(struct sockaddr_in *, struct socket *);

static struct {
    struct socket_bucket *hash_table[NR_HASH];
    struct socket_bucket *shash_table[NR_HASH]; //for sock addr hash table.
    struct socket_bucket *sb_free_p;
    struct socket_bucket *sb_trav_head;
    struct socket_bucket *sb_trav_tail;
#if LOCK_TYPE_MUTEX
    struct mutex ht_lock;
#else
    spinlock_t ht_lock;
#endif
} ht;

static struct socket_bucket SB[NR_SOCKET_BUCKET];

static inline unsigned int _hashfn(struct sockaddr_in *address)
{
    return (unsigned)((*address).sin_port ^ (*address).sin_addr.s_addr) % NR_HASH;
}

static inline unsigned int _shashfn(struct sockaddr_in *address, struct socket *s)
{
    return (unsigned)((*address).sin_port ^ (*address).sin_addr.s_addr ^ (unsigned long)s) % NR_HASH;
}

/**
 *Apply a existed socket from socket pool.
 */
struct socket * apply_socket_from_sockp(struct sockaddr *address)
{
    struct socket_bucket *p;

    SOCKP_LOCK();

    for (p = HASH(address); p; p = p->sb_next) {
        if (KEY_MATCH(address, &p->address)) {
            if (p->sock_in_use || !SOCK_ESTABLISHED(p->sock))
                continue;

            REMOVE_FROM_HLIST(HASH(address), p);
            
            p->uc++; //inc used count
            p->sock_in_use = 1; //set "in use" tag.

            SOCKP_UNLOCK();
            return p->sock;
        }
    }

    SOCKP_UNLOCK();
    return NULL;
}


/**
 *shutdown: to scan all sock pool to close the expired socket.
 */
void shutdown_timeout_sock_list(void)
{
    struct socket_bucket *p; 
    
    SOCKP_LOCK();
    
    for (p = ht.sb_trav_head; p; p = p->sb_trav_next) {
        
        if (p->connpd_fd < 0)
            continue;
        if (!SOCK_ESTABLISHED(p->sock) ||
                (!p->sock_in_use && 
                 (jiffies - p->last_used_jiffies > TIMEOUT * HZ)
                )) {

            orig_sys_close(p->connpd_fd);

            REMOVE_FROM_HLIST(HASH(&p->address), p);
            REMOVE_FROM_TLIST(p);
            REMOVE_FROM_SHLIST(SHASH(&p->address, p->sock), p);

            PUT_SB(p);
        }
    }

    SOCKP_UNLOCK();
}

/**
 *Free a socket which is returned by 'apply_socket_from_sockp'
 */
struct socket_bucket * free_socket_to_sockp(struct sockaddr *address, struct socket *s)
{
    struct socket_bucket *p, *sb = NULL;
    
    SOCKP_LOCK();

    for (p = SHASH(address, s); p; p = p->sb_snext) {
        if (SKEY_MATCH(address, s, &p->address, p->sock)) {
            if (!p->sock_in_use) //can't release it repeatedly!
                break;
            sb = p;

            sb->sock_in_use = 0; //clear "in use" tag.
            sb->last_used_jiffies = jiffies;

            INSERT_TO_HLIST(HASH(address), sb);
        }
    }

    SOCKP_UNLOCK();

    return sb;
}


/**
 *Get a empty slot from sockp;
 */
#if LRU
static struct socket_bucket * get_empty_slot(struct sockaddr *addr)
#else
static struct socket_bucket * get_empty_slot(void)
#endif
{
    struct socket_bucket *p; 
#if LRU
    struct socket_bucket *lru = NULL;
    unsigned int uc = ~0;
#endif

    p = ht.sb_free_p;

    do {
        if (!p->sb_in_use) {
            ht.sb_free_p = p->sb_free_next;
            return p;
        }

#if LRU
        /*connpd_fd: -1, was not attached to connpd*/
        if (!p->sock_in_use && !KEY_MATCH(addr, &p->address) && p->connpd_fd >= 0 && p->uc <= uc) { 
            lru = p;
            uc = p->uc;
        }
#endif

        p = p->sb_free_next;
    } while (p != ht.sb_free_p);

#if LRU
    if (lru) {
        if (close_pending_fds_push(lru->connpd_fd) < 0)
            return NULL;
        ht.sb_free_p = lru->sb_free_next;
        REMOVE_FROM_HLIST(HASH(&lru->address), lru);
        REMOVE_FROM_TLIST(lru);
        REMOVE_FROM_SHLIST(SHASH(&lru->address, lru->sock), lru);
        return lru;
    } 
#endif

    return NULL;
}

/**
 *Insert a new socket to sockp.
 */
struct socket_bucket * insert_socket_to_sockp(struct sockaddr *address, struct socket *s)
{
    struct socket_bucket *empty = NULL;

    SOCKP_LOCK();

#if LRU
    if (!(empty = get_empty_slot(address))) 
#else
    if (!(empty = get_empty_slot())) 
#endif
        goto unlock_ret;

    empty->sb_in_use = 1; //initial.
    empty->sock_in_use = 0;
    empty->sock = s;
    empty->last_used_jiffies = jiffies;
    empty->connpd_fd = -1;
    empty->uc = 0;

    SOCKADDR_COPY(&empty->address, address);

    INSERT_TO_HLIST(HASH(&empty->address), empty);
    INSERT_TO_TLIST(empty);
    INSERT_TO_SHLIST(SHASH(&empty->address, s), empty);

unlock_ret:
    SOCKP_UNLOCK();

    return empty;
}

int sockp_init()
{
    struct socket_bucket *sb_tmp;
    
    //init sockp freelist.
    ht.sb_free_p = sb_tmp = SB;
    while (sb_tmp < SB + NR_SOCKET_BUCKET) {
        sb_tmp->sb_free_prev = sb_tmp - 1;
        sb_tmp->sb_free_next = sb_tmp + 1;
        sb_tmp++;
    }
    sb_tmp--;
    SB[0].sb_free_prev = sb_tmp;
    sb_tmp->sb_free_next = SB;

    SOCKP_LOCK_INIT();

    return 1;
}

/*
 *destroy the sockp and the hash table.
 */
void sockp_destroy(void)
{
    memset((void *)SB, 0, sizeof(SB));
    memset((void *)&ht, 0, sizeof(ht));
    SOCKP_LOCK_DESTROY();
}