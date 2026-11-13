/*
 * SSDT-VTOY-GENERIC - board-independent fixes for macOS on Intel and AMD desktops (VtoyBoot "intel"/"amd" profiles).
 * Every object is invisible to other OSes (_STA checks Darwin) and decides at runtime whether it is needed, so the
 * same table can be loaded on any board:
 *   - RTC0 : a legacy PNP0B00 clock, only when the board has AWAC (\_SB.AWAC) and none of the usual legacy RTC
 *            devices exists. macOS has no AWAC driver; without any RTC the kernel never finishes BSD init
 *            (seen on the Dell 5820). Its _INI sets STAS = 1 to hide AWAC, the same thing SSDT-AWAC does.
 *   - EC   : fake embedded controller named EC (AppleBusPowerController wants one; desktops rarely have a usable one)
 *   - USBX : USB port power properties
 * Uses names that do not exist on common boards (\_SB.VTRC, \_SB.EC, \_SB.USBX). If a board already defines \_SB.EC
 * or \_SB.USBX at the root of \_SB this table will not load - such boards need their own profile.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
DefinitionBlock ("", "SSDT", 2, "VTOY", "GENERIC", 0x00001000)
{
    External (STAS, IntObj)
    External (_SB_.AWAC, DeviceObj)
    External (_SB_.PC00.LPC0.RTC_, DeviceObj)
    External (_SB_.PCI0.LPCB.RTC_, DeviceObj)
    External (_SB_.PCI0.LPC0.RTC_, DeviceObj)
    External (_SB_.PCI0.SBRG.RTC_, DeviceObj)
    External (_SB_.PCI0.SBRG.RTC0, DeviceObj)
    External (_SB_.PCI0.LPC_.RTC_, DeviceObj)
    External (_SB_.PC00.LPCB.RTC_, DeviceObj)

    Scope (\_SB)
    {
        Method (VTNR, 0, NotSerialized)  // 1 = AWAC present and no legacy RTC found
        {
            If (!CondRefOf (\_SB.AWAC)) { Return (Zero) }
            If (CondRefOf (\_SB.PC00.LPC0.RTC)) { Return (Zero) }
            If (CondRefOf (\_SB.PC00.LPCB.RTC)) { Return (Zero) }
            If (CondRefOf (\_SB.PCI0.LPCB.RTC)) { Return (Zero) }
            If (CondRefOf (\_SB.PCI0.LPC0.RTC)) { Return (Zero) }
            If (CondRefOf (\_SB.PCI0.LPC.RTC)) { Return (Zero) }
            If (CondRefOf (\_SB.PCI0.SBRG.RTC)) { Return (Zero) }
            If (CondRefOf (\_SB.PCI0.SBRG.RTC0)) { Return (Zero) }
            Return (One)
        }

        Device (VTRC)
        {
            Name (_HID, EisaId ("PNP0B00"))
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x0070, 0x0070, 0x01, 0x08)
                IRQNoFlags () {8}
            })
            Method (_INI, 0, NotSerialized)
            {
                If (CondRefOf (\STAS)) { \STAS = One }
            }
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin") && VTNR ()) { Return (0x0F) }
                Return (Zero)
            }
        }

        Device (EC)
        {
            Name (_HID, "ACID0001")
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }

        Device (USBX)
        {
            Name (_ADR, Zero)
            Method (_DSM, 4, NotSerialized)
            {
                If ((Arg2 == Zero)) { Return (Buffer (One) { 0x03 }) }
                Return (Package (0x08)
                {
                    "kUSBSleepPowerSupply", 0x13EC,
                    "kUSBSleepPortCurrentLimit", 0x0834,
                    "kUSBWakePowerSupply", 0x13EC,
                    "kUSBWakePortCurrentLimit", 0x0834
                })
            }
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
    }
}
