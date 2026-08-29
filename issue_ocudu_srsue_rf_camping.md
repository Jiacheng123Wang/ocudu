# srsUE over-the-air attach fails on upstream `dev`: SSB tracking collapses at CAMPING after a perfect cell search (ZMQ works end-to-end)

## Description

Following the official tutorial [Building a 5G network with srsUE](https://docs.ocudu.org/tutorials/srsue/) on the latest upstream `dev` branch, srsUE cannot complete the attach over RF. The cell search and MIB decode succeed with excellent signal quality, but as soon as the UE transitions to CAMPING, its SSB tracking collapses and SIB1 is never received, so the UE never attaches.

The **identical gNB binary, srsUE binary and network topology work end-to-end over ZMQ** (UE receives an IP and pings the core with 0% packet loss). The failure is therefore specific to the RF (UHD/USRP) path.

## Setup Details

| Component | Detail |
|---|---|
| gNB | OCUDU upstream `dev`, commit `ba62102d2b`, Ubuntu 24.04 (Intel NUC9, i7), UHD 4.6.0, USRP B210 with GPSDO (GPS-locked) |
| UE | srsRAN_4G srsUE, master `f8ec15ac6` (also reproduced on `release_23_11`), Ubuntu 24.04 (laptop, i7-9750H), UHD 4.6.0, USRP B210 without GPSDO (internal clock) |
| Core | Open5GS (native install), AMF on the UE machine (192.168.100.153:38412) |
| RF | Radios ~1 m apart; antennas on both TX/RX and RX2 ports |

Two cell configurations were tested:

1. The stock tutorial config `gnb_rf_b210_fdd_srsUE.yml` (20 MHz, dl_arfcn 368500 = 1842.5 MHz). Note: this frequency carries a strong commercial LTE B3 signal at our location (measured ~18 dB above thermal noise with the gNB switched off), so the cell search only reaches SNR ≈ −3 dB here.
2. A 5 MHz variant at dl_arfcn 361500 = 1807.5 MHz (`gnb_rf_b210_fdd_srsUE_5mhz.yml`, SSB arfcn 361470), in a measured-clean part of band n3. Here the cell search reaches **SNR ≈ +26 dB** and the residual CFO is ≈ +30 Hz (the UE's internal-clock offset is pre-compensated with `freq_offset = 5100` in the srsUE config).

Both configurations show the **same failure signature**; configuration 2 is the cleanest reproducer because the signal quality at the UE is excellent.

## Actual Behavior

**Cell search phase — works perfectly (configuration 2):**

```
[RRC-NR] Proc "Cell Selection" - Cell search found ARFCN=0 PCI=1 epre=-29.3 snr=+27.8 cfo=+25.7 delay=-0.0
         sfn=482 ssb_idx=0 hrf=n scs=15 ssb_offset=8 dmrs_typeA_pos=pos2 coreset0=0 ss0=0 barred=n
[PHY-SA] SYNC: SFN synchronised successfully (SFN=486).
[PHY-SA] Cell Select: SFN synchronized. CAMPING...
```

**CAMPING phase — collapses immediately and never recovers:**

```
[PHY1-NR] [ 4870] SSB-CSI: rsrp=-56.2 epre=-32.5 n0=-32.5 snr=-23.7 cfo=+158.2  delay=-15.8
[PHY1-NR] [ 4880] SSB-CSI: rsrp=-58.2 epre=-32.4 n0=-32.4 snr=-25.8 cfo=-1732.7 delay=+0.6
[PHY1-NR] [ 4890] SSB-CSI: rsrp=-53.5 epre=-32.7 n0=-32.7 snr=-20.8 cfo=-3388.3 delay=-16.4
```

The raw SSB energy (`epre`) stays constant, but the correlation (`rsrp`) drops ~20–25 dB the moment CAMPING starts; the delay/CFO estimates then oscillate wildly (±16 samples, ±3 kHz). The SIB1 PDCCH DMRS correlation stays at ~0.02–0.1 (threshold 0.5), so SIB1 is never decoded and the UE is stuck ("Attaching UE..." forever). The same collapse occurs with the stock 20 MHz tutorial config (there: search snr −2.9 dB, then `rsrp` swings −31→−55 dBfs during CAMPING).

On the UE side we also observe a constant `Detected RF gap of -250.0 us. Tx'ing zeroes.` per slot plus occasional TX underflows, but the collapse persists when TX is disabled (`tx_gain=0`) and when `continuous_tx=true`.

## Expected Behavior

The [tutorial](https://docs.ocudu.org/tutorials/srsue/) shows this configuration attaching successfully ("RRC Connected", "PDU Session Establishment successful"). In our lab the ZMQ path with the **same gNB binary** (commit `ba62102d2b`) and the **same srsUE binary** completes the whole procedure:

```
Random Access Transmission: prach_occasion=0, preamble_index=0, ra-rnti=0x39, tti=174
Random Access Complete.     c-rnti=0x4601, ta=0
RRC Connected
PDU Session Establishment successful. IP: 10.45.0.62
RRC NR reconfiguration successful.

$ sudo ip netns exec ue1 ping -c 10 10.45.0.1
10 packets transmitted, 10 received, 0% packet loss, time 904ms
```

## Steps to Reproduce

1. Build OCUDU at upstream `dev` (commit `ba62102d2b`) and srsRAN_4G srsUE (master or `release_23_11`) on Ubuntu 24.04 with UHD 4.6.0.
2. Start Open5GS (AMF reachable), then:

```bash
# gNB (machine A)
sudo build/apps/gnb/gnb -c configs/gnb_rf_b210_fdd_srsUE_5mhz.yml

# srsUE (machine B)
sudo ./srsue ue_rf_5mhz.conf
```

3. Observe: cell search succeeds (SNR +26 dB) → CAMPING → SIB1 never received → attach never completes.
4. Control: run the ZMQ configs (`configs/gnb_zmq.yaml` + `ue_ocudu_zmq_Ubuntu.conf`) — attach completes and ping works.

## Diagnostic files

Configs and full logs are available and can be attached on request (also as debug-level logs / PCAPs if useful):

- `gnb_rf_b210_fdd_srsUE_5mhz.yml` — 5 MHz n3 cell, `pdcch.common.coreset0_index: 0`, `ss0_index: 0`
- `ue_rf_5mhz.conf` — srsUE config (5 MHz/25 PRB, `dl_nr_arfcn=361500`, `ssb_nr_arfcn=361470`, `freq_offset=5100`, UHD B210)
- `ue.log` / `gnb.log` — full run logs

Key gNB-side facts: N2 setup to the AMF completes; the gNB transmits continuously (verified by spectrum capture at the UE location — ~26 dB above noise floor); PRACH "detections" are noise false alarms (metric ≈ 1.0–1.3, power ≈ −72 dBm) with Msg3 PUSCH always crc=KO, consistent with the UE never transmitting.

## Preliminary localization (what we already ruled out)

Because the ZMQ path works with identical binaries, we first suspected environmental RF factors. All of the following were tested and the CAMPING collapse persisted:

- **RF interference** — excluded: moved to a measured-clean 5 MHz window; search SNR +26 dB (see above).
- **UE clock offset** — the UE B210 free-runs (no GPSDO); residual CFO reduced from +5199 Hz to ~+30 Hz via `freq_offset` — collapse unchanged.
- **srsUE version** — identical on master `f8ec15ac6` and `release_23_11`.
- **Sample rate** — identical at 7.68 / 11.52 / 23.04 Msps (also rules out the odd-decimation CIC path).
- **UHD backend** — identical with the legacy multi_usrp and the RFNoC backend (`device_args=type=b200,rfnoc=1`).
- **UE TX activity** — identical with `tx_gain=0`, `continuous_tx=true`, and with `time_adv_nsamples` removed.
- **srsUE digital CFO loop** — identical with the CFO compensation loop disabled in the source.
- **gNB platform/branch** — identical on Ubuntu (UHD 4.6) and macOS (UHD 4.10), on upstream `dev` and on our `dev_usrsctp` work branch (which only carries macOS-porting changes).

This points at an interaction between srsUE's SA CAMPING synchronization and the UHD radio path that does not occur over ZMQ. We would appreciate any insight into whether a recent `dev`-branch change in the RF/LPHY path (e.g. SSB timing or the continuous-TX behavior) could affect srsUE's camping tracking, or whether this is a known srsUE limitation with a recommended workaround.

## Possible Solution

None yet — we are hoping the maintainers can help identify whether the regression is on the gNB side (OCUDU `dev`) or is a known srsUE/UHD incompatibility, and suggest the appropriate fix or workaround.
