# Minimal x86-64 OS — USB / xHCI Project State

Last updated: 2026-09-08

## Goal

Build a simple x86-64 operating-system kernel targeting reasonably modern PC hardware with UEFI firmware. The immediate hardware goal is USB keyboard + USB mouse, while keeping USB/xHCI implementation out of the kernel if practical.

## Preferred architecture

```
Kernel
  |
  | small interface
  v
USB service
  |-- xHCI driver
  |-- minimal USB handling
  |-- HID keyboard
  |-- HID mouse
  v
xHCI controller
  |-- keyboard
  `-- mouse
```

The kernel should see an abstract input interface rather than USB/xHCI details.

## Current direction

- Target xHCI as the common modern USB controller.
- Initially support one keyboard and one mouse.
- Prefer USB HID boot-protocol keyboard and mouse.
- Avoid implementing a complete general-purpose USB stack.
- Prefer a separate USB service over putting xHCI code in the kernel.
- UEFI is currently viewed primarily as the boot environment, not as the long-term USB runtime abstraction.
- Coreboot/libpayload is a possible source of reusable xHCI code; coreboot firmware replacement is not required.
- Require a PCI xHCI controller with a 64-bit MMIO BAR capability. The assigned MMIO address may be below or above 4 GiB; genuine legacy 32-bit-only BAR support is not a target.
- Treat PCI MMIO BAR width, the assigned MMIO physical address, and xHCI DMA addressing capability as separate properties. In particular, a 64-bit BAR does not by itself establish 64-bit DMA capability; inspect the controller's xHCI addressing capability before relying on 64-bit DMA.

## Verification requirement

Hardware and platform assumptions must be independently verified before they become implementation requirements or design decisions.

For PCI/xHCI work:

- Prefer the applicable PCI and xHCI specifications as primary sources for defined behaviour.
- Cross-check important implementation assumptions against mature open-source implementations such as Linux, coreboot/libpayload, or other established low-level USB/xHCI implementations.
- Use OSDev documentation/forum discussions for practical bare-metal issues and unusual platform behaviour.
- Use vendor/chipset documentation where controller-specific behaviour is relevant.
- Clearly distinguish:
  1. specification-defined behaviour,
  2. vendor-specific behaviour,
  3. established implementation practice,
  4. behaviour actually observed on our test hardware, and
  5. our own inference or assumption.
- Do not promote an inference to a project requirement without verification.
- When sources disagree or behaviour is uncertain, record the uncertainty and test it rather than silently choosing an assumption.
- For important hardware claims, record the source or evidence in the relevant project documentation.

## Experimental safety rules

- Read-only diagnostics come before controller modification.
- The Toshiba development machine is intentionally designated as the sacrificial hardware platform for active xHCI experiments. This is a deliberate project decision to accelerate development.
- Do not perform PCI configuration writes, xHCI MMIO writes, controller resets, ownership changes, DMA activation, ring setup, or interrupt setup on other production/dual-boot hardware unless the experiment has been explicitly reviewed and accepted.
- Active xHCI initialization and DMA experiments should use the Toshiba or other hardware explicitly designated as sacrificial.
- Before active DMA experiments, investigate IOMMU/DMA isolation and establish exactly which memory can be accessed by the controller.
- Prefer experiments that can be independently reset/recovered and that cannot modify storage.
- Treat any uncertainty about controller ownership, firmware state, DMA, or register semantics as a reason to stop and verify.

## Current phase

Read-only xHCI/PCI diagnostics and architecture investigation.

The Toshiba is the designated sacrificial development machine. Once the remaining read-only baseline questions are answered, move promptly to active xHCI initialization experiments on the Toshiba rather than waiting for another sacrificial machine.

## Immediate roadmap

1. Complete the current read-only xHCI baseline with V18.
2. Review the V18 results against the project goal and the verified xHCI architecture.
3. Begin active xHCI initialization experiments on the Toshiba.
4. Establish the minimum DMA/ring/context machinery required for one USB device.
5. Enumerate a USB device and establish basic control/bulk/interrupt transfer capability as required by the chosen HID path.
6. Implement HID boot-protocol keyboard support.
7. Implement HID boot-protocol mouse support.
8. Put the USB/xHCI implementation behind the planned USB service boundary.
9. Provide the kernel with an abstract keyboard/mouse input interface.
10. Integrate the USB service and input path into the minimal x86-64 OS.
11. Use additional non-sacrificial hardware for compatibility testing once the core implementation is stable.

The objective is to keep experiments focused on advancing this roadmap; do not create additional diagnostic versions merely for extra confidence when the existing evidence is sufficient to move forward.
