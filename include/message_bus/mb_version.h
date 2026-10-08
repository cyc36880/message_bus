/**
 * @file mb_version.h
 * @brief message_bus 版本信息。
 */
#ifndef MESSAGE_BUS_MB_VERSION_H
#define MESSAGE_BUS_MB_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

#define MB_VERSION_MAJOR 1
#define MB_VERSION_MINOR 1
#define MB_VERSION_PATCH 0

/** 形如 "1.1.0" 的版本字符串。 */
#define MB_VERSION_STRING "1.1.0"

/** 版本号打包成单个整数，便于比较：major << 16 | minor << 8 | patch。 */
#define MB_VERSION_NUMBER ((MB_VERSION_MAJOR << 16) | (MB_VERSION_MINOR << 8) | MB_VERSION_PATCH)

/** @return 运行期版本字符串（"1.1.0"）。 */
const char *mb_version_string(void);

/** @return 运行期版本整数（见 MB_VERSION_NUMBER）。 */
unsigned int mb_version_number(void);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_BUS_MB_VERSION_H */
