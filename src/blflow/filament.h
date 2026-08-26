// filament.h - what is loaded, and what the fan should do about it.
//
// This is the device-side glue around two pure pieces: filament_match.h (the
// matcher and the effective-profile rules, REWORK-SPEC 16.2/16.3) and the
// generated filament_db.h table. Everything stateful or Arduino-flavoured lives
// here; the rules themselves stay host-testable.
//
// There is deliberately no cached "current filament" global. Resolving is a
// table walk over 90 records and costs microseconds, so the fan loop, the status
// builder and the MQTT publisher each resolve from the snapshot they already
// hold. That removes a whole class of bug - a stale cache after a tray change
// mid-print - and is why "tray changes take effect immediately" is true by
// construction rather than by cache invalidation.

#ifndef BLSF_FILAMENT_H
#define BLSF_FILAMENT_H

#include <ArduinoJson.h>

#include "config.h"
#include "filament_match.h"
#include "printer_parse.h"

namespace blsf {

// Everything known about the filament in the active tray.
struct FilamentStatus {
    TraySource          source;      // ams | external | manual | none
    int16_t             ams;         // unit id (>= 128 for an AMS-HT), -1 for the external holder
    int16_t             slot;        // 0..3, 254 external, -1 none
    bool                trayKnown;   // the printer has described this tray
    TrayReport          tray;        // its raw fields (empty strings when unknown)
    char                id[24];      // guide id, "" when nothing matched
    char                family[32];  // "PA-GF" even when the guide only has `pa`
    const FilamentInfo* info;        // guide record, nullptr when unmatched
    FilamentEffective   eff;         // the numbers the fan controller uses
};

// Pure apart from reading the PROGMEM tables: same inputs, same answer.
FilamentStatus filamentResolve(const PrinterReport& p, const FilamentConfig& fc,
                               const FanConfig& fan);

// The `filament` block of the status document (REWORK-SPEC 16.4).
void filamentToJson(JsonObject out, const PrinterReport& p, const FilamentConfig& fc,
                    const FanConfig& fan);

// GET /api/filaments - the embedded guide table for the UI's override editor.
void filamentDbToJson(JsonObject out);

}  // namespace blsf

#endif  // BLSF_FILAMENT_H
