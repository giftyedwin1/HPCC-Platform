#ifndef JPLANE_HPP
#define JPLANE_HPP

#include "jfile.hpp"
#include <dlfcn.h>

inline const IPropertyTree * getStoragePlaneConfig(const char * name, bool required)
{
    // Runtime may have getStoragePlaneConfig(const char*, bool) or getStoragePlane(const char*)
    // Use dlsym to find whichever exists
    typedef IPropertyTree * (*fn2_t)(const char *, bool);
    typedef IPropertyTree * (*fn1_t)(const char *);
    static fn2_t fn2 = (fn2_t)dlsym(RTLD_DEFAULT, "_Z21getStoragePlaneConfigPKcb");
    static fn1_t fn1 = (fn1_t)dlsym(RTLD_DEFAULT, "_Z15getStoragePlanePKc");

    IPropertyTree * result = nullptr;
    if (fn2)
        result = fn2(name, required);
    else if (fn1)
        result = fn1(name);

    if (!result && required)
        throw makeStringExceptionV(99, "Storage plane '%s' not found", name);
    return result;
}

#endif
