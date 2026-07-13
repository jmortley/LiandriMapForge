# Tracked grammar fixtures

Small, source-of-truth excerpts used by the emitters and the bridge — tracked
(the parent `capture/*.t3d` is gitignored; `!fixtures/` re-includes this dir).

## Actor T3D (Layer A / bridge import)
- `utpainvolume.t3d` — a real `UTPainVolume` brush actor (polylist + BodySetup
  AggGeom + DamagePerSec/sounds), excerpted from the DM-Deck capture.
- `generic_lift.t3d` — a real `Generic_Lift_C` (SCS components + `Lift Destination`/
  `Lift Time`), same source.

These are the grammar reference for the goo-pool / lift emitter work, and valid
inputs to `import_t3d` (with the referenced assets preloaded).

## Blueprint graph T3D (Tier 2)
Graph fixtures are **captured from a live editor**, not hand-authored — node text
carries pin GUIDs and `LinkedTo` cross-refs that must be internally consistent,
so transcribing by hand is error-prone. Capture with the bridge:

```
bridge_export_graph(
    asset="/Game/RestrictedAssets/Blueprints/Mutators/.../Mutator_LowGrav",
    save_to=".../capture/fixtures/graph_lowgrav.t3d")
```

Then re-home + import into a fresh BP via `forge_bp_graph(..., graph_t3d=<that text>)`
(`rehome_graph_text` repoints the owning-class refs to the new class). Self-context
*variables* referenced by the graph must be added first (`variables=[...]` /
`add_variable`) or compile reports "Could not find a variable named X".

Good seed candidates (shipped BP mutators, mostly simple graphs): `Mutator_LowGrav`,
`Mutator_BigHead`, `Mutator_FriendlyFire`, `Mutators/LeapFrag/Mutator_LeapFrag`.
