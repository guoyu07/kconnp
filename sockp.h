/**
 *Header for sockp.c
 *Version 0.0.1 05/27/2012
 *Author Zhigang Zhang <zzgang2008@gmail.com>
 */
#ifndef _SOCKP_H
#define _SOCKP_H

#include <linux/in.h> /*define struct sockaddr_in*/
#include <linux/net.h> /*define struct socket*/
#include <net/tcp_states.h>

#define NR_SOCKET_BUCKET 200
#define NR_HASH (NR_SOCKET_BUCKET/2 + 1)
#define TIMEOUT 30 //seconds
#define LOCK_TYPE_MUTEX 0
#define LRU 1 //LRU replace algorithm


struct socket_bucket {
    struct sockaddr address;
    struct socket *sock;
    unsigned char sb_in_use;
    unsigned char sock_in_use; /*tag: if it is in use*/
    unsigned long last_used_jiffies; /*the last used jiffies*/
    unsigned long uc; /*used count*/
    struct socket_bucket *sb_prev;
    struct socket_bucket *sb_next; /*for hash table*/
    struct socket_bucket *sb_sprev;
    struct socket_bucket *sb_snext; /*for with sock addr hash table*/
    struct socket_bucket *sb_trav_prev; /*traverse all used buckets*/
    struct socket_bucket *sb_trav_next;
    struct socket_bucket *sb_free_prev;
    struct socket_bucket *sb_free_next;
    int connpd_fd;
};


/**
 *Apply a existed socket from socket pool.
 */
extern struct socket * apply_socket_from_sockp(struct sockaddr *);

/**
 *Free a socket which is returned by 'apply_socket_from_sockp', return the bucket of this socket.
 */
extern struct socket_bucket * free_socket_to_sockp(struct sockaddr *, struct socket *);

/**
 *Insert a new socket to sockp, return the new bucket of this socket.
 */
extern struct socket_bucket * insert_socket_to_sockp(struct sockaddr *, struct socket *);

extern void shutdown_timeout_sock_list(void);

extern int sockp_init(void);
extern void sockp_destroy(void);

#endif