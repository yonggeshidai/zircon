// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "syscalls_system_priv.h"

#include <arch/arch_ops.h>
#include <arch/mp.h>
#include <fbl/auto_call.h>
#include <inttypes.h>
#include <kernel/timer.h>
#include <platform.h>
#include <trace.h>
#include <vm/vm_aspace.h>

#include <arch/x86/bootstrap16.h>
#include <arch/x86/acpi.h>
extern "C" {
#include <acpica/acpi.h>
#include <acpica/accommon.h>
#include <acpica/achware.h>
}

#define LOCAL_TRACE 0

namespace {

// This thread performs the work for suspend/resume.  We use a separate thread
// rather than the invoking thread to let us lean on the context switch code
// path to persist all of the usermode thread state that is not saved on a plain
// mode switch.
zx_status_t suspend_thread(void* raw_arg) {
    auto arg = reinterpret_cast<const zx_system_powerctl_arg_t*>(raw_arg);
    uint8_t target_s_state = arg->acpi_transition_s_state.target_s_state;
    uint8_t sleep_type_a = arg->acpi_transition_s_state.sleep_type_a;
    uint8_t sleep_type_b = arg->acpi_transition_s_state.sleep_type_b;

    // Acquire resources for suspend and resume if necessary.
    fbl::RefPtr<VmAspace> temp_aspace;
    x86_realmode_entry_data* bootstrap_data;
    struct x86_realmode_entry_data_registers regs;
    paddr_t bootstrap_ip;
    zx_status_t status;
    status = x86_bootstrap16_acquire(reinterpret_cast<uintptr_t>(_x86_suspend_wakeup),
                                     &temp_aspace,
                                     reinterpret_cast<void**>(&bootstrap_data),
                                     &bootstrap_ip);
    if (status != ZX_OK) {
        return status;
    }
    auto bootstrap_cleanup = fbl::MakeAutoCall([&bootstrap_data]() {
        x86_bootstrap16_release(bootstrap_data);
    });

    // Setup our resume path
    ACPI_TABLE_FACS* facs = nullptr;
    ACPI_STATUS acpi_status = AcpiGetTable((char *)ACPI_SIG_FACS, 1,
                                           reinterpret_cast<ACPI_TABLE_HEADER**>(&facs));
    if (acpi_status != AE_OK) {
        return ZX_ERR_BAD_STATE;
    }
    acpi_status = AcpiHwSetFirmwareWakingVector(facs, bootstrap_ip, 0);
    if (acpi_status != AE_OK) {
        return ZX_ERR_BAD_STATE;
    }
    auto wake_vector_cleanup = fbl::MakeAutoCall([facs]() {
        AcpiHwSetFirmwareWakingVector(facs, 0, 0);
    });

    bootstrap_data->registers_ptr = reinterpret_cast<uintptr_t>(&regs);

    arch_disable_ints();

    // Save system state.
    platform_suspend();
    arch_suspend();

    // Do the actual suspend
    TRACEF("Entering x86_acpi_transition_s_state\n");
    acpi_status = x86_acpi_transition_s_state(&regs, target_s_state,
                                              sleep_type_a, sleep_type_b);
    if (acpi_status != AE_OK) {
        arch_enable_ints();
        TRACEF("x86_acpi_transition_s_state failed: %x\n", acpi_status);
        return ZX_ERR_INTERNAL;
    }
    TRACEF("Left x86_acpi_transition_s_state\n");

    // If we're here, we've resumed and need to restore our CPU context
    DEBUG_ASSERT(arch_ints_disabled());

    arch_resume();
    platform_resume();
    timer_thaw_percpu();

    DEBUG_ASSERT(arch_ints_disabled());
    arch_enable_ints();
    return ZX_OK;
}

} // namespace

zx_status_t arch_system_powerctl(uint32_t cmd, const zx_system_powerctl_arg_t* arg) {
    if (cmd != ZX_SYSTEM_POWERCTL_ACPI_TRANSITION_S_STATE) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    uint8_t target_s_state = arg->acpi_transition_s_state.target_s_state;
    uint8_t sleep_type_a = arg->acpi_transition_s_state.sleep_type_a;
    uint8_t sleep_type_b = arg->acpi_transition_s_state.sleep_type_b;
    if (target_s_state == 0 || target_s_state > 5) {
        TRACEF("Bad S-state: S%u\n", target_s_state);
        return ZX_ERR_INVALID_ARGS;
    }

    // If not a shutdown, ensure CPU 0 is the only cpu left running.
    if (target_s_state != 5 && mp_get_online_mask() != cpu_num_to_mask(0)) {
        TRACEF("Too many CPUs running for state S%u\n", target_s_state);
        return ZX_ERR_BAD_STATE;
    }

    // Acquire resources for suspend and resume if necessary.
    zx_status_t status;
    if (target_s_state < 5) {
        // If we're not shutting down, prepare a resume path and execute the
        // suspend on a separate thread (see comment on |suspend_thread()| for
        // explanation).
        thread_t* t = thread_create("suspend-thread", suspend_thread,
                                    const_cast<zx_system_powerctl_arg_t*>(arg),
                                    HIGHEST_PRIORITY, DEFAULT_STACK_SIZE);
        if (!t) {
            return ZX_ERR_NO_MEMORY;
        }

        status = thread_resume(t);
        ASSERT(status == ZX_OK);

        zx_status_t retcode;
        status = thread_join(t, &retcode, ZX_TIME_INFINITE);
        ASSERT(status == ZX_OK);

        if (retcode != ZX_OK) {
            return retcode;
        }
    } else {
        struct x86_realmode_entry_data_registers regs;

        DEBUG_ASSERT(target_s_state == 5);
        arch_disable_ints();

        ACPI_STATUS acpi_status = x86_acpi_transition_s_state(&regs, target_s_state,
                                                              sleep_type_a, sleep_type_b);
        arch_enable_ints();
        if (acpi_status != AE_OK) {
            return ZX_ERR_INTERNAL;
        }
    }

    return ZX_OK;
}
