/*
 * Copyright (c) 2020-2022, Liav A. <liavalb@hotmail.co.il>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <AK/Types.h>
#include <Kernel/Arch/x86_64/PCI/IDELegacyModeController.h>
#include <Kernel/Bus/PCI/API.h>
#include <Kernel/Library/LockRefPtr.h>
#include <Kernel/Sections.h>
#include <Kernel/Storage/ATA/ATADiskDevice.h>
#include <Kernel/Storage/ATA/GenericIDE/Channel.h>

namespace Kernel {

UNMAP_AFTER_INIT NonnullLockRefPtr<PCIIDELegacyModeController> PCIIDELegacyModeController::initialize(PCI::DeviceIdentifier const& device_identifier, bool force_pio)
{
    return adopt_lock_ref(*new PCIIDELegacyModeController(device_identifier, force_pio));
}

UNMAP_AFTER_INIT PCIIDELegacyModeController::PCIIDELegacyModeController(PCI::DeviceIdentifier const& device_identifier, bool force_pio)
    : PCI::Device(device_identifier.address())
    , m_prog_if(device_identifier.prog_if())
    , m_interrupt_line(device_identifier.interrupt_line())
{
    PCI::enable_io_space(device_identifier.address());
    PCI::enable_memory_space(device_identifier.address());
    PCI::enable_bus_mastering(device_identifier.address());
    enable_pin_based_interrupts();
    initialize(force_pio);
}

bool PCIIDELegacyModeController::is_pci_native_mode_enabled() const
{
    return (m_prog_if.value() & 0x05) != 0;
}

bool PCIIDELegacyModeController::is_pci_native_mode_enabled_on_primary_channel() const
{
    return (m_prog_if.value() & 0x1) == 0x1;
}

bool PCIIDELegacyModeController::is_pci_native_mode_enabled_on_secondary_channel() const
{
    return (m_prog_if.value() & 0x4) == 0x4;
}

bool PCIIDELegacyModeController::is_bus_master_capable() const
{
    return m_prog_if.value() & (1 << 7);
}

static char const* detect_controller_type(u8 programming_value)
{
    switch (programming_value) {
    case 0x00:
        return "ISA Compatibility mode-only controller";
    case 0x05:
        return "PCI native mode-only controller";
    case 0x0A:
        return "ISA Compatibility mode controller, supports both channels switched to PCI native mode";
    case 0x0F:
        return "PCI native mode controller, supports both channels switched to ISA compatibility mode";
    case 0x80:
        return "ISA Compatibility mode-only controller, supports bus mastering";
    case 0x85:
        return "PCI native mode-only controller, supports bus mastering";
    case 0x8A:
        return "ISA Compatibility mode controller, supports both channels switched to PCI native mode, supports bus mastering";
    case 0x8F:
        return "PCI native mode controller, supports both channels switched to ISA compatibility mode, supports bus mastering";
    default:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

UNMAP_AFTER_INIT void PCIIDELegacyModeController::initialize(bool force_pio)
{
    dbgln("IDE controller @ {}: interrupt line was set to {}", pci_address(), m_interrupt_line.value());
    dbgln("IDE controller @ {}: {}", pci_address(), detect_controller_type(m_prog_if.value()));
    {
        auto bus_master_base = IOAddress(PCI::get_BAR4(pci_address()) & (~1));
        dbgln("IDE controller @ {}: bus master base was set to {}", pci_address(), bus_master_base);
    }

    auto initialize_and_enumerate = [&force_pio](IDEChannel& channel) -> void {
        {
            auto result = channel.allocate_resources_for_pci_ide_controller({}, force_pio);
            // FIXME: Propagate errors properly
            VERIFY(!result.is_error());
        }
        {
            auto result = channel.detect_connected_devices();
            // FIXME: Propagate errors properly
            VERIFY(!result.is_error());
        }
    };

    if (!is_bus_master_capable())
        force_pio = true;

    OwnPtr<IOWindow> primary_base_io_window;
    OwnPtr<IOWindow> primary_control_io_window;
    if (!is_pci_native_mode_enabled_on_primary_channel()) {
        primary_base_io_window = IOWindow::create_for_io_space(IOAddress(0x1F0), 8).release_value_but_fixme_should_propagate_errors();
        primary_control_io_window = IOWindow::create_for_io_space(IOAddress(0x3F6), 4).release_value_but_fixme_should_propagate_errors();
    } else {
        auto primary_base_io_window = IOWindow::create_for_pci_device_bar(pci_address(), PCI::HeaderType0BaseRegister::BAR0).release_value_but_fixme_should_propagate_errors();
        auto pci_primary_control_io_window = IOWindow::create_for_pci_device_bar(pci_address(), PCI::HeaderType0BaseRegister::BAR1).release_value_but_fixme_should_propagate_errors();
        // Note: the PCI IDE specification says we should access the IO address with an offset of 2
        // on native PCI IDE controllers.
        primary_control_io_window = pci_primary_control_io_window->create_from_io_window_with_offset(2, 4).release_value_but_fixme_should_propagate_errors();
    }

    VERIFY(primary_base_io_window);
    VERIFY(primary_control_io_window);

    OwnPtr<IOWindow> secondary_base_io_window;
    OwnPtr<IOWindow> secondary_control_io_window;

    if (!is_pci_native_mode_enabled_on_primary_channel()) {
        secondary_base_io_window = IOWindow::create_for_io_space(IOAddress(0x170), 8).release_value_but_fixme_should_propagate_errors();
        secondary_control_io_window = IOWindow::create_for_io_space(IOAddress(0x376), 4).release_value_but_fixme_should_propagate_errors();
    } else {
        secondary_base_io_window = IOWindow::create_for_pci_device_bar(pci_address(), PCI::HeaderType0BaseRegister::BAR2).release_value_but_fixme_should_propagate_errors();
        auto pci_secondary_control_io_window = IOWindow::create_for_pci_device_bar(pci_address(), PCI::HeaderType0BaseRegister::BAR3).release_value_but_fixme_should_propagate_errors();
        // Note: the PCI IDE specification says we should access the IO address with an offset of 2
        // on native PCI IDE controllers.
        secondary_control_io_window = pci_secondary_control_io_window->create_from_io_window_with_offset(2, 4).release_value_but_fixme_should_propagate_errors();
    }
    VERIFY(secondary_base_io_window);
    VERIFY(secondary_control_io_window);

    auto primary_bus_master_io = IOWindow::create_for_pci_device_bar(pci_address(), PCI::HeaderType0BaseRegister::BAR4, 16).release_value_but_fixme_should_propagate_errors();
    auto secondary_bus_master_io = primary_bus_master_io->create_from_io_window_with_offset(8).release_value_but_fixme_should_propagate_errors();

    // FIXME: On IOAPIC based system, this value might be completely wrong
    // On QEMU for example, it should be "u8 irq_line = 22;" to actually work.
    auto irq_line = m_interrupt_line.value();

    if (is_pci_native_mode_enabled()) {
        VERIFY(irq_line != 0);
    }

    auto primary_channel_io_window_group = IDEChannel::IOWindowGroup { primary_base_io_window.release_nonnull(), primary_control_io_window.release_nonnull(), move(primary_bus_master_io) };
    auto secondary_channel_io_window_group = IDEChannel::IOWindowGroup { secondary_base_io_window.release_nonnull(), secondary_control_io_window.release_nonnull(), move(secondary_bus_master_io) };

    if (is_pci_native_mode_enabled_on_primary_channel()) {
        m_channels.append(IDEChannel::create(*this, irq_line, move(primary_channel_io_window_group), IDEChannel::ChannelType::Primary));
    } else {
        m_channels.append(IDEChannel::create(*this, move(primary_channel_io_window_group), IDEChannel::ChannelType::Primary));
    }
    initialize_and_enumerate(m_channels[0]);
    m_channels[0].enable_irq();

    if (is_pci_native_mode_enabled_on_secondary_channel()) {
        m_channels.append(IDEChannel::create(*this, irq_line, move(secondary_channel_io_window_group), IDEChannel::ChannelType::Secondary));
    } else {
        m_channels.append(IDEChannel::create(*this, move(secondary_channel_io_window_group), IDEChannel::ChannelType::Secondary));
    }
    initialize_and_enumerate(m_channels[1]);
    m_channels[1].enable_irq();
}

}
