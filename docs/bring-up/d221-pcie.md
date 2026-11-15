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
- The memory dump (`brcmf_pcie_get_memdump`, 1376256 bytes from `0x170000`)
  holds the firmware image unchanged except for 80 bytes from offset
  `0xa2112`, so the ARM core ran briefly. The firmware console is empty, and
  the last word of RAM (the shared-area address the host waits for) stays 0.
- `irq: no irq domain found for pci@3,0` at brcmfmac registration: the port's
  INTx/MSI routing is still missing.

## What remains for a working Wi-Fi port

1. Find where the firmware stops: the 80 changed bytes and the CR4 trap
   state in the memory dump.
2. NVRAM selection: read the OTP (`brcmf_pcie_read_otp`, as on Apple Silicon)
   or name the module in the device tree, then try both GINS profiles.
3. The port's interrupt: an irq domain (MSI or INTx) for `pci@3,0`.
4. Host wake is not needed: brcmfmac uses it only on SDIO.
