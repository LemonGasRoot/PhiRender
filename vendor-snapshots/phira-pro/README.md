# Phira Pro source snapshot

The working checkout remains separate at `work/Phira-Pro`; it already has its
own Git repository and pre-existing uncommitted work. The parent repository
does not stage or reset that checkout.

- Upstream: https://github.com/Phira-Pro/Phira-Pro
- Base commit: `6f0b80607fab4a838737f552289f07c50f5ae26b`
- `local-changes.patch`: tracked-file changes relative to that commit.
- `untracked/`: previously untracked files that belong to this development
  checkout, including the separately licensed CC0 fallback skin.

After deliberately editing that checkout, run
`powershell -File tools/snapshot-phira.ps1` from the project root, inspect `git diff`, and commit
the updated snapshot. Git in the parent repository cannot automatically
restore files inside the separate checkout; the snapshot is restored using the
fresh-checkout procedure below.

For a fresh development checkout, run `powershell -ExecutionPolicy Bypass
-File .\\tools\\restore-phira.ps1` from the project root. The script clones the
upstream repository at that exact commit, validates and applies the patch, and
copies the contents of `untracked/`. It refuses to overwrite an existing
`work/Phira-Pro` checkout. The matching GPL-3.0 license text is included here
as `LICENSE-GPL-3.0.txt`.

This snapshot is for restoring the existing source state; it does not grant
additional rights to redistribute third-party assets. The local upstream
`assets/respack`, fonts and user's own assets are not included here. See the
project's license and third-party notices before public distribution.
