/*
 * Dell Precision 5820 Tower X-Series (BIOS 2.18.0, DSDT DELL/CBX3)
 *  - fake EC device (macOS needs one, the board has no PNP0C09)
 *  - USBX power properties
 *  - enable legacy RTC instead of AWAC (STAS = 1) for Darwin
 * CPUs: the DSDT already declares 56 Processor() objects (\_SB.SCK0.CPxx, each with _HID ACPI0007),
 * 20 of them enabled in the MADT. Do NOT add SSDT-PLUG-ALT: its 64 extra Processor objects (IDs 0-63)
 * collide with the real ones and macOS hangs at "AppleACPICPU: ProcessorId=". No plugin-type SSDT is
 * needed either - macOS 12.3+ enables XCPM without it.
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
