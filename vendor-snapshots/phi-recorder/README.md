# Phi-recorder integration snapshot

Upstream: https://github.com/RyderLyx/phi-recorder, commit in BASE_COMMIT.txt.
The tracked upstream changes are in local-changes.patch. New source and regression files are in untracked/; Saira is supplied by the existing project font resource.

The pinned upstream checkout has no standalone LICENSE. On 2026-11-08, the PhiRender project owner explicitly confirmed that the phi-recorder author granted permission to publish the relevant engine code with PhiRender under GPL-3.0-only. This project-specific grant covers code, not an automatic relicensing of third-party images, audio, fonts or shader resources. The project owner retains the underlying permission evidence. See THIRD_PARTY_NOTICES.md for separate resource terms.

User-provided skins and official game artwork are not included here. The local runtime build may use the user's installed resources.

The bundled native source reports version 2.0.0. Upstream test media and upstream documentation are excluded; HUD uses the separately licensed Saira font. PhiRender-specific GUI, worker and converter code remains outside this native source directory.
