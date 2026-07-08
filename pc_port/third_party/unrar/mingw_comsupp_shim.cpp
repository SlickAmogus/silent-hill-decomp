/* MinGW ships no comsupp library; unrar's isnt.cpp (Windows 11 detection via
 * WMI) needs exactly one helper from it. On x64 __stdcall collapses to the
 * default convention, so this definition mangles identically to comdef.h's
 * declaration. */
#include <windows.h>
#include <oleauto.h>

namespace _com_util
{
BSTR ConvertStringToBSTR(const char* s)
{
    int  n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    BSTR b = SysAllocStringLen(NULL, n ? (UINT)(n - 1) : 0);
    if (b != NULL && n > 0)
    {
        MultiByteToWideChar(CP_ACP, 0, s, -1, b, n);
    }
    return b;
}
}
