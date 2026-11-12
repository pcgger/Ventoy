/*
 * Dell Precision 5820 Tower X-Series (BIOS 2.18.0, DSDT DELL/CBX3)
 *  - fake EC device (macOS needs one, the board has no PNP0C09)
 *  - USBX power properties
 *  - enable legacy RTC instead of AWAC (STAS = 1) for Darwin
 * CPU objects are ACPI0007 devices: use the stock SSDT-PLUG-ALT alongside this table.
 */
DefinitionBlock ("", "SSDT", 2, "VTOY", "P5820", 0x00001000)
{
    External (_SB_.PC00.LPC0, DeviceObj)
    External (STAS, IntObj)

    Scope (\)
    {
        Method (_INI, 0, NotSerialized)
        {
            If (_OSI ("Darwin"))
            {
                STAS = One
            }
        }
    }

    Scope (\_SB.PC00.LPC0)
    {
        Device (EC)
        {
            Name (_HID, "ACID0001")
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin"))
                {
                    Return (0x0F)
                }
                Return (Zero)
            }
        }
    }

    Scope (\_SB)
    {
        Device (USBX)
        {
            Name (_ADR, Zero)
            Method (_DSM, 4, NotSerialized)
            {
                If ((Arg2 == Zero))
                {
                    Return (Buffer (One) { 0x03 })
                }
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
                If (_OSI ("Darwin"))
                {
                    Return (0x0F)
                }
                Return (Zero)
            }
        }
    }
}
