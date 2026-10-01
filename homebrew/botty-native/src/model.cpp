// SPDX-License-Identifier: GPL-3.0-or-later
#include "model.hpp"
#include "json_flat.hpp"
namespace botty {
Probe parseHealth(std::string_view body) noexcept {
    FlatJSON json;if(!json.parse(body))return Probe::malformed;
    const auto version=json.string("version");
    if(json.string("app")!="Botty"||json.string("titleId")!="BTTY00001"||version.empty())return Probe::incompatible;
    const int api=json.number("apiVersion");
    if(api==-1)return version=="0.1.0"?Probe::legacy:Probe::incompatible;
    return api==1?Probe::ready:Probe::incompatible;
}
const char* probeText(Probe p) noexcept {
    switch(p) {
    case Probe::checking:return "CHECKING LOCAL SERVICE";
    case Probe::ready:return "BOTTY API V1 CONNECTED";
    case Probe::legacy:return "BOTTY 0.1.0 CONNECTED";
    case Probe::unavailable:return "SESSION REQUIRED";
    case Probe::rejected:return "LOCAL ACCESS REJECTED";
    case Probe::incompatible:return "SERVICE VERSION NOT SUPPORTED";
    case Probe::malformed:return "INVALID SERVICE RESPONSE";
    case Probe::transmissionUnavailable:return "TRANSMISSION UNAVAILABLE";
    case Probe::workerError:return "NETWORK WORKER UNAVAILABLE";
    }
    return "UNKNOWN STATUS";
}
}
