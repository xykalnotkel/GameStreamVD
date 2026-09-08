/* ProbeE: hanya meng-include Driver.h, tanpa badan apa pun.
   Kalau ini gagal, penyebab tabrakan header ada di Driver.h atau header yang
   di-include-nya (GsVirtual.h / Edid.h / FrameSink.h). */
#include "../driver/DisplayDriver/Driver.h"
#include "../driver/DisplayDriver/Driver.tmh"

extern "C" BOOL WINAPI DllMain(_In_ HINSTANCE, _In_ UINT, _In_opt_ LPVOID) { return TRUE; }

extern "C" DRIVER_INITIALIZE DriverEntry;

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    WDF_DRIVER_CONFIG_INIT(&Config, nullptr);
    return WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
}
