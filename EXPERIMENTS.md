# Experiment Log

## Compatibility boundary

All active and future controller experiments in this project require HCIVERSION >= 1.0. xHCI 0.x/0.96 controllers are explicitly unsupported and must be rejected before any controller initialization or active experiment.

## Current status

- **E001 (Dell XPS 8950)**: Read-only diagnostic completed. Reports xHCI 1.20, 64 slots, 25 ports, controller was running. No writes performed.
- **E002 (Toshiba Satellite P50)**: Read-only capability/DMA probe completed. Reports xHCI 1.00, 32 slots, 18 ports, controller running. Supported Protocol capability documented.
- **E003–E004**: Read-only BAR/MMIO isolation tests planned but not yet executed.
- **Historical series V03–V27**: Source implementations exist. V27 is superseded by the gated V29 halted-initialization test for this stage; earlier assumptions about physical-address identity and teardown lifetime must not be carried forward.

- **U20 (Toshiba Satellite P50)**: V28/U20 UEFI→service HID handoff validation completed successfully. Controller reports xHCI 1.00, PCI 8086:8C31, BAR 0xF7C00000, OPBASE 0xB0; capabilities: 32 slots, 8 interrupters, 19 ports, AC64=1, HIGH=1, CNR=0. One physical Microsoft VID 045E/PID 07B2 composite USB device on port 4 exposed two supported HID interfaces: keyboard IF=0, EP=0x81, MPS=8, interval=4, report descriptor 75 bytes; mouse IF=1, EP=0x82, MPS=10, interval=1, report descriptor 223 bytes. UEFI reported 6 USB I/O handles, 2 HID candidates, 2 handoff devices. Handoff magic=0x48494458, version=1, size=744. Result: Success. No direct xHCI MMIO writes, no service DMA, no port reset; discovery remained UEFI-only. This validates the UEFI→service discovery boundary and keyboard/mouse-only filtering on the Toshiba.

- **V29 (Toshiba Satellite P50)**: Gate 3 halted initialization preparation completed successfully on hardware using build commit `141a245dfa6f2580b956f8e3e1f89f30f770baf9`. Observed xHCI 1.00, PCI 8086:8C31, BAR 0xF7C00000 (raw BAR low dword displayed as F7C00004/00000000), OPBASE=0x80, 32 slots, 16 scratchpads, AC64=1, HCH=1 and CNR=0 after reset. EFI_PCI_IO_PROTOCOL Map() produced the controller-visible DMA addresses. CONFIG, DCBAAP, CRCR and primary event-ring registers were programmed while halted. CRCR write passed at device address 0x00000000C6B01000 using split low/high DWORD MMIO access; event-ring base and dequeue-pointer readback also passed. Observed DMA addresses: COMMAND/DCBAA=0x00000000C67FF000, scratchpad array=0x00000000C6B00000, CRCR=0x00000000C6B01000, ERSTBA=0x00000000C6B04000, ERDP=0x00000000C6B02000. Final result: `RESULT=Success`, `FAIL STAGE=NONE`, 27 MMIO reads, 14 MMIO writes. All controller pointers were cleared before DMA release. No Run/Stop, doorbell, command execution, interrupt delivery, or USB transfer occurred. Gate 3 is therefore a hardware PASS on the Toshiba.

- **V30 (Toshiba Satellite P50)**: Gate 4 controller-start test completed successfully. V30 reused the UEFI DMA mapping contract, initialized valid CONFIG/DCBAAP/CRCR/primary event-ring state, disabled CPU interrupt delivery, set Run/Stop, observed the controller reach `HCH=0`, then halted and reset it. Hardware output: `RUN: HCH=0 PASS`, `HALT: HCH=1 PASS`, `RESET: CNR=0 HCH=1 PASS`, `COMMANDS=0`, `DOORBELLS=0`, `CPU-INTERRUPTS=0`, `EVENTS=0`, `RESULT=Success`, `FAIL STAGE=NONE`. All controller pointers were cleared before DMA release. No command or USB transfer occurred. Gate 4 is therefore a hardware PASS on the Toshiba.

- **V31 (Toshiba Satellite P50)**: Gate 5 hardware execution completed successfully on 2026-09-11. The keyboard was not connected, as intended for this controller-only command test. Output recorded: xHCI 1.00, PCI 8086:8C31, BAR F7C00004/00000000, OPBASE 0x80, 32 slots, 16 scratchpads, PAGESIZE 4096, DBOFF 0x3000, RTSOFF 0x2000, Protocol Slot Type 0. Exactly one Enable Slot command was submitted at command TRB device address `0x00000000C6803000`, exactly one Doorbell 0 was rung, and exactly one Command Completion Event was observed: type=33, completion code=1 (Success), Slot ID=1, command TRB pointer=`0x00000000C6803000`. CPU interrupts=0, commands=1, doorbells=1, events=1. Reset recovery passed and all controller pointers were cleared before DMA release. Final result: `V31 ENABLE SLOT: PASS SLOT=1`, `RESULT=Success`, `MMIO READS=66`, `WRITES=29`.

## Recording rules

An experiment is marked **completed** only when its result was observed on hardware and its output or a faithful transcript is retained. A source file or Git commit records implementation work, not a hardware result. Each active experiment must record its machine, boot medium, exact binary revision, preconditions, observed output, recovery action, and whether a power cycle was required.

- **V28**: UEFI -> service HID handoff discovery test. V28 is read-only: no xHCI MMIO writes, no DMA allocation, no port reset, no xHCI ring setup.

