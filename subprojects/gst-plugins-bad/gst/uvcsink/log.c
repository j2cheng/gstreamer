#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <unistd.h>

#include "log.h"


static pthread_once_t log_level_initialized = PTHREAD_ONCE_INIT;
static int curr_log_level = 2; // allow ERROR + WARNING

static
void log_level_init(void)
{
    const char *env = getenv("LOG_LEVEL");

    if(!env) return;
    else if(0 == strcmp("NONE", env)) curr_log_level = 0;
    else if(0 == strcmp("ERROR", env)) curr_log_level = 1;
    else if(0 == strcmp("WARN", env)) curr_log_level = 2;
    else if(0 == strcmp("INFO", env)) curr_log_level = 3;
    else if(0 == strcmp("DEBUG", env)) curr_log_level = 4;
    else if(0 == strcmp("TRACE", env)) curr_log_level = 5;
    else
    {
        fprintf(
            stderr,
            "%s:%d %s unsupported %s\n",
            BASE_FILE_NAME, __LINE__, __FUNCTION__, env);
        assert(0);
        return;
    }
    printf(
        "INFO  [%-6d] %s:%-4d %s level: %s\n",
           getpid(), BASE_FILE_NAME, __LINE__, __FUNCTION__, env);
}

int log_level(void)
{
    pthread_once(&log_level_initialized, log_level_init);
    return curr_log_level;
}