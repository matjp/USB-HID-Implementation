# Toshiba Satellite P50 xHCI diagnostic

This diagnostic is read-only. It targets PCI 00:14.0 on the Toshiba P50 described in HARDWARE.md and checks the PCI class for xHCI rather than requiring a generation-specific device ID. It prints capability, operational, root-port and extended-capability state, then waits 30 seconds before exiting.

The binary is built locally from GNU-EFI and is not committed here because GitHub Contents API access available to this session is text-only.