- **V29**: halted xHCI initialization preparation artifact. Uses EFI_PCI_IO_PROTOCOL common-buffer allocation/mapping for controller-referenced memory; performs halt/reset/CNR-clear and programs CONFIG, DCBAA, CRCR and primary event-ring registers while halted. No Run/Stop, doorbell, command, or transfer. Hardware execution is on Toshiba only. **Hardware result: PASS.**

- **V30**: controller-start artifact. Uses the V29 DMA contract, starts the controller without a command or doorbell, polls HCH, then halts/resets and tears down safely. CPU interrupt delivery remains disabled. Hardware execution is on Toshiba only. **Hardware result: PASS.**

- **V31**: one-command command-ring/event-ring artifact. Submits exactly one Enable Slot command and consumes exactly one Command Completion Event by polling. CPU interrupts remain disabled. Hardware execution is on Toshiba only. **Hardware result: PASS.**

## Gate 6 review outcome

The next stage is deliberately narrower than a general USB driver. The normative xHCI baseline requires the normal device-slot lifecycle: Enable Slot first, then Address Device to transition the slot toward the addressed/default state, followed by configuration using USB SET_CONFIGURATION plus xHCI Configure Endpoint with matching endpoint contexts. The xHCI requirements also require software to wait for command completions before issuing subsequent commands.

Reference: Intel xHCI Specification / Requirements Specification, device-slot lifecycle and command-completion sequencing.

UEFI `EFI_PCI_IO_PROTOCOL` remains the DMA boundary. Common-buffer mappings are coherent between processor and bus master, and controller DMA must use the `DeviceAddress` returned by `Map()`; mappings must remain live until DMA is finished and then be unmapped/freed.

Reference: UEFI Specification 2.9A, EFI PCI I/O Protocol, `Map()` / `Unmap()` / `AllocateBuffer()` common-buffer DMA requirements.

Implementation review conclusions for Gate 6:

1. **Port selection/reset must be explicit.** The V28 root-port/interface facts are hints and validation inputs; the bridge must verify the expected port state and perform the required port reset rather than assuming UEFI's previous enumeration remains valid.
2. **Slot lifecycle must be explicit.** Gate 6 begins with one Enable Slot completion, then allocates/programs the device context and DCBAA entry before Address Device. The V31 Slot ID must not be reused across a reset; every Gate 6 run starts from a fresh controller state.
3. **Address Device requires real device-context state.** Input/Output Device Context layout, context size, alignment, DCBAAP entry, EP0 context, and Address Device command fields require a dedicated implementation review before coding.
4. **EP0 control transfers are the first new transfer path.** Descriptor requests must use a transfer ring/TD appropriate for control endpoint 0, with Setup/Data/Status stages and correct TRB cycle/link semantics. Completion events must be polled and validated before the next operation.
5. **Do not trust only the UEFI descriptor snapshot.** V28's descriptor facts can identify the expected keyboard, but Gate 6 must retrieve enough descriptors from the device to prove that the live device matches the intended keyboard interface/endpoint before configuration.
6. **Configuration is a two-sided operation.** The USB device must receive the appropriate SET_CONFIGURATION request and xHCI must receive a matching Configure Endpoint command. The two must not be treated as interchangeable.
7. **HID boot protocol comes after configuration.** Set Protocol should be issued only after the selected HID interface/endpoint is identified and the configuration is active. Continuous keyboard polling is a later gate.
8. **CPU interrupts remain disabled.** Initial Gate 6 command and transfer completions will continue to be polled, keeping interrupt routing outside the first keyboard bring-up.
9. **Safety recovery remains mandatory.** Once xHCI can reference DMA memory, every error path must conservatively assume the controller may still be active until `HCH=1` is confirmed. No DMA buffer may be freed while controller references remain possible.

Gate 6 implementation must therefore follow: **design review → implementation review → commit → CI/provenance → post-build review → Toshiba hardware test**. No V32 hardware execution is authorized from documentation alone.

## Project focus

**Consolidation task**: Transform V24–V27 into one self-contained `xhci_bridge_init()` with a portable platform operations layer. V28 validated the UEFI handoff; V29 completed the halted-initialization preparation gate; V30 completed the controller-start gate; V31 completed the first command-ring/completion gate. The next experimental stage is Gate 6: one pre-connected wired keyboard, introduced through explicit port reset and device-slot/address/configuration steps. Do not skip directly to continuous HID report polling.

**DMA safety**: V29, V30 and V31 validated the EFI_PCI_IO_PROTOCOL mapping path through halted initialization, a running controller, and a command completion. Before Address Device or control transfers, command/transfer-ring semantics, device-context layout and DMA lifetime must be re-reviewed. Active xHCI/DMA experiments use the Toshiba (sacrificial) only; the Dell XPS 8950 is read-only.

**Fixed two-device bring-up**: Use a versioned `known_hid_device` snapshot from UEFI to identify the known root port and expected keyboard facts, then independently perform xHCI device setup. No hubs, hot-plug, arbitrary descriptors, or mouse traffic in the first Gate 6 test.

## Safety rule

Any experiment that starts/configures xHCI DMA should be performed on sacrificial hardware first.

## Platform for future work

- E003–E004 (read-only MMIO BAR tests) are intentionally between PCI configuration probing and MMIO. If they succeed while E003 fails, the evidence points at the EFI MMIO access path rather than PCI enumeration or BAR interpretation.
