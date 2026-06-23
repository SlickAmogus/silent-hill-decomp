/* xtl.h — Minimal shim for RXDK dsound.h
 * This replaces RXDK's xtl.h and provides only what DirectSound needs,
 * without pulling in D3D, winsockx, or other heavy XDK headers.
 * Must live in xbox_compat/ so it's found before RXDK's include dir. */

#ifndef _XTL_
#define _XTL_

#ifndef _INC_XTL
#define _INC_XTL

/* nxdk's windows.h gives us basic types (DWORD, HANDLE, BOOL, etc.) */
#include <windows.h>

/* --- HRESULT / COM calling conventions ----------------------------------- */

#ifndef STDAPICALLTYPE
#define STDAPICALLTYPE  __stdcall
#endif

#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE __stdcall
#endif

#ifndef STDMETHODVCALLTYPE
#define STDMETHODVCALLTYPE __cdecl
#endif

#ifndef EXTERN_C
#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C extern
#endif
#endif

#ifndef STDAPI
#define STDAPI          EXTERN_C HRESULT STDAPICALLTYPE
#endif

#ifndef STDAPI_
#define STDAPI_(type)   EXTERN_C type STDAPICALLTYPE
#endif

#ifndef DECLSPEC_IMPORT
#define DECLSPEC_IMPORT
#endif

#ifndef WINOLEAPI
#define WINOLEAPI        EXTERN_C DECLSPEC_IMPORT HRESULT STDAPICALLTYPE
#define WINOLEAPI_(type) EXTERN_C DECLSPEC_IMPORT type STDAPICALLTYPE
#endif

/* HRESULT helpers (nxdk's winerror.h has error codes but not these macros) */
#ifndef SUCCEEDED
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#endif
#ifndef FAILED
#define FAILED(hr)    ((HRESULT)(hr) < 0)
#endif

#ifndef MAKE_HRESULT
#define MAKE_HRESULT(sev,fac,code) \
    ((HRESULT)(((unsigned long)(sev) << 31) | ((unsigned long)(fac) << 16) | ((unsigned long)(code))))
#endif

#ifndef _HRESULT_TYPEDEF_
#define _HRESULT_TYPEDEF_(sc) ((HRESULT)sc)
#endif

#ifndef S_OK
#define S_OK            ((HRESULT)0x00000000L)
#endif
#ifndef S_FALSE
#define S_FALSE         ((HRESULT)0x00000001L)
#endif
#ifndef E_FAIL
#define E_FAIL          ((HRESULT)0x80004005L)
#endif
#ifndef E_OUTOFMEMORY
#define E_OUTOFMEMORY   ((HRESULT)0x8007000EL)
#endif
#ifndef E_NOTIMPL
#define E_NOTIMPL       ((HRESULT)0x80004001L)
#endif
#ifndef E_INVALIDARG
#define E_INVALIDARG    ((HRESULT)0x80070057L)
#endif
#ifndef CLASS_E_NOAGGREGATION
#define CLASS_E_NOAGGREGATION ((HRESULT)0x80040110L)
#endif

/* --- GUID / IID / REFIID ------------------------------------------------ */

#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct _GUID {
    unsigned long  Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char  Data4[8];
} GUID;
#endif

typedef GUID IID;
typedef GUID CLSID;
typedef GUID *LPGUID;

#ifndef _REFIID_DEFINED
#define _REFIID_DEFINED
#ifdef __cplusplus
#define REFIID const IID &
#else
#define REFIID const IID *
#endif
#endif

#ifndef _REFCLSID_DEFINED
#define _REFCLSID_DEFINED
#ifdef __cplusplus
#define REFCLSID const IID &
#else
#define REFCLSID const IID *
#endif
#endif

#ifndef DEFINE_GUID
#ifdef INITGUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    EXTERN_C const GUID name = { l, w1, w2, { b1,b2,b3,b4,b5,b6,b7,b8 } }
#else
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    EXTERN_C const GUID name
#endif
#endif

/* --- IUnknown ------------------------------------------------------------ */

#ifndef __IUnknown_FWD_DEFINED__
#define __IUnknown_FWD_DEFINED__
typedef struct IUnknown IUnknown;
typedef IUnknown *LPUNKNOWN;
typedef IUnknown *PUNKNOWN;
#endif

/* --- COM interface declaration macros ------------------------------------ */

#ifdef __cplusplus

#define DECLARE_INTERFACE(iface) struct iface
#define DECLARE_INTERFACE_(iface, baseiface) struct iface : public baseiface

#define STDMETHOD(method)        virtual HRESULT STDMETHODCALLTYPE method
#define STDMETHOD_(type,method)  virtual type STDMETHODCALLTYPE method
#define PURE = 0
#define THIS_
#define THIS void

