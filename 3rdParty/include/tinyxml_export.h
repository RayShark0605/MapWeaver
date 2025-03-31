
#ifndef TINYXML_EXPORT_H
#define TINYXML_EXPORT_H

#ifdef TINYXML_STATIC_DEFINE
#  define TINYXML_EXPORT
#  define TINYXML_NO_EXPORT
#else
#  ifndef TINYXML_EXPORT
#    ifdef unofficial_tinyxml_EXPORTS
        /* We are building this library */
#      define TINYXML_EXPORT 
#    else
        /* We are using this library */
#      define TINYXML_EXPORT 
#    endif
#  endif

#  ifndef TINYXML_NO_EXPORT
#    define TINYXML_NO_EXPORT 
#  endif
#endif

#ifndef TINYXML_DEPRECATED
#  define TINYXML_DEPRECATED __declspec(deprecated)
#endif

#ifndef TINYXML_DEPRECATED_EXPORT
#  define TINYXML_DEPRECATED_EXPORT TINYXML_EXPORT TINYXML_DEPRECATED
#endif

#ifndef TINYXML_DEPRECATED_NO_EXPORT
#  define TINYXML_DEPRECATED_NO_EXPORT TINYXML_NO_EXPORT TINYXML_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef TINYXML_NO_DEPRECATED
#    define TINYXML_NO_DEPRECATED
#  endif
#endif

#endif /* TINYXML_EXPORT_H */
