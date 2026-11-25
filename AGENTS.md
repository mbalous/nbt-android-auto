# Head unit safety

**NEVER run `/sbin/shutdown` on the BMW NBT head unit.** This applies to humans and agents, whether working over
SSH, on the serial console, or through scripts or automation. The command causes a boot lock that can only be cleared
by disconnecting the vehicle battery.

To reset the head unit, **use the vehicle's ZGW (central gateway)** to send the diagnostic reset to the NBT.
Follow the [documented NBT reset procedure](src/idrive/reboot/README.md).

## Head unit SSH

Connect the Linux or WSL host to the vehicle's ENET network. The usual address is `169.254.199.99`, user `root`.
Verify the head unit's SSH host key before connecting and keep host-key checking enabled. Use password authentication:

```sh
ssh -oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa -oMACs=+hmac-sha1 \
    -oPreferredAuthentications=password,keyboard-interactive -oPubkeyAuthentication=no \
    -oIdentitiesOnly=yes -oStrictHostKeyChecking=yes root@169.254.199.99
```

If OpenSSL blocks SHA-1 signatures (for example, SSH reports `error in libcrypto`), prefix that command with
`OPENSSL_ENABLE_SHA1_SIGNATURES=1`. Apply this compatibility override only to the SSH process, not globally.

The default password and SSH setup are documented in [README.md](README.md#enable-ssh); it may have been changed.
Treat passwords as sensitive: never include them in commands, process arguments, logs, new files, or responses. Use the
interactive prompt, or a protected pipe/file descriptor with `sshpass -d` when noninteractive access is necessary.
Do not repeatedly retry failed authentication. See [docs/WORKFLOW.md](docs/WORKFLOW.md#ssh-shortcut) for details.

Use read-only commands unless the user has authorized changes. The head unit shell lacks `head`; use `sed 3q` instead.
