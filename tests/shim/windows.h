#pragma once
// Shim kecil supaya header driver yang portable bisa diuji di Linux.
#include <cstdint>
#include <cstddef>
typedef unsigned char  BYTE;
typedef unsigned short UINT16;
typedef unsigned int   UINT;
typedef int            INT;
typedef unsigned long  ULONG;

// SAL annotations -> no-op di luar MSVC
#ifndef _In_
#define _In_
#define _Out_
#define _Inout_
#define _In_opt_
#define _Out_writes_bytes_(x)
#define _Inout_updates_bytes_(x)
#define _Out_writes_bytes_all_(x)
#define _Use_decl_annotations_
#endif
typedef unsigned long long UINT64;
