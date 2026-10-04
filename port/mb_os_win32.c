/**
 * @file mb_os_win32.c
 * @brief Windows（PC 模拟器）的 OS 抽象层实现。
 *
 * 用编译期常量 MB_CONFIG_OS == MB_OS_WIN32 选中，CMake 会自动处理。
 * 依赖仅限 Win32 API，MSVC / MinGW 均可编译。
 */
#include "message_bus/mb_config.h"

#if MB_CONFIG_OS != MB_OS_WIN32
#error "mb_os_win32.c only compiles when MB_CONFIG_OS == MB_OS_WIN32"
#endif

#include "message_bus/mb_os.h"

#include <stdlib.h>

mb_mutex_t *mb_os_mutex_create(void)
{
    mb_mutex_t *mutex = (mb_mutex_t *)malloc(sizeof(*mutex));

    if (mutex == NULL) {
        return NULL;
    }
    /* CRITICAL_SECTION 天然可重入，正好满足总线对递归锁的要求 */
    if (!InitializeCriticalSectionAndSpinCount(&mutex->win32, 4000)) {
        free(mutex);
        return NULL;
    }
    return mutex;
}

void mb_os_mutex_lock(mb_mutex_t *mutex)
{
    EnterCriticalSection(&mutex->win32);
}

void mb_os_mutex_unlock(mb_mutex_t *mutex)
{
    LeaveCriticalSection(&mutex->win32);
}

void mb_os_mutex_destroy(mb_mutex_t *mutex)
{
    if (mutex == NULL) {
        return;
    }
    DeleteCriticalSection(&mutex->win32);
    free(mutex);
}

uint32_t mb_os_time_ms(void)
{
    return (uint32_t)GetTickCount64();
}

void mb_os_sleep_ms(uint32_t ms)
{
    Sleep((DWORD)ms);
}

void *mb_os_malloc(size_t size)
{
    return malloc(size);
}

void *mb_os_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

void mb_os_free(void *ptr)
{
    free(ptr);
}
