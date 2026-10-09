# Time and private station location

Current native ESP-IDF design, 2026-10-09. The reusable implementations are
`firmware/common/connectivity/src/network_time.*` and the optional
[`location` component](../../firmware/common/location/README.md). Boards own
their network lifecycle and RTC hardware; shared code owns policy, validation
and the telemetry contract. No Arduino API is involved.

## Router and offline time first

Sampling remains on the monotonic 10-second schedule. An available RTC or an
authenticated phone `SET_TIME` can supply UTC without internet. Wi-Fi association
does not establish UTC. Until a real anchor exists, UTC fields remain null and
the file belongs to the unanchored boot.

Automatic SNTP tries these sources in order:

1. NTP addresses advertised by the router through DHCP option 42.
2. The two explicitly configured local or approved server addresses.
3. `time.cloudflare.com`, only when public fallback is enabled and earlier
   candidates fail. Public fallback is enabled by default as the owner requested.

A router must actually advertise or run a time service. The firmware does not
assume that the default gateway is an NTP server. DHCP discovery is prepared
before association; a new lease supplies fresh router addresses. Settings
changes alone do not disconnect Wi-Fi. DNS resolution and network responses
run asynchronously, with bounded attempts and generation checks so a late
lookup cannot replace a newer policy. One endpoint is tried at a time. DNS has
a 30-second budget, a resolved endpoint has a 15-second response budget, and
failed cycles retry with a 30-to-300-second backoff. A successful cycle waits
the configured interval, normally 3,600 seconds.

Only a supported, valid SNTP response produces `clock_status=3`. The storage
worker consumes the anchor through its existing clock-epoch queue. No callback
accesses the card, and finalized files are never redated. Losing Wi-Fi keeps
the existing monotonic estimate; the status reports the age of the latest
network synchronization and when that evidence is stale. A board without a
usable RTC starts unanchored again after reboot.

The common Android/iOS session checks current STATUS clock evidence on each
authenticated connection and refreshes unset/RTC clocks, external anchors older
than six hours, or current estimates more than two seconds from the phone. That accepted host anchor can change
`clock_status` back to 1 even after a successful network anchor, until a later
SNTP cycle succeeds. The network service's last-sync evidence describes that
service; it does not mean NTP exclusively owns the active logger clock.

The implementation uses ordinary SNTP, which is unauthenticated. Encrypted BLE
time provisioning remains useful offline; NTP adds automatic availability, and
does not make an authenticity or precision guarantee. Cloudflare explicitly
documents its [public NTP service](https://developers.cloudflare.com/time-services/ntp/).

## Deployment and country policy

Set `ntp.public_fallback=0` to restrict time acquisition to approved DHCP or
configured endpoints. Disable `ntp.dhcp` too when an arbitrary router should
not select the source. DNS country suffixes, country pool names, anycast and
the station's declared country are not proof that traffic stays inside a
country. Select deployment-approved endpoints and network policy explicitly.
An offline RTC or authenticated phone time remains available without an
external server. See [configuration keys and pages](ble-sync-protocol.md#device-configuration-v2).

The requested [public server gist](https://gist.github.com/mutin-sa/eea1c396b1e610a2da1e5550d94b0453)
was read in full through GitHub's API on 2026-10-09: revision
`3b49ddfb9d081682f551e3eaf04fc9a1b507d4e6`, its complete 8,642-byte file and
196 comments across two API pages. The web view hides earlier comments. It
provides discovery leads, including national services, operator restrictions,
obsolete entries and leap-smear warnings. It is not an availability monitor,
operator permission list or country-law source. No server directory is copied
into firmware and no country restrictions are inferred from it.

Do not mix leap-smearing providers with unsmeared sources. Google's
[operator guidance](https://developers.google.com/time/smear) explains the
compatibility concern. The default public provider is a single unsmeared
service. Generic `pool.ntp.org` and other vendors' zones are not embedded as
appliance defaults: the [NTP Pool vendor rules](https://www.ntppool.org/en/vendors.html)
require an appliance's own vendor zone.

## Station location from the phone

Location means the monitoring device or station's location. It starts unset.
In the Android/iOS mobile app's Device settings, the owner stands beside the
sensor, chooses a resolution and taps **Use phone location**. Permission and a
foreground one-shot fix are requested only from this tap. The phone converts
the fix to H3 before it enters UI state or the device command. Exact phone
coordinates are never persisted, logged or sent to firmware. **Save on device**
sends only a cell, maximum resolution and optional declared country; the app
reads page 1 back before reporting confirmation. This does not enable uploads.
Older firmware without that page shows setup as unsupported.

The default maximum H3 resolution is 5, configurable from 0 through 15.
Lower resolutions disclose larger areas. Resolution 5 averages about 253 km²;
actual area varies, and an approximate phone fix does not become accurate by
choosing a finer cell. The official [H3 resolution table](https://h3geo.org/docs/core-library/restable/)
describes these averages. Fine cells can identify a specific place. Public
sharing requires its own disclosure policy.

Firmware validates the cell using pinned official H3 v4.5.0 and coarsens it to
the configured maximum before persistence. A supplied coarser cell stays
coarser. Changing the maximum to a finer value cannot recover discarded
location; the phone requires a new fix to create a finer candidate. The
public latitude/longitude are always the [H3 grid center](https://h3geo.org/docs/api/indexing/),
derived from the retained cell, not the original fix or a claimed device point.

Optional `location.country` is an owner-declared assigned ISO 3166-1 alpha-2
code, such as `EG`. H3 does not provide administrative country lookup. A cell
may span a border and its center cannot prove a country's assignment.

## Parquet provenance

CoreS3 schema v4 and Waveshare schema v3 append four nullable columns while
preserving all earlier fields:

| Column | Physical type | Meaning |
| --- | --- | --- |
| `h3_cell_id` | INT64 | Valid retained H3 cell, stored as a numeric index |
| `h3_resolution` | INT32 | Actual cell resolution, which may be coarser than the maximum |
| `h3_center_lat_e7` | INT64 | Grid-center latitude in 10⁻⁷ degrees |
| `h3_center_lon_e7` | INT64 | Grid-center longitude in 10⁻⁷ degrees |

Unset or invalid location produces four nulls. Each sample captures its
location settings before entering the worker queue. A cell, precision cap or
country change splits the batch/open file, so a later setting never relabels
buffered readings. Footer metadata contains `country_iso3166_1_alpha2`,
`country_source=owner-declared`, `location_source=owner-provisioned-h3-grid-center`,
the retained cell, actual and maximum resolutions, and center units. Missing
country or cell metadata says `unknown`. Existing archives are immutable.

Host tests and native builds verify contracts and compilation. Router option
42, phone permissions/fixes, RTC writes, radio coexistence and physical card
durability still require dated device evidence.
