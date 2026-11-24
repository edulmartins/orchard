# iPhone X (d221) A11 PCIe topology

The A11 (`t8015`) has a PCIe host bridge and Wi-Fi that no current device in
this repository describes. This file records the controller and DART topology
read from a physical iPhone X's runtime ADT (see `d221-bringup.md`), so the
driver and device-tree work has a single source. It mirrors what the T8010
PCIe bring-up did, but the A11 layout differs from it in several places.

The values below are from the runtime ADT of one `iPhone10,6` (`d221`),
firmware `iBoot-8422.142.2.700.1`. The property and node names used in this
file (`rc`, `phy`, `phy-ip`, `portN`) follow the driver and this
repository; they are not ADT properties, which are unnamed.

## Host bridge (`apcie`)

| Field | Value |
| --- | --- |
| `compatible` | `apcie,t8015` in the ADT; `apple,t8015-pcie` in the device tree |
| config | `0x610000000` (0x1000000) |
| `rc` | `0x600000000` (0x50000) |
| `phy` | `0x600080000` (0x4000) |
| `phy-ip` | `0x6000c0000` (0x40000) |
| port regs | `0x60N000000`, N = 1..4, each 0x4000 |
| interrupts (SPI) | 340, 343, 346, 349 (ports 0–3) |
| `clock-gates` | 56, 57, 58 |
| `power-gates` | 56 |
| MSI | address `0xbffff000`, vector offset 352 |
| `perst-to-refclk-delay-us` | 60 |

The ADT also lists a 0x4000 window at `0x60N004000` next to each port's
register block. Nothing identifies it: the ADT entries are unnamed and
"nvmmu" does not occur in the iOS 16.7 d221 kernelcache. The device tree
leaves it out until something uses it.

The ADT also carries the SoC tunables this controller needs:
`apcie-common-tunables`, `apcie-phy-tunables`, and per-bridge
`apcie-config-tunables` / `pcie-rc-tunables`. These are the A11-specific
PHY/config register programs; they are not transcribed here, and the h9p
driver's T8015 match data arrives with this series' `PCI: apple-h9p: Add T8015 support` patch.

## Wi-Fi port (pci-bridge3)

| Field | Value |
| --- | --- |
| `apcie-port` | 3 |
| `maximum-link-speed` | 2 |
| `msi-vector-base` | 24 |
| `function-clkreq` | GPIO 0xa3 |
| `function-device_wake` | GPIO 0x28 |
| `function-perst` | GPIO 0xa7 |
| endpoint | `wlan-pcie,bcm4357` (the `herculesk` module, see the d221 recipe), iommu-parent = the `dart-apcie3` mapper |

Bus numbering follows from what the driver does at runtime, not from the ADT:
it hides ports that are not enabled and the host bridge renumbers buses, so
with only port 3 enabled this port comes up as bus 1 and the endpoint's
requester ID is `0x100`. The board `iommu-map` entry uses that ID.

## DARTs

Both are siblings of the controller in the ADT, `compatible =
"apple,t8015-dart", "apple,s5l8960x-dart"`, page size 4096, `dart-options 7`,
`error-reflector 0x233ffc000`, `stt-idx-width 10`, `fetch-config 0xe0303`
and `diag-config 0x80000100` (the `0x80000100` differs from the `0x00000100`
of the s5l8960x, t7001 and s8003 DARTs).

The ADT `vm-offset` is `0x0800000080000000` and `vm-size` is `0x3ffff000`.
The board `iommu-map` uses a `0x80000000` aperture of `0x3ffff000` bytes,
the same pair the other Apple boards use; the meaning of bit 59 in
`vm-offset` is not established here.

