KL7 patches for ios-sweet XNU series (xnu-8796.141.3)

Apply AFTER all ios-sweet patches (0001..0081). Names 0101/0102 sort after them.

Manual:
  cp 0101-*.patch 0102-*.patch  <darwin-xnu-build>/patches/        # next to sweet's 00xx patches
  # build.sh applies *.sh first, then every *.patch in sorted order via `git apply`

Check only (in a patched xnu tree):
  git apply --check 0101-kl7-early-framebuffer-geometry.patch
  git apply --check 0102-kl7-match-platform-cpu-fbcon-nodes.patch

0101: early framebuffer geometry (0xFE519E00, 1080x2436, stride 4352) + Device mapping
0102: platform/CPU/fbcon node names (tecno,kl7; arm,cortex-a55/a76; kl7-fbcon)
Not compiled yet. Send build errors back.
