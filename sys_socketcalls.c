#include <asm/uaccess.h>
#include <linux/net.h>
#include <linux/file.h>
#include <linux/version.h>
#include "sockp.h"
#include "connp.h"
#include "sys_call.h"
#include "lkm_util.h"

static inline int connp_move_addr_to_kernel(void __user *uaddr, int ulen, struct sockaddr *kaddr)
{
    if (ulen < 0 || ulen > sizeof(struct sockaddr_storage))
        return -EINVAL;

    if (ulen == 0)
        return 0;

    if (copy_from_user(kaddr, uaddr, ulen))
        return -EFAULT;

    return 0;
}

#ifdef __NR_socketcall /*32 bits*/
asmlinkage long connp_sys_socketcall(int call, unsigned long __user *args)
{
    unsigned long a[6];
    int err;

    switch(call) {
        case SYS_CONNECT:
            if (copy_from_user(a, args, 3 * sizeof(a[0])))
                return -EFAULT;
            err = connp_sys_connect(a[0], (struct sockaddr __user *)a[1], a[2]);
            break;

        case SYS_SHUTDOWN:
            if (copy_from_user(a, args, 2 * sizeof(a[0])))
                return -EFAULT;
            err = connp_sys_shutdown(a[0], a[1]); 
            break;
        default:
            //Clean the stack in this function and jmp orig_sys_socketcall directly.
            asm volatile("leave\n\t"
                    "jmp *%1\n\t" //change eip to orig_sys_socketcall directly.
                    :"=a"(err) //dummy for no compile warning.
                    :"m"(orig_sys_socketcall));
            break;
    }

    return err;
}
#endif

asmlinkage long connp_sys_connect(int fd, 
        struct sockaddr __user * uservaddr, int addrlen)
{
    struct sockaddr_storage servaddr;
    int err;
    
    err = connp_move_addr_to_kernel(uservaddr, addrlen, (struct sockaddr *)&servaddr);
    if (err < 0)
        return -EFAULT;
    
    if ((err = fetch_conn_from_connp(fd, (struct sockaddr *)&servaddr))) {
        if (err == CONN_BLOCK)
            return 0;
        else if (err == CONN_NONBLOCK)
            return -EINPROGRESS;
    }

    return orig_sys_connect(fd, uservaddr, addrlen);
}

asmlinkage long connp_sys_shutdown(int fd, int way)
{
    if (insert_into_connp_if_permitted(fd)) 
        return orig_sys_close(fd); //only remove the fd of the file table.
    else
        return orig_sys_shutdown(fd, way);
}
