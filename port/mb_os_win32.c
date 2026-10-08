/**
 * @file mb_os_win32.c
 * @brief Windows（PC 模拟器）的 OS 抽象层实现。
 *
 * 用编译期常量 MB_CONFIG_OS == MB_OS_WIN32 选中，CMake 会自动处理。
 * 依赖仅限 Win32 API，MSVC / MinGW 均可编译。
 */
#include "message_bus/mb_config.h"

/*
 * 只有被选中的后端才产生代码；未选中时本文件编译为空目标文件。
 * 这样 Arduino / PlatformIO 这类会递归扫描并编译库内所有 .c 的构建系统
 * 无需配置源文件过滤，也不会因为多编译了别的 port 文件而触发 #error。
 */
#if MB_CONFIG_OS == MB_OS_WIN32

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

mb_sem_t *mb_os_sem_create(uint32_t initial, uint32_t max)
{
    mb_sem_t *sem;

    /* Win32 的信号量计数上限是 LONG_MAX，超了直接判非法 */
    if (max == 0 || max > 0x7FFFFFFFu || initial > max) {
        return NULL;
    }

    sem = (mb_sem_t *)malloc(sizeof(*sem));
    if (sem == NULL) {
        return NULL;
    }
    sem->win32 = CreateSemaphore(NULL, (LONG)initial, (LONG)max, NULL);
    if (sem->win32 == NULL) {
        free(sem);
        return NULL;
    }
    return sem;
}

bool mb_os_sem_wait(mb_sem_t *sem, uint32_t timeout_ms)
{
    DWORD wait = (timeout_ms == MB_WAIT_FOREVER) ? INFINITE : (DWORD)timeout_ms;

    return WaitForSingleObject(sem->win32, wait) == WAIT_OBJECT_0;
}

bool mb_os_sem_signal(mb_sem_t *sem)
{
    /* 计数已达上限时 ReleaseSemaphore 返回 FALSE（GetLastError = ERROR_TOO_MANY_POSTS） */
    return ReleaseSemaphore(sem->win32, 1, NULL) != 0;
}

void mb_os_sem_destroy(mb_sem_t *sem)
{
    if (sem == NULL) {
        return;
    }
    CloseHandle(sem->win32);
    free(sem);
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

#endif /* MB_CONFIG_OS == MB_OS_WIN32 */