#else /* C */

#define DECLARE_INTERFACE(iface) \
    typedef struct iface { const struct iface##Vtbl *lpVtbl; } iface; \
    typedef struct iface##Vtbl iface##Vtbl; \
    struct iface##Vtbl

#define DECLARE_INTERFACE_(iface, baseiface) DECLARE_INTERFACE(iface)

#define STDMETHOD(method)        HRESULT (STDMETHODCALLTYPE *method)
#define STDMETHOD_(type,method)  type (STDMETHODCALLTYPE *method)
#define PURE
#define THIS_ struct IUnknown *This,
#define THIS  struct IUnknown *This

#endif

#define STDMETHODV(method)       STDMETHOD(method)
#define STDMETHODV_(type,method) STDMETHOD_(type,method)
#define BEGIN_INTERFACE
#define END_INTERFACE

/* --- D3DXVECTOR3 stub (used by DS3DBUFFER/DS3DLISTENER) ------------------ */

#ifndef D3DXVECTOR3_DEFINED
#define D3DXVECTOR3_DEFINED
typedef struct _D3DXVECTOR3 {
    float x, y, z;
} D3DXVECTOR3;
#endif

/* --- KSDATAFORMAT_SUBTYPE (used by WAVEFORMATEXTENSIBLE) ----------------- */

#ifndef KSDATAFORMAT_SUBTYPE_PCM
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_PCM,
    0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#endif

#ifndef KSDATAFORMAT_SUBTYPE_XBOX_ADPCM
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_XBOX_ADPCM,
    0x00000069, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#endif

/* --- CALLBACK (already in nxdk's windef.h but guard it) ------------------ */

#ifndef CALLBACK
#define CALLBACK __stdcall
#endif

/* --- FLOAT (already in nxdk but guard) ----------------------------------- */

#ifndef FLOAT
typedef float FLOAT;
#endif

/* --- REFERENCE_TIME ------------------------------------------------------ */

#ifndef REFERENCE_TIME
typedef long long REFERENCE_TIME;
typedef REFERENCE_TIME *LPREFERENCE_TIME;
#endif

/* --- FLT_MIN / FLT_MAX (needed by dsound.h constants) -------------------- */

#include <float.h>

/* --- XMO status flags (used by DSSTREAMSTATUS_READY) --------------------- */

#ifndef XMO_STATUSF_ACCEPT_INPUT_DATA
#define XMO_STATUSF_ACCEPT_INPUT_DATA  0x00000001
#endif

/* --- Xbox audio flags (from xbox.h, used by dsound.h DSSPEAKER_* macros) -- */

#ifndef XC_AUDIO_FLAGS_STEREO
#define XC_AUDIO_FLAGS_STEREO       0x00000000
#define XC_AUDIO_FLAGS_MONO         0x00000001
#define XC_AUDIO_FLAGS_SURROUND     0x00000002
#define XC_AUDIO_FLAGS_ENABLE_AC3   0x00010000
#define XC_AUDIO_FLAGS_ENABLE_DTS   0x00020000
#define XC_AUDIO_FLAGS_BASICMASK    0x0000FFFF
#define XC_AUDIO_FLAGS_ENCODEDMASK  0xFFFF0000
#define XC_AUDIO_FLAGS_BASIC(c)     ((c) & XC_AUDIO_FLAGS_BASICMASK)
#define XC_AUDIO_FLAGS_ENCODED(c)   ((c) & XC_AUDIO_FLAGS_ENCODEDMASK)
#define XC_AUDIO_FLAGS_COMBINED(b,e) ((b) | (e))
#endif

/* DSEFFECTIMAGEDESC / DSEFFECTIMAGELOC — defined by dsound.h itself,
 * we just need forward declarations for our xtl.h shim users that
 * don't include dsound.h. dsound.h will provide the full definitions. */

/* --- HWND stub (Xbox doesn't have real windows) -------------------------- */

#ifndef HWND
typedef void *HWND;
#endif

/* --- MMTIME (Xbox-specific, in dsound.h) --------------------------------- */
/* Already defined in dsound.h itself, no need here */

/* --- __RPC_FAR stub ------------------------------------------------------ */
#ifndef __RPC_FAR
#define __RPC_FAR
#endif
#ifndef __MIDL_CONST
#define __MIDL_CONST
#endif

/* --- Suppress RXDK's xbox.h include (dsound.h only needs types) ---------- */
#define _INC_XBOX

/* --- Suppress RXDK's winsockx.h include --------------------------------- */
#define _WINSOCKX_

#endif /* _INC_XTL */
#endif /* _XTL_ */
