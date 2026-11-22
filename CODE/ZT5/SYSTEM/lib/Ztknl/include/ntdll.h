/*
 * Zratro OS - Windows NTDLL/Kernel32 Compatibility Layer
 * 
 * 完整的 Windows API 模拟支持 (WPS 级别)
 */

#ifndef NT_H
#define NT_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== Windows 调用约定 ========== */

#ifndef NTAPI
#define NTAPI
#endif

#ifndef WINAPI
#define WINAPI
#endif

#ifndef CALLBACK
#define CALLBACK
#endif

/* ========== Windows 基本类型 ========== */

typedef uint64_t HANDLE;
typedef uint64_t HMODULE;
typedef uint64_t HINSTANCE;
typedef uint64_t HWND;
typedef uint64_t HDC;
typedef uint64_t HGDIOBJ;
typedef uint64_t HICON;
typedef uint64_t HCURSOR;
typedef uint64_t HBRUSH;
typedef uint64_t HFONT;
typedef uint64_t HMENU;
typedef uint64_t HACCEL;
typedef uint64_t HHOOK;
typedef uint64_t HKEY;
typedef uint64_t SC_HANDLE;
typedef uint64_t SOCKET;
typedef uint64_t HGLOBAL;
typedef uint64_t HLOCAL;
typedef uint64_t HRSRC;
typedef uint64_t HRGN;
typedef uint64_t HBITMAP;
typedef uint64_t HPALETTE;
typedef uint64_t HPEN;
typedef uint64_t HMETAFILE;
typedef uint64_t HENHMETAFILE;
typedef uint64_t HCRYPTPROV;
typedef uint64_t HCRYPTKEY;
typedef uint64_t HCRYPTHASH;
typedef uint64_t HRESULT;

typedef uint32_t DWORD;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef int BOOL;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef uint16_t WCHAR;
typedef char CHAR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef CHAR *LPSTR;
typedef const CHAR *LPCSTR;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef int INT;
typedef uint32_t UINT;
typedef uint64_t ULONG_PTR;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef int64_t LONG_PTR;
typedef ULONG_PTR DWORD_PTR;
typedef ULONG_PTR SIZE_T;
typedef LONG_PTR SSIZE_T;
typedef void *PVOID;
typedef const void *PCVOID;
typedef DWORD *LPDWORD, *PDWORD;
typedef LONG *LPLONG, *PLONG;
typedef WORD *LPWORD, *PWORD;
typedef BYTE *LPBYTE, *PBYTE;
typedef INT *LPINT, *PINT;
typedef UINT *LPUINT, *PUINT;
typedef WCHAR *LPWCH, *PWCH;
typedef CHAR *LPCH, *PCH;
typedef ULONG_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef ULONG_PTR UINT_PTR;
typedef LONG_PTR INT_PTR;
typedef DWORD_PTR *PDWORD_PTR;
typedef WORD ATOM;
typedef DWORD COLORREF;
typedef DWORD ALG_ID;

/* 函数指针类型 */
typedef void *FARPROC;
typedef void *NEARPROC;
typedef void *PROC;

/* 坐标类型 */
typedef int16_t SHORT;
typedef uint16_t USHORT;

/* 点结构 */
typedef struct tagPOINT {
    LONG x;
    LONG y;
} POINT, *LPPOINT;

/* 尺寸结构 */
typedef struct tagSIZE {
    LONG cx;
    LONG cy;
} SIZE, *LPSIZE;

/* 矩形结构 */
typedef struct tagRECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *LPRECT;

/* 变参列表 */
typedef __builtin_va_list va_list;

/* 其他常量 */
#define INVALID_FILE_SIZE        ((DWORD)0xFFFFFFFF)

/* 句柄常量 */
#define INVALID_HANDLE_VALUE    ((HANDLE)-1)
#define NULL_HANDLE             ((HANDLE)0)
#define INVALID_SOCKET          ((SOCKET)-1)

/* 布尔值 */
#define TRUE    1
#define FALSE   0

/* 颜色宏 */
#define RGB(r,g,b)          ((COLORREF)(((BYTE)(r)|((WORD)((BYTE)(g))<<8))|(((DWORD)(BYTE)(b))<<16)))
#define GetRValue(rgb)      ((BYTE)(rgb))
#define GetGValue(rgb)      ((BYTE)(((WORD)(rgb)) >> 8))
#define GetBValue(rgb)      ((BYTE)((rgb)>>16))

/* 路径最大长度 */
#define MAX_PATH 260

/* ========== NT 状态码 ========== */

#define STATUS_SUCCESS                      0x00000000
#define STATUS_ERROR                        0xC0000001
#define STATUS_NOT_IMPLEMENTED              0xC0000002
#define STATUS_INVALID_PARAMETER            0xC000000D
#define STATUS_NO_MEMORY                    0xC0000017
#define STATUS_ACCESS_DENIED                0xC0000022
#define STATUS_OBJECT_NAME_NOT_FOUND        0xC0000034
#define STATUS_OBJECT_PATH_NOT_FOUND        0xC000003A
#define STATUS_PENDING                      0x00000103
#define STATUS_TIMEOUT                      0x00000102

/* ========== Windows 错误码 ========== */

#define ERROR_SUCCESS                   0
#define ERROR_INVALID_FUNCTION          1
#define ERROR_FILE_NOT_FOUND            2
#define ERROR_PATH_NOT_FOUND            3
#define ERROR_ACCESS_DENIED             5
#define ERROR_INVALID_HANDLE            6
#define ERROR_NOT_ENOUGH_MEMORY         8
#define ERROR_INVALID_PARAMETER         87
#define ERROR_MOD_NOT_FOUND             126
#define ERROR_NO_MORE_ITEMS             259
#define ERROR_CALL_NOT_IMPLEMENTED      120
#define ERROR_FILE_EXISTS               80
#define ERROR_BUFFER_OVERFLOW           111
#define ERROR_INSUFFICIENT_BUFFER       122
#define ERROR_NO_DATA                   232
#define ERROR_NOT_SUPPORTED             50

/* COM 错误码 */
#define S_OK                            0
#define S_FALSE                         1
#define E_NOTIMPL                       0x80004001
#define E_NOINTERFACE                   0x80004002
#define E_POINTER                       0x80004003
#define E_ABORT                         0x80004004
#define E_FAIL                          0x80004005
#define E_ACCESSDENIED                  0x80070005
#define E_OUTOFMEMORY                   0x8007000E
#define E_INVALIDARG                    0x80070057
#define CLASS_E_CLASSNOTAVAILABLE       0x80040111

/* 其他常量 */
#define NO_ERROR                        0
#define INVALID_FILE_ATTRIBUTES         ((DWORD)0xFFFFFFFF)

/* ========== 内存保护常量 ========== */

#define PAGE_NOACCESS          0x01
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04
#define PAGE_EXECUTE           0x10
#define PAGE_EXECUTE_READ      0x20
#define PAGE_EXECUTE_READWRITE 0x40

#define MEM_COMMIT             0x00001000
#define MEM_RESERVE            0x00002000
#define MEM_RELEASE            0x00008000
#define MEM_PRIVATE            0x00020000
#define MEM_MAPPED             0x00040000

/* 内存状态 */
#define MEM_FREE               0x00010000

/* ========== 文件访问常量 ========== */

#define GENERIC_READ           0x80000000
#define GENERIC_WRITE          0x40000000
#define GENERIC_EXECUTE        0x20000000
#define GENERIC_ALL            0x10000000

#define FILE_SHARE_READ        0x00000001
#define FILE_SHARE_WRITE       0x00000002

#define CREATE_NEW             1
#define CREATE_ALWAYS          2
#define OPEN_EXISTING          3
#define OPEN_ALWAYS            4
#define TRUNCATE_EXISTING      5

#define FILE_ATTRIBUTE_NORMAL          0x00000080
#define FILE_ATTRIBUTE_DIRECTORY       0x00000010

#define STD_INPUT_HANDLE        ((DWORD)-10)
#define STD_OUTPUT_HANDLE       ((DWORD)-11)
#define STD_ERROR_HANDLE        ((DWORD)-12)

/* ========== 进程/线程常量 ========== */

#define PROCESS_TERMINATE       0x0001
#define PROCESS_VM_OPERATION    0x0008
#define PROCESS_VM_READ         0x0010
#define PROCESS_VM_WRITE        0x0020
#define PROCESS_ALL_ACCESS      0x001FFFFF

#define THREAD_TERMINATE        0x0001
#define THREAD_ALL_ACCESS       0x001FFFFF

#define CREATE_SUSPENDED        0x00000004

/* ========== 等待常量 ========== */

#define INFINITE                0xFFFFFFFF
#define WAIT_OBJECT_0           0
#define WAIT_TIMEOUT            0x00000102
#define WAIT_FAILED             ((DWORD)0xFFFFFFFF)

/* ========== 结构体定义 ========== */