| DART | reg | interrupt (SPI) | mapper |
| --- | --- | --- | --- |
| `dart-apcie2` | `0x603008000` | 347 | `mapper-apcie2` |
| `dart-apcie3` | `0x604008000` | 350 | `mapper-apcie3` (the Wi-Fi's iommu-parent) |

## Link bring-up

On one `d221` (kernel `7.3.0-rc1-orchard-apple-16k`), port 3 trains to L0
(LTSSM `0x11`, in 102 ms) and the endpoint enumerates as `0000:01:00.0`
`14e4:440a` (BCM4357) at 5.0 GT/s x1, behind root port `0000:00:03.0`
`106b:100a`. `link_present` reads 0 throughout; `BIT(3)` of LINKSTS is a
T8015 card-detect semantic, not a failure. The SMC `gP0e` rail is active-high.

## Wi-Fi on hardware (2026-11-15)

Branch `d221-wifi-0.3` adds the T8015 DART, the BCM4357 (`14e4:440a`) entry in
brcmfmac and the chip's RAM base. One `d221`, RAM boot of the `d221-check`
payload (kernel `7.3.0-rc1-orchard-apple-16k` with `CONFIG_BRCMDBG`), three
loads of brcmfmac with `debug=0x180004`:

- The phone stays up. With the RAM base at `0x180000` (2026-11-08) it crashed
  right after `brcmf_fw_alloc_request` every time. The `4357b1` firmware
  (13.10.688.4) is linked at `0x170000`: 652 of its 697 text pointers match only
  that base. `brcmf_chip_get_raminfo` reports `base=0x170000 size=0x150000`.
- The DART now translates: the endpoint joins iommu group 0 (type `DMA`), and
  the DART error register stays 0 through the load.
- The firmware does not come up: `FW failed to initialize` 5 s after
  `Bring ARM in running state`, then `Dongle setup failed` and
  `brcmf_fw_crashed`. No `wlan` interface appears.
- No NVRAM is found by default: brcmfmac asks for `brcmfmac4357-pcie.txt`, and
  the layer only has `brcmfmac4357-pcie.apple,herculesk-GINS-{m-5.3,u-6.1}.txt`.
  The Apple OTP-based name selection does not run. With the `GINS-m-5.3`
  profile copied to the generic name (RAM only), NVRAM and the random seed
  are written and the result is the same.
- The one memory dump kept (`brcmf_pcie_get_memdump`, 1376256 bytes from
  `0x170000`) is from the load without NVRAM: the end of RAM holds neither
  NVRAM text nor the random seed's `0xfeedc0de` footer. It holds the firmware
  image unchanged except for 80 bytes from offset `0xa2111`: the RAM range the
  firmware found (`0x170000`-`0x2c0000`), stack pointers near `0x2bdff0`, heap
  pointers and 32 bytes of entropy. So the ARM core ran its early init (heap,
  then the DRBG behind `bcm_rng`) and stopped there; the globals its init sets
  next (at `0x170e04`, `0x170e08`) are still 0. The firmware console is empty,
  and the last word of RAM (the shared-area address the host waits for) stays 0.
  The load with NVRAM left no dump.
- `irq: no irq domain found for pci@3,0` at brcmfmac registration: the PCI
  core looks for an INTx route, and apple-h9p has only MSI.

## Hypotheses and changes (2026-11-23)

Branch `d221-wifi-0.3.6` rebases the series on Orchard 0.3.6 and adds the
following. Built as kernel `7.3.0-rc1-orchard-apple-16k-wifi812`, not yet run.

1. **The module's UART (most likely cause of the stall).** The ADT has two
   `wlan` nodes: the PCIe endpoint below `apcie/pci-bridge3`, and the
   platform-control function `wlan-pcie-uart,bcm4357` (`module-instance
   herculesk`, `function-reg_on`, the host-wake GPIO, `wifi-calibration-msf`,
   `local-mac-address`) below **uart4** (`0x22e610000`, `function-tx` AP GPIO
   215, `function-rts` AP GPIO 217). Every board whose PCIe Wi-Fi comes up in
   this repository (A9 iPad 5, A10 iPad 6 and 7) holds that UART through the
   `apple,t8010-wlan-uart-provider` platform control, which muxes TX and RTS,
   keeps the UART's power domain on and resets the block before pci-pwrctrl
   powers the endpoint. The D221 had none of it. If the module waits on those
   lines (flow control of its own UART, or a strap), it stops in early init,
   as the dump shows, with or without NVRAM. Patch
   `devices/t8015/0022`: the provider node for uart4 in `t8015.dtsi`
   (disabled), enabled on the D221 with its pins and linked from `wifi0`.
2. **NVRAM by OTP.** The board type (`apple,herculesk`) is set, but the 0.3
   series read the OTP size from ChipCommon, which is 0 on this chip, so no
   OTP was read (no `OTP data` lines in the log) and only the bare names were
   tried. The BCM4357 has an always-on backplane (separate GCI `0x840` and PMU
   `0x827` cores), like the BCM4377 family, whose OTP is shadowed in the GCI
   core. `drivers/wifi/0018` now reads the first KiB of the GCI shadow
   (`0x1000`) and looks for Apple's system-vendor tuple (`0x15`, header
   `0x8`); brcmfmac then asks for `apple,herculesk-GINS-<vendor>-<version>`
   before the bare board type. Without a record nothing changes.
3. **The port's interrupt is not missing.** apple-h9p gives each port 8 MSI
   vectors from the controller's block at SPI 352; port 3's base is 24, as the
   ADT's `msi-vector-base`. brcmfmac enables MSI before it requests its
   interrupt, after the firmware is up. The `no irq domain` line is the PCI
   core's INTx lookup through the root port node, which has no
   `interrupt-map`; it is harmless while MSI works. Check `/proc/interrupts`
   for `brcmf_pcie_intr` once the firmware comes up.
4. **Device wake.** The D221 uses the J172 power data: REG_ON, then 100 ms,
   with `device-wake` held low. The iPhone 7 (D11, D111) pulses it and leaves
   it high. Not changed here; next to try if 1 does not help.
5. **The 32 kHz sleep clock.** `brcmf_of_probe` reports no LPO clock. If iBoot
   leaves the PMIC's 32 kHz output off, the chip's PMU would wait for its ILP
   clock. Not changed here.

## Next test

On the installed system, with the `wifi812` kernel booted:

1. `dmesg | grep -i -e wlan -e brcmf -e pwrctrl`: the provider binds
   (`apple-wlan-provider 22e610000.wlan-platform-control`) before the Wi-Fi
   endpoint powers up.
2. Reload brcmfmac with `debug=0x180004` and keep every
   `/sys/class/devcoredump/devcd*/data`: the `OTP data` words, the
   `Apple OTP record at` line and the NVRAM name it asks for.
3. If `FW failed to initialize` remains, the dump of the load with NVRAM is
   the one to read.

## What remains for a working Wi-Fi port

1. Run the `wifi812` kernel (above).
2. If the firmware still stops: device wake (4), then the LPO clock (5).
3. Host wake is not needed: brcmfmac uses it only on SDIO.
