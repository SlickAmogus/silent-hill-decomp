/* How much video memory THIS process holds and how much Windows will let it
 * keep. The GL extensions report only what is free on the card as a whole,
 * which cannot tell a game that grew from another program that took the
 * memory; Windows demotes a process that is over its budget to system memory,
 * and every draw that touches a demoted texture then crawls. */
#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <dxgi1_4.h>

/* dxgi.dll is loaded by name so the build needs no import library for it. */
static const GUID s_iidFactory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const GUID s_iidAdapter3 = { 0x645967a4, 0x1392, 0x4310, { 0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd } };

int Pc_Vram_Process(int* usedMb, int* budgetMb)
{
    typedef HRESULT (WINAPI *CreateFactory)(REFIID, void**);
    static IDXGIAdapter3* s_adapter = NULL;
    static int            s_tried   = 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO info;

    if (!s_tried)
    {
        HMODULE       dll    = LoadLibraryA("dxgi.dll");
        CreateFactory create = dll ? (CreateFactory)(void*)GetProcAddress(dll, "CreateDXGIFactory1") : NULL;
        IDXGIFactory1* factory = NULL;

        s_tried = 1;
        if (create != NULL && SUCCEEDED(create(&s_iidFactory1, (void**)&factory)) && factory != NULL)
        {
            IDXGIAdapter1* ad;
            IDXGIAdapter1* best = NULL;
            SIZE_T         bestMem = 0;
            UINT           i;

            /* The card with the most dedicated memory is the one a game is
             * drawn on in every setup this port has been reported from. */
            for (i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &ad) == S_OK; i++)
            {
                DXGI_ADAPTER_DESC1 desc;

                if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &desc)) && desc.DedicatedVideoMemory > bestMem)
                {
                    if (best != NULL)
                        IDXGIAdapter1_Release(best);
                    best    = ad;
                    bestMem = desc.DedicatedVideoMemory;
                }
                else
                    IDXGIAdapter1_Release(ad);
            }
            if (best != NULL)
            {
                IDXGIAdapter1_QueryInterface(best, &s_iidAdapter3, (void**)&s_adapter);
                IDXGIAdapter1_Release(best);
            }
            IDXGIFactory1_Release(factory);
        }
    }
    if (s_adapter == NULL ||
        FAILED(IDXGIAdapter3_QueryVideoMemoryInfo(s_adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return 0;
    *usedMb   = (int)(info.CurrentUsage >> 20);
    *budgetMb = (int)(info.Budget >> 20);
    return 1;
}
#else
int Pc_Vram_Process(int* usedMb, int* budgetMb)
{
    (void)usedMb;
    (void)budgetMb;
    return 0;
}
#endif
