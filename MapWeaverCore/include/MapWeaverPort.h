#ifndef MAP_WEAVER_CORE_PORT_H
#define MAP_WEAVER_CORE_PORT_H

#if defined(MAPWEAVERCORE_STATIC)
#define MAPWEAVERCORE_PORT

// Windows: 用 __declspec(dllexport/dllimport)
#elif defined(_WIN32) || defined(_WIN64)
#if defined(MAPWEAVERCORE_EXPORTS)
#define MAPWEAVERCORE_PORT __declspec(dllexport)
#else
#define MAPWEAVERCORE_PORT __declspec(dllimport)
#endif

// 非 Windows: 用 ELF 的可见性属性，或留空
#else
#if defined(__GNUC__) || defined(__clang__)
#define MAPWEAVERCORE_PORT __attribute__((visibility("default")))
#else
#define MAPWEAVERCORE_PORT
#endif
#endif

#endif