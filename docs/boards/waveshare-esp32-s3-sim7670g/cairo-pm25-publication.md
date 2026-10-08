# Cairo PMS5003T publication contract

Status: project data-quality policy, 2026-09-30. Firmware source now supports
owner-provisioned PMS5003T identity and a separate opt-in batch candidate; that
v2 image has passed host checks but has not been flashed or read back on the
board. The host daily publication pipeline, Cairo reference validation and
dashboard presentation are still to be implemented. This is not an Egyptian
compliance method or an instrument certification.

## What the device already records

The Waveshare logger writes one scheduled row every 10 seconds. For PM2.5, use
`pm25_atmospheric_ug_m3` (the unmodified decoded atmospheric-environment word),
not `pm25_cf1_ug_m3`. Both words are retained. The original 32 UART bytes are
not archived, so a later decoder cannot replay the frame; the stored raw word,
parser error counters and firmware/dictionary identity are the audit trail for
the present schema. The parser checks `42 4d`, length 28 and checksum, explicitly
selects PMS5003T, and stores only fresh, error-free measurements after the
30-second boot warm-up. `pms_status`, frame age, frame receipt time, checksum and
length error counts stay with each row. Because only the latest frame is taken
at each scheduled deadline, changes in the sensor's frame transmission rate do
not give additional weight to periods of changing concentration. Version 2
also stores provisioned sticker digits per row, plus an optional count-based
batch candidate and explicit status. It does not replace the atmospheric PM2.5
word; `calibration_id` remains `unknown`.

`event_time_utc_ns` is null until a host supplies a clock anchor, and becomes
null again after a Waveshare reboot until another anchor. Never assign upload
time or a guessed timestamp to these rows. Keep the original file and columns
even after a quality filter or correction is introduced.

## Deployment record required before outdoor publication

Create a time-effective record for each deployment, linked to station/device
identity and the immutable file set. Record the sensor's own identifier if
available, firmware image hash, dictionary/configuration versions, coordinates,
inlet height above ground, indoor/outdoor status, site type (for example roadside
or residential), inlet/enclosure arrangement, start/end UTC times and who made
each change. Record maintenance, cleaning, sensor replacement, blocked airflow,
rain ingress and condensation as time intervals. Do not infer placement or
sensor identity from the board name or station UUID.

The PMS5003T temperature/RH reading comes from inside the sensor/enclosure. It
does not establish humidity at the outdoor air inlet. For an outdoor site, log a
separate inlet-near RH/temperature measurement or mark inlet humidity unknown;
document rain protection, ventilation and separation from walls, exhausts and
the sensor's own outlet. Flag fog/condensation and maintenance periods for
review. Do not automatically discard high PM values merely because they are
spikes; retain the original observations and the reason for each exclusion.

## Host aggregation policy to implement before publishing daily values

1. Archive and expose the original fixed 10-second rows with UTC timestamp,
   station/device/boot identity, atmospheric PM2.5, PMS5003T RH and status,
   clock status and parser counters. A row is eligible for an environmental
   aggregate only when UTC is anchored, `pms_status = 4`, PM2.5 is present,
   and no recorded deployment/maintenance quality flag invalidates it.
2. Deduplicate by station, boot and sequence; keep clock corrections and epochs
   auditable. Group scheduled rows into UTC hours and then `Africa/Cairo`
   calendar hours.
   For this **project's provisional quality rule**, an hour is complete with at
   least 270 valid 10-second slots of the 360 expected; report the valid slot
   count and percentage. A day is complete with at least 18 complete local
   hours; report valid and expected local hours and percentage. Compute the
   daily mean from complete hourly means, with each hour weighted equally.
   Local days with clock changes can have 23 or 25 expected hours. Missing slots
   remain missing, never zero. Label means below either coverage threshold
   incomplete, and do not compare those means with a 24-hour limit.
3. Retain `raw`, `corrected`, correction formula/version and calibration status
   as distinct fields or datasets. No correction is selected yet. Run the
   assembled enclosure beside a suitable Cairo PM2.5 reference across varied
   conditions, document paired-data coverage and validation, and repeat checks
   after sensor replacement or material maintenance. Roughly 30 days for a
   24-hour comparison is EPA project guidance, not an Egyptian legal rule.
4. Publish as **community PMS5003T observations** with coverage, site context,
   calibration status and quality flags. Only after a local correction has been
   evaluated should a complete corrected local-day mean be described as
   above/below the **numerical** Egyptian 24-hour value. A peak or 10-second
   row is not a legal exceedance. Do not call the feed an official compliance
   determination. Define annual coverage and averaging policy before any annual
   comparison; there is no annual-result pipeline yet.

