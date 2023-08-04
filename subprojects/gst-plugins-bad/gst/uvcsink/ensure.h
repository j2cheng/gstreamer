#pragma once

#include <assert.h>
#include <errno.h>
#include <string.h>

#define CRASH(hint) assert(0 && #hint)

#define ENSURE_COND(cond) \
    do \
    { \
        if(!(cond)) \
        { \
            LOG_ERROR("[" #cond "]: %s(%d)", strerror(errno), errno); \
            CRASH(#cond); \
        } \
    } while(0)

#define ENSURE_EQ(value, cond)                    ENSURE_COND((value) == (cond))
#define ENSURE_NEQ(value, cond)                   ENSURE_COND((value) != (cond))

#define ENSURE_COND_ELSE_GOTO(cond, label) \
    do \
    { \
        if(!(cond)) \
        { \
            LOG_ERROR("[" #cond "]: %s(%d)", strerror(errno), errno); \
            goto label; \
        } \
    } while(0)

#define ENSURE_COND_ELSE_RETURN(cond, result) \
    do \
    { \
        if(!(cond)) \
        { \
            LOG_ERROR("[" #cond "]: %s(%d)", strerror(errno), errno); \
            return result; \
        } \
    } while(0)

#define ENSURE_EQ_ELSE_GOTO(value, expr, label) \
    ENSURE_COND_ELSE_GOTO((value) == (expr), label)

#define ENSURE_NEQ_ELSE_GOTO(value, expr, label) \
    ENSURE_COND_ELSE_GOTO((value) != (expr), label)

#define ENSURE_EQ_ELSE_RETURN(value, expr, result) \
    ENSURE_COND_ELSE_RETURN((value) == (expr), result)

#define ENSURE_NEQ_ELSE_RETURN(value, expr, result) \
    ENSURE_COND_ELSE_RETURN((value) != (expr), result)
