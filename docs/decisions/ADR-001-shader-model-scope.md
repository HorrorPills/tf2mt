# ADR-001: Target TF2's SM2 (dxlevel 95) shader path first

Status: accepted (2026-10-07, M1)

## Context
PLAN.md §8.1 assumed tf2mt would present an adapter identity that makes Source choose dxlevel 98 (SM3). The M1 census
shows that, with the identity DXVK reports today (vendor 0x106b "Apple M1 Max"), TF2 selects the **SM2 path**:
`vs_2_0` + `ps_2_x` only, 1,190 shaders live, using 23 PS / 26 VS opcodes with no pixel-shader control flow.
The game also ships an SM3 path (≈110k `*_30` combos).

## Decision
1. tf2mt's adapter identity and caps will initially reproduce what Source accepts today, so it keeps choosing the
   SM2 path. The M4 translator targets exactly the census subset (ps_2_x + vs_2_0); SM3 opcodes stay loud stubs.
2. DXVK remains a pixel-exact oracle for this path, since both renderers feed Source identical caps.
3. SM3 / dxlevel 98 is a later, separate decision (ADR), driven by measured visual or performance benefit. It would
   need a census pass at dxlevel 98 and the extra translator work (dynamic flow control, vFace/vPos, more registers).

## Consequences
* Smaller, faster-to-validate translator (M4). The 318k-shader VCS superset still contains the SM3 path for later.
* Visual parity with what owners see today under DXVK at the same preset.
* Risk: some effects only exist on the SM3 path. Mitigation: census at higher presets before M7.
