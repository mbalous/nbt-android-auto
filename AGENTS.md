# Head unit safety

**NEVER run `/sbin/shutdown` on the BMW NBT head unit.** This applies to humans and agents, whether working over
SSH, on the serial console, or through scripts or automation. The command causes a boot lock that can only be cleared
by disconnecting the vehicle battery.

If a restart is needed, follow the [documented NBT restart procedure](src/idrive/reboot/README.md) instead.
