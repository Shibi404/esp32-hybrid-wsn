# Screenshot Checklist

**Deliverable:** *Output screenshots*. Capture these twelve and drop them in
`docs/images/` with the given filenames — the technical report references them
by name, so matching the names means the report is complete the moment the
last one lands.

| # | Filename | What to capture | Source | Proves |
|---|---|---|---|---|
| 1 | `01_boot_banner.png` | Boot banner: node id, role, MAC, core version, channel, CCA state | any serial | Rubric 1 — setup and addressing |
| 2 | `02_ble_scan_rssi.png` | A full BLE round: per-neighbour median RSSI, sample count, estimated distance | N1 serial | Rubric 2 — advertising, scanning, RSSI |
| 3 | `03_localization_result.png` | The `LOCALIZATION:` line with estimate, truth, error, RMSE, anchor count | N1 serial | Rubric 2 — distance estimation + error |
| 4 | `04_dashboard_localization.png` | Dashboard panel 2: the map with anchors, true position, estimate, error line | browser | Rubric 2 + dashboard |
| 5 | `05_mac_sense_backoff_ack.png` | A clean `CCA idle for DIFS -> backoff = N` → `TX` → `ACK ... delivered` sequence | any serial | Rubric 3 — sensing, backoff, ACK |
| 6 | `06_collision_avoided.png` | N1 and N2 side by side during `collide`: different backoffs, one **FROZEN**, both delivered | two serials | Rubric 3 — collision handling (**required**) |
| 7 | `07_collision_forced_retry.png` | `collide hard`: ACK TIMEOUT → `CW 8 -> 16` → retry → delivered | any serial | Rubric 3 — timeout + retransmission |
| 8 | `08_schedule_update.png` | `SCHEDULE UPDATED to v2: alarm raised - dynamic slot granted (N2)` with the before/after slot table | N3 serial | Rubric 4 — dynamic schedule (**required**) |
| 9 | `09_sleep_duty_cycle.png` | `sched` output: unused-slot line, sleep slots, radio duty cycle % | N1 serial | Rubric 4 — unused slots + sleep/wake |
| 10 | `10_aggregation.png` | `Aggregated 3 readings ... 3 packets / 141 B -> 1 packet / 75 B (46.8 %)` plus the min/avg/max line | N3 serial | Rubric 6 — aggregation + reduction |
| 11 | `11_route_failure_recovery.png` | The full `fail 3` cascade: ACK timeouts → LINK FAILURE DETECTED → ALTERNATE ROUTE SELECTED → relay → sink receiving on `1->2->4` | N1 + N4 serials | Rubric 7 — all five recovery steps (**required**) |
| 12 | `12_performance_metrics.png` | `perf` on the sink showing all seven metrics | N4 serial | Rubric 8 — performance analysis |

### Also worth capturing

| Filename | What |
|---|---|
| `13_dashboard_full.png` | The whole dashboard, all seven panels, one screen |
| `14_dashboard_routing.png` | Panel 4 mid-failover, with the green edge moved to `1→2→4` |
| `15_neighbour_table.png` | `nbr` output: RSSI, ACK counts, loss %, cost per neighbour |
| `16_hardware_setup.png` | A photo of the four boards laid out, with a tape measure visible |
| `17_graphs.png` | The PNGs from `tools/plot_metrics.py` |

### Tips

- Use a **white/light serial-monitor theme** if the report will be printed.
- Capture the window including the `>` command you typed — it makes each
  screenshot self-explanatory without a caption.
- For #6, arrange two serial monitors side by side **before** typing `collide`;
  the interesting window is about two seconds long.
- `debug on` adds slot-boundary and CCA detail if a screenshot looks sparse.
- Every screenshot has a timestamp in it. Keep them from one continuous run so
  the numbers in the report are mutually consistent.
