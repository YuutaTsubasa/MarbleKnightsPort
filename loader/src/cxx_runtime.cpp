// Pulls the NDK's libc++abi/libunwind into the link: the Darwin shim hands
// their __cxa_* entry points and typeinfo vtables to UnityFramework.
#include <stdexcept>
#include <typeinfo>

extern "C" const void *mk_cxx_runtime_anchor[] = {
    &typeid(std::bad_cast),
    &typeid(std::length_error),
};
