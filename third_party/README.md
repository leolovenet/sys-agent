# Third-party source

`ftpsrv` is included as a Git submodule pinned to commit
`7c82402e8f9a53400ea33b82eebd961dfa83a422`. It is the small, single-threaded FTP core used
by Sphaira. Its source files identify the project as MIT licensed with SPDX headers, including
`src/ftpsrv.h` and `src/ftpsrv_vfs.h`.

sys-agent supplies its own SD-only VFS and lifecycle integration. It does not use Sphaira's
UI, installer, BIS/save/gamecard mounts, or Sphaira-specific callbacks.
