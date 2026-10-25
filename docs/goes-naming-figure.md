# GOES observation and the PADO / CREST / SIREN naming story

Figma slide 3 combines the satellite render with a redraw of the October 1989
GOES-7 observation that appeared in the original slide.

The illustrations explain the names: PADO is a wave, CREST is the wave's crest,
and SIREN is an alarm that warns of danger. This is a naming metaphor. The plot
shows a historical particle environment measured by GOES-7, not a physical ocean
wave, a CREST measurement, or a SIREN model output.

## Data

- Source: [NOAA/NCEI GOES-7 EPS monthly CSV](https://www.ncei.noaa.gov/data/goes-space-environment-monitor/access/avg/1989/10/goes07/csv/g07_eps_5m_19891001_19891031.csv).
- Displayed period: 18–31 October 1989, UTC (1 November exclusive).
- Corrected integral proton channels: `p3_flux_ic` (>10 MeV), `p4_flux_ic`
  (>30 MeV), and `p7_flux_ic` (>100 MeV).
- Units: pfu = protons / (cm² s sr). The vertical axis is logarithmic.
- All 4,032 five-minute records are retained. Nonpositive/missing values are
  gaps, with no interpolation, smoothing or resampling.
- The >10 MeV series contains 4,015 valid records and 17 gaps. Its maximum is
  42,200 pfu at 20 October 1989 16:00 UTC in this archived product.

## Assets and reproduction

The chart SVG, PNG preview, source hash and channel checks are under
`assets/mission/goes7-oct1989-protons.*`. The three original vector illustrations
are `pado-wave.svg`, `crest-wave.svg`, and `siren-beacon.svg` in the same folder.

With numpy and matplotlib available:

```bash
python tools/plot_goes_mission.py
```

The script downloads the original CSV to ignored `build/data/` and derives the
output location from the repository root. `--source` accepts an existing CSV.
The raw monthly data are not included in the source tree.
