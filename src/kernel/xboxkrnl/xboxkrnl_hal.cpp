/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <cstdlib>

#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

void HalReturnToFirmware_entry(u32 routine) {
  // void
  // IN FIRMWARE_REENTRY  Routine

  // Routine must be 1 'HalRebootRoutine'
  assert_true(routine == 1);

  // There is no dashboard to return to, so hard-exit the way ReXApp::OnClosing
  // does: flush the logs, then std::_Exit. CRT exit() would run the EXE's
  // atexit handlers and static destructors on this guest thread while the
  // other threads keep running. OnShutdown would tear down the runtime this
  // thread runs on, and KernelState::TerminateTitle() never returns to a guest
  // caller, so neither is used.
  REXKRNL_ERROR("Game requested shutdown via HalReturnToFirmware");
  rex::FlushLogging();
  std::_Exit(0);
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__HalReturnToFirmware, rex::kernel::xboxkrnl::HalReturnToFirmware_entry)

REX_EXPORT_STUB(__imp__HalGetCurrentAVPack);
REX_EXPORT_STUB(__imp__HalGpioControl);
REX_EXPORT_STUB(__imp__HalOpenCloseODDTray);
REX_EXPORT_STUB(__imp__HalReadWritePCISpace);
REX_EXPORT_STUB(__imp__HalRegisterPowerDownNotification);
REX_EXPORT_STUB(__imp__HalRegisterSMCNotification);
REX_EXPORT_STUB(__imp__HalSendSMCMessage);
REX_EXPORT_STUB(__imp__HalSetAudioEnable);
REX_EXPORT_STUB(__imp__HalIsExecutingPowerDownDpc);
REX_EXPORT_STUB(__imp__HalGetPowerUpCause);
REX_EXPORT_STUB(__imp__HalRegisterPowerDownCallback);
REX_EXPORT_STUB(__imp__HalRegisterBackgroundModeTransitionCallback);
REX_EXPORT_STUB(__imp__HalClampUnclampOutputDACs);
REX_EXPORT_STUB(__imp__HalPowerDownToBackgroundMode);
REX_EXPORT_STUB(__imp__HalNotifyAddRemoveBackgroundTask);
REX_EXPORT_STUB(__imp__HalCallBackgroundModeNotificationRoutines);
REX_EXPORT_STUB(__imp__HalGetMemoryInformation);
REX_EXPORT_STUB(__imp__HalNotifyBackgroundModeTransitionComplete);
REX_EXPORT_STUB(__imp__HalFinalizePowerLossRecovery);
REX_EXPORT_STUB(__imp__HalSetPowerLossRecovery);
REX_EXPORT_STUB(__imp__HalRegisterXamPowerDownCallback);
REX_EXPORT_STUB(__imp__HalRegisterHdDvdRomNotification);
REX_EXPORT_STUB(__imp__HalGetNotedArgonErrors);
REX_EXPORT_STUB(__imp__HalReadArgonEeprom);
REX_EXPORT_STUB(__imp__HalWriteArgonEeprom);
REX_EXPORT_STUB(__imp__HalConfigureVeDevice);
