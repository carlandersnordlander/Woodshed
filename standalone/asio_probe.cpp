// Standalone ASIO diagnostic.
//
// RtAudio reports every ASIOInit failure as the generic "Hardware input or output is not
// present or available" (ASE_NotPresent), because asio.cpp returns ASE_NotPresent whenever the
// driver's init() returns false - for ANY reason - and RtAudio prints its own canned string for
// that code instead of the driver's message. The real explanation is written by the driver into
// ASIODriverInfo::errorMessage, which RtAudio discards.
//
// This tool talks to the ASIO host wrapper directly and prints that message verbatim, so we can
// see what the driver actually objects to. It also retries with a null sysRef to test whether the
// window handle RtAudio passes (GetForegroundWindow()) is what the driver is unhappy about.

#define WIN32_LEAN_AND_MEAN_UNDEFINED_BY_BUILD
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstring>

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

namespace
{

const char* AsioErrorName(ASIOError e)
{
  switch (e)
  {
    case ASE_OK: return "ASE_OK";
    case ASE_SUCCESS: return "ASE_SUCCESS";
    case ASE_NotPresent: return "ASE_NotPresent (driver init() returned false)";
    case ASE_HWMalfunction: return "ASE_HWMalfunction";
    case ASE_InvalidParameter: return "ASE_InvalidParameter";
    case ASE_InvalidMode: return "ASE_InvalidMode";
    case ASE_SPNotAdvancing: return "ASE_SPNotAdvancing";
    case ASE_NoClock: return "ASE_NoClock";
    case ASE_NoMemory: return "ASE_NoMemory";
    default: return "<unknown>";
  }
}

const char* HresultName(HRESULT hr)
{
  if (hr == S_OK) return "S_OK (this thread is now STA)";
  if (hr == S_FALSE) return "S_FALSE (already initialized STA - fine)";
  if (hr == RPC_E_CHANGED_MODE) return "RPC_E_CHANGED_MODE (thread is MTA - ASIO needs STA!)";
  return "<other>";
}

// Runs loadDriver + ASIOInit for one driver and reports everything the driver tells us.
// Returns true if ASIOInit succeeded.
bool TryDriver(AsioDrivers& drivers, char* name, void* sysRef, const char* sysRefLabel)
{
  std::printf("    --- attempt with sysRef = %s (%p) ---\n", sysRefLabel, sysRef);

  if (!drivers.loadDriver(name))
  {
    std::printf("    loadDriver FAILED - COM could not instantiate the driver in this process.\n");
    return false;
  }
  std::printf("    loadDriver OK - the driver DLL loaded into this %d-bit process.\n",
              static_cast<int>(sizeof(void*) * 8));

  ASIODriverInfo info;
  std::memset(&info, 0, sizeof(info));
  info.asioVersion = 2;
  info.sysRef = sysRef;

  const ASIOError result = ASIOInit(&info);
  std::printf("    ASIOInit -> %ld  %s\n", static_cast<long>(result), AsioErrorName(result));
  std::printf("    DRIVER SAYS: \"%s\"   <<<<<< the real message\n", info.errorMessage);

  if (result != ASE_OK)
  {
    drivers.removeCurrentDriver();
    return false;
  }

  std::printf("    driver name    : %s\n", info.name);
  std::printf("    driver version : %ld\n", info.driverVersion);

  long inputs = 0, outputs = 0;
  if (ASIOGetChannels(&inputs, &outputs) == ASE_OK)
    std::printf("    channels       : %ld in, %ld out\n", inputs, outputs);

  ASIOSampleRate rate = 0.0;
  if (ASIOGetSampleRate(&rate) == ASE_OK)
    std::printf("    sample rate    : %.0f Hz\n", static_cast<double>(rate));

  long minSize = 0, maxSize = 0, preferredSize = 0, granularity = 0;
  if (ASIOGetBufferSize(&minSize, &maxSize, &preferredSize, &granularity) == ASE_OK)
    std::printf("    buffer sizes   : min %ld, max %ld, preferred %ld, granularity %ld\n", minSize, maxSize,
                preferredSize, granularity);

  ASIOExit();
  drivers.removeCurrentDriver();
  return true;
}

} // namespace

int main()
{
  std::printf("=== ASIO diagnostic ===\n\n");

  const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::printf("CoInitializeEx      : 0x%08lX  %s\n", static_cast<unsigned long>(hr), HresultName(hr));
  std::printf("process bitness     : %d-bit\n", static_cast<int>(sizeof(void*) * 8));
  std::printf("GetForegroundWindow : %p\n\n", static_cast<void*>(GetForegroundWindow()));

  AsioDrivers drivers;
  const long numDrivers = drivers.asioGetNumDev();
  std::printf("registered ASIO drivers: %ld\n", numDrivers);

  if (numDrivers == 0)
    std::printf("  (nothing under HKLM\\SOFTWARE\\ASIO that this process can see)\n");

  for (long i = 0; i < numDrivers; i++)
  {
    char name[64];
    std::memset(name, 0, sizeof(name));
    if (drivers.asioGetDriverName(static_cast<int>(i), name, sizeof(name)) != 0)
    {
      std::printf("\n[%ld] <could not read driver name>\n", i);
      continue;
    }

    std::printf("\n[%ld] %s\n", i, name);

    // RtAudio passes GetForegroundWindow(); try that first so we reproduce its exact conditions,
    // then fall back to a null sysRef to see whether the window handle is the problem.
    if (TryDriver(drivers, name, GetForegroundWindow(), "GetForegroundWindow() [what RtAudio uses]"))
      continue;

    std::printf("\n");
    TryDriver(drivers, name, nullptr, "nullptr");
  }

  std::printf("\n=== done ===\n");
  if (hr == S_OK)
    CoUninitialize();
  return 0;
}
