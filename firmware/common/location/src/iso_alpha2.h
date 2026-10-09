#pragma once

// ISO 3166-1 assigned alpha-2 codes, snapshot 2026-10-09.
// UN M49 ISO-alpha2 table plus ISO's TW assignment. Country/area codes do not
// assert sovereignty and are never inferred from the H3 cell.
// https://unstats.un.org/unsd/methodology/m49/overview/
// https://www.iso.org/iso-3166-country-codes.html
// https://www.iso.org/obp/ui/#iso:code:3166:TW
namespace aq::location {
inline constexpr char kIsoAlpha2[] =
    "AD AE AF AG AI AL AM AO AQ AR AS AT AU AW AX AZ BA BB BD BE "
    "BF BG BH BI BJ BL BM BN BO BQ BR BS BT BV BW BY BZ CA CC CD "
    "CF CG CH CI CK CL CM CN CO CR CU CV CW CX CY CZ DE DJ DK DM "
    "DO DZ EC EE EG EH ER ES ET FI FJ FK FM FO FR GA GB GD GE GF "
    "GG GH GI GL GM GN GP GQ GR GS GT GU GW GY HK HM HN HR HT HU "
    "ID IE IL IM IN IO IQ IR IS IT JE JM JO JP KE KG KH KI KM KN "
    "KP KR KW KY KZ LA LB LC LI LK LR LS LT LU LV LY MA MC MD ME "
    "MF MG MH MK ML MM MN MO MP MQ MR MS MT MU MV MW MX MY MZ NA "
    "NC NE NF NG NI NL NO NP NR NU NZ OM PA PE PF PG PH PK PL PM "
    "PN PR PS PT PW PY QA RE RO RS RU RW SA SB SC SD SE SG SH SI "
    "SJ SK SL SM SN SO SR SS ST SV SX SY SZ TC TD TF TG TH TJ TK "
    "TL TM TN TO TR TT TV TW TZ UA UG UM US UY UZ VA VC VE VG VI "
    "VN VU WF WS YE YT ZA ZM ZW";
} // namespace aq::location
