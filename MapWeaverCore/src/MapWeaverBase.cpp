#include "MapWeaverBase.h"
#include "ogr_srs_api.h"

void InitProjSearchPath(const std::string& projDataDirUtf8)
{
    const char* projSearchPaths[] =
    {
        projDataDirUtf8.c_str(),
        nullptr
    };
    OSRSetPROJSearchPaths(projSearchPaths);
}