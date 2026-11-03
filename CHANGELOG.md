# Changelog

Notable changes to orchard: what a user, a device owner or a contributor would
notice. Refactors, CI and fixes to unreleased work are left out; the Git
history has those.

Add an entry under **Unreleased** in the same pull request as the change. When
releasing, rename that section to the version (`## [0.3.0] - YYYY-MM-DD`). The
release workflow uses that section as the GitHub release notes and fails if it
is missing or empty.

## [Unreleased]

### Highlights

m1n1 now does the boot work: it reads the boot entries from the device's own
storage, paints the splash and starts the system; the host only picks the
entry. The iPad 6 gets speakers and an accelerometer for screen rotation.

### Devices

- iPhone X (GSM, d221): Linux boots from RAM, with a device description and a
  Wi-Fi firmware recipe. Experimental. (Eduardo Martins)
- iPad Air (J85): measured RAM-boot status documented. (Christopher M. Smith)
- iPad Air 2 (J81): native-root boot, desktop and peripheral status recorded.
  (micro23)

### Install & launch

- m1n1 reads and starts boot entries itself and paints the entry's splash at
  the start of the boot; `orchard launch` only selects the entry, and the
  initrd no longer paints the splash a second time.
- After an iBoot started over USB, m1n1 releases the NVMe boot-from-host
  strap, so storage works on that boot path.
- `orchard shell`, `exec`, `push` and `pull` also reach an installed system,
  not only the recovery.

### Hardware

- iPhone X (GSM, d221): internal storage through the Apple ANS2 NVMe driver
  (`CONFIG_NVME_APPLE`); the recovery's storage fence loads it, and the
  installer's partition writes are tested. No install yet. (Eduardo Martins)
- iPad 6 (J71b): speakers through the MCA audio path, with an ALSA UCM
  profile. (Dalton Alexandre)
- iPad 5: UCM profile for the speakers and internal microphone.
- iPad 6 (J71b): the always-on processor (AOP) runs as an in-kernel driver,
  replacing the userspace DMA owner; its accelerometer is an IIO device with
  the panel's mount matrix, so desktops can rotate the screen. Based on Dalton
  Alexandre's AOP work.
- A10 (T8010): shared IOP mailbox for the GPU and AOP drivers; the NCO clock's
  boot rate is described per board.
- iPad 6 (J71b): sleeps with the GPU desktop and wakes instantly with the Power
  button, touch included. A battery-gauge
  poll that rebooted every HDQ-gauge iPad in sleep is fixed; the other A10
  boards carry the same support, untested.
- Installed systems allow suspend only on boards whose device data says it
  works (`orchard-sleep` in the platform layer); distributions decide when to
  suspend. How to bring it up on another board: docs/bring-up/suspend.md.

### Development

- RAM payloads built in an orchard checkout take the static tools (busybox,
  dropbear, Wi-Fi tools) from the newest nightly when their recipe matches,
  checked against its SHA256SUMS, instead of compiling them;
  `ORCHARD_PUBLISHED=off` builds them locally. (thisiscamk)

### Contributing

- Pull requests are usually reworked to fit the tree before merging, with the
  author's authorship kept, unless the author opts out (see CONTRIBUTING.md).

## [0.2.0] - 2026-10-31

### Highlights

Orchard is now one repository: the CLI and the hardware support merged into a
single `orchard` that runs from any checkout. The kernel is torvalds' tree with
ordered patch layers instead of a fork, built once per platform for every
device. Orchard owns the platform layer (kernel, devicetrees, modules, initrd),
so a distribution builds one device-neutral root for all devices. A new
installer ships with each release.

### Devices

- iPad 6 (Wi-Fi, J71b) remains the qualified device.
- New, experimental: iPad 6 (Cellular), iPad 7, iPhone 7 and 7 Plus,
  including touchscreen firmware and, for the iPhone 7, Wi-Fi and SmartIO
  firmware.
- New, experimental: iPad 5 (J71t, J72t, J71s, J72s), from Berkay Kösebay's
  port.
- New, experimental: iPad Air 2 (J81, J82) with touchscreen and Bluetooth,
  from Martin Puškáč's A8X port; tested on hardware by micro23.
- New, experimental: iPhone 6 (N61) with SPI and touchscreen drivers, from
  Shayne Legault.
- A7, A8 and A8X build as their own platform (`apple-4k`) next to A9–A11
  (`apple-16k`).
- Every device shows its support status and a feature matrix with caveats
  before installing.

### Install & launch

- Installer v2: a guided install that runs on the device, picks targets from
  the partition layout, sets the account password and a sign-in PIN, imports
  this computer's SSH key, and starts the new system right away.
- Each distribution release carries its own installer, so a release installs
  with the tools it was tested with.
- One recovery image for every device that shares a kernel; it only
  bootstraps and reports status.
- `orchard launch` boots an installed system from its boot entry on the
  device. m1n1 reads storage on A9 and A10. Existing installs keep launching
  with the launcher they were installed with.
- Every installed system offers a USB serial login and SSH.
- Every NVMe namespace except the install target is fenced off during an
  install.

### Hardware

- iPad 6: charges from identified Lightning sources, and at 500 mA from
  unidentified ones.
- iPad 6: touchscreen calibration comes from the device at boot.
- Wi-Fi: brcmfmac fixes for receive buffer mapping and the firmware
  handshake; Wi-Fi P2P is disabled for the iPad 5 and 6 firmware. (Dalton
  Alexandre)
- Kernel: Landlock, FUSE, Bluetooth RFCOMM and UHID (Bluetooth LE keyboards
  and mice) enabled. (Dalton Alexandre)

### Development

- `orchard build` and `orchard run` boot any described device from RAM, with
  USB networking, SSH and file transfer.
- Builds share one cache: each file is stored once, `orchard cache` explains
  misses and `--prune` cleans up. The kernel builds incrementally in one
  tree per platform through ccache, and switching patch sets takes half the
  time.
- Firmware recipes are declarative steps (schema 2). Firmware is read from an
  IPSW over HTTP ranges, including LZSS kernelcaches and the board's device
  tree; `orchard` can explore an IPSW during bring-up.
- Orchard Simulator, an iPad-like VM window on macOS, and `tools/qemu-install`
  for testing a release on a virtual disk.
- Builds work on Fedora SELinux hosts and x86_64 hosts, and Linux hosts find
  RAM-boot USB devices. (Anders Haas Franzen, Josh Slaughter, Dalton
  Alexandre)
- One Pongo for every device, built so Xcode 16.3 and later work.
- New guides: contributing, device bring-up, and reading firmware from an
  IPSW.

### Compatibility

- Python 3.11 or later is required.
- Distributions no longer ship a kernel or run kernel-install; orchard writes
  the boot entry and ships the initrd. A distribution release pins one
  orchard release.
- The 0.1 CLI still launches installs made with it.

### Contributors

Berkay Kösebay, Dalton Alexandre, Shayne Legault, micro23, Anders Haas
Franzen, Josh Slaughter, and the ports by Martin Puškáč (A8X) and Pauli1Go
(A10) that this release integrates.
