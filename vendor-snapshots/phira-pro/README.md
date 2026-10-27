# Phira Pro source snapshot

The working checkout remains separate at `work/Phira-Pro`; it already has its
own Git repository and pre-existing uncommitted work. The parent repository
does not stage or reset that checkout.

- Upstream: https://github.com/Phira-Pro/Phira-Pro
- Base commit: `6f0b80607fab4a838737f552289f07c50f5ae26b`
- `local-changes.patch`: tracked-file changes relative to that commit.
- `untracked/`: previously untracked files that belong to this development
  checkout, including the separately licensed CC0 fallback skin.

For a fresh development checkout, clone the upstream repository at that exact
commit into `work/Phira-Pro`, run `git apply --check` on the patch, then apply
it and copy the contents of `untracked/` into the corresponding locations.
Do not apply the patch to an existing checkout without first checking its
state: doing so can overwrite or conflict with local work.

This snapshot is for restoring the existing source state; it does not grant
additional rights to redistribute third-party assets. The local upstream
`assets/respack`, fonts and user's own assets are not included here. See the
project's license and third-party notices before public distribution.
