# v0.8.0 hotfix: non-mapped __CTF segment

The XNU Mach-O packer now accepts Apple's non-mapped `__CTF` metadata segment when `vmsize == 0` and `filesize > 0`. It keeps the segment's load-command slot for chained-fixup segment-index alignment, while omitting it from payload segments. The packaging regression test includes this segment.

Validated locally: `make hosttest loadertest xnutest` and legacy `make all`. The actual Sweet kernel file was not available in this environment, so the specific kernel build must be rerun on the developer machine.
