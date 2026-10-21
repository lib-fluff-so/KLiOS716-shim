# ios-kl7 v0.7.1 — handoff marker fix

## Why this build exists

The physical KL7 run of v0.7.0 proved that the shim could find the test payload in the live initrd, validate its CRC, copy it to `0x140000000`, branch to it, and return to EL1. The final result check failed because the test payload assembled its result marker with the wrong 16-bit halfword.

## Fix

In `src/handoff_test.S`, the third halfword is now `0x5345` rather than `0x4553`, so the payload writes `0x3054534554374c4b`, matching `TEST_PAYLOAD_RESULT` (`KL7TEST0` in little-endian memory). No handoff ABI, address, MMU, CPU or screen behaviour changed.

## Expected device result

After flashing this image and extracting the new ramoops console, the ending should include:

```text
handoff: test payload returned to shim at EL=4
handoff: args=... sig=30544f4f42374c4b x0=... payload_pc=140000000
handoff: payload_magic=3054534554374c4b sctlr=30d00800 currentel=4
handoff: PASS; payload executed above 4 GiB, MMU off, x0=boot_args
progress 100%: payload execution and handoff ABI verified
```

Addresses and SCTLR value may vary; the decisive condition is `handoff: PASS`. The fixed build has not been confirmed on the phone yet.
