---
description: Rule requiring critical self-review before completing tasks
globs: ["*"]
---

# Mandatory Workflow: Critical Self-Review Before Completion

Always perform a rigorous, critical self-review of your work BEFORE concluding any task.

## Review Principles

1. **Challenge Assumptions**: Never assume existing boilerplate, libraries, or generated bindings are 100% accurate. Cross-reference directly with physical schematics and hardware datasheets.
2. **Verify Protocol & Timings**: Check SPI Mode (CPOL/CPHA), chip addressing modes (e.g. HAEN broadcast), reset pulse widths, and power sequencing.
3. **Prevent Boot Blocking**: Ensure peripheral drivers initialize asynchronously or non-blockingly to avoid kernel freezes.
4. **Inspect Memory & Flash Layouts**: Ensure bootloader partitions and storage offsets never collide.
5. **Verify Beyond Build**: Do not consider a task complete simply because code compiles; verify OS enumeration and real device behavior.