## Calibration investigation for this unit

Before picking any correction, photograph or transcribe the **PMS5003T module's
own sticker/serial and batch** and link it to the deployment history. The
firmware can now receive that identity through authenticated CONFIG, but the
unit's saved profile and the scanned label have not been physically verified
in a v2 board readback. AirGradient reports that some Plantower
batches can read near zero at low PM concentrations and publishes separate
PMS5003T batch factors. Its batch factors are applied to the **>0.3 µm particle
count** (`particles_gt03_per_01l` here), not multiplied into the atmospheric
PM2.5 mass field. The logger already preserves both inputs, so this comparison
can happen off-device without changing or overwriting its raw rows. The factors
were developed for AirGradient devices/batches; a matching model or batch
label is a hypothesis to test, not proof the formula is valid for this enclosure
in Cairo. Do not select a factor from model name alone or silently stack it
with another correction. For the supplied `PMS5003T-202604081332` label, the
published AirGradient batch candidate is
`0.003964 * particles_gt03_per_01l + 0` µg/m³. Firmware only emits this
candidate after an owner opt-in and valid fresh PMS frame, and labels its status
as candidate only. The date portion must be a real calendar date; the sticker
may have 1–9 numeric unit digits after the eight-digit batch.

## Future peer network

Several nearby units can exchange signed or otherwise attributable readings
and compare overlapping time slots to detect a unit that drifts, fails or
disagrees with colocated peers. First co-locate the units so their differences
under the same air are measured; later roadside and residential locations may
legitimately differ. A peer consensus cannot establish absolute PM2.5 accuracy
if every unit shares the same batch bias, humidity bias or placement problem.
Automatic correction needs a reference anchor and a separately validated,
versioned method. Until then, peer comparison should raise a quality flag or
request inspection, while retaining every original row.

For the Cairo co-location, compare at least these separately: unmodified
atmospheric PM2.5; any exact-batch, count-based AirGradient candidate if the
module is identified; and a locally fitted correction against the reference.
Fit and validate on separate time periods, compare bias/error across low and
high concentrations and RH, and retain the reference's method, timestamp
alignment, paired sample coverage, formula inputs/units and version. If an EPA
formula is tested, identify the exact formula, input field and RH source. The
published AirGradient default was developed for other monitors/aerosols; it is
not automatically a Cairo calibration. Recheck after replacement and across
seasons. With several units, first co-locate them to check reproducibility. A
remote reference is a weaker fallback only if both sites represent comparable
background air rather than different roadside or local plumes.

The published EEAA comparison table lists PM2.5 at 80 µg/m³ for a 24-hour mean
and 50 µg/m³ for a year, for urban and industrial outdoor air, with no one-hour
PM2.5 entry. EEAA also lists later regulatory amendments, including a 2024
decree. Confirm the controlling Arabic text and method with EEAA before a formal
submission; do not encode these values as instant firmware alarms.

## Standards mathematics: keep the forms separate

For this project's **provisional** fixed-slot method, let `x_i` be eligible
atmospheric PM2.5 in µg/m³ from a scheduled 10-second row. For a complete hour
`h`, `H_h = sum(x_i) / n_h`, where `n_h >= 270` valid slots. For a complete Cairo
local day `d`, `D_d = sum(H_h) / m_d`, where `m_d >= 18` complete local hours.
Report `n_h / 360` and `m_d / expected_local_hours` as coverage. The expected
local-hour count comes from the `Africa/Cairo` date, including clock-change
days. These thresholds and the equal-hour weighting are **project choices**,
not completeness or calculation rules found in the cited Egyptian table. A
calendar-year project mean would require its own prespecified day/year coverage
rule; do not silently average only the days that happen to be available.

| Framework | PM2.5 values and mathematical form | Interpretation here |
| --- | --- | --- |
| Egypt, published EEAA table | 80 µg/m³ for a 24-hour mean; 50 µg/m³ for a year | Compare a complete, evaluated Cairo local-day mean with the **numerical** 24-hour value. The cited table does not give a PM2.5 one-hour value or a percentile/allowed-days rule. Confirm the controlling Arabic regulation and formal averaging/completeness method with EEAA. |
| WHO 2021 global health guidance | Annual mean 5 µg/m³; 99th percentile of the year's 24-hour means 15 µg/m³ (roughly 3–4 days above it in a complete year) | A single day above 15 is informative, but WHO's stated short-term form is a **yearly percentile**. WHO guidelines are not Egyptian law. |
| US EPA ambient standard | Primary annual 9.0 µg/m³ as the three-year mean of annual means; 24-hour 35 µg/m³ as the three-year mean of each year's 98th percentile of daily means | A daily reading above 35 is not by itself a US regulatory violation. These forms require regulatory monitoring and data-validity rules, so they are comparisons only for this community sensor. |

