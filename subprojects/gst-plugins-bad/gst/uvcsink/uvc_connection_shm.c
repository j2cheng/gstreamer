#ifdef SHM_SUPPORT_ENABLED

#include <assert.h>
#include <fcntl.h>

#include <sys/mman.h>

#include "ensure.h"
#include "log.h"
#include "util.h"
#include "uvc_connection.h"


static
int shm_unmmap(uvc_connection_t *conn)
{
    assert(conn);

    LOG_INFO("[%d] mem %s(%d)", conn->idx, conn->mem.name, conn->mem.fd);

    for(size_t i = 0; i < length_of(conn->mem.addr); ++i)
    {
        if(!conn->mem.addr[i]) continue;
        ENSURE_NEQ_ELSE_RETURN(-1, munmap(conn->mem.addr[i], conn->mem.size), -1);
        conn->mem.addr[i] = NULL;
    }

    return 0;
}

static
int shm_mmap(uvc_connection_t *conn)
{
    assert(conn);
    assert(conn->mem.size);

    LOG_INFO("[%d] mem %s(%d)", conn->idx, conn->mem.name, conn->mem.fd);

    for(size_t i = 0; i < length_of(conn->mem.addr); ++i)
    {
        void *addr =
            mmap(
                NULL, conn->mem.size,
                PROT_READ | PROT_WRITE, MAP_SHARED,
                conn->mem.fd, 0);

        ENSURE_NEQ_ELSE_GOTO(MAP_FAILED, addr, failed);
        conn->mem.addr[i] = addr;
        LOG_INFO("%zu/%p size %zu", i, addr, conn->mem.size);
    }
    return 0;
failed:
    (void)shm_unmmap(conn);
    return -1;
}

static
int shm_release(uvc_connection_t *conn)
{
    assert(conn);
    if(-1 == conn->mem.fd) return 0;

    LOG_INFO("[%d] mem %s(%d)", conn->idx, conn->mem.name, conn->mem.fd);
    memset(conn->mem.name, 0, sizeof(conn->mem.name));
    ENSURE_NEQ_ELSE_RETURN(-1, close(conn->mem.fd), -1);
    conn->mem.fd = -1;
    return 0;
}

static
int shm_acquire(uvc_connection_t *conn)
{
    assert(conn);
    assert(-1 != conn->mem.fd);

    LOG_INFO("[%d] mem %s(%d)", conn->idx, conn->mem.name, conn->mem.fd);

    if(-1 == shm_mmap(conn)) goto failed;
    return 0;
failed:
    (void)shm_release(conn);
    return -1;
}

int uvc_connection_mem_acquire(uvc_connection_t *conn)
{
    return shm_acquire(conn);
}

int uvc_connection_mem_release(uvc_connection_t *conn)
{
    return shm_release(conn);
}

#endif /* SHM_SUPPORT_ENABLED */