typedef struct {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *LPFILETIME;

typedef struct {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} PROCESS_INFORMATION, *LPPROCESS_INFORMATION;

typedef struct {
    DWORD cb;
    char *lpReserved;
    char *lpDesktop;
    char *lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize;
    DWORD dwXCountChars, dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    BYTE *lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;

typedef struct {
    DWORD cb;
    WCHAR *lpReserved;
    WCHAR *lpDesktop;
    WCHAR *lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize;
    DWORD dwXCountChars, dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    BYTE *lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOW, *LPSTARTUPINFOW;

typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;

typedef struct {
    ULONG_PTR dwPageSize;
    LPVOID lpMinimumApplicationAddress;
    LPVOID lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

typedef struct {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    size_t RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

typedef struct {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct { DWORD Offset; DWORD OffsetHigh; };
        PVOID Pointer;
    };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef struct {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct {
    DWORD Data1;
    WORD Data2;
    WORD Data3;
    BYTE Data4[8];
} GUID, *LPGUID, CLSID, IID;

typedef const CLSID *REFCLSID;
typedef const IID *REFIID;

typedef union {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

/* ========== SEH 异常处理 ========== */

#define EXCEPTION_EXECUTE_HANDLER       0x1
#define EXCEPTION_CONTINUE_SEARCH       0x0
#define EXCEPTION_CONTINUE_EXECUTION    0xFFFFFFFF

#define STATUS_ACCESS_VIOLATION         0xC0000005
#define STATUS_STACK_OVERFLOW           0xC00000FD
#define EXCEPTION_BREAKPOINT            0x80000003

typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[15];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PVOID ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

typedef LONG (NTAPI *PVECTORED_EXCEPTION_HANDLER)(PEXCEPTION_POINTERS ExceptionInfo);
typedef LONG (WINAPI *PTOP_LEVEL_EXCEPTION_FILTER)(PEXCEPTION_POINTERS ExceptionInfo);
typedef PTOP_LEVEL_EXCEPTION_FILTER LPTOP_LEVEL_EXCEPTION_FILTER;

/* ========== TLS 线程局部存储 ========== */

#define TLS_MINIMUM_AVAILABLE       64
#define TLS_OUT_OF_INDEXES          0xFFFFFFFF

/* ========== 窗口消息 ========== */

#define WM_NULL                         0x0000
#define WM_CREATE                       0x0001
#define WM_DESTROY                      0x0002
#define WM_MOVE                         0x0003
#define WM_SIZE                         0x0005
#define WM_ACTIVATE                     0x0006
#define WM_SETFOCUS                     0x0007
#define WM_KILLFOCUS                    0x0008
#define WM_ENABLE                       0x000A
#define WM_SETTEXT                      0x000C
#define WM_GETTEXT                      0x000D
#define WM_GETTEXTLENGTH                0x000E
#define WM_PAINT                        0x000F
#define WM_CLOSE                        0x0010
#define WM_QUIT                         0x0012
#define WM_ERASEBKGND                   0x0014
#define WM_SHOWWINDOW                   0x0018
#define WM_SETCURSOR                    0x0020
#define WM_SETFONT                      0x0030
#define WM_GETFONT                      0x0031
#define WM_COMMAND                      0x0111
#define WM_SYSCOMMAND                   0x0112
#define WM_TIMER                        0x0113
#define WM_HSCROLL                      0x0114
#define WM_VSCROLL                      0x0115
#define WM_INITDIALOG                   0x0110
#define WM_MOUSEMOVE                    0x0200
#define WM_LBUTTONDOWN                  0x0201
#define WM_LBUTTONUP                    0x0202
#define WM_RBUTTONDOWN                  0x0204
#define WM_RBUTTONUP                    0x0205
#define WM_MOUSEWHEEL                   0x020A
#define WM_KEYDOWN                      0x0100
#define WM_KEYUP                        0x0101
#define WM_CHAR                         0x0102
#define WM_SYSKEYDOWN                   0x0104
#define WM_SYSKEYUP                     0x0105
#define WM_APP                          0x8000
#define WM_USER                         0x0400

/* 消息框类型 */
#define MB_OK                       0x00000000L
#define MB_OKCANCEL                 0x00000001L
#define MB_YESNO                    0x00000004L
#define MB_ICONERROR                0x00000010L
#define MB_ICONQUESTION             0x00000020L
#define MB_ICONWARNING              0x00000030L
#define MB_ICONINFORMATION          0x00000040L

#define IDOK                        1
#define IDCANCEL                    2
#define IDYES                       6
#define IDNO                        7

/* 显示窗口命令 */
#define SW_HIDE                     0
#define SW_SHOWNORMAL               1
#define SW_SHOW                     5
#define SW_MINIMIZE                 6
#define SW_MAXIMIZE                 3
#define SW_RESTORE                  9

/* 窗口风格 */
#define WS_OVERLAPPED           0x00000000L
#define WS_POPUP                0x80000000L
#define WS_CHILD                0x40000000L
#define WS_MINIMIZE             0x20000000L
#define WS_VISIBLE              0x10000000L
#define WS_DISABLED             0x08000000L
#define WS_CLIPSIBLINGS         0x04000000L
#define WS_CLIPCHILDREN         0x02000000L
#define WS_MAXIMIZE             0x01000000L
#define WS_CAPTION              0x00C00000L
#define WS_BORDER               0x00800000L
#define WS_VSCROLL              0x00200000L
#define WS_HSCROLL              0x00100000L
#define WS_SYSMENU              0x00080000L
#define WS_THICKFRAME           0x00040000L
#define WS_MINIMIZEBOX          0x00020000L
#define WS_MAXIMIZEBOX          0x00010000L
#define WS_OVERLAPPEDWINDOW     (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)

#define WS_EX_DLGMODALFRAME     0x00000001L
#define WS_EX_TOOLWINDOW        0x00000080L
#define WS_EX_CLIENTEDGE        0x00000200L
#define WS_EX_APPWINDOW         0x00040000L

/* GetWindowLong 偏移 */
#define GWL_STYLE               (-16)
#define GWL_EXSTYLE             (-20)
#define GWL_WNDPROC             (-4)
#define GWL_HINSTANCE           (-6)
#define GWL_ID                  (-12)
#define GWL_USERDATA            (-21)

/* 窗口消息结构体 */
typedef struct tagMSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG, *LPMSG;

/* 窗口过程 */
typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void (CALLBACK *TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);
typedef INT_PTR (CALLBACK *DLGPROC)(HWND, UINT, WPARAM, LPARAM);

/* 窗口类 */
typedef struct tagWNDCLASSA {
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName;
    LPCSTR lpszClassName;
} WNDCLASSA, *LPWNDCLASSA, WNDCLASSW, *LPWNDCLASSW;

typedef struct tagWNDCLASSEXA {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName;
    LPCSTR lpszClassName;
    HICON hIconSm;
} WNDCLASSEXA, *LPWNDCLASSEXA, WNDCLASSEXW, *LPWNDCLASSEXW;

/* 绘图结构 */
typedef struct tagPAINTSTRUCT {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT, *LPPAINTSTRUCT;

/* 图标信息 */
typedef struct _ICONINFO {
    BOOL fIcon;
    DWORD xHotspot;
    DWORD yHotspot;
    HBITMAP hbmMask;
    HBITMAP hbmColor;
} ICONINFO, *PICONINFO;

/* 菜单标志 */
#define MF_INSERT           0x00000000L
#define MF_CHANGE           0x00000080L
#define MF_APPEND           0x00000100L
#define MF_DELETE           0x00000200L
#define MF_BYCOMMAND        0x00000000L
#define MF_BYPOSITION       0x00000400L
#define MF_SEPARATOR        0x00000800L
#define MF_ENABLED          0x00000000L
#define MF_GRAYED           0x00000001L
#define MF_DISABLED         0x00000002L
#define MF_CHECKED          0x00000008L
#define MF_STRING           0x00000000L
#define MF_BITMAP           0x00000004L
#define MF_POPUP            0x00000010L

/* 标准图标/光标 */
#define IDI_APPLICATION     32512
#define IDI_HAND            32513
#define IDI_QUESTION        32514
#define IDI_EXCLAMATION     32515
#define IDI_ASTERISK        32516

#define IDC_ARROW           32512
#define IDC_IBEAM           32513
#define IDC_WAIT            32514
#define IDC_CROSS           32515
#define IDC_HAND            32649

/* ========== GDI 定义 ========== */

#define TRANSPARENT             1
#define OPAQUE                  2

/* 设备能力索引 */
#define DRIVERVERSION           0
#define TECHNOLOGY              2
#define HORZSIZE                4
#define VERTSIZE                5
#define HORZRES                 8
#define VERTRES                 9
#define BITSPIXEL               12
#define PLANES                  14
#define NUMBRUSHES              16
#define NUMPENS                 18
#define NUMFONTS                22
#define NUMCOLORS               24
#define LOGPIXELSX              88
#define LOGPIXELSY              90

/* GDI 对象类型 */
#define OBJ_PEN             1
#define OBJ_BRUSH           2
#define OBJ_DC              3
#define OBJ_FONT            6
#define OBJ_BITMAP          7
#define OBJ_REGION          8

/* 标准对象 */
#define WHITE_BRUSH         0
#define LTGRAY_BRUSH        1
#define GRAY_BRUSH          2
#define DKGRAY_BRUSH        3
#define BLACK_BRUSH         4
#define NULL_BRUSH          5
#define WHITE_PEN           6
#define BLACK_PEN           7
#define NULL_PEN            8
#define SYSTEM_FONT         13

/* 画笔样式 */
#define PS_SOLID            0
#define PS_DASH             1
#define PS_DOT              2
#define PS_DASHDOT          3
#define PS_DASHDOTDOT       4
#define PS_NULL             5

/* 画刷样式 */
#define BS_SOLID            0
#define BS_NULL             1
#define BS_HATCHED          2
#define BS_PATTERN          3

/* 光栅操作 */
#define SRCCOPY             0x00CC0020
#define SRCAND              0x008800C6
#define SRCINVERT           0x00660046
#define SRCPAINT            0x00EE0086
#define BLACKNESS           0x00000042
#define WHITENESS           0x00FF0062

/* 文本对齐 */
#define TA_LEFT             0
#define TA_RIGHT            2
#define TA_CENTER           6
#define TA_TOP              0
#define TA_BOTTOM           8
#define TA_BASELINE         24

/* 映射模式 */
#define MM_TEXT             1
#define MM_ISOTROPIC        7
#define MM_ANISOTROPIC      8

/* 位图信息 */
typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER, *LPBITMAPINFOHEADER;

typedef struct tagRGBQUAD {
    BYTE rgbBlue;
    BYTE rgbGreen;
    BYTE rgbRed;
    BYTE rgbReserved;
} RGBQUAD;

typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
} BITMAPINFO, *LPBITMAPINFO;

/* 逻辑字体 */
typedef struct tagLOGFONTA {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    CHAR lfFaceName[32];
} LOGFONTA, *LPLOGFONTA;

typedef struct tagLOGFONTW {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    WCHAR lfFaceName[32];
} LOGFONTW, *LPLOGFONTW;

/* 文本度量 */
typedef struct tagTEXTMETRICA {
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    LONG tmWeight;
    LONG tmOverhang;
    LONG tmDigitizedAspectX;
    LONG tmDigitizedAspectY;
    BYTE tmFirstChar;
    BYTE tmLastChar;
    BYTE tmDefaultChar;
    BYTE tmBreakChar;
    BYTE tmItalic;
    BYTE tmUnderlined;
    BYTE tmStruckOut;
    BYTE tmPitchAndFamily;
    BYTE tmCharSet;
} TEXTMETRICA, *LPTEXTMETRICA;

typedef struct tagTEXTMETRICW {
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    LONG tmWeight;
    LONG tmOverhang;
    LONG tmDigitizedAspectX;
    LONG tmDigitizedAspectY;
    WCHAR tmFirstChar;
    WCHAR tmLastChar;
    WCHAR tmDefaultChar;
    WCHAR tmBreakChar;
    BYTE tmItalic;
    BYTE tmUnderlined;
    BYTE tmStruckOut;
    BYTE tmPitchAndFamily;
    BYTE tmCharSet;
} TEXTMETRICW, *LPTEXTMETRICW;

/* 逻辑画刷 */
typedef struct tagLOGBRUSH {
    UINT lbStyle;
    COLORREF lbColor;
    ULONG_PTR lbHatch;
} LOGBRUSH, *LPLOGBRUSH;

/* 区域 */
#define RGN_AND             1
#define RGN_OR              2
#define RGN_XOR             3
#define RGN_DIFF            4
#define RGN_COPY            5
#define ERROR               0
#define NULLREGION          1
#define SIMPLEREGION        2
#define COMPLEXREGION       3

/* ========== 注册表定义 ========== */

#define HKEY_CLASSES_ROOT               ((HKEY)0x80000000)
#define HKEY_CURRENT_USER               ((HKEY)0x80000001)
#define HKEY_LOCAL_MACHINE              ((HKEY)0x80000002)
#define HKEY_USERS                      ((HKEY)0x80000003)
#define HKEY_CURRENT_CONFIG             ((HKEY)0x80000005)

#define KEY_QUERY_VALUE                 0x0001
#define KEY_SET_VALUE                   0x0002
#define KEY_CREATE_SUB_KEY              0x0004
#define KEY_ENUMERATE_SUB_KEYS          0x0008
#define KEY_ALL_ACCESS                  0x000F003F

#define REG_NONE                        0
#define REG_SZ                          1
#define REG_EXPAND_SZ                   2
#define REG_BINARY                      3
#define REG_DWORD                       4
#define REG_MULTI_SZ                    7
#define REG_QWORD                       11

#define REG_OPENED_EXISTING_KEY         0x00000002L

typedef DWORD REGSAM;

/* ========== 服务定义 ========== */

#define SERVICE_WIN32_OWN_PROCESS       0x00000010
#define SERVICE_WIN32_SHARE_PROCESS     0x00000020
#define SERVICE_INTERACTIVE_PROCESS     0x00000100

#define SERVICE_AUTO_START              0x00000002
#define SERVICE_DEMAND_START            0x00000003
#define SERVICE_DISABLED                0x00000004

#define SERVICE_STOPPED                 0x00000001
#define SERVICE_START_PENDING           0x00000002
#define SERVICE_STOP_PENDING            0x00000003
#define SERVICE_RUNNING                 0x00000004

#define SERVICE_CONTROL_STOP            0x00000001
#define SERVICE_CONTROL_PAUSE           0x00000002
#define SERVICE_CONTROL_CONTINUE        0x00000003

#define SC_MANAGER_CONNECT              0x0001
#define SC_MANAGER_CREATE_SERVICE       0x0002
#define SC_MANAGER_ALL_ACCESS           0x000F003F

#define SERVICE_ALL_ACCESS              0x000F01FF

typedef struct _SERVICE_STATUS {
    DWORD dwServiceType;
    DWORD dwCurrentState;
    DWORD dwControlsAccepted;
    DWORD dwWin32ExitCode;
    DWORD dwServiceSpecificExitCode;
    DWORD dwCheckPoint;
    DWORD dwWaitHint;
} SERVICE_STATUS, *LPSERVICE_STATUS;

/* ========== Shell 定义 ========== */

#define CSIDL_DESKTOP                 0x0000
#define CSIDL_PROGRAMS                0x0002
#define CSIDL_PERSONAL                0x0005
#define CSIDL_FAVORITES               0x0006
#define CSIDL_STARTUP                 0x0007
#define CSIDL_RECENT                  0x0008
#define CSIDL_SENDTO                  0x0009
#define CSIDL_STARTMENU               0x000B
#define CSIDL_DESKTOPDIRECTORY        0x0010
#define CSIDL_FONTS                   0x0014
#define CSIDL_APPDATA                 0x001A
#define CSIDL_LOCAL_APPDATA           0x001C
#define CSIDL_WINDOWS                 0x0024
#define CSIDL_SYSTEM                  0x0025
#define CSIDL_PROGRAM_FILES           0x0026
#define CSIDL_PROFILE                 0x0028
#define CSIDL_PROGRAM_FILES_COMMON    0x002B
#define CSIDL_COMMON_APPDATA          0x0023

/* ========== COM 定义 ========== */

typedef enum tagCOINIT {
    COINIT_APARTMENTTHREADED  = 0x2,
    COINIT_MULTITHREADED      = 0x0,
    COINIT_DISABLE_OLE1DDE    = 0x4,
    COINIT_SPEED_OVER_MEMORY  = 0x8,
} COINIT;

typedef enum tagCLSCTX {
    CLSCTX_INPROC_SERVER          = 0x1,
    CLSCTX_INPROC_HANDLER         = 0x2,
    CLSCTX_LOCAL_SERVER           = 0x4,
    CLSCTX_REMOTE_SERVER          = 0x10,
    CLSCTX_ALL                    = CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER | CLSCTX_REMOTE_SERVER,
} CLSCTX;

typedef WCHAR OLECHAR;
typedef OLECHAR *BSTR;
typedef const OLECHAR *LPCOLESTR;
typedef OLECHAR *LPOLESTR;

#define CRYPT_VERIFYCONTEXT        0xF0000000
#define PROV_RSA_FULL              1

#define CALG_MD5                   0x00008003
#define CALG_SHA                   0x00008004
#define CALG_SHA_256               0x0000800C
#define CALG_RSA_SIGN              0x00002400
#define CALG_RSA_KEYX              0x0000A400
#define CALG_DES                   0x00006601
#define CALG_3DES                  0x00006603
#define CALG_AES_128               0x0000660E
#define CALG_AES_256               0x00006610

/* ========== DLL 初始化函数 ========== */

void dll_init(void);

/* ========== PE 进程 TEB/PEB 初始化 ========== */

int pe_init_teb_peb(uint64_t image_base, uint64_t stack_top, uint64_t stack_size,
                    const char *cmdline);
uint64_t pe_get_peb_address(void);
uint64_t pe_get_teb_address(void);

/* ========== Windows syscall 处理器 ========== */

uint64_t windows_syscall_handler(uint64_t num, uint64_t arg0, uint64_t arg1,
                                  uint64_t arg2, uint64_t arg3, uint64_t arg4,
                                  uint64_t arg5, uint64_t arg6, uint64_t arg7);

/* ========== DLL 导出函数查找 ========== */

void *ntdll_get_proc(const char *name);
void *kernel32_get_proc(const char *name);
void *user32_get_proc(const char *name);
void *gdi32_get_proc(const char *name);
void *advapi32_get_proc(const char *name);

/* ========== KERNEL32 文件操作 ========== */

HANDLE WINAPI win_CreateFileA(LPCSTR filename, DWORD access, DWORD share,
                               LPSECURITY_ATTRIBUTES security, DWORD creation,
                               DWORD flags, HANDLE templ);
BOOL WINAPI win_ReadFile(HANDLE file, LPVOID buf, DWORD len, LPDWORD read, LPOVERLAPPED overlapped);
BOOL WINAPI win_WriteFile(HANDLE file, LPCVOID buf, DWORD len, LPDWORD written, LPOVERLAPPED overlapped);
BOOL WINAPI win_CloseHandle(HANDLE handle);
DWORD WINAPI win_GetFileSize(HANDLE file, LPDWORD filesizehigh);
BOOL WINAPI win_DeleteFileA(LPCSTR filename);
BOOL WINAPI win_MoveFileA(LPCSTR existing, LPCSTR newname);
BOOL WINAPI win_CopyFileA(LPCSTR existing, LPCSTR newname, BOOL failifexists);
HANDLE WINAPI win_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data);
BOOL WINAPI win_FindNextFileA(HANDLE handle, LPWIN32_FIND_DATAA data);
BOOL WINAPI win_FindClose(HANDLE handle);
DWORD WINAPI win_GetFileAttributesA(LPCSTR filename);

/* ========== KERNEL32 进程/线程操作 ========== */

BOOL WINAPI win_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES proc_attr,
                                LPSECURITY_ATTRIBUTES thread_attr, BOOL inherit,
                                DWORD flags, LPVOID env, LPCSTR cwd,
                                LPSTARTUPINFOA startup, LPPROCESS_INFORMATION info);
BOOL WINAPI win_TerminateProcess(HANDLE process, DWORD exit_code);
DWORD WINAPI win_GetCurrentProcessId(void);
DWORD WINAPI win_GetCurrentThreadId(void);
HANDLE WINAPI win_GetCurrentProcess(void);
HANDLE WINAPI win_GetCurrentThread(void);
DWORD WINAPI win_WaitForSingleObject(HANDLE handle, DWORD ms);
HANDLE WINAPI win_CreateThread(LPSECURITY_ATTRIBUTES attr, size_t stack,
                                void *start, void *param, DWORD flags, LPDWORD tid);
DWORD WINAPI win_ResumeThread(HANDLE thread);
DWORD WINAPI win_SuspendThread(HANDLE thread);
void WINAPI win_ExitProcess(UINT exit_code);
void WINAPI win_ExitThread(DWORD exit_code);
DWORD WINAPI win_GetLastError(void);
void WINAPI win_SetLastError(DWORD error);

/* ========== KERNEL32 内存操作 ========== */

LPVOID WINAPI win_VirtualAlloc(LPVOID addr, size_t size, DWORD type, DWORD protect);
BOOL WINAPI win_VirtualFree(LPVOID addr, size_t size, DWORD type);
BOOL WINAPI win_VirtualProtect(LPVOID addr, size_t size, DWORD new_prot, LPDWORD old_prot);
LPVOID WINAPI win_HeapCreate(DWORD options, size_t init, size_t max);
BOOL WINAPI win_HeapDestroy(HANDLE heap);
LPVOID WINAPI win_HeapAlloc(HANDLE heap, DWORD flags, size_t size);
LPVOID WINAPI win_HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, size_t size);
BOOL WINAPI win_HeapFree(HANDLE heap, DWORD flags, LPVOID mem);
size_t WINAPI win_HeapSize(HANDLE heap, DWORD flags, LPCVOID mem);

/* ========== KERNEL32 模块操作 ========== */

HMODULE WINAPI win_LoadLibraryA(LPCSTR name);
HMODULE WINAPI win_LoadLibraryW(LPCWSTR name);
BOOL WINAPI win_FreeLibrary(HMODULE module);
FARPROC WINAPI win_GetProcAddress(HMODULE module, LPCSTR name);
HMODULE WINAPI win_GetModuleHandleA(LPCSTR name);
HMODULE WINAPI win_GetModuleHandleW(LPCWSTR name);
DWORD WINAPI win_GetModuleFileNameA(HMODULE module, LPSTR buf, DWORD size);

/* ========== KERNEL32 同步操作 ========== */

void WINAPI win_Sleep(DWORD ms);
HANDLE WINAPI win_CreateEventA(LPSECURITY_ATTRIBUTES attr, BOOL manual, BOOL init, LPCSTR name);
BOOL WINAPI win_SetEvent(HANDLE event);
BOOL WINAPI win_ResetEvent(HANDLE event);
HANDLE WINAPI win_CreateMutexA(LPSECURITY_ATTRIBUTES attr, BOOL owner, LPCSTR name);
BOOL WINAPI win_ReleaseMutex(HANDLE mutex);
HANDLE WINAPI win_CreateSemaphoreA(LPSECURITY_ATTRIBUTES attr, LONG init, LONG max, LPCSTR name);
BOOL WINAPI win_ReleaseSemaphore(HANDLE sem, LONG count, LONG *prev);

/* ========== KERNEL32 控制台操作 ========== */

HANDLE WINAPI win_GetStdHandle(DWORD std);
BOOL WINAPI win_SetStdHandle(DWORD std, HANDLE handle);
DWORD WINAPI win_GetFileType(HANDLE file);
BOOL WINAPI win_WriteConsoleA(HANDLE handle, LPCVOID buf, DWORD len, LPDWORD written, LPVOID reserved);
BOOL WINAPI win_ReadConsoleA(HANDLE handle, LPVOID buf, DWORD len, LPDWORD read, LPVOID reserved);

/* ========== KERNEL32 字符串操作 ========== */

int WINAPI win_lstrcmpA(LPCSTR s1, LPCSTR s2);
int WINAPI win_lstrcmpiA(LPCSTR s1, LPCSTR s2);
LPSTR WINAPI win_lstrcpyA(LPSTR dest, LPCSTR src);
int WINAPI win_lstrlenA(LPCSTR s);

/* ========== KERNEL32 系统信息 ========== */

void WINAPI win_GetSystemInfo(LPSYSTEM_INFO info);
DWORD WINAPI win_GetTickCount(void);
ULONGLONG WINAPI win_GetTickCount64(void);
BOOL WINAPI win_QueryPerformanceCounter(PLARGE_INTEGER count);
BOOL WINAPI win_QueryPerformanceFrequency(PLARGE_INTEGER freq);
DWORD WINAPI win_GetCurrentDirectoryA(DWORD size, LPSTR buf);
BOOL WINAPI win_SetCurrentDirectoryA(LPCSTR path);
DWORD WINAPI win_GetTempPathA(DWORD size, LPSTR buf);
DWORD WINAPI win_GetTempFileNameA(LPCSTR path, LPCSTR prefix, UINT unique, LPSTR buf);

/* ========== KERNEL32 环境和命令行 ========== */

LPSTR WINAPI win_GetCommandLineA(void);
LPWSTR WINAPI win_GetCommandLineW(void);
BOOL WINAPI win_GetComputerNameA(LPSTR buf, LPDWORD size);
BOOL WINAPI win_GetComputerNameW(LPWSTR buf, LPDWORD size);
BOOL WINAPI win_GetUserNameA(LPSTR buf, LPDWORD size);
BOOL WINAPI win_GetUserNameW(LPWSTR buf, LPDWORD size);
void WINAPI win_GetStartupInfoA(LPSTARTUPINFOA info);
void WINAPI win_GetStartupInfoW(LPSTARTUPINFOW info);

/* ========== KERNEL32 内存查询 ========== */

SIZE_T WINAPI win_VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION info, SIZE_T len);
BOOL WINAPI win_VirtualProtect(LPVOID addr, SIZE_T size, DWORD new_prot, LPDWORD old_prot);

/* ========== KERNEL32 文件时间 ========== */

BOOL WINAPI win_FileTimeToLocalFileTime(const FILETIME *ft, LPFILETIME lft);
BOOL WINAPI win_LocalFileTimeToFileTime(const FILETIME *lft, LPFILETIME ft);
void WINAPI win_GetSystemTimeAsFileTime(LPFILETIME ft);

/* ========== KERNEL32 环境变量 ========== */

DWORD WINAPI win_GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size);
BOOL WINAPI win_SetEnvironmentVariableA(LPCSTR name, LPCSTR value);

/* ========== KERNEL32 动态分配 ========== */

HGLOBAL WINAPI win_GlobalAlloc(UINT flags, size_t size);
HGLOBAL WINAPI win_GlobalFree(HGLOBAL mem);
size_t WINAPI win_GlobalSize(HGLOBAL mem);
LPVOID WINAPI win_GlobalLock(HGLOBAL mem);
BOOL WINAPI win_GlobalUnlock(HGLOBAL mem);
HGLOBAL WINAPI win_LocalAlloc(UINT flags, size_t size);
HGLOBAL WINAPI win_LocalFree(HGLOBAL mem);

/* ========== KERNEL32 其他 ========== */

DWORD WINAPI win_GetVersion(void);
void WINAPI win_OutputDebugStringA(LPCSTR str);
BOOL WINAPI win_IsDebuggerPresent(void);
void WINAPI win_DebugBreak(void);

/* ========== SEH 异常处理函数 ========== */

void WINAPI win_RaiseException(DWORD code, DWORD flags, DWORD count, ULONG_PTR *args);
PVOID WINAPI win_AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
ULONG WINAPI win_RemoveVectoredExceptionHandler(PVOID handle);
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI win_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);
LONG WINAPI win_UnhandledExceptionFilter(PEXCEPTION_POINTERS info);
void WINAPI win_RaiseFailFastException(PEXCEPTION_RECORD record, PVOID context, DWORD flags);

/* ========== TLS 线程局部存储函数 ========== */

DWORD WINAPI win_TlsAlloc(void);
BOOL WINAPI win_TlsFree(DWORD index);
LPVOID WINAPI win_TlsGetValue(DWORD index);
BOOL WINAPI win_TlsSetValue(DWORD index, LPVOID value);

/* ========== USER32 窗口管理函数 ========== */

HWND WINAPI win_CreateWindowExA(DWORD exstyle, LPCSTR classname, LPCSTR winname,
                                 DWORD style, int x, int y, int w, int h,
                                 HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
HWND WINAPI win_CreateWindowExW(DWORD exstyle, LPCWSTR classname, LPCWSTR winname,
                                 DWORD style, int x, int y, int w, int h,
                                 HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
BOOL WINAPI win_DestroyWindow(HWND hwnd);
BOOL WINAPI win_ShowWindow(HWND hwnd, int cmd);
BOOL WINAPI win_UpdateWindow(HWND hwnd);
BOOL WINAPI win_MoveWindow(HWND hwnd, int x, int y, int w, int h, BOOL repaint);
BOOL WINAPI win_GetWindowRect(HWND hwnd, LPRECT rect);
BOOL WINAPI win_GetClientRect(HWND hwnd, LPRECT rect);
BOOL WINAPI win_SetWindowTextA(HWND hwnd, LPCSTR text);
int WINAPI win_GetWindowTextA(HWND hwnd, LPSTR buf, int max);
int WINAPI win_GetWindowTextLengthA(HWND hwnd);
HWND WINAPI win_GetParent(HWND hwnd);
HWND WINAPI win_SetParent(HWND hwnd, HWND parent);
LONG WINAPI win_SetWindowLongA(HWND hwnd, int index, LONG newval);
LONG_PTR WINAPI win_SetWindowLongPtrA(HWND hwnd, int index, LONG_PTR newval);
LONG WINAPI win_GetWindowLongA(HWND hwnd, int index);
LONG_PTR WINAPI win_GetWindowLongPtrA(HWND hwnd, int index);
BOOL WINAPI win_SetForegroundWindow(HWND hwnd);
HWND WINAPI win_GetForegroundWindow(void);
BOOL WINAPI win_EnableWindow(HWND hwnd, BOOL enable);
BOOL WINAPI win_IsWindowEnabled(HWND hwnd);
BOOL WINAPI win_IsWindowVisible(HWND hwnd);
BOOL WINAPI win_IsWindow(HWND hwnd);
HWND WINAPI win_GetDesktopWindow(void);
HWND WINAPI win_GetFocus(void);
HWND WINAPI win_SetFocus(HWND hwnd);
HWND WINAPI win_GetActiveWindow(void);

ATOM WINAPI win_RegisterClassA(const WNDCLASSA *wndclass);
ATOM WINAPI win_RegisterClassExA(const WNDCLASSEXA *wndclass);
BOOL WINAPI win_UnregisterClassA(LPCSTR classname, HINSTANCE inst);

BOOL WINAPI win_GetMessageA(LPMSG msg, HWND hwnd, UINT filtermin, UINT filtermax);
BOOL WINAPI win_PeekMessageA(LPMSG msg, HWND hwnd, UINT filtermin, UINT filtermax, UINT remove);
BOOL WINAPI win_TranslateMessage(const MSG *msg);
LRESULT WINAPI win_DispatchMessageA(const MSG *msg);
LRESULT WINAPI win_SendMessageA(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
BOOL WINAPI win_PostMessageA(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
BOOL WINAPI win_PostQuitMessage(int exitcode);
LRESULT WINAPI win_DefWindowProcA(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
LRESULT WINAPI win_CallWindowProcA(WNDPROC proc, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

HDC WINAPI win_BeginPaint(HWND hwnd, LPPAINTSTRUCT ps);
BOOL WINAPI win_EndPaint(HWND hwnd, const PAINTSTRUCT *ps);
BOOL WINAPI win_InvalidateRect(HWND hwnd, const RECT *rect, BOOL erase);
BOOL WINAPI win_ValidateRect(HWND hwnd, const RECT *rect);

int WINAPI win_MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type);

UINT_PTR WINAPI win_SetTimer(HWND hwnd, UINT_PTR idevent, UINT elapse, TIMERPROC callback);
BOOL WINAPI win_KillTimer(HWND hwnd, UINT_PTR idevent);

BOOL WINAPI win_GetCursorPos(LPPOINT pt);
BOOL WINAPI win_SetCursorPos(int x, int y);
HWND WINAPI win_WindowFromPoint(POINT pt);
BOOL WINAPI win_SetCapture(HWND hwnd);
HWND WINAPI win_ReleaseCapture(void);
HWND WINAPI win_GetCapture(void);
SHORT WINAPI win_GetKeyState(int vkey);
SHORT WINAPI win_GetAsyncKeyState(int vkey);
int WINAPI win_GetKeyboardState(BYTE *kbstate);

HICON WINAPI win_LoadIconA(HINSTANCE inst, LPCSTR name);
HCURSOR WINAPI win_LoadCursorA(HINSTANCE inst, LPCSTR name);
HCURSOR WINAPI win_SetCursor(HCURSOR cursor);

HMENU WINAPI win_CreateMenu(void);
HMENU WINAPI win_CreatePopupMenu(void);
BOOL WINAPI win_DestroyMenu(HMENU menu);
BOOL WINAPI win_AppendMenuA(HMENU menu, UINT flags, UINT_PTR id, LPCSTR data);
HMENU WINAPI win_GetMenu(HWND hwnd);
BOOL WINAPI win_SetMenu(HWND hwnd, HMENU menu);

/* ========== GDI32 图形设备接口函数 ========== */

HDC WINAPI win_GetDC(HWND hwnd);
HDC WINAPI win_GetWindowDC(HWND hwnd);
int WINAPI win_ReleaseDC(HWND hwnd, HDC hdc);
HDC WINAPI win_CreateCompatibleDC(HDC hdc);
BOOL WINAPI win_DeleteDC(HDC hdc);

HGDIOBJ WINAPI win_SelectObject(HDC hdc, HGDIOBJ obj);
BOOL WINAPI win_DeleteObject(HGDIOBJ obj);
HGDIOBJ WINAPI win_GetStockObject(int index);

HPEN WINAPI win_CreatePen(int style, int width, COLORREF color);
HBRUSH WINAPI win_CreateSolidBrush(COLORREF color);
HBRUSH WINAPI win_CreateHatchBrush(int style, COLORREF color);

HFONT WINAPI win_CreateFontA(int height, int width, int escapement, int orientation,
                              int weight, DWORD italic, DWORD underline, DWORD strikeout,
                              DWORD charset, DWORD outprecision, DWORD clipprecision,
                              DWORD quality, DWORD pitchfamily, LPCSTR face);
HFONT WINAPI win_CreateFontIndirectA(const LOGFONTA *lplf);

HBITMAP WINAPI win_CreateBitmap(int width, int height, UINT planes, UINT bpp, const void *data);
HBITMAP WINAPI win_CreateCompatibleBitmap(HDC hdc, int width, int height);

BOOL WINAPI win_MoveToEx(HDC hdc, int x, int y, LPPOINT pt);
BOOL WINAPI win_LineTo(HDC hdc, int x, int y);
BOOL WINAPI win_Rectangle(HDC hdc, int x1, int y1, int x2, int y2);
BOOL WINAPI win_Ellipse(HDC hdc, int x1, int y1, int x2, int y2);
int WINAPI win_FillRect(HDC hdc, const RECT *rect, HBRUSH brush);

BOOL WINAPI win_BitBlt(HDC hdcdest, int xdest, int ydest, int width, int height,
                        HDC hdcsrc, int xsrc, int ysrc, DWORD rop);
BOOL WINAPI win_StretchBlt(HDC hdcdest, int xdest, int ydest, int widthdest, int heightdest,
                            HDC hdcsrc, int xsrc, int ysrc, int widthsrc, int heightsrc, DWORD rop);

int WINAPI win_TextOutA(HDC hdc, int x, int y, LPCSTR str, int count);
BOOL WINAPI win_GetTextMetricsA(HDC hdc, LPTEXTMETRICA lptm);
BOOL WINAPI win_GetTextExtentPoint32A(HDC hdc, LPCSTR str, int count, LPSIZE size);

COLORREF WINAPI win_SetTextColor(HDC hdc, COLORREF color);
COLORREF WINAPI win_GetTextColor(HDC hdc);
COLORREF WINAPI win_SetBkColor(HDC hdc, COLORREF color);
int WINAPI win_SetBkMode(HDC hdc, int mode);

HRGN WINAPI win_CreateRectRgn(int x1, int y1, int x2, int y2);
int WINAPI win_SelectClipRgn(HDC hdc, HRGN rgn);

int WINAPI win_GetDeviceCaps(HDC hdc, int index);

/* ========== ADVAPI32 注册表函数 ========== */

LONG WINAPI win_RegOpenKeyA(HKEY key, LPCSTR subkey, HKEY *result);
LONG WINAPI win_RegOpenKeyExA(HKEY key, LPCSTR subkey, DWORD opts, REGSAM access, HKEY *result);
LONG WINAPI win_RegCloseKey(HKEY key);
LONG WINAPI win_RegCreateKeyA(HKEY key, LPCSTR subkey, HKEY *result);
LONG WINAPI win_RegCreateKeyExA(HKEY key, LPCSTR subkey, DWORD reserved, LPSTR class, DWORD opts,
                                 REGSAM access, LPSECURITY_ATTRIBUTES security, HKEY *result, LPDWORD disposition);
LONG WINAPI win_RegDeleteKeyA(HKEY key, LPCSTR subkey);
LONG WINAPI win_RegDeleteValueA(HKEY key, LPCSTR value);
LONG WINAPI win_RegEnumKeyA(HKEY key, DWORD index, LPSTR name, DWORD size);
LONG WINAPI win_RegEnumValueA(HKEY key, DWORD index, LPSTR name, LPDWORD namesize, LPDWORD reserved,
                               LPDWORD type, LPBYTE data, LPDWORD datasize);
LONG WINAPI win_RegQueryValueExA(HKEY key, LPCSTR value, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size);
LONG WINAPI win_RegSetValueExA(HKEY key, LPCSTR value, DWORD reserved, DWORD type, const BYTE *data, DWORD len);

/* ========== ADVAPI32 服务函数 ========== */

SC_HANDLE WINAPI win_OpenSCManagerA(LPCSTR machine, LPCSTR database, DWORD access);
SC_HANDLE WINAPI win_OpenServiceA(SC_HANDLE manager, LPCSTR name, DWORD access);
BOOL WINAPI win_CloseServiceHandle(SC_HANDLE handle);
BOOL WINAPI win_DeleteService(SC_HANDLE service);
SC_HANDLE WINAPI win_CreateServiceA(SC_HANDLE manager, LPCSTR name, LPCSTR display, DWORD access,
                                     DWORD type, DWORD start, DWORD error, LPCSTR path,
                                     LPCSTR group, LPDWORD tag, LPCSTR deps, LPCSTR account, LPCSTR password);
BOOL WINAPI win_StartServiceA(SC_HANDLE service, DWORD count, LPCSTR *args);
BOOL WINAPI win_ControlService(SC_HANDLE service, DWORD ctrl, LPSERVICE_STATUS status);
BOOL WINAPI win_QueryServiceStatus(SC_HANDLE service, LPSERVICE_STATUS status);

/* ========== ADVAPI32 安全函数 ========== */

typedef void *PSECURITY_DESCRIPTOR;
typedef void *PSID;
typedef void *PACL;

BOOL WINAPI win_InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR desc, DWORD revision);
BOOL WINAPI win_IsValidSecurityDescriptor(PSECURITY_DESCRIPTOR desc);
BOOL WINAPI win_IsValidSid(void *sid);
BOOL WINAPI win_IsValidAcl(void *acl);
BOOL WINAPI win_AdjustTokenPrivileges(HANDLE token, BOOL disable, void *privs,
                                       DWORD len, void *oldprivs, LPDWORD oldlen);
BOOL WINAPI win_LookupPrivilegeValueA(LPCSTR system, LPCSTR name, void *luid);

/* ========== ADVAPI32 加密函数 ========== */

BOOL WINAPI win_CryptAcquireContextA(HCRYPTPROV *prov, LPCSTR container, LPCSTR provider, DWORD type, DWORD flags);
BOOL WINAPI win_CryptReleaseContext(HCRYPTPROV prov, DWORD flags);
BOOL WINAPI win_CryptGenKey(HCRYPTPROV prov, ALG_ID algid, DWORD flags, HCRYPTKEY *key);
BOOL WINAPI win_CryptDestroyKey(HCRYPTKEY key);
BOOL WINAPI win_CryptCreateHash(HCRYPTPROV prov, ALG_ID algid, HCRYPTKEY key, DWORD flags, HCRYPTHASH *hash);
BOOL WINAPI win_CryptDestroyHash(HCRYPTHASH hash);
BOOL WINAPI win_CryptHashData(HCRYPTHASH hash, const BYTE *data, DWORD len, DWORD flags);
BOOL WINAPI win_CryptEncrypt(HCRYPTKEY key, HCRYPTHASH hash, BOOL final, DWORD flags, BYTE *data, DWORD *len, DWORD buflen);
BOOL WINAPI win_CryptDecrypt(HCRYPTKEY key, HCRYPTHASH hash, BOOL final, DWORD flags, BYTE *data, DWORD *len);

/* ========== SHELL32 Shell 操作函数 ========== */

BOOL WINAPI win_ShellExecuteA(HWND hwnd, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, int show);
HRESULT WINAPI win_SHGetFolderPathA(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPSTR path);
BOOL WINAPI win_SHGetFileInfoA(LPCSTR path, DWORD attr, void *info, UINT size, UINT flags);
BOOL WINAPI win_Shell_NotifyIconA(DWORD msg, void *data);

/* ========== COMDLG32 公共对话框函数 ========== */

BOOL WINAPI win_GetOpenFileNameA(void *info);
BOOL WINAPI win_GetSaveFileNameA(void *info);
DWORD WINAPI win_CommDlgExtendedError(void);
BOOL WINAPI win_ChooseColorA(void *info);
BOOL WINAPI win_ChooseFontA(void *info);
BOOL WINAPI win_PrintDlgA(void *info);
BOOL WINAPI win_PageSetupDlgA(void *info);

/* ========== OLE32 COM/OLE 函数 ========== */

HRESULT WINAPI win_CoInitialize(LPVOID reserved);
void WINAPI win_CoUninitialize(void);
HRESULT WINAPI win_CoInitializeEx(LPVOID reserved, DWORD coinit);
HRESULT WINAPI win_CoCreateInstance(REFCLSID rclsid, void *outer, DWORD clsctx, REFIID riid, LPVOID *ppv);
HRESULT WINAPI win_CoGetClassObject(REFCLSID rclsid, DWORD clsctx, void *server, REFIID riid, LPVOID *ppv);

LPVOID WINAPI win_CoTaskMemAlloc(size_t size);
LPVOID WINAPI win_CoTaskMemRealloc(LPVOID mem, size_t size);
void WINAPI win_CoTaskMemFree(LPVOID mem);

BSTR WINAPI win_SysAllocString(const OLECHAR *str);
void WINAPI win_SysFreeString(BSTR bstr);
UINT WINAPI win_SysStringLen(BSTR bstr);
BSTR WINAPI win_SysAllocStringLen(const OLECHAR *str, UINT len);

HRESULT WINAPI win_StringFromCLSID(REFCLSID rclsid, LPOLESTR *str);
HRESULT WINAPI win_CLSIDFromString(LPCOLESTR str, CLSID *clsid);

HRESULT WINAPI win_CoRegisterClassObject(REFCLSID rclsid, void *obj, DWORD clsctx, DWORD flags, LPDWORD reg);
HRESULT WINAPI win_CoRevokeClassObject(DWORD reg);

/* ========== WINSOCK2 Definitions ========== */

#define WINSOCK_VERSION     0x0202

#define WSAEINTR            10004
#define WSAEBADF            10009
#define WSAEACCES           10013
#define WSAEFAULT           10014
#define WSAEINVAL           10022
#define WSAEMFILE           10024
#define WSAEWOULDBLOCK      10035
#define WSAEINPROGRESS      10036
#define WSAEALREADY         10037
#define WSAENOTSOCK         10038
#define WSAEDESTADDRREQ     10039
#define WSAEMSGSIZE         10040
#define WSAEPROTOTYPE       10041
#define WSAENOPROTOOPT      10042
#define WSAEPROTONOSUPPORT  10043
#define WSAESOCKTNOSUPPORT  10044
#define WSAEOPNOTSUPP       10045
#define WSAEPFNOSUPPORT     10046
#define WSAEAFNOSUPPORT     10047
#define WSAEADDRINUSE       10048
#define WSAEADDRNOTAVAIL    10049
#define WSAENETDOWN         10050
#define WSAENETUNREACH      10051
#define WSAENETRESET        10052
#define WSAECONNABORTED     10053
#define WSAECONNRESET       10054
#define WSAENOBUFS          10055
#define WSAEISCONN          10056
#define WSAENOTCONN         10057
#define WSAESHUTDOWN        10058
#define WSAETIMEDOUT        10060
#define WSAECONNREFUSED     10061
#define WSAEHOSTDOWN        10064
#define WSAEHOSTUNREACH     10065
#define WSASYSCALLFAILURE   10107
#define WSAVERNOTSUPPORTED  10092
#define WSAHOST_NOT_FOUND   11001
#define WSATRY_AGAIN        11002
#define WSANO_RECOVERY      11003
#define WSANO_DATA          11004
#define WSAEINVALPROCTABLE  11103
#define WSAENOMEM           10055

#define WIN_AF_UNSPEC       0
#define WIN_AF_INET         2
#define WIN_AF_INET6        23
#define WIN_AF_BTH          32

#define WIN_SOCK_STREAM     1
#define WIN_SOCK_DGRAM      2
#define WIN_SOCK_RAW        3
#define WIN_SOCK_RDM        4
#define WIN_SOCK_SEQPACKET  5

#define WIN_IPPROTO_IP      0
#define WIN_IPPROTO_ICMP    1
#define WIN_IPPROTO_IGMP    2
#define WIN_IPPROTO_TCP     6
#define WIN_IPPROTO_UDP     17
#define WIN_IPPROTO_IPV6    41
#define WIN_IPPROTO_ICMPV6  58
#define WIN_IPPROTO_RAW     255

#define WSA_FLAG_OVERLAPPED         0x01
#define WSA_FLAG_MULTIPOINT_C_ROOT  0x02
#define WSA_FLAG_MULTIPOINT_C_LEAF  0x04
#define WSA_FLAG_MULTIPOINT_D_ROOT  0x08
#define WSA_FLAG_MULTIPOINT_D_LEAF  0x10

#define WIN_SD_RECEIVE   0
#define WIN_SD_SEND      1
#define WIN_SD_BOTH      2

#define WIN_SOL_SOCKET    0xFFFF
#define WIN_SO_DEBUG      0x0001
#define WIN_SO_ACCEPTCONN 0x0002
#define WIN_SO_REUSEADDR  0x0004
#define WIN_SO_KEEPALIVE  0x0008
#define WIN_SO_DONTROUTE  0x0010
#define WIN_SO_BROADCAST  0x0020
#define WIN_SO_USELOOPBACK 0x0040
#define WIN_SO_LINGER     0x0080
#define WIN_SO_OOBINLINE  0x0100
#define WIN_SO_SNDBUF     0x1001
#define WIN_SO_RCVBUF     0x1002
#define WIN_SO_SNDLOWAT   0x1003
#define WIN_SO_RCVLOWAT   0x1004
#define WIN_SO_SNDTIMEO   0x1005
#define WIN_SO_RCVTIMEO   0x1006
#define WIN_SO_ERROR      0x1007
#define WIN_SO_TYPE       0x1008

#define WIN_FD_SETSIZE    64
#define SOCKET_ERROR      (-1)

#define FIONBIO           0x8004667E
#define FIONREAD          0x4004667F
#define SIOCATMARK        0x40047307

typedef struct _win_fd_set {
    uint32_t fd_count;
    SOCKET fd_array[WIN_FD_SETSIZE];
} win_fd_set;

typedef win_fd_set *LPFD_SET;

typedef struct _win_timeval {
    LONG tv_sec;
    LONG tv_usec;
} win_timeval;

typedef win_timeval *LPTIMEVAL;

typedef struct _WSADATA {
    WORD wVersion;
    WORD wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char *lpVendorInfo;
} WSADATA, *LPWSADATA;

typedef struct _win_sockaddr {
    uint16_t sa_family;
    char sa_data[14];
} win_sockaddr;

typedef struct _win_sockaddr_in {
    int16_t sin_family;
    uint16_t sin_port;
    struct {
        uint32_t s_addr;
    } sin_addr;
    char sin_zero[8];
} win_sockaddr_in;

typedef struct _win_sockaddr_in6 {
    int16_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    struct {
        uint8_t s6_addr[16];
    } sin6_addr;
    uint32_t sin6_scope_id;
} win_sockaddr_in6;

typedef struct _win_addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    size_t ai_addrlen;
    char *ai_canonname;
    struct _win_addrinfo *ai_next;
    win_sockaddr *ai_addr;
} win_addrinfo;

typedef win_addrinfo *LPADDRINFO;

#define WIN_AI_PASSIVE     0x0001
#define WIN_AI_CANONNAME   0x0002
#define WIN_AI_NUMERICHOST 0x0004
#define WIN_AI_ADDRCONFIG  0x0400

typedef struct _win_hostent {
    char *h_name;
    char **h_aliases;
    int16_t h_addrtype;
    int16_t h_length;
    char **h_addr_list;
} win_hostent;

typedef win_hostent *LPHOSTENT;

typedef struct _win_protoent {
    char *p_name;
    char **p_aliases;
    int16_t p_proto;
} win_protoent;

typedef win_protoent *LPPROTOENT;

typedef struct _win_servent {
    char *s_name;
    char **s_aliases;
    int16_t s_port;
    char *s_proto;
} win_servent;

typedef struct _win_linger {
    uint16_t l_onoff;
    uint16_t l_linger;
} win_linger;

typedef struct _WSABUF {
    ULONG len;
    CHAR *buf;
} WSABUF, *LPWSABUF;

typedef OVERLAPPED WSAOVERLAPPED, *LPWSAOVERLAPPED;

typedef struct _WSAPROTOCOLCHAIN {
    int ChainLen;
    DWORD ChainEntries[7];
} WSAPROTOCOLCHAIN;

typedef struct _WSAPROTOCOL_INFOA {
    DWORD dwServiceFlags1;
    DWORD dwServiceFlags2;
    DWORD dwServiceFlags3;
    DWORD dwServiceFlags4;
    DWORD dwProviderFlags;
    GUID ProviderId;
    DWORD dwCatalogEntryId;
    WSAPROTOCOLCHAIN ProtocolChain;
    int iVersion;
    int iAddressFamily;
    int iMaxSockAddr;
    int iMinSockAddr;
    int iSocketType;
    int iProtocol;
    int iProtocolMaxOffset;
    int iNetworkByteOrder;
    int iSecurityScheme;
    DWORD dwMessageSize;
    DWORD dwProviderReserved;
    CHAR szProtocol[256];
} WSAPROTOCOL_INFOA;

typedef struct _WSANETWORKEVENTS {
    LONG lNetworkEvents;
    int iErrorCode[10];
} WSANETWORKEVENTS;

#define WIN_FD_READ        0x00000001
#define WIN_FD_WRITE       0x00000002
#define WIN_FD_OOB         0x00000004
#define WIN_FD_ACCEPT      0x00000008
#define WIN_FD_CONNECT     0x00000010
#define WIN_FD_CLOSE       0x00000020

/* ========== IPHLPAPI Definitions ========== */

#define MAX_ADAPTER_NAME_LENGTH        256
#define MAX_ADAPTER_DESCRIPTION_LENGTH 128
#define MAX_ADAPTER_ADDRESS_LENGTH     8

#define MIB_IF_TYPE_ETHERNET     6
#define MIB_IF_TYPE_LOOPBACK     24
#define IF_TYPE_ETHERNET_CSMACD  6

typedef struct _IP_ADAPTER_INFO {
    struct _IP_ADAPTER_INFO *Next;
    DWORD ComboIndex;
    char AdapterName[MAX_ADAPTER_NAME_LENGTH + 4];
    char Description[MAX_ADAPTER_DESCRIPTION_LENGTH + 4];
    UINT AddressLength;
    BYTE Address[MAX_ADAPTER_ADDRESS_LENGTH];
    DWORD Index;
    UINT Type;
    UINT DhcpEnabled;
    void *CurrentIpAddress;
    struct {
        char IpAddress[16];
        char IpMask[16];
        struct { char *Next; } *Next;
    } IpAddressList;
    struct {
        char Gateway[16];
        struct { char *Next; } *Next;
    } GatewayList;
    struct {
        char PrimaryWinsServer[16];
        char SecondaryWinsServer[16];
        struct { char *Next; } *Next;
    } PrimaryWinsServer;
    struct {
        char DhcpServer[16];
        struct { char *Next; } *Next;
    } DhcpServer;
    DWORD Flags;
    DWORD Mtu;
    DWORD Speed;
} IP_ADAPTER_INFO, *PIP_ADAPTER_INFO;

typedef struct _IP_ADAPTER_ADDRESSES {
    ULONG Length;
    DWORD IfIndex;
    struct _IP_ADAPTER_ADDRESSES *Next;
    char *AdapterName;
    void *FirstUnicastAddress;
    void *FirstAnycastAddress;
    void *FirstMulticastAddress;
    void *FirstDnsServerAddress;
    WCHAR *DnsSuffix;
    WCHAR *Description;
    WCHAR *FriendlyName;
    BYTE PhysicalAddress[MAX_ADAPTER_ADDRESS_LENGTH];
    ULONG PhysicalAddressLength;
    ULONG Flags;
    ULONG Mtu;
    DWORD IfType;
    DWORD OperStatus;
} IP_ADAPTER_ADDRESSES, *PIP_ADAPTER_ADDRESSES;

typedef struct _MIB_IPADDRROW {
    DWORD dwAddr;
    DWORD dwIndex;
    DWORD dwMask;
    DWORD dwBCastAddr;
    DWORD dwReasmSize;
    unsigned short unused1;
    unsigned short wType;
} MIB_IPADDRROW;

typedef struct _MIB_IPADDRTABLE {
    DWORD dwNumEntries;
    MIB_IPADDRROW table[1];
} MIB_IPADDRTABLE, *PMIB_IPADDRTABLE;

/* ========== DNSAPI Definitions ========== */

#define DNS_TYPE_A       0x0001
#define DNS_TYPE_NS      0x0002
#define DNS_TYPE_CNAME   0x0005
#define DNS_TYPE_MX      0x000F
#define DNS_TYPE_AAAA    0x001C

#define DNS_QUERY_STANDARD           0x00000000
#define DNS_QUERY_ACCEPT_TRUNCATED   0x00000001
#define DNS_QUERY_USE_TCP_ONLY       0x00000002
#define DNS_QUERY_NO_RECURSION       0x00000004
#define DNS_QUERY_BYPASS_CACHE       0x00000008
#define DNS_QUERY_NO_WIRE_QUERY      0x00000010
#define DNS_QUERY_WIRE_ONLY          0x00000020
#define DNS_QUERY_RETURN_MESSAGE     0x00000100

typedef struct _DNS_A_DATA {
    DWORD IpAddress;
} DNS_A_DATA, *PDNS_A_DATA;

typedef struct _DNS_AAAA_DATA {
    BYTE Ip6Address[16];
} DNS_AAAA_DATA;

typedef struct _DNS_RECORDA {
    struct _DNS_RECORDA *pNext;
    char *pName;
    WORD wType;
    WORD wDataLength;
    DWORD dwTtl;
    DWORD dwReserved;
    union {
        DNS_A_DATA A;
        DNS_AAAA_DATA AAAA;
    } Data;
} DNS_RECORDA, *PDNS_RECORDA;

#define DNS_ERROR_RCODE_NAME_ERROR  9003
#define DNS_ERROR_RCODE_NO_ERROR    0

/* ========== WININET Definitions ========== */

typedef uint64_t HINTERNET;
typedef uint16_t INTERNET_PORT;

#define INTERNET_INVALID_PORT_NUMBER  0
#define INTERNET_DEFAULT_FTP_PORT     21
#define INTERNET_DEFAULT_HTTP_PORT    80
#define INTERNET_DEFAULT_HTTPS_PORT   443

#define INTERNET_FLAG_RELOAD          0x80000000
#define INTERNET_FLAG_RAW_DATA        0x40000000
#define INTERNET_FLAG_EXISTING_CONNECT 0x20000000
#define INTERNET_FLAG_ASYNC           0x10000000
#define INTERNET_FLAG_PASSIVE         0x08000000
#define INTERNET_FLAG_NO_CACHE_WRITE  0x04000000
#define INTERNET_FLAG_SECURE          0x00800000
#define INTERNET_FLAG_KEEP_CONNECTION 0x00400000
#define INTERNET_FLAG_NO_AUTO_REDIRECT 0x00200000
#define INTERNET_FLAG_NO_UI           0x00000200

#define INTERNET_OPEN_TYPE_PRECONFIG  0
#define INTERNET_OPEN_TYPE_DIRECT     1
#define INTERNET_OPEN_TYPE_PROXY      3

#define INTERNET_SERVICE_FTP          1
#define INTERNET_SERVICE_HTTP         3

#define HTTP_STATUS_OK                 200
#define HTTP_STATUS_REDIRECT           302
#define HTTP_STATUS_NOT_FOUND          404

/* ========== DLL Export Lookup Functions (Networking) ========== */

void *ole32_get_proc(const char *name);
void *shell32_get_proc(const char *name);
void *comdlg32_get_proc(const char *name);
void *ws2_32_get_proc(const char *name);
void *iphlpapi_get_proc(const char *name);
void *dnsapi_get_proc(const char *name);
void *wininet_get_proc(const char *name);
void *winhttp_get_proc(const char *name);
void *mswsock_get_proc(const char *name);

/* ========== WS2_32 Winsock2 Functions ========== */

int WINAPI win_WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData);
int WINAPI win_WSACleanup(void);
int WINAPI win_WSAGetLastError(void);
void WINAPI win_WSASetLastError(int iError);
SOCKET WINAPI win_socket(int af, int type, int protocol);
int WINAPI win_closesocket(SOCKET s);
int WINAPI win_connect(SOCKET s, const win_sockaddr *name, int namelen);
int WINAPI win_send(SOCKET s, const char *buf, int len, int flags);
int WINAPI win_recv(SOCKET s, char *buf, int len, int flags);
int WINAPI win_sendto(SOCKET s, const char *buf, int len, int flags, const win_sockaddr *to, int tolen);
int WINAPI win_recvfrom(SOCKET s, char *buf, int len, int flags, win_sockaddr *from, int *fromlen);
int WINAPI win_bind(SOCKET s, const win_sockaddr *addr, int namelen);
int WINAPI win_listen(SOCKET s, int backlog);
SOCKET WINAPI win_accept(SOCKET s, win_sockaddr *addr, int *addrlen);
int WINAPI win_select(int nfds, win_fd_set *readfds, win_fd_set *writefds, win_fd_set *exceptfds, const win_timeval *timeout);
int WINAPI win_shutdown(SOCKET s, int how);
int WINAPI win_getsockname(SOCKET s, win_sockaddr *name, int *namelen);
int WINAPI win_getpeername(SOCKET s, win_sockaddr *name, int *namelen);
int WINAPI win_getsockopt(SOCKET s, int level, int optname, char *optval, int *optlen);
int WINAPI win_setsockopt(SOCKET s, int level, int optname, const char *optval, int optlen);
int WINAPI win_ioctlsocket(SOCKET s, LONG cmd, ULONG *argp);
SOCKET WINAPI win_WSASocketA(int af, int type, int protocol, LPVOID lpProtocolInfo, DWORD g, DWORD dwFlags);
SOCKET WINAPI win_WSASocketW(int af, int type, int protocol, LPVOID lpProtocolInfo, DWORD g, DWORD dwFlags);
int WINAPI win_WSASend(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped, void *lpCompletionRoutine);
int WINAPI win_WSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped, void *lpCompletionRoutine);
int WINAPI win_WSAAsyncSelect(SOCKET s, HWND hWnd, unsigned int wMsg, LONG lEvent);
int WINAPI win_WSAEventSelect(SOCKET s, HANDLE hEventObject, LONG lNetworkEvents);
int WINAPI win_WSAEnumNetworkEvents(SOCKET s, HANDLE hEventObject, WSANETWORKEVENTS *lpNetworkEvents);
BOOL WINAPI win_WSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags);
int WINAPI win_getaddrinfo(const char *nodename, const char *servname, const win_addrinfo *hints, win_addrinfo **result);
void WINAPI win_freeaddrinfo(win_addrinfo *pAddrInfo);
win_hostent *WINAPI win_gethostbyname(const char *name);
int WINAPI win_gethostname(char *name, int namelen);
win_protoent *WINAPI win_getprotobyname(const char *name);
win_servent *WINAPI win_getservbyname(const char *name, const char *proto);
uint32_t WINAPI win_ntohl(uint32_t netlong);
uint16_t WINAPI win_ntohs(uint16_t netshort);
uint32_t WINAPI win_htonl(uint32_t hostlong);
uint16_t WINAPI win_htons(uint16_t hostshort);
uint32_t WINAPI win_inet_addr(const char *cp);
char *WINAPI win_inet_ntoa(uint32_t in_addr);

/* ========== IPHLPAPI Functions ========== */
DWORD WINAPI win_GetAdaptersInfo(IP_ADAPTER_INFO *pAdapterInfo, ULONG *pOutBufLen);
DWORD WINAPI win_GetAdaptersAddresses(ULONG Family, DWORD Flags, void *Reserved, IP_ADAPTER_ADDRESSES *pAdapterAddresses, ULONG *pOutBufLen);
DWORD WINAPI win_GetBestInterface(uint32_t dwDestAddr, DWORD *pdwBestIfIndex);
DWORD WINAPI win_GetIpAddrTable(MIB_IPADDRTABLE *pIpAddrTable, ULONG *pdwSize, BOOL bOrder);
DWORD WINAPI win_GetIfTable(void *pIfTable, ULONG *pdwSize, BOOL bOrder);
DWORD WINAPI win_GetNumberOfInterfaces(DWORD *lpdwNumIf);
DWORD WINAPI win_NotifyAddrChange(HANDLE *Handle, LPOVERLAPPED overlapped);
DWORD WINAPI win_NotifyRouteChange(HANDLE *Handle, LPOVERLAPPED overlapped);

/* ========== DNSAPI Functions ========== */
int WINAPI win_DnsQuery_A(const char *pszName, WORD wType, DWORD Options, void *pExtra, DNS_RECORDA **ppQueryResults, void *pReserved);
int WINAPI win_DnsQuery_W(const WCHAR *pszName, WORD wType, DWORD Options, void *pExtra, DNS_RECORDA **ppQueryResults, void *pReserved);
void WINAPI win_DnsRecordListFree(DNS_RECORDA *pRecordList, DWORD FreeType);

/* ========== WININET Functions ========== */
HINTERNET WINAPI win_InternetOpenA(const char *lpszAgent, DWORD dwAccessType, const char *lpszProxy, const char *lpszProxyBypass, DWORD dwFlags);
HINTERNET WINAPI win_InternetConnectA(HINTERNET hInternet, const char *lpszServerName, INTERNET_PORT nServerPort, const char *lpszUserName, const char *lpszPassword, DWORD dwService, DWORD dwFlags, ULONG_PTR dwContext);
BOOL WINAPI win_InternetCloseHandle(HINTERNET hInternet);
HINTERNET WINAPI win_HttpOpenRequestA(HINTERNET hConnect, const char *lpszVerb, const char *lpszObjectName, const char *lpszVersion, const char *lpszReferrer, const char **lplpszAcceptTypes, DWORD dwFlags, ULONG_PTR dwContext);
BOOL WINAPI win_HttpSendRequestA(HINTERNET hRequest, const char *lpszHeaders, DWORD dwHeadersLength, LPVOID lpOptional, DWORD dwOptionalLength);
BOOL WINAPI win_InternetReadFile(HINTERNET hFile, LPVOID lpBuffer, DWORD dwNumberOfBytesToRead, LPDWORD lpdwNumberOfBytesRead);
BOOL WINAPI win_InternetSetOptionA(HINTERNET hInternet, DWORD dwOption, LPVOID lpBuffer, DWORD dwBufferLength);
BOOL WINAPI win_InternetGetLastResponseInfoA(LPDWORD lpdwError, LPSTR lpszBuffer, LPDWORD lpdwBufferLength);
BOOL WINAPI win_HttpQueryInfoA(HINTERNET hRequest, DWORD dwInfoLevel, LPVOID lpBuffer, LPDWORD lpdwBufferLength, LPDWORD lpdwIndex);
BOOL WINAPI win_HttpAddRequestHeadersA(HINTERNET hRequest, LPCSTR lpszHeaders, DWORD dwHeadersLength, DWORD dwModifiers);
DWORD WINAPI win_InternetErrorDlg(HWND hWndParent, HINTERNET hInternet, DWORD dwError, DWORD dwFlags, LPVOID lppvData);

/* ========== WINHTTP Functions ========== */
HINTERNET WINAPI win_WinHttpOpen(LPCWSTR pszAgentW, DWORD dwAccessType, LPCWSTR pszProxyW, LPCWSTR pszProxyBypassW, DWORD dwFlags);
HINTERNET WINAPI win_WinHttpConnect(HINTERNET hSession, LPCWSTR pszServerNameW, INTERNET_PORT nServerPort, DWORD dwReserved);
HINTERNET WINAPI win_WinHttpOpenRequest(HINTERNET hConnect, LPCWSTR pszVerbW, LPCWSTR pszObjectNameW, LPCWSTR pszVersionW, LPCWSTR pszReferrerW, LPCWSTR *ppszAcceptTypesW, DWORD dwFlags);
BOOL WINAPI win_WinHttpSendRequest(HINTERNET hRequest, LPCWSTR lpszHeadersW, DWORD dwHeadersLength, LPVOID lpOptional, DWORD dwOptionalLength, DWORD dwTotalLength, ULONG_PTR dwContext);
BOOL WINAPI win_WinHttpReceiveResponse(HINTERNET hRequest, LPVOID lpReserved);
BOOL WINAPI win_WinHttpReadData(HINTERNET hRequest, LPVOID lpBuffer, DWORD dwNumberOfBytesToRead, LPDWORD lpdwNumberOfBytesRead);
BOOL WINAPI win_WinHttpCloseHandle(HINTERNET hInternet);
BOOL WINAPI win_WinHttpSetOption(HINTERNET hInternet, DWORD dwOption, LPVOID lpBuffer, DWORD dwBufferLength);
BOOL WINAPI win_WinHttpQueryHeaders(HINTERNET hRequest, DWORD dwInfoLevel, LPCWSTR pszNameW, LPVOID lpBuffer, LPDWORD lpdwBufferLength, LPDWORD lpdwIndex);
BOOL WINAPI win_WinHttpQueryDataAvailable(HINTERNET hRequest, LPDWORD lpdwNumberOfBytesAvailable);

/* ========== MSWSOCK Functions ========== */
int WINAPI win_WSARecvMsg(SOCKET s, void *lpMsg, DWORD *lpdwNumberOfBytesRecvd, LPWSAOVERLAPPED lpOverlapped, void *lpCompletionRoutine);
BOOL WINAPI win_TransmitFile(SOCKET hSocket, HANDLE hFile, DWORD nNumberOfBytesToWrite, DWORD nNumberOfBytesPerSend, LPOVERLAPPED lpOverlapped, LPVOID lpTransmitBuffers, DWORD dwReserved);
BOOL WINAPI win_AcceptEx(SOCKET sListenSocket, SOCKET sAcceptSocket, PVOID lpOutputBuffer, DWORD dwReceiveDataLength, DWORD dwLocalAddressLength, DWORD dwRemoteAddressLength, LPDWORD lpdwBytesReceived, LPOVERLAPPED lpOverlapped);
void WINAPI win_GetAcceptExSockaddrs(PVOID lpOutputBuffer, DWORD dwReceiveDataLength, DWORD dwLocalAddressLength, DWORD dwRemoteAddressLength, win_sockaddr **LocalSockaddr, LPINT LocalSockaddrLength, win_sockaddr **RemoteSockaddr, LPINT RemoteSockaddrLength);

#endif /* NT_H */