The **US AQI** is a separate public communication index, not the Egyptian limit
or a PM2.5 concentration. For a valid PM2.5 24-hour concentration `C`, EPA
truncates `C` to one decimal place, selects the two concentration breakpoints
that enclose it (`BP_lo`, `BP_hi`) and their AQI values (`I_lo`, `I_hi`), then
calculates `I = (I_hi - I_lo) * (C - BP_lo) / (BP_hi - BP_lo) + I_lo` and rounds
the index to an integer. The current US PM2.5 AQI breakpoints include 0.0–9.0
for AQI 0–50 and 9.1–35.4 for AQI 51–100. An hourly or instant PMS value is
not a daily AQI input. EPA's real-time **NowCast** instead uses up to 12 hourly
means with time-varying weights and its own missing-data rule; it must be
labeled separately from a complete local-calendar-day mean.

Calibration is another equation, independent of these standards. A candidate
local fit could be `C_corrected = a * C_atmospheric + b`, or a count-based fit
`C_corrected = a * particles_gt03_per_01l + b`; RH terms require measured RH
at the time scale and location used when fitting. The `a`, `b` and any RH
coefficients must come from paired reference data and be versioned. AirGradient
batch coefficients are for its **count-based** path, not multipliers for
`pm25_atmospheric_ug_m3`. Do not apply a daily-fitted formula to individual
10-second rows without validating that use, or compare a corrected mean with
an uncorrected standard label without saying which data was used.

## Release gates

For a first public **uncalibrated observation feed**:

- [ ] Time-effective deployment and maintenance record exists.
- [ ] Inlet-near humidity is measured or explicitly unavailable; outdoor
      enclosure and inlet placement have been inspected.
- [ ] Rows carry UTC, sensor status and original atmospheric PM2.5; rows without
      an anchored clock are retained in the archive but absent from a timed feed.
- [ ] Public presentation labels community observations, calibration status,
      and quality flags without an official compliance claim.

Before publishing **corrected daily means or numerical-limit comparisons**:

- [ ] Host pipeline implements the fixed-slot, `Africa/Cairo` coverage policy
      above, preserves raw rows and can reproduce each published mean.
- [ ] Cairo reference comparison and any correction are versioned and reviewed.
- [ ] PMS5003T module batch/serial has been recorded, and any count-based batch
      candidate has been evaluated against the Cairo reference before use.
- [ ] Daily presentation shows valid counts, coverage and incomplete days.

Sources: [EEAA ambient air comparison table](https://www.eeaa.gov.eg/Uploads/Project/Files/20230209110915832.pdf),
[EEAA law and amendments](https://www.eeaa.gov.eg/Laws/55/index),
[WHO 2021 PM2.5 guideline table](https://www.ncbi.nlm.nih.gov/books/NBK574591/table/ch3.tab24/),
[US EPA NAAQS table](https://www.epa.gov/criteria-air-pollutants/naaqs-table),
[US EPA AQI calculation guide](https://nepis.epa.gov/Exe/ZyPURL.cgi?Dockey=P101AP0Q.txt),
[Plantower PMS5003 protocol](https://cdn-shop.adafruit.com/product-files/3686/plantower-pms5003-manual_v2-3.pdf),
[EPA air sensor guidebook](https://www.epa.gov/air-sensor-toolbox/how-use-air-sensors-air-sensor-guidebook),
[EPA co-location guidance](https://nepis.epa.gov/Exe/ZyPURL.cgi?Dockey=P1017AWL.TXT),
[AirGradient batch factors](https://www.airgradient.com/documentation/kb/pm-batch-correction-factors/),
[AirGradient PM calibration guide](https://www.airgradient.com/documentation/kb/pm-sensor-calibration-guide/),
[AirGradient batch background](https://www.airgradient.com/documentation/kb/kb-diy-enhance-the-accuracy-of-the-pms-sensors/),
[AirGradient PMS5003 update](https://www.airgradient.com/blog/update-on-pms5003-calibration/),
[Open Air Foundation calibration toolkit](https://openair.foundation/undp-toolkit/setup/calibration/).
