/*
 * Dell Precision 5820 Tower X-Series (BIOS 2.18.0, DSDT DELL/CBX3)
 *  - fake EC device (macOS needs one, the board has no PNP0C09)
 *  - USBX power properties
 *  - RTC: the DSDT has only the AWAC clock (\_SB.AWAC, ACPI000E, present when STAS == 0) and NO legacy
 *    PNP0B00 device. macOS cannot use AWAC, so for Darwin: hide AWAC (STAS = 1) and add RTC0 (PNP0B00,
 *    CMOS ports 0x70-0x77, IRQ 8). Without an RTC the kernel never finished BSD init: no BSD names, the
 *    GUID scheme never attached to the Ventoy VDisk, no "Still waiting for root device" - just a hang.
 * CPUs: the DSDT already declares 56 Processor() objects (\_SB.SCK0.CPxx, each with _HID ACPI0007),
 * 20 of them enabled in the MADT. Do NOT add SSDT-PLUG-ALT: its 64 extra Processor objects (IDs 0-63)
 * collide with the real ones and macOS hangs at "AppleACPICPU: ProcessorId=". No plugin-type SSDT is
 * needed either - macOS 12.3+ enables XCPM without it.
 */
DefinitionBlock ("", "SSDT", 2, "VTOY", "P5820", 0x00002000)
{
    External (_SB_.PC00.LPC0, DeviceObj)
    External (STAS, IntObj)

    Scope (\_SB.PC00.LPC0)
    {
        Device (RTC0)
        {
            Name (_HID, EisaId ("PNP0B00"))  // AT Real-Time Clock
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x0070, 0x0070, 0x01, 0x08)
                IRQNoFlags () {8}
            })
            // Hide AWAC (its _STA needs STAS == 0). Done in this device's _INI rather than \_INI / \_SB._INI:
            // the DSDT already defines _INI methods, a second one would make this table fail to load.
            // _INI only runs when _STA reports the device present, i.e. under Darwin only.
            Method (_INI, 0, NotSerialized)
            {
                STAS = One
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
